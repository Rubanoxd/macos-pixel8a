#!/bin/sh
# Runs ON THE PHONE. PMU through the secure monitor; GPU registers only if pd_g3d reports on.
pmu() { sysctl -w kern.akita_phys_addr=$1 >/dev/null && v=$(sysctl -n kern.akita_pmu_val) && printf '%s = 0x%08x\n' "$2" "$v"; }
pmu 0x15461A80 "pd_g3d CONFIG"
pmu 0x15461A84 "pd_g3d STATUS"
pmu 0x15461C80 "pd_embedded_g3d CONFIG"
pmu 0x15461C84 "pd_embedded_g3d STATUS"
sysctl -w kern.akita_phys_addr=0x15461A84 >/dev/null; s=$(sysctl -n kern.akita_pmu_val)
if [ $((s & 1)) -eq 1 ]; then
  for r in 0x1F000000:GPU_ID 0x1F000004:L2_FEATURES 0x1F000014:MMU_FEATURES 0x1F000100:SHADER_PRESENT 0x1F000020:GPU_IRQ_RAWSTAT; do
    sysctl -w kern.akita_phys_addr=${r%%:*} >/dev/null; printf '%s = 0x%08x\n' "${r#*:}" "$(sysctl -n kern.akita_phys_val)"
  done
else
  echo "G3D domain off: GPU registers not read"
fi
