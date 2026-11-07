// vkinfo: load PanVK as an ICD (no loader) and list the physical devices it finds through darwin-drm -> MaliService.
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <vulkan/vulkan.h>
#include <signal.h>
#include <sys/ucontext.h>
#include <mach/arm/thread_status.h>
#include <unistd.h>

static void where(const char *what, uint64_t a)
{
   Dl_info info;
   a &= 0x0000007fffffffffULL;
   if (dladdr((void *)a, &info) && info.dli_fname)
      printf("  %s %#llx = %s + %#llx (%s)\n", what, (unsigned long long)a, info.dli_fname,
             (unsigned long long)(a - (uint64_t)info.dli_fbase), info.dli_sname ? info.dli_sname : "?");
   else
      printf("  %s %#llx\n", what, (unsigned long long)a);
}

static void crash(int sig, siginfo_t *si, void *ctx)
{
   ucontext_t *uc = ctx;
   printf("signal %d at %p\n", sig, si->si_addr);
   where("pc", (uint64_t)__darwin_arm_thread_state64_get_pc(uc->uc_mcontext->__ss));
   where("lr", (uint64_t)__darwin_arm_thread_state64_get_lr(uc->uc_mcontext->__ss));
   uint64_t fp = (uint64_t)__darwin_arm_thread_state64_get_fp(uc->uc_mcontext->__ss);
   for (int i = 0; i < 12 && fp; i++) {
      uint64_t *frame = (uint64_t *)(fp & 0x0000007fffffffffULL);
      where("ret", frame[1]);
      fp = frame[0];
   }
   _exit(128 + sig);
}

typedef PFN_vkVoidFunction (*GetProc)(VkInstance, const char *);
typedef VkResult (*Negotiate)(uint32_t *);

int main(int argc, char **argv)
{
   setvbuf(stdout, NULL, _IONBF, 0);
   struct sigaction sa = {0};
   sa.sa_sigaction = crash;
   sa.sa_flags = SA_SIGINFO;
   sigaction(SIGILL, &sa, NULL);
   sigaction(SIGSEGV, &sa, NULL);
   sigaction(SIGBUS, &sa, NULL);
   sigaction(SIGTRAP, &sa, NULL);
   sigaction(SIGABRT, &sa, NULL);
   const char *path = argc > 1 ? argv[1] : "./libvulkan_panfrost.dylib";
   setenv("PAN_I_WANT_A_BROKEN_VULKAN_DRIVER", "1", 0);
   void *lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
   if (!lib) {
      printf("dlopen: %s\n", dlerror());
      return 1;
   }
   Negotiate negotiate = (Negotiate)dlsym(lib, "vk_icdNegotiateLoaderICDInterfaceVersion");
   GetProc gipa = (GetProc)dlsym(lib, "vk_icdGetInstanceProcAddr");
   uint32_t version = 5;
   if (negotiate)
      printf("ICD interface: %d (version %u)\n", negotiate(&version), version);
   if (!gipa) {
      printf("no vk_icdGetInstanceProcAddr\n");
      return 1;
   }
   PFN_vkCreateInstance createInstance = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
   VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "vkinfo", 1, NULL, 0, VK_API_VERSION_1_3};
   VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, 0, NULL};
   VkInstance instance;
   VkResult r = createInstance(&ici, NULL, &instance);
   printf("vkCreateInstance: %d\n", r);
   if (r != VK_SUCCESS)
      return 1;
   PFN_vkEnumeratePhysicalDevices enumerate = (PFN_vkEnumeratePhysicalDevices)gipa(instance, "vkEnumeratePhysicalDevices");
   PFN_vkGetPhysicalDeviceProperties props = (PFN_vkGetPhysicalDeviceProperties)gipa(instance, "vkGetPhysicalDeviceProperties");
   uint32_t count = 0;
   r = enumerate(instance, &count, NULL);
   printf("vkEnumeratePhysicalDevices: %d, %u device(s)\n", r, count);
   VkPhysicalDevice devices[4];
   if (count > 4)
      count = 4;
   enumerate(instance, &count, devices);
   for (uint32_t i = 0; i < count; i++) {
      VkPhysicalDeviceProperties p;
      props(devices[i], &p);
      printf("device %u: %s, Vulkan %u.%u.%u, driver %#x, vendor %#x device %#x, type %d\n", i, p.deviceName,
             VK_API_VERSION_MAJOR(p.apiVersion), VK_API_VERSION_MINOR(p.apiVersion), VK_API_VERSION_PATCH(p.apiVersion),
             p.driverVersion, p.vendorID, p.deviceID, p.deviceType);
   }
   return 0;
}
