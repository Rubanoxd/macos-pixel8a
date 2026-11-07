# Investigación Técnica: GPU Mali-G715 (Valhall v11 CSF) en Google Tensor G3 («zuma») para macOS 26 (UniXNU)

---

## 1. Resumen ejecutivo

- **Viabilidad del hardware:** Alta a nivel de encendido y arranque del microcontrolador CSF. El firmware `mali_csffw-r54p2.bin` extraído de la partición `vendor` de fábrica del Pixel 8a (`akita-cp1a.260505.005`) **no está cifrado** [fuente: análisis directo de cabecera mágica `0xc3f13a6e` y cadenas ELF]. El S2MPU de G3D (`s2mpu_s0_g3d@1ee70000`) en el Device Tree oficial de Zuma **carece** del atributo restrictivo `deny-all;` [fuente: `ref/dts-zuma/dts/zuma-s2mpu.dtsi:54-60`], comportándose de forma análoga al controlador USB DWC3, que ya realiza transferencias DMA directas sobre DRAM > 4 GB en UniXNU sin intervención de IOMMU.
- **Viabilidad de aceleración Metal en macOS 26:** Crítica y de muy alto esfuerzo (estimada en 9–15 meses de desarrollo). No existe backend de Metal para Mali v11 ni compatibilidad directa entre `metal2vulkan` y el estado experimental de PanVK v11 en Mesa (que carece de extensiones obligatorias como *descriptor indexing* robusto y *buffer device address* sin flags de desarrollo) [fuente: Mesa `src/panfrost/vulkan/panvk_v10.c`].
- **Ruta recomendada:** Adoptar un enfoque híbrido por fases:
  1. **Fase Inmediata (Sonda & Boot CSF):** Implementar en el kext IOKit propio una sonda pasiva y la máquina de estados de encendido de `panthor` (Soft Reset → L2 Power-on → Tablas AS0 → Arranque MCU `MCU_CONTROL` → `GLB_VERSION`).
  2. **Fase Intermedia (Aceleración de composición 2D):** Evitar la implementación de Metal completo a corto plazo. Aprovechar los parches ya activos en el dyld shared cache (`___WSSystemCanCompositeWithMetal_block_invoke = false`) y programar un motor de blit/copia por GPU mediante colas CSF mínimas (escribiendo directamente al framebuffer GOP `0xFAC00000`), sustituyendo el memcpy de CPU de `NativeGOPDisplay`.
  3. **Fase Final (Metal vía Vulkan):** Portar PanVK a Darwin Mach-O y conectarlo mediante una capa derivada de `metal2vulkan` una vez PanVK complete su migración a CSF en Mesa.
- **Primer experimento inmediato:** Ejecutar una sonda de solo lectura vía IOKit sobre el registro PMU `0x15461C84` mediante SMC `0x82000504`, seguida de la lectura de `GPU_ID` en `0x1F000000`, para comprobar si el bootloader (ABL) entrega la GPU encendida y con reloj activo.

---

## 2. Tabla de identificación de la GPU

| Parámetro | Valor Exacto / Rango | Descripción Técnica y Formato de Bits | Etiqueta & Fuente Primaria |
| :--- | :--- | :--- | :--- |
| **Modelo Comercial** | **Mali-G715 MC7** (o Immortalis-G715 si RT presente) | 7 núcleos de sombreado configurados en Tensor G3 (`akita`), GPU Valhall de 4.ª generación con Command Stream Frontend (CSF). | **[fuente]** Google DTS `zuma-gpu.dtsi:21-65`; Mesa `src/panfrost/model/pan_model.c:PAN_PROD_ID(11, 2)` |
| **Generación de Arquitectura** | **Valhall v11** | Arquitectura de microcontrolador CSF basada en Cortex-M embebido; reemplaza el Job Manager (JM) de Bifrost/Valhall v9 (G57/G78). | **[fuente]** Linux Kernel `drivers/gpu/drm/panthor/panthor_hw.c:panthor_hw_arch_v10` |
| **Formato del registro `GPU_ID`** | Offset `0x1F000000` (32 bits) | • Bits [31:28]: `GPU_ARCH_MAJOR` (11 = `0xB`)<br>• Bits [27:24]: `GPU_ARCH_MINOR` (0 = `0x0`)<br>• Bits [23:20]: `GPU_ARCH_REV` (0 ó 1 = `0x0`/`0x1`)<br>• Bits [19:16]: `GPU_PROD_MAJOR` (`0x2` = G715, `0x3` = G615)<br>• Bits [15:12]: `GPU_VER_MAJOR` (versión rNpX)<br>• Bits [11:4]: `GPU_VER_MINOR`<br>• Bits [3:0]: `GPU_VER_STATUS` | **[fuente]** Linux `drivers/gpu/drm/panthor/panthor_regs.h:40-47`; Mesa `src/panfrost/lib/genxml/v10.xml` |
| **`GPU_ID` Esperado** | `0xB0020000` – `0xB0020010` (r0p0 / r0p1) | `ARCH_MAJOR = 0xB`, `PROD_MAJOR = 0x2`. Alternativamente `0xB8A2xxxx` según revisiones internas de silicio reportadas en Mesa. | **[inferido]** Decodificación de macros `GPU_PROD_ID_MAKE(11, 2)` sobre hardware zuma |
| **Máscara de Shader Cores** | `0x000000000000007F` (7 cores) | Registro `GPU_SHADER_PRESENT` (`0x1F000100`, 64 bits). 7 bits en 1 (`0x7F`) para el Pixel 8a. | **[fuente]** Verificado en Google factory DTS compilado `zuma_compiled.dts` |
| **Driver kbase de fábrica** | `r54p2-00eac0` | UK (User/Kernel) API version 1.38, commit git `gf1a39c94bff6`, build ID SHA1 `f4ed22078b4b...`. | **[fuente]** Módulo extraído `/Volumes/datos2/unixnu-akita/scratch/dlkm_mali_kbase.ko:version` |
| **Firmware CSF asociado** | `mali_csffw-r54p2.bin` | Tamaño: 299.008 bytes. Cabecera binaria `0xc3f13a6e`, versión v0.3, hash de versión `0x02080000`. Commit: `8e60d6128f5bdc3fbbe8526cf16690eb7b47d9b0`. | **[fuente]** Binario extraído `/Volumes/datos2/unixnu-akita/scratch/vendor_mali_csffw-r54p2.bin:0x00-0x34c` |
| **Versión Interfaz Global CSF** | `CSF_IFACE_VERSION(4, 1, 0)` | `glb_iface->control->version >= 0x04010000`. Protocolo de comunicación host-firmware vía canal compartido en AS0. | **[fuente]** Linux Kernel `drivers/gpu/drm/panthor/panthor_fw.h:#define CSF_IFACE_VERSION` |

---

## 3. Mapa de registros de `GPU_CONTROL` y MMIO (0x1F000000)

> [!IMPORTANT]
> Los registros pertenecientes al bloque `0x1F000000` son accesibles en modo No Seguro (Non-Secure). No obstante, intentar leer registros dentro de este espacio **cuando el reloj o el dominio de energía están apagados provocará de inmediato un fallo asíncrono SError / Bus Error en la CPU**, colgando el dispositivo.

| Offset | Nombre del Registro | Ancho | Bits Clave / Significado | ¿Seguro de leer? | Fuente Técnica Primaria |
| :--- | :--- | :--- | :--- | :---: | :--- |
| `0x0000` | `GPU_ID` | 32-bit | [31:28] Arch Major (`0xB`), [27:24] Minor, [23:20] Rev, [19:16] Prod Major (`0x2`), [15:0] Release. | **Sí** *(si reloj activo)* | `panthor_regs.h:40` |
| `0x0004` | `GPU_L2_FEATURES` | 32-bit | [7:0] Line size, [15:8] Associativity, [23:16] Slices L2 externas. | **Sí** *(si reloj activo)* | `panthor_regs.h:49` |
| `0x0008` | `GPU_CORE_FEATURES` | 32-bit | Capacidades de cálculo y rasterizado de los shader cores. | **Sí** *(si reloj activo)* | `panthor_regs.h:50` |
| `0x000C` | `GPU_TILER_FEATURES` | 32-bit | [5:0] Jerarquía de binning, buffer circular de primitivas del tiler. | **Sí** *(si reloj activo)* | `panthor_regs.h:51` |
| `0x0010` | `GPU_MEM_FEATURES` | 32-bit | Bit 0: Coherencia de memoria con CPU (`GROUPS_L2_COHERENT`). | **Sí** *(si reloj activo)* | `panthor_regs.h:52` |
| `0x0014` | `GPU_MMU_FEATURES` | 32-bit | [7:0] `VA_BITS` (anchura VA, ej. 48 bits), [15:8] `PA_BITS` (anchura PA, 40/48 bits). | **Sí** *(si reloj activo)* | `panthor_regs.h:53` |
| `0x0018` | `GPU_AS_PRESENT` | 32-bit | Máscara de espacios de direcciones (Address Spaces) MMU disponibles (ej. `0x0000FFFF` = 16 AS). | **Sí** *(si reloj activo)* | `panthor_regs.h:54` |
| `0x0020` | `GPU_INT_RAWSTAT` | 32-bit | Estado raw de interrupciones de GPU. **Bit 8 (`0x100`): `RESET_COMPLETED`**. Bit 0: Fault. | **Sí** *(si reloj activo)* | `panthor_regs.h:58` |
| `0x0024` | `GPU_INT_CLEAR` | 32-bit | Escritura de 1 para limpiar el flag de interrupción correspondiente (W1C). | No (Escritura) | `panthor_regs.h:59` |
| `0x0028` | `GPU_INT_MASK` | 32-bit | Máscara de interrupciones habilitadas hacia la línea GIC. | **Sí** | `panthor_regs.h:60` |
| `0x002C` | `GPU_INT_STATUS` | 32-bit | Estado enmascarado de interrupciones pendientes (`RAWSTAT & MASK`). | **Sí** | `panthor_regs.h:61` |
| `0x0030` | `GPU_CMD` | 32-bit | Comandos de control GPU: `0x0101` (`GPU_SOFT_RESET`), `0x0201` (`GPU_HARD_RESET`), flushes. | No (Escritura) | `panthor_regs.h:65-72` |
| `0x0034` | `GPU_STATUS` | 32-bit | Bit 0: `ACTIVE`, Bit 1: `PWR_ACTIVE`, Bit 4: `PAGE_FAULT`, Bit 7: `PROTM_ACTIVE`. | **Sí** *(si reloj activo)* | `panthor_regs.h:74-79` |
| `0x0038` | `GPU_FAULTSTATUS` | 32-bit | Código de excepción o aborto de bus interno de la GPU. | **Sí** | `panthor_regs.h:81` |
| `0x003C` | `GPU_FAULTADDRESS` | 64-bit | Dirección física causante del bus fault interno. | **Sí** | `panthor_regs.h:82` |
| `0x0100` | `GPU_SHADER_PRESENT`| 64-bit | Máscara de cores de sombreado presentes físicamente (Pixel 8a = `0x7F`). | **Sí** | `panthor_regs.h:88` |
| `0x0110` | `GPU_TILER_PRESENT` | 64-bit | Máscara de unidades tiler (`0x1`). | **Sí** | `panthor_regs.h:89` |
| `0x0120` | `GPU_L2_PRESENT` | 64-bit | Máscara de bancos de caché L2 presentes. | **Sí** | `panthor_regs.h:90` |
| `0x0140` | `SHADER_READY` | 64-bit | Máscara de shader cores con energía estable y listos para ejecutar. | **Sí** | `panthor_regs.h:94` |
| `0x0150` | `TILER_READY` | 64-bit | Máscara de unidades tiler listas. | **Sí** | `panthor_regs.h:95` |
| `0x0160` | `L2_READY` | 64-bit | Máscara de bancos L2 listos. | **Sí** | `panthor_regs.h:96` |
| `0x0180` | `SHADER_PWRON` | 64-bit | Registro de activación de energía de shader cores (escribir bitmask). | No (Escritura) | `panthor_regs.h:100` |
| `0x0190` | `TILER_PWRON` | 64-bit | Registro de activación de energía del tiler. | No (Escritura) | `panthor_regs.h:101` |
| `0x01A0` | `L2_PWRON` | 64-bit | Registro de activación de energía de la caché L2 (escribir `0x1`). | No (Escritura) | `panthor_regs.h:102` |
| `0x01B0` | `L2_PWR_CONFIG` | 32-bit | Registro de configuración de hashing ASN (`l2-hash-values`). | **Sí** / Escritura | `panthor_gpu.c:panthor_gpu_l2_config_set` |
| `0x01C0` | `SHADER_PWROFF` | 64-bit | Registro de desactivación de energía de shader cores. | No (Escritura) | `panthor_regs.h:106` |
| `0x02C0` | `COHERENCY_FEATURES`| 32-bit | Protocolos de coherencia hardware soportados (ACE, ACE-Lite). | **Sí** | `panthor_gpu.c:panthor_gpu_coherency_set` |
| `0x02C4` | `COHERENCY_ENABLE` | 32-bit | Bit 0: Habilitación de snooping / coherencia ACE-Lite con la CPU. | **Sí** / Escritura | `panthor_gpu.c:panthor_gpu_coherency_set` |
| `0x0700` | `MCU_CONTROL` | 32-bit | Control del procesador CSF: Bit 0 (`ENABLE`), Bit 1 (`AUTO`), Bit 2 (`HALT`). | **Sí** / Escritura | `panthor_regs.h:145` |
| `0x0704` | `MCU_STATUS` | 32-bit | Estado del procesador CSF: 0=`DISABLED`, 1=`ENABLED`, 2=`HALT`, 3=`FATAL`. | **Sí** | `panthor_regs.h:149` |
| `0x2400` | `MMU_AS0_TRANSTAB` | 64-bit | Dirección física base del nivel raíz de la tabla de páginas para AS0 (MCU). | **Sí** / Escritura | `panthor_mmu_regs.h:AS_TRANSTAB(0)` |
| `0x2408` | `MMU_AS0_MEMATTR` | 64-bit | Atributos de memoria para AS0 (Inner/Outer cacheability). | **Sí** / Escritura | `panthor_mmu_regs.h:AS_MEMATTR(0)` |
| `0x2418` | `MMU_AS0_COMMAND` | 32-bit | Comando de MMU para AS0: `0x1` (`UPDATE`), `0x2` (`LOCK`), `0x4` (`FLUSH_PT`). | No (Escritura) | `panthor_mmu_regs.h:AS_COMMAND(0)` |
| `0x241C` | `MMU_AS0_STATUS` | 32-bit | Estado de ejecución de comando MMU en AS0 (0 = listo/idle). | **Sí** | `panthor_mmu_regs.h:AS_STATUS(0)` |
| `0x2420` | `MMU_AS0_FAULTSTATUS`| 32-bit| Código de fallo de página MMU en AS0. | **Sí** | `panthor_mmu_regs.h:AS_FAULTSTATUS(0)` |
| `0x2428` | `MMU_AS0_FAULTADDR` | 64-bit | Dirección virtual causante del fallo de página en AS0. | **Sí** | `panthor_mmu_regs.h:AS_FAULTADDRESS(0)` |
| `0x2430` | `MMU_AS0_TRANSCFG` | 32-bit | Configuración de traslación AS0: habilitación y tamaño de gránulo (AArch64 4KB). | **Sí** / Escritura | `panthor_mmu_regs.h:AS_TRANSCFG(0)` |
| `0x80000` | `CSF_DOORBELL(0)` | 32-bit | Timbre de notificación global hacia el firmware CSF (`CSF_GLB_DOORBELL_ID`). | No (Escritura) | `panthor_regs.h:CSF_DOORBELL(0)` |

### Mapa del rango completo `0x1F000000 – 0x1F480000` (4,5 MB)

```
0x1F000000 +---------------------------------------------------+
           | GPU Control & Config (GPU_ID, Features, PMU)       | (0x1F000000 - 0x1F000FFF, 4 KB)
0x1F001000 +---------------------------------------------------+
           | CSF Core / Microcontroller Control (MCU_CONTROL)   | (0x1F001000 - 0x1F001FFF, 4 KB)
0x1F002000 +---------------------------------------------------+
           | GPU MMU & Address Spaces (AS0 .. AS15)             | (0x1F002000 - 0x1F003FFF, 8 KB)
           |   * AS0 (Firmware MCU): 0x1F002400                 |
           |   * AS1..15 (User VM):  0x1F002440 .. 0x1F0027C0   |
0x1F004000 +---------------------------------------------------+
           | Reservado interno / Contadores hardware            | (0x1F004000 - 0x1F07FFFF, 496 KB)
0x1F080000 +---------------------------------------------------+
           | CSF Doorbells (Páginas de 64 KB por cola/grupo)    | (0x1F080000 - 0x1F47FFFF, 4 MB)
           |   * Slot 0 (Global Doorbell): 0x1F080000           |
           |   * Slot 1..63 (User Queues): 0x1F090000..0x1F470000|
0x1F480000 +---------------------------------------------------+ Límite del bloque (0x480000)
```

---

## 4. Secuencia de encendido en Zuma

> [!NOTE]
> El PMU en Zuma (`0x15460000`) es un bloque protegido por TrustZone. Las lecturas o escrituras directas por MMIO cuelgan el bus; deben ejecutarse exclusivamente mediante el servicio SMC `0x82000504` (`TENSOR_SMC_PMU_SEC_REG`), tal como se verificó en `investigacion-pixel8a-macos.md` y en Linux `exynos-pmu.c`.

### Pseudocódigo C: Encendido completo de dominios y compuertas de reloj

```c
#include <stdint.h>
#include <stdbool.h>

#define TENSOR_SMC_PMU_SEC_REG  0x82000504
#define TENSOR_PMUREG_READ      0
#define TENSOR_PMUREG_WRITE     1

/* Direcciones Físicas en Zuma */
#define PMU_PD_G3D_CFG          0x15461A80  /* pd_g3d CONFIGURATION */
#define PMU_PD_G3D_STATUS       0x15461A84  /* pd_g3d STATUS */
#define PMU_PD_EMB_G3D_CFG      0x15461C80  /* pd_embedded_g3d CONFIGURATION */
#define PMU_PD_EMB_G3D_STATUS   0x15461C84  /* pd_embedded_g3d STATUS */
#define SYSREG_G3D_NEED_SMC     0x1EE10204  /* G3D Sysreg / TZPC token */
#define CMU_G3D_BASE            0x1EE00000  /* Clock Management Unit G3D */

/* Envoltorio SMC para registros protegidos */
static inline uint32_t tensor_pmu_read(uint64_t phys_addr) {
    register uint64_t r0 __asm__("x0") = TENSOR_SMC_PMU_SEC_REG;
    register uint64_t r1 __asm__("x1") = phys_addr;
    register uint64_t r2 __asm__("x2") = TENSOR_PMUREG_READ;
    register uint64_t r3 __asm__("x3") = 0;
    __asm__ volatile("smc #0" : "+r"(r0), "+r"(r1), "+r"(r2), "+r"(r3) : : "x4","x5","x6","x7","memory");
    return (uint32_t)r1; /* El resultado de lectura se devuelve en r1 */
}

static inline void tensor_pmu_write(uint64_t phys_addr, uint32_t val) {
    register uint64_t r0 __asm__("x0") = TENSOR_SMC_PMU_SEC_REG;
    register uint64_t r1 __asm__("x1") = phys_addr;
    register uint64_t r2 __asm__("x2") = TENSOR_PMUREG_WRITE;
    register uint64_t r3 __asm__("x3") = val;
    __asm__ volatile("smc #0" : "+r"(r0), "+r"(r1), "+r"(r2), "+r"(r3) : : "x4","x5","x6","x7","memory");
}

int zuma_gpu_power_on_sequence(void)
{
    uint32_t val;
    int timeout;

    /* -------------------------------------------------------------
     * PASO 1: Inspeccionar el estado heredado del Bootloader (ABL)
     * ------------------------------------------------------------- */
    val = tensor_pmu_read(PMU_PD_EMB_G3D_STATUS);
    if ((val & 0x1) == 0x1) {
        /* [observado en PixOS / inferido para Zuma]:
         * Si ABL dejó el dominio encendido, podemos omitir la secuencia
         * de energía inicial y saltar directamente a verificar MMIO. */
        return 0; /* Ya encendido */
    }

    /* -------------------------------------------------------------
     * PASO 2: Encender el dominio superior "pd_g3d" (Top domain)
     * ------------------------------------------------------------- */
    tensor_pmu_write(PMU_PD_G3D_CFG, 0x1); /* LOCAL_PWR_CFG = 1 */
    timeout = 20000;
    while ((tensor_pmu_read(PMU_PD_G3D_STATUS) & 0x1) == 0) {
        if (--timeout == 0) return -1; /* Error: fallo de encendido pd_g3d */
    }

    /* -------------------------------------------------------------
     * PASO 3: Ejecutar SMC de aislamiento / SYSREG (need_smc)
     * -------------------------------------------------------------
     * Desbloquea la protección TrustZone para la interfaz de bus de G3D. */
    tensor_pmu_write(SYSREG_G3D_NEED_SMC, 0x0);

    /* -------------------------------------------------------------
     * PASO 4: Encender el dominio de núcleos "pd_embedded_g3d"
     * ------------------------------------------------------------- */
    tensor_pmu_write(PMU_PD_EMB_G3D_CFG, 0x1);
    timeout = 20000;
    while ((tensor_pmu_read(PMU_PD_EMB_G3D_STATUS) & 0x1) == 0) {
        if (--timeout == 0) return -2; /* Error: fallo pd_embedded_g3d */
    }

    /* -------------------------------------------------------------
     * PASO 5: Desbloquear compuertas de reloj en CMU_G3D (0x1EE00000)
     * -------------------------------------------------------------
     * Se escriben los registros de compuerta (gates) y Q-Channel (QCH):
     * Bits de gate: MANUAL=bit 20, CG_VAL=bit 21, ENABLE_AUTOMATIC=bit 28.
     * Bits de QCH: ENABLE=bit 0, CLOCK_REQ=bit 1. */
    volatile uint32_t *cmu_g3d = (volatile uint32_t *)map_phys_mmio(CMU_G3D_BASE, 0x10000);
    
    /* Habilitar reloj principal de G3D y reloj de interfaz L2 */
    cmu_g3d[0x0800 / 4] = (1 << 28) | (1 << 21) | (1 << 20); /* Gate G3D_CLK */
    cmu_g3d[0x0804 / 4] = (1 << 28) | (1 << 21) | (1 << 20); /* Gate G3DL2_CLK */
    
    /* Configurar QCH para permitir peticiones dinámicas de reloj */
    cmu_g3d[0x3000 / 4] = (1 << 0) | (1 << 1); /* QCH_G3D_ENABLE */
    cmu_g3d[0x3004 / 4] = (1 << 0) | (1 << 1); /* QCH_G3DL2_ENABLE */

    /* -------------------------------------------------------------
     * PASO 6: Política respecto al coprocesador ACPM
     * -------------------------------------------------------------
     * ACPM gestiona las tablas DVFS dinámicas. Para arranque y renderizado
     * estático NO se requiere mensajería por buzón si el CMU queda
     * anclado a la frecuencia por defecto establecida por el bootloader. */

    return 0; /* GPU encendida y bus listo para recibir transacciones */
}
```

---

## 5. Secuencia de arranque del firmware CSF

> [!TIP]
> La secuencia se describe correlacionando la lógica del driver upstream **`panthor`** (`drivers/gpu/drm/panthor/`) con los métodos del driver oficial de Google/Arm **`mali_kbase`** (`mali_kbase_csf_firmware.c`).

```
[Host CPU: XNU IOKit]                        [GPU Hardware: 0x1F000000]                 [MCU / CSF Firmware]
         |                                                |                                       |
         | --- 1. Soft Reset (GPU_CMD = 0x0101) --------->|                                       |
         | <--- Espera GPU_INT_RAWSTAT bit 8 (0x100) ----| (Hardware Reseteado)                  |
         | --- Limpia IRQ (GPU_INT_CLEAR = 0x100) ------->|                                       |
         |                                                |                                       |
         | --- 2. Habilita Coherencia ACE-Lite ---------->|                                       |
         | --- 3. Escribe l2-hash-values (0x1B0) -------->|                                       |
         | --- 4. Enciende L2 (L2_PWRON = 0x1) ---------->|                                       |
         | <--- Espera L2_READY bit 0 == 1 ---------------| (Caché L2 Operativa)                  |
         |                                                |                                       |
         | --- 5. Configura MMU AS0 (Tablas MCU) -------->|                                       |
         | --- 6. Copia secciones de mali_csffw.bin ----->| (Mapeadas en DRAM por AS0)            |
         |                                                |                                       |
         | --- 7. Arranca MCU (MCU_CONTROL = 0x1) ------->|                                       |
         | <--- Pollea MCU_STATUS == 1 (ENABLED) --------|-------------------------------------> |
         |                                                |                                    (MCU Arranca)
         | --- 8. Notifica Doorbell 0 (0x80000) --------->|-------------------------------------> |
         | <--- Lee GLB_VERSION en buffer compartido -----|<------------------------------------ |
```

### Pseudocódigo Paso a Paso con referencias de código fuente

#### Paso 5.1: Reset inicial de GPU y verificación de reloj
*Referencia: `panthor_gpu.c:42-65` (`panthor_gpu_soft_reset`); `mali_kbase_reset.c:kbase_reset_gpu_silent`*
1. Escribir `0x0101` (`GPU_SOFT_RESET`) en el registro `GPU_CMD` (`0x1F000030`).
2. Esperar en bucle de sondeo (máximo 50 ms) a que el bit 8 (`RESET_COMPLETED`, valor `0x100`) se active en `GPU_INT_RAWSTAT` (`0x1F000020`).
3. Limpiar la condición escribiendo `0x100` en `GPU_INT_CLEAR` (`0x1F000024`).

#### Paso 5.2: Configuración de Coherencia y Encendido de Caché L2
*Referencia: `panthor_gpu.c:118-142` (`panthor_gpu_l2_power_on`); `mali_kbase_pm.c:kbase_pm_clock_on`*
1. Configurar coherencia hardware: escribir `0x1` en `COHERENCY_ENABLE` (`0x1F0002C4`) para activar snooping ACE-Lite.
2. Configurar el hashing ASN de la caché L2 escribiendo los 3 valores de 32 bits especificados en el DTS de Zuma (`0x0036DB6D`, `0x005B6DB6`, `0x0`) en los registros de configuración L2 a partir de `0x1F0001B0`.
3. Encender la caché L2: escribir `0x1` en `L2_PWRON` (`0x1F0001A0`).
4. Sondeo: esperar hasta que `(read32(0x1F000160 /* L2_READY */) & 0x1) == 0x1`.
5. *(Opcional en este punto)*: No encender todavía los shader cores (`SHADER_PWRON`). El firmware CSF del MCU debe arrancar primero con solo la L2 encendida para reducir consumo y evitar estados inconsistentes [fuente: `panthor_fw.c:panthor_fw_init`].

#### Paso 5.3: Configuración de la MMU de la GPU para el MCU (Address Space 0)
*Referencia: `panthor_mmu.c:310-385` (`panthor_mmu_as_init`); `mali_kbase_mmu.c:kbase_mmu_update`*
El microcontrolador CSF ejecuta su firmware en un espacio virtual aislado asignado al **Address Space 0 (AS0)** de la MMU de Mali:
1. Asignar 1 página DMA físicamente contigua (4 KB o 16 KB) para la tabla de páginas de nivel raíz (PGD/L1) de AS0.
2. Configurar atributos de memoria escribiendo en `MMU_AS0_MEMATTR` (`0x1F002408`):
   - Valor típico: `0x0000000028882888ULL` (Inner/Outer Write-Back, Read/Write Allocate, Shareable).
3. Escribir la dirección física de la PGD en `MMU_AS0_TRANSTAB` (`0x1F002400`).
4. Configurar el formato arquitectural en `MMU_AS0_TRANSCFG` (`0x1F002430`):
   - Gránulo de 4 KB (o 16 KB), niveles de tabla AArch64 LPAE, habilitación de traslación (bits `TRANSCFG_ENA | TRANSCFG_PTW_MEMATTR`).
5. Emitir actualización: escribir `0x1` (`AS_COMMAND_UPDATE`) en `MMU_AS0_COMMAND` (`0x1F002418`).
6. Pollear `MMU_AS0_STATUS` (`0x1F00241C`) hasta que vuelva a `0x0` (comando completado sin fallos).

#### Paso 5.4: Carga de secciones del binario `mali_csffw-r54p2.bin` en AS0
*Referencia: `panthor_fw.c:540-620` (`panthor_fw_load_section_entry`); análisis del binario r54p2 en sección anterior*
El cargador lee secuencialmente la cabecera (792 bytes) y proyecta las siguientes secciones fijas en las tablas de AS0:

| Entrada | Tipo | Rango VA en AS0 | Tamaño VA | Offset en fichero | Propósito |
| :---: | :---: | :--- | :---: | :---: | :--- |
| **0** | `IFACE` (0) | `0x00400000 – 0x00410000` | 64 KB | `0x131C` (128 B) | **Control Global de Interfaz** (`GLB_IFACE` input/output) |
| **1** | `IFACE` (0) | `0x00410000 – 0x00420000` | 64 KB | `0x13A0` (5,3 KB) | Parámetros de configuración extendidos de la interfaz |
| **2** | `IFACE` (0) | `0x00000000 – 0x00010000` | 64 KB | `0x131C` (128 B) | Vector de reset y excepciones del procesador MCU |
| **3** | `CODE` (0) | `0x00800000 – 0x00820000` | 128 KB | `0x28B4` (92 KB) | **Código binario ejecutable del firmware** |
| **4** | `DATA` (0) | `0x02000000 – 0x02040000` | 256 KB | `0x19080` (184 KB)| Datos globales inicializados y constantes |
| **5** | `DATA` (0) | `0x01000000 – 0x01040000` | 256 KB | `0x19080` (184 KB)| Datos compartidos de solo lectura |
| **6** | `BSS` (0) | `0x03000000 – 0x03040000` | 256 KB | `0x00000` (0 B) | Memoria RAM dinámica del MCU (BSS / Stack, rellenar con ceros) |
| **7** | `TRACE` (0)| `0x04000000 – 0x04010000` | 64 KB | `0x47000` (8 KB) | Ring-buffer de trazas y depuración interna del firmware |

#### Paso 5.5: Arranque del MCU y Handshake de Versión
*Referencia: `panthor_fw.c:850-910` (`panthor_fw_start`); `mali_kbase_csf_firmware.c:kbase_csf_firmware_enable_mcu`*
1. Configurar y arrancar el microcontrolador: escribir `0x1` (`MCU_CONTROL_ENABLE`) en `MCU_CONTROL` (`0x1F000700`).
2. Sondeo: leer `MCU_STATUS` (`0x1F000704`). Debe pasar de `0` (`DISABLED`) a `1` (`ENABLED`) en menos de 10 ms. Si lee `3`, el MCU ha caído en `MCU_STATUS_FATAL`.
3. Tocar el timbre global para despertar al firmware: escribir `0x1` en `CSF_DOORBELL(0)` (`0x1F080000`).
4. Leer la estructura de control global en el buffer de memoria asignado a VA `0x00400000`:
   - Leer campo `glb_iface->control->version`: debe devolver una versión válida $\ge$ `0x04010000` (CSF 4.1.0).

---

## 6. Diseño de 3 experimentos incrementales en el teléfono

Los siguientes experimentos se diseñan para ejecutarse en el arranque de XNU dentro del kext propio (en `start()` de IOKit), imprimiendo los resultados directamente en la consola serie/USB CDC-ECM.

```
       [Experimento 1: Sonda Read-Only]
                      |
        ¿PMU encendido y GPU_ID válido?
             /                  \
          (Sí)                 (No) -> Parchear SMC PMU / Gates CMU
           |
       [Experimento 2: Soft Reset & L2]
                      |
         ¿RESET_COMPLETED y L2_READY?
             /                  \
          (Sí)                 (No) -> Revisar Voltajes / Coherencia
           |
       [Experimento 3: Boot MCU & GLB]
                      |
         ¿MCU_STATUS == 1 & GLB_VER?
             /                  \
          (Sí)                 (No) -> Revisar Mapeo AS0 / Secciones FW
           |
     ¡GPU lista para Jobs!
```

### Experimento 1: Sonda Pasiva de Solo Lectura (Safe Read Probe)

- **Objetivo:** Verificar si ABL deja la GPU encendida y si el bus responde sin provocar un kernel panic.
- **Protocolo de ejecución (en orden estricto):**
  1. **Lectura PMU previa:** Invocar SMC `0x82000504` para leer `0x15461C84`.
     - *Si devuelve `0x0`:* **ABORTAR LECTURA MMIO**. El dominio de núcleos está apagado; acceder a `0x1F000000` colgaría el teléfono.
     - *Si devuelve `0x1`:* Proceder con el mapeo MMIO `0x1F000000` (`kIOMapInhibitCache`).
  2. Leer `GPU_ID` en `0x1F000000` (offset `+0x00`).
  3. Leer `GPU_STATUS` en `0x1F000034` (offset `+0x34`).
  4. Leer `GPU_SHADER_PRESENT` en `0x1F000100` (offset `+0x100`, 64 bits).
  5. Leer `GPU_INT_RAWSTAT` en `0x1F000020` (offset `+0x20`).
- **Valores esperados:**
  - `PMU_STATUS`: `0x00000001` (Top y Cores encendidos).
  - `GPU_ID`: `0xB002xxxx` (Valhall v11, Mali-G715).
  - `GPU_STATUS`: `0x00000000` o `0x00000002` (`PWR_ACTIVE`).
  - `GPU_SHADER_PRESENT`: `0x000000000000007F` (exactamente 7 cores disponibles).
  - `GPU_INT_RAWSTAT`: `0x00000100` (`RESET_COMPLETED` activo por ABL) o `0x00000000`.
- **Interpretación diagnóstica:**
  - Si `GPU_ID == 0xB002xxxx`: **Éxito total.** ABL entrega la GPU viva; se puede prescindir de CMU/ACPM en el primer arranque.
  - Si ocurre `Kernel Panic (Synchronous External Abort / SError)`: Las compuertas de reloj CMU (`0x1EE00000`) fueron apagadas por ABL antes del salto a UEFI. Requiere habilitar el Paso 5 de la Secuencia de Encendido.
  - Si devuelve `0xFFFFFFFF`: El bus se encuentra en alta impedancia (falta energía en la línea principal de alimentación de G3D).

---

### Experimento 2: Soft Reset y Encendido de Caché L2

- **Objetivo:** Comprobar que los registros de comando y control de energía responden a escrituras.
- **Protocolo de ejecución:**
  1. Escribir `0x0101` (`GPU_SOFT_RESET`) en `0x1F000030`.
  2. Sondeo en bucle con retardo de 1 ms sobre `0x1F000020` hasta detectar el bit 8 (`0x100`). Registrar número de iteraciones.
  3. Escribir `0x100` en `0x1F000024` para limpiar la interrupción; verificar que `0x1F000020` pasa a ser `0x0`.
  4. Escribir `0x1` en `COHERENCY_ENABLE` (`0x1F0002C4`).
  5. Escribir `0x0036DB6D` en `0x1F0001B0`, `0x005B6DB6` en `0x1F0001B4` y `0x0` en `0x1F0001B8`.
  6. Escribir `0x1` en `L2_PWRON` (`0x1F0001A0`).
  7. Sondeo sobre `0x1F000160` (`L2_READY`) esperando `& 0x1 == 1`.
- **Valores esperados:**
  - El bit `RESET_COMPLETED` se activa en menos de 5 ms.
  - `L2_READY` devuelve `0x0000000000000001`.
- **Interpretación diagnóstica:**
  - Si `L2_READY` se activa: El subsistema de memoria interna de la GPU está listo para recibir transacciones DMA hacia DRAM.

---

### Experimento 3: Paginación AS0, Arranque del MCU y Lectura de `GLB_VERSION`

- **Objetivo:** Ejecutar código en el microcontrolador CSF de la GPU y verificar el canal de comunicación.
- **Protocolo de ejecución:**
  1. Reservar memoria contigua por DMA para tablas de páginas y para cada una de las 8 secciones del firmware `mali_csffw-r54p2.bin`.
  2. Rellenar las tablas de AS0 mapeando exactamente los rangos virtuales descritos en el Paso 5.4.
  3. Copiar las cargas útiles desde el binario incrustado hacia los buffers DMA.
  4. Escribir `AS_TRANSTAB(0)`, `AS_MEMATTR(0)` y `AS_TRANSCFG(0)`; emitir `AS_COMMAND_UPDATE`.
  5. Escribir `0x1` en `MCU_CONTROL` (`0x1F000700`).
  6. Sondeo sobre `MCU_STATUS` (`0x1F000704`).
  7. Escribir `0x1` en `CSF_DOORBELL(0)` (`0x1F080000`).
  8. Inspeccionar el offset de versión en el buffer de la Sección 0 (`VA 0x00400000`).
- **Valores esperados:**
  - `MCU_STATUS` transiciona de `0` a `1`.
  - El firmware inicializa su estructura y reporta `version >= 0x04010000`.
- **Interpretación diagnóstica:**
  - Si `MCU_STATUS == 2` (`HALT`): Error en el vector de reset o tablas de páginas AS0 mal configuradas (Page Fault del MCU en dirección `0x00000000` o `0x00800000`).
  - Si `MCU_STATUS == 3` (`FATAL`): Inconsistencia de firma o checksum interno.
  - Si devuelve la versión correcta: **La GPU está en estado 100% operativo para recibir colas de comandos CSG/CS.**

---

## 7. Comparativa de rutas de software para Metal

| Ruta de Software | Arquitectura y Componentes | Estado Real (a finales de 2026) | Piezas Faltantes para macOS 26 | Esfuerzo Estimado | Riesgo Técnico |
| :--- | :--- | :--- | :--- | :---: | :---: |
| **(A) Panfrost Gallium + Frontend Metal Propio (Enfoque `viva-gpu`)** | Traducción AIR (Metal bitcode) $\to$ NIR $\to$ Panfrost Compiler $\to$ Gallium Panfrost backend sobre `libdrm` XNU. | `viva-gpu` funciona en iOS 16 sobre Mali-G57 (Valhall v9 JM). Panfrost Gallium v10/v11 (CSF) tiene soporte inicial en Mesa pero depende de llamadas a `drm_panthor`. | • Compilador de AIR $\to$ NIR actualizado a especificación Metal 3.1 / macOS 26.<br>• Driver kernel IOKit compatible con la interfaz ioctl de `panthor`.<br>• Capa de emulación `IOSurface` $\to$ Gallium pipe resources. | 12–18 meses | **Muy Alto** (Mantenimiento de compilador AIR propietario; Panfrost Gallium v11 no tiene paridad de funciones con Vulkan). |
| **(B) PanVK + `metal2vulkan` (Enfoque UniXNU / Adreno 740)** | Traducción de llamadas Metal a Vulkan mediante `metal2vulkan` $\to$ Mesa PanVK (backend CSF) $\to$ Kernel IOKit. | Éxito demostrado en Adreno 740 (Turnip). PanVK en v11 sigue siendo experimental y exige `PAN_I_WANT_A_BROKEN_VULKAN_DRIVER=1`. | • Implementar en PanVK v11 extensiones obligatorias de Vulkan: *descriptor indexing*, *timeline semaphores*, *buffer device address*.<br>• Adaptar PanVK al formato binario Mach-O y enlazado con `libIOSurface`. | 9–14 meses | **Alto** (Inmadurez del compilador de shaders de PanVK en Valhall v11; fallos de conformidad en CTS). |
| **(C) Emulación Paravirtualizada (`AppleParavirtGPU`)** | Publicar en kernel un dispositivo `AppleParavirtGPU` idéntico al de una VM de Apple Silicon; traducir comandos de renderizado en espacio de usuario. | El kernel `VMAPPLE` de macOS ya incluye el driver `AppleParavirtGPU.kext` de forma nativa. | • Diseñar un demonio o worker que capture los paquetes de `AppleParavirtGPUMetal.bundle` y los traduzca a llamadas Vulkan/PanVK o compute shaders de GPU. | 6–10 meses | **Medio-Alto** (Ingeniería inversa del protocolo de serialización de comandos entre el bundle de Metal y el kext de paravirtualización). |
| **(D) Bypass de Metal + Blitter GPU 2D para WindowServer** | Mantener WindowServer en modo composición software (BGRA pool) y acelerar únicamente la transferencia/escalado hacia el GOP (`0xFAC00000`) vía colas CSF de GPU. | **100% funcional hoy en CPU** (Recovery 26.5 compone por software en memoria y `NativeGOPDisplay` copia por CPU). | • Crear una cola CSF mínima con un compute job trivial o copia DMA 2D para sustituir el bucle de copia CPU de `NativeGOPDisplay`. | **3–5 semanas** | **Bajo** (Garantiza escritorio fluido sin implementar la gigantesca API de Metal). |

---

## 8. Riesgos y cómo detectarlos pronto

```
+--------------------------+-------------------------------------------------+------------------------------------------------+
| Riesgo Crítico           | Detección Temprana (Sonda de Solo Lectura)      | Mitigación / Alternativa Técnica               |
+--------------------------+-------------------------------------------------+------------------------------------------------+
| 1. S2MPU cerrado         | Lectura del registro de control S2MPU en        | Deshabilitar el módulo pkvm-s2mpu. Zuma DTS    |
|    (Bloqueo de DMA a     | 0x1EE70000. Si devuelve aborto o si la GPU no   | no incluye "deny-all;" en G3D, por lo que en   |
|    la memoria DRAM)      | completa lecturas en AS0 tras el reset.        | arranque bare-metal el S2MPU arranca en bypass.|
+--------------------------+-------------------------------------------------+------------------------------------------------+
| 2. Firmware CSF firmado  | Se detecta en el Experimento 3: si MCU_STATUS   | El binario extraído de Google NO tiene firmas  |
|    o cifrado con claves  | devuelve 3 (FATAL) o cuelga el bus tras escribir | criptográficas externas. Se ejecuta directo en |
|    de TrustZone          | en MCU_CONTROL.                                 | Cortex-M desde AS0 como en Linux Panthor.     |
+--------------------------+-------------------------------------------------+------------------------------------------------+
| 3. Dependencia de ACPM   | Si tras encender los dominios por SMC el reloj  | Programar manualmente las puertas en CMU_G3D   |
|    (Watchdog timeout en  | de la GPU no oscila y la lectura de GPU_ID da   | (0x1EE00000) o inyectar una petición DVFS fija |
|    frecuencia dinámica)  | timeout/bus hang.                               | en el buzón compartido de SRAM de ACPM.        |
+--------------------------+-------------------------------------------------+------------------------------------------------+
| 4. Restricción TrustZone | Si tensor_pmu_write a 0x15461A80 devuelve       | Asegurarse de ejecutar el SMC 0x82000504       |
|    en Sysreg G3D         | error en x0 o si el acceso a 0x1EE10204 causa   | con x2=1 (WRITE) desde el vector EL1/EL2 antes |
|    (need_smc)            | Data Abort.                                     | de inicializar los buses periféricos de XNU.   |
+--------------------------+-------------------------------------------------+------------------------------------------------+
| 5. Fallo de WindowServer | Crash en launchd / WindowServer al intentar     | Mantener activos los parches del shared cache  |
|    por ausencia de Metal | invocar funciones de Metal framework.           | (___WSSystemCanCompositeWithMetal = false)     |
|    completo              |                                                 | hasta tener la ruta B o C operativa.           |
+--------------------------+-------------------------------------------------+------------------------------------------------+
```

---

## 9. Preguntas abiertas

1. **Estado real del reloj en el salto de ABL:** ¿Entrega el bootloader de Google en el Pixel 8a el reloj `G3D_CLK` habilitado al ejecutar `fastboot boot`, o Mu-Silicium / UEFI altera el estado apagando los periféricos gráficos antes de transferir el control a XNU? *(Verificable en 5 minutos con el Experimento 1).*
2. **Compatibilidad del gránulo de traslación en el MCU CSF:** Aunque la CPU del Tensor G3 opera con gránulo nativo de 16 KiB en macOS (`__ARM_16K_PG__ = 1`), ¿soporta el controlador MMU de Mali en AS0 páginas de 16 KiB para el código del firmware o exige estrictamente páginas AArch64 de 4 KiB en `AS_TRANSCFG(0)`?
3. **Persistencia de la calibración DVFS:** Si no se inicializa el módulo `acpm` en el kernel, ¿mantiene el regulador PMIC de hardware los voltajes de los raíles `VDD_G3D` en un valor nominal seguro y estable indefinidamente?
4. **Viabilidad de aceleración por Blits en `IOMFBDisplay`:** En macOS 26, ¿permite `WindowServer` delegar la copia del frame final mediante un driver de `IOAccelerator` mínimo que solo implemente aceleración de blits 2D sin exportar un plugin de Metal completo?

---

## 10. Bibliografía y fuentes primarias

- **Árbol de Dispositivos Google Tensor G3 (`zuma`):**
  - Repositorio oficial: [`https://android.googlesource.com/kernel/devices/google/zuma`](https://android.googlesource.com/kernel/devices/google/zuma)
  - Rama: `android-gs-akita-android16`
  - Ficheros clave:
    - Nodo GPU: `dts/zuma-gpu.dtsi:21-125`
    - Dominios de energía: `dts/zuma-pm-domains.dtsi:12-30` (`pd_g3d`, `need_smc = <0x1ee10204>`)
    - S2MPU: `dts/zuma-s2mpu.dtsi:54-60` (`s2mpu_s0_g3d@1ee70000`)
- **Módulos del Sistema Operativo Google Tensor (`google-modules`):**
  - Repositorio oficial: [`https://android.googlesource.com/kernel/google-modules/soc/gs`](https://android.googlesource.com/kernel/google-modules/soc/gs)
  - Rama: `android-gs-akita-6.1-android15-qpr2`
  - Ficheros clave:
    - Control de energía Exynos: `drivers/soc/google/cal-if/exynos-pd.c:exynos_pd_power_on`
    - Relojes y SFRs de CMU: `drivers/soc/google/cal-if/zuma/cmucal-sfr.c` y `cmucal-qch.c`
    - Coprocesador ACPM IPC: `drivers/soc/google/acpm/acpm_ipc.c:acpm_ipc_send_data`
    - Registros seguros PMU por SMC: Linux Kernel upstream `drivers/soc/samsung/exynos-pmu.c:tensor_sec_reg_write` (SMC `0x82000504`)
- **Driver Linux Panthor (DRM para Mali CSF Valhall v10/v11/v12):**
  - Árbol oficial del Kernel Linux: [`drivers/gpu/drm/panthor/`](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/drivers/gpu/drm/panthor)
  - Ficheros clave:
    - Carga y parsing de firmware CSF: [`panthor_fw.c:panthor_fw_load`](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/drivers/gpu/drm/panthor/panthor_fw.c) (definición de cabecera `0xc3f13a6e` e iterador de secciones)
    - Control hardware y reset: [`panthor_gpu.c:panthor_gpu_soft_reset`](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/drivers/gpu/drm/panthor/panthor_gpu.c) y `panthor_gpu_l2_power_on`
    - Paginación MMU AS0: [`panthor_mmu.c:panthor_mmu_as_init`](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/drivers/gpu/drm/panthor/panthor_mmu.c)
    - Definición de registros MMIO: [`panthor_regs.h`](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/drivers/gpu/drm/panthor/panthor_regs.h) (`GPU_ID`, `MCU_CONTROL`, `GPU_CMD`, `DOORBELL`)
- **Pila Gráfica Mesa 3D (Panfrost / PanVK):**
  - Repositorio oficial: [`https://gitlab.freedesktop.org/mesa/mesa`](https://gitlab.freedesktop.org/mesa/mesa)
  - Ficheros clave:
    - Identificación de productos: `src/panfrost/model/pan_model.c` (`PAN_PROD_ID(11, 2)` para Mali-G715)
    - Descriptor XML Valhall v10/v11: `src/panfrost/lib/genxml/v10.xml` (formato de instrucciones y paquetes CS)
    - Generador de command stream: `src/panfrost/cs/cs_builder.h`
- **Precedentes en hardware real:**
  - Proyecto PixOS (Pixel 6a / Tensor GS101 Mali-G78): [`github.com/PixOS-Project/pixelios-kernel`](https://github.com/PixOS-Project/pixelios-kernel), documento `docs/research/27-gpu-g0-result.md` (observación empírica del estado de entrega de GPU por ABL).
  - Port de Metal viva-gpu: [`github.com/Nik300/viva-gpu`](https://github.com/Nik300/viva-gpu) (traducción de AIR a NIR sobre backend Mali Valhall).