// Checks that the Khronos validation layer loads: creates an instance with VK_LAYER_KHRONOS_validation and a
// debug messenger and a device. With any argument it also makes one deliberate error (a zero-size buffer),
// which must be reported. Build: clang tools/vvl-check.c -o /tmp/vvl-check -lvulkan; run /tmp/vvl-check [bad].
// The implicit gamescope layer injects extensions without their dependencies, so expect two harmless
// "Missing extension required by ..." reports from it on the Frame.
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <string.h>
static int errors;
static VKAPI_ATTR VkBool32 VKAPI_CALL cb(VkDebugUtilsMessageSeverityFlagBitsEXT s, VkDebugUtilsMessageTypeFlagsEXT t, const VkDebugUtilsMessengerCallbackDataEXT* d, void* u) {
  if (s & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) errors++;
  printf("[%s] %.160s\n", (s & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) ? "ERROR" : "info", d->pMessage); return VK_FALSE; }
int main(int argc, char** argv) {
  const char* layer = "VK_LAYER_KHRONOS_validation"; const char* ext = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
  VkApplicationInfo ai = {VK_STRUCTURE_TYPE_APPLICATION_INFO, 0, "t", 1, "t", 1, VK_API_VERSION_1_1};
  VkDebugUtilsMessengerCreateInfoEXT mi = {VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
  mi.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
  mi.messageType = 7; mi.pfnUserCallback = cb;
  VkInstanceCreateInfo ci = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, &mi, 0, &ai, 1, &layer, 1, &ext};
  VkInstance inst; VkResult r = vkCreateInstance(&ci, 0, &inst);
  printf("vkCreateInstance with %s: %d\n", layer, r); if (r) return 1;
  uint32_t n = 1; VkPhysicalDevice pd; vkEnumeratePhysicalDevices(inst, &n, &pd);
  VkPhysicalDeviceProperties p; vkGetPhysicalDeviceProperties(pd, &p); printf("device: %s\n", p.deviceName);
  float pr = 1; VkDeviceQueueCreateInfo q = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, 0, 0, 0, 1, &pr};
  VkDeviceCreateInfo di = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, 0, 0, 1, &q};
  VkDevice dev; r = vkCreateDevice(pd, &di, 0, &dev); printf("vkCreateDevice: %d\n", r);
  if (argc > 1) { VkBufferCreateInfo b = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, 0, 0, 0 /*size 0: invalid*/, VK_BUFFER_USAGE_TRANSFER_SRC_BIT}; VkBuffer buf; vkCreateBuffer(dev, &b, 0, &buf); }
  vkDestroyDevice(dev, 0); vkDestroyInstance(inst, 0);
  printf("validation errors: %d\n", errors); return 0; }
