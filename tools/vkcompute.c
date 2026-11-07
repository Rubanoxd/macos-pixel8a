// vkcompute: one compute dispatch through PanVK -> darwin-drm -> MaliService, checked by the CPU.
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>
#include "fill.spv.h"

#define N 1024
#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { printf("%s failed: %d\n", #x, r_); return 1; } } while (0)

static PFN_vkGetInstanceProcAddr gipa;
static VkInstance inst;
#define F(name) PFN_##name name = (PFN_##name)gipa(inst, #name)

int main(int argc, char **argv)
{
   setvbuf(stdout, NULL, _IONBF, 0);
   setenv("PAN_I_WANT_A_BROKEN_VULKAN_DRIVER", "1", 0);
   void *lib = dlopen(argc > 1 ? argv[1] : "./libvulkan_panfrost.dylib", RTLD_NOW | RTLD_LOCAL);
   if (!lib) { printf("dlopen: %s\n", dlerror()); return 1; }
   gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vk_icdGetInstanceProcAddr");
   PFN_vkCreateInstance createInstance = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
   VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "vkcompute", 1, NULL, 0, VK_API_VERSION_1_3};
   VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, 0, NULL};
   CHECK(createInstance(&ici, NULL, &inst));
   F(vkEnumeratePhysicalDevices); F(vkGetPhysicalDeviceMemoryProperties); F(vkGetPhysicalDeviceQueueFamilyProperties);
   F(vkCreateDevice); F(vkGetDeviceProcAddr);
   uint32_t n = 1;
   VkPhysicalDevice pd;
   CHECK(vkEnumeratePhysicalDevices(inst, &n, &pd));
   uint32_t qn = 0;
   vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, NULL);
   VkQueueFamilyProperties qf[8];
   if (qn > 8) qn = 8;
   vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, qf);
   uint32_t family = 0;
   while (family < qn && !(qf[family].queueFlags & VK_QUEUE_COMPUTE_BIT)) family++;
   float prio = 1.0f;
   VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, family, 1, &prio};
   VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, NULL, 0, 1, &qci, 0, NULL, 0, NULL, NULL};
   VkDevice dev;
   CHECK(vkCreateDevice(pd, &dci, NULL, &dev));
   printf("device created (queue family %u of %u)\n", family, qn);
#define D(name) PFN_##name name = (PFN_##name)vkGetDeviceProcAddr(dev, #name)
   D(vkGetDeviceQueue); D(vkCreateBuffer); D(vkGetBufferMemoryRequirements); D(vkAllocateMemory); D(vkBindBufferMemory);
   D(vkMapMemory); D(vkCreateShaderModule); D(vkCreateDescriptorSetLayout); D(vkCreatePipelineLayout);
   D(vkCreateComputePipelines); D(vkCreateDescriptorPool); D(vkAllocateDescriptorSets); D(vkUpdateDescriptorSets);
   D(vkCreateCommandPool); D(vkAllocateCommandBuffers); D(vkBeginCommandBuffer); D(vkCmdBindPipeline);
   D(vkCmdBindDescriptorSets); D(vkCmdDispatch); D(vkEndCommandBuffer); D(vkCreateFence); D(vkQueueSubmit);
   D(vkWaitForFences); D(vkInvalidateMappedMemoryRanges);
   VkQueue q;
   vkGetDeviceQueue(dev, family, 0, &q);

   VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, N * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
   VkBuffer buf;
   CHECK(vkCreateBuffer(dev, &bci, NULL, &buf));
   VkMemoryRequirements mr;
   vkGetBufferMemoryRequirements(dev, buf, &mr);
   VkPhysicalDeviceMemoryProperties mp;
   vkGetPhysicalDeviceMemoryProperties(pd, &mp);
   uint32_t type = 0;
   for (; type < mp.memoryTypeCount; type++)
      if ((mr.memoryTypeBits & (1u << type)) && (mp.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
         break;
   VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size, type};
   VkDeviceMemory mem;
   CHECK(vkAllocateMemory(dev, &mai, NULL, &mem));
   CHECK(vkBindBufferMemory(dev, buf, mem, 0));
   uint32_t *p;
   CHECK(vkMapMemory(dev, mem, 0, VK_WHOLE_SIZE, 0, (void **)&p));
   memset(p, 0xee, N * 4);
   printf("buffer: %llu bytes, memory type %u (flags %#x)\n", (unsigned long long)mr.size, type, mp.memoryTypes[type].propertyFlags);

   VkShaderModuleCreateInfo smci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof(fill_spv), fill_spv};
   VkShaderModule sm;
   CHECK(vkCreateShaderModule(dev, &smci, NULL, &sm));
   VkDescriptorSetLayoutBinding b = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL};
   VkDescriptorSetLayoutCreateInfo dslci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 1, &b};
   VkDescriptorSetLayout dsl;
   CHECK(vkCreateDescriptorSetLayout(dev, &dslci, NULL, &dsl));
   VkPipelineLayoutCreateInfo plci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &dsl, 0, NULL};
   VkPipelineLayout pl;
   CHECK(vkCreatePipelineLayout(dev, &plci, NULL, &pl));
   VkComputePipelineCreateInfo cpci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, NULL, 0,
      {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_COMPUTE_BIT, sm, "main", NULL}, pl, VK_NULL_HANDLE, 0};
   VkPipeline pipe;
   CHECK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpci, NULL, &pipe));
   printf("pipeline compiled\n");
   VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
   VkDescriptorPoolCreateInfo dpci = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, 1, 1, &ps};
   VkDescriptorPool dp;
   CHECK(vkCreateDescriptorPool(dev, &dpci, NULL, &dp));
   VkDescriptorSetAllocateInfo dsai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, dp, 1, &dsl};
   VkDescriptorSet ds;
   CHECK(vkAllocateDescriptorSets(dev, &dsai, &ds));
   VkDescriptorBufferInfo dbi = {buf, 0, VK_WHOLE_SIZE};
   VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, ds, 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &dbi, NULL};
   vkUpdateDescriptorSets(dev, 1, &w, 0, NULL);

   VkCommandPoolCreateInfo cpi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, 0, family};
   VkCommandPool cp;
   CHECK(vkCreateCommandPool(dev, &cpi, NULL, &cp));
   VkCommandBufferAllocateInfo cbai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, cp, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
   VkCommandBuffer cb;
   CHECK(vkAllocateCommandBuffers(dev, &cbai, &cb));
   VkCommandBufferBeginInfo cbbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL};
   CHECK(vkBeginCommandBuffer(cb, &cbbi));
   vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
   vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0, NULL);
   vkCmdDispatch(cb, N / 64, 1, 1);
   CHECK(vkEndCommandBuffer(cb));
   VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, NULL, 0};
   VkFence fence;
   CHECK(vkCreateFence(dev, &fci, NULL, &fence));
   VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cb, 0, NULL};
   CHECK(vkQueueSubmit(q, 1, &si, fence));
   printf("submitted, waiting\n");
   CHECK(vkWaitForFences(dev, 1, &fence, VK_TRUE, 5000000000ull));
   VkMappedMemoryRange range = {VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, NULL, mem, 0, VK_WHOLE_SIZE};
   vkInvalidateMappedMemoryRanges(dev, 1, &range);
   unsigned bad = 0;
   for (unsigned i = 0; i < N; i++)
      if (p[i] != i * 3 + 7) bad++;
   printf("result: %u of %u wrong; v[0]=%u v[1]=%u v[1023]=%u (want 7 10 3076)\n", bad, N, p[0], p[1], p[1023]);
   printf(bad ? "COMPUTE FAILED\n" : "COMPUTE PASSED: a Vulkan compute shader ran on the Mali-G715 under macOS\n");
   return bad != 0;
}
