// Mock OpenXR runtime, Vulkan side (xrmock_gfx.h): XR_KHR_vulkan_enable2.
// The instance and device are made with the app's own vkGetInstanceProcAddr
// (no libvulkan link); swapchain images are VkImages on the app's device.
// At each release the mock checks that the GPU had finished the app's work
// (SteamVR on the Frame shows a released image at once, finished or not):
// it waits for the app's queue to go idle and reports a release that had to
// wait (the app released before its GPU work was done).
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#define XR_USE_GRAPHICS_API_VULKAN
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include "xrmock_gfx.h"

static PFN_vkGetInstanceProcAddr gGipa;
static VkInstance gInstance;
static VkPhysicalDevice gPhys;
static VkDevice gDevice;
static VkQueue gQueue;
static uint32_t gQueueFamily;
static PFN_vkGetDeviceProcAddr gGdpa;

// Memory of every swapchain image, mapped when it is host visible (the exact release check).
#define MAX_IMAGES 256
static struct {
    VkImage image;
    VkDeviceMemory memory;
    unsigned char* map;
    VkDeviceSize size;
} gImages[MAX_IMAGES];
static int gImageCount = 0;

#define DEV(name) ((PFN_##name)gGdpa(gDevice, #name))
#define INST(name) ((PFN_##name)gGipa(gInstance, #name))

static int ownsBinding(XrStructureType t) { return t == XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR; }

static int describeBinding(const XrBaseInStructure* n, char* out, size_t cap) {
    const XrGraphicsBindingVulkan2KHR* b = (const XrGraphicsBindingVulkan2KHR*)n;
    int ok = b->instance == gInstance && b->physicalDevice == gPhys && b->device == gDevice;
    if (gDevice && b->device == gDevice) {
        gGdpa = (PFN_vkGetDeviceProcAddr)gGipa(gInstance, "vkGetDeviceProcAddr");
        DEV(vkGetDeviceQueue)(gDevice, b->queueFamilyIndex, b->queueIndex, &gQueue);
        gQueueFamily = b->queueFamilyIndex;
    }
    return snprintf(out, cap, "binding=VULKAN2 handles=%s queueFamily=%u queueIndex=%u", ok ? "made-by-runtime" : "FOREIGN", b->queueFamilyIndex,
                    b->queueIndex);
}

static XrResult XRAPI_CALL vkRequirements(XrInstance i, XrSystemId s, XrGraphicsRequirementsVulkanKHR* r) {
    (void)i;
    (void)s;
    // As SteamVR: instances of Vulkan 1.0 to 1.2.
    r->minApiVersionSupported = XR_MAKE_VERSION(1, 0, 0);
    r->maxApiVersionSupported = XR_MAKE_VERSION(1, 2, 0);
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL vkCreateInstanceXr(XrInstance i, const XrVulkanInstanceCreateInfoKHR* ci, VkInstance* out, VkResult* vr) {
    (void)i;
    gGipa = ci->pfnGetInstanceProcAddr;
    PFN_vkCreateInstance create = (PFN_vkCreateInstance)gGipa(VK_NULL_HANDLE, "vkCreateInstance");
    const VkApplicationInfo* app = ci->vulkanCreateInfo->pApplicationInfo;
    if (app && app->apiVersion > VK_API_VERSION_1_2) {
        fprintf(stderr, "xrmock: ERROR instance apiVersion above 1.2\n");
        *vr = VK_ERROR_INCOMPATIBLE_DRIVER;
        return XR_SUCCESS;
    }
    *vr = create(ci->vulkanCreateInfo, ci->vulkanAllocator, out);
    gInstance = *out;
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL vkGetDevice(XrInstance i, const XrVulkanGraphicsDeviceGetInfoKHR* gi, VkPhysicalDevice* out) {
    (void)i;
    PFN_vkEnumeratePhysicalDevices en = (PFN_vkEnumeratePhysicalDevices)gGipa(gi->vulkanInstance, "vkEnumeratePhysicalDevices");
    uint32_t n = 1;
    VkPhysicalDevice d = VK_NULL_HANDLE;
    VkResult r = en(gi->vulkanInstance, &n, &d);
    if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || !d) return XR_ERROR_RUNTIME_FAILURE;
    gPhys = *out = d;
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL vkCreateDeviceXr(XrInstance i, const XrVulkanDeviceCreateInfoKHR* ci, VkDevice* out, VkResult* vr) {
    (void)i;
    PFN_vkCreateDevice create = (PFN_vkCreateDevice)ci->pfnGetInstanceProcAddr(gInstance, "vkCreateDevice");
    *vr = create(ci->vulkanPhysicalDevice, ci->vulkanCreateInfo, ci->vulkanAllocator, out);
    gDevice = *out;
    return XR_SUCCESS;
}

static PFN_xrVoidFunction getProc(const char* name) {
    if (!strcmp(name, "xrGetVulkanGraphicsRequirements2KHR")) return (PFN_xrVoidFunction)vkRequirements;
    if (!strcmp(name, "xrCreateVulkanInstanceKHR")) return (PFN_xrVoidFunction)vkCreateInstanceXr;
    if (!strcmp(name, "xrGetVulkanGraphicsDevice2KHR")) return (PFN_xrVoidFunction)vkGetDevice;
    if (!strcmp(name, "xrCreateVulkanDeviceKHR")) return (PFN_xrVoidFunction)vkCreateDeviceXr;
    return NULL;
}

// As SteamVR offers them on the Frame.
static const int64_t kFormats[] = {VK_FORMAT_R8G8B8A8_SRGB,   VK_FORMAT_B8G8R8A8_SRGB,    VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_A2B10G10R10_UNORM_PACK32,
                                   VK_FORMAT_D32_SFLOAT,      VK_FORMAT_D16_UNORM,        VK_FORMAT_D24_UNORM_S8_UINT,   VK_FORMAT_D32_SFLOAT_S8_UINT};

static int formats(const int64_t** list) {
    *list = kFormats;
    return (int)(sizeof(kFormats) / sizeof(kFormats[0]));
}

static int64_t formatOf(MockFormatKind kind) {
    switch (kind) {
    case MOCK_FMT_EYE:
    case MOCK_FMT_UI: return VK_FORMAT_R8G8B8A8_SRGB;
    case MOCK_FMT_MOTION: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case MOCK_FMT_DEPTH: return VK_FORMAT_D24_UNORM_S8_UINT;
    }
    return VK_FORMAT_R8G8B8A8_SRGB;
}

static void* createImages(int64_t format, uint64_t usage, int width, int height, int mips, int count) {
    if (!gDevice) return NULL;
    if (!gGdpa) gGdpa = (PFN_vkGetDeviceProcAddr)gGipa(gInstance, "vkGetDeviceProcAddr");
    VkImage* images = (VkImage*)calloc((size_t)count, sizeof(VkImage));
    int depth = format == VK_FORMAT_D24_UNORM_S8_UINT || format == VK_FORMAT_D32_SFLOAT || format == VK_FORMAT_D16_UNORM || format == VK_FORMAT_D32_SFLOAT_S8_UINT;
    VkImageUsageFlags u = 0;
    if (usage & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT) u |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (usage & XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) u |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if (usage & XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT) u |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    // MOCK_XR_DUMP and MOCK_XR_STAMP copy released images out.
    if (!depth && (getenv("MOCK_XR_DUMP") || getenv("MOCK_XR_STAMP"))) u |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (usage & XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT) u |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (usage & XR_SWAPCHAIN_USAGE_SAMPLED_BIT) u |= VK_IMAGE_USAGE_SAMPLED_BIT;
    // The compositor samples every image.
    u |= VK_IMAGE_USAGE_SAMPLED_BIT;
    VkPhysicalDeviceMemoryProperties mp;
    INST(vkGetPhysicalDeviceMemoryProperties)(gPhys, &mp);
    for (int i = 0; i < count; i++) {
        VkImageCreateInfo ci;
        memset(&ci, 0, sizeof(ci));
        ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format = (VkFormat)format;
        ci.extent.width = (uint32_t)width;
        ci.extent.height = (uint32_t)height;
        ci.extent.depth = 1;
        ci.mipLevels = (uint32_t)mips;
        ci.arrayLayers = 1;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = u;
        // As the spec asks of a runtime (and what lets the app write an sRGB image through a UNORM view).
        // No format list: the driver may then lay the image out uncompressed (Turnip: no UBWC), which a
        // runtime's images may be too. MOCK_XR_MUTABLE=0 leaves the flag out (the old mock).
        if ((usage & XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT) && !(getenv("MOCK_XR_MUTABLE") && getenv("MOCK_XR_MUTABLE")[0] == '0'))
            ci.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
        ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (DEV(vkCreateImage)(gDevice, &ci, NULL, &images[i]) != VK_SUCCESS) {
            fprintf(stderr, "xrmock: vkCreateImage failed\n");
            continue;
        }
        VkMemoryRequirements req;
        DEV(vkGetImageMemoryRequirements)(gDevice, images[i], &req);
        // Host visible where the device allows it (unified memory on the Frame): the exact
        // release check reads the image's bytes.
        uint32_t type = UINT32_MAX;
        const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        for (uint32_t t = 0; t < mp.memoryTypeCount && getenv("MOCK_XR_BUSY_MS"); t++) {
            if ((req.memoryTypeBits & (1u << t)) && (mp.memoryTypes[t].propertyFlags & want) == want) {
                type = t;
                break;
            }
        }
        for (uint32_t t = 0; t < mp.memoryTypeCount && type == UINT32_MAX; t++) {
            if (req.memoryTypeBits & (1u << t)) type = t;
        }
        VkMemoryAllocateInfo ai;
        memset(&ai, 0, sizeof(ai));
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = type;
        VkDeviceMemory mem;
        if (DEV(vkAllocateMemory)(gDevice, &ai, NULL, &mem) == VK_SUCCESS) {
            DEV(vkBindImageMemory)(gDevice, images[i], mem, 0);
            if (gImageCount < MAX_IMAGES) {
                gImages[gImageCount].image = images[i];
                gImages[gImageCount].memory = mem;
                gImages[gImageCount].size = req.size;
                void* map = NULL;
                if ((mp.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) &&
                    DEV(vkMapMemory)(gDevice, mem, 0, VK_WHOLE_SIZE, 0, &map) == VK_SUCCESS)
                    gImages[gImageCount].map = (unsigned char*)map;
                gImageCount++;
            }
        }
    }
    // As a runtime hands them out: color images in COLOR_ATTACHMENT_OPTIMAL,
    // depth in DEPTH_STENCIL_ATTACHMENT_OPTIMAL (XR_KHR_vulkan_enable2).
    if (gQueue) {
        VkCommandPoolCreateInfo pci;
        memset(&pci, 0, sizeof(pci));
        pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pci.queueFamilyIndex = gQueueFamily;
        VkCommandPool pool;
        DEV(vkCreateCommandPool)(gDevice, &pci, NULL, &pool);
        VkCommandBufferAllocateInfo ai;
        memset(&ai, 0, sizeof(ai));
        ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        ai.commandPool = pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        VkCommandBuffer cb;
        DEV(vkAllocateCommandBuffers)(gDevice, &ai, &cb);
        VkCommandBufferBeginInfo bi;
        memset(&bi, 0, sizeof(bi));
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        DEV(vkBeginCommandBuffer)(cb, &bi);
        for (int i = 0; i < count; i++) {
            VkImageMemoryBarrier b;
            memset(&b, 0, sizeof(b));
            b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            b.dstAccessMask = depth ? VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT : VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            b.newLayout = depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = images[i];
            b.subresourceRange.aspectMask = depth ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
            b.subresourceRange.levelCount = (uint32_t)mips;
            b.subresourceRange.layerCount = 1;
            DEV(vkCmdPipelineBarrier)(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
        }
        DEV(vkEndCommandBuffer)(cb);
        VkSubmitInfo si;
        memset(&si, 0, sizeof(si));
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cb;
        DEV(vkQueueSubmit)(gQueue, 1, &si, VK_NULL_HANDLE);
        DEV(vkQueueWaitIdle)(gQueue);
        DEV(vkDestroyCommandPool)(gDevice, pool, NULL);
    }
    return images;
}

static XrResult fillImages(void* images, int count, XrSwapchainImageBaseHeader* out) {
    const VkImage* im = (const VkImage*)images;
    XrSwapchainImageVulkan2KHR* v = (XrSwapchainImageVulkan2KHR*)out;
    for (int i = 0; i < count; i++) {
        if (v[i].type != XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR) return XR_ERROR_VALIDATION_FAILURE;
        v[i].image = im[i];
    }
    return XR_SUCCESS;
}

// How long the app's queue took to go idle at a release (ms).
static double releaseWaitMs(void) {
    if (!gQueue) return 0.0;
    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    DEV(vkQueueWaitIdle)(gQueue);
    clock_gettime(CLOCK_MONOTONIC, &b);
    return (b.tv_sec - a.tv_sec) * 1e3 + (b.tv_nsec - a.tv_nsec) / 1e6;
}

// The exact check: SAMPLES chunks of the image's memory at its release, compared again once
// an empty submission made at the release (so after all work queued before it) completed.
// Lane U: a waiter thread waits those submissions in order and times them: the time from the release
// to the completion of a submission made inside xrReleaseSwapchainImage is how long SteamVR's own
// client submission there waits behind the app's queued GPU work (the release-queue line). And the
// strict check (no queue involved): the image's bytes at its release against its bytes at its next
// acquire; any difference is a write after the release, however it was ordered on the queue.
#define SAMPLES 512
#define CHUNK 64
#define PENDING 64
#define LAT_CHAINS 64
static struct {
    int image, chain, frame;
    int64_t t0;
    VkFence fence;
    unsigned char bytes[SAMPLES * CHUNK];
} gPending[PENDING];
static int gHead = 0, gCount = 0, gChecked = 0, gChanged = 0, gChangedTaken = 0, gThreadUp = 0;
static VkFence gFreeFences[PENDING];
static int gFreeFenceCount = 0;
static pthread_mutex_t gPendMutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gPendCond = PTHREAD_COND_INITIALIZER;
static pthread_t gWaiter;
// Release-queue latencies (ms) per swapchain and frame parity.
static struct { float* ms; int n, cap; } gLat[LAT_CHAINS][2];
// Strict: per image, its sample at the last release (valid until the next acquire).
static unsigned char* gRelBytes[MAX_IMAGES];
static int gRelValid[MAX_IMAGES];
static int gStrictChecked = 0, gStrictChanged = 0;

static int64_t vkNowNs(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000000000ll + t.tv_nsec;
}

static void sample(int image, unsigned char* out) {
    VkDeviceSize step = gImages[image].size / SAMPLES;
    for (int k = 0; k < SAMPLES; k++) memcpy(out + k * CHUNK, gImages[image].map + (VkDeviceSize)k * step, CHUNK);
}

static void* waiterMain(void* arg) {
    (void)arg;
    static unsigned char now[SAMPLES * CHUNK];
    for (;;) {
        pthread_mutex_lock(&gPendMutex);
        while (gCount == 0) pthread_cond_wait(&gPendCond, &gPendMutex);
        int i = gHead;
        VkFence fence = gPending[i].fence;
        pthread_mutex_unlock(&gPendMutex);
        // Ring slot i stays ours until it is popped below.
        while (DEV(vkWaitForFences)(gDevice, 1, &fence, VK_TRUE, 100000000ull) == VK_TIMEOUT) {
        }
        int64_t t1 = vkNowNs();
        sample(gPending[i].image, now);
        int changed = memcmp(now, gPending[i].bytes, sizeof(now)) != 0;
        DEV(vkResetFences)(gDevice, 1, &fence);
        pthread_mutex_lock(&gPendMutex);
        int c = gPending[i].chain, par = gPending[i].frame & 1;
        if (c >= 0 && c < LAT_CHAINS) {
            if (gLat[c][par].n == gLat[c][par].cap) {
                gLat[c][par].cap = gLat[c][par].cap ? 2 * gLat[c][par].cap : 4096;
                gLat[c][par].ms = (float*)realloc(gLat[c][par].ms, sizeof(float) * (size_t)gLat[c][par].cap);
            }
            gLat[c][par].ms[gLat[c][par].n++] = (float)((t1 - gPending[i].t0) / 1e6);
        }
        gChanged += changed;
        gChecked++;
        gFreeFences[gFreeFenceCount++] = fence;
        gHead = (gHead + 1) % PENDING;
        gCount--;
        pthread_cond_broadcast(&gPendCond);
        pthread_mutex_unlock(&gPendMutex);
    }
    return NULL;
}

// Returns how many released images changed after their release since the last call (wait != 0: once
// every queued check finished).
static int pollSnapshots(int wait) {
    pthread_mutex_lock(&gPendMutex);
    if (wait) {
        struct timespec until;
        clock_gettime(CLOCK_REALTIME, &until);
        until.tv_sec += 3;
        while (gCount > 0 && pthread_cond_timedwait(&gPendCond, &gPendMutex, &until) == 0) {
        }
    }
    int changed = gChanged - gChangedTaken;
    gChangedTaken = gChanged;
    pthread_mutex_unlock(&gPendMutex);
    return changed;
}

static int findMapped(VkImage image) {
    for (int k = 0; k < gImageCount; k++)
        if (gImages[k].image == image && gImages[k].map && gImages[k].size >= SAMPLES * CHUNK) return k;
    return -1;
}

// At a release, inside xrReleaseSwapchainImage: the app holds its queue lock (SteamVR's client
// submits there too). chain, frame: the swapchain and the xrWaitFrame count, for the latency lines.
static void releaseSnapshot(void* images, int index, int chain, int frame) {
    if (!gQueue) return;
    int found = findMapped(((VkImage*)images)[index]);
    if (found < 0) return;
    int64_t t0 = vkNowNs();
    pthread_mutex_lock(&gPendMutex);
    if (!gThreadUp) gThreadUp = pthread_create(&gWaiter, NULL, waiterMain, NULL) == 0 ? 1 : -1;
    if (gThreadUp < 0 || gCount == PENDING) {
        pthread_mutex_unlock(&gPendMutex);
        return;
    }
    VkFence fence = gFreeFenceCount ? gFreeFences[--gFreeFenceCount] : VK_NULL_HANDLE;
    int slot = (gHead + gCount) % PENDING;  // only this thread adds; the waiter only pops the head
    pthread_mutex_unlock(&gPendMutex);
    if (fence == VK_NULL_HANDLE) {
        VkFenceCreateInfo fi;
        memset(&fi, 0, sizeof(fi));
        fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        if (DEV(vkCreateFence)(gDevice, &fi, NULL, &fence) != VK_SUCCESS) return;
    }
    sample(found, gPending[slot].bytes);
    if (!gRelBytes[found]) gRelBytes[found] = (unsigned char*)malloc(SAMPLES * CHUNK);
    if (gRelBytes[found]) {
        memcpy(gRelBytes[found], gPending[slot].bytes, SAMPLES * CHUNK);
        gRelValid[found] = 1;
    }
    VkSubmitInfo si;
    memset(&si, 0, sizeof(si));
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    if (DEV(vkQueueSubmit)(gQueue, 1, &si, fence) != VK_SUCCESS) {
        pthread_mutex_lock(&gPendMutex);
        gFreeFences[gFreeFenceCount++] = fence;
        pthread_mutex_unlock(&gPendMutex);
        return;
    }
    pthread_mutex_lock(&gPendMutex);
    gPending[slot].image = found;
    gPending[slot].chain = chain;
    gPending[slot].frame = frame;
    gPending[slot].t0 = t0;
    gPending[slot].fence = fence;
    gCount++;
    pthread_cond_broadcast(&gPendCond);
    pthread_mutex_unlock(&gPendMutex);
}

// Strict check at xrAcquireSwapchainImage: 1 when the image's bytes differ from those at its last
// release (written after it was released), 0 same or not checked.
static int acquireCheck(void* images, int index) {
    int found = findMapped(((VkImage*)images)[index]);
    if (found < 0 || !gRelValid[found]) return 0;
    static unsigned char now[SAMPLES * CHUNK];
    sample(found, now);
    gRelValid[found] = 0;
    gStrictChecked++;
    int changed = memcmp(now, gRelBytes[found], SAMPLES * CHUNK) != 0;
    gStrictChanged += changed;
    return changed;
}

static int cmpFloat(const void* a, const void* b) {
    float x = *(const float*)a, y = *(const float*)b;
    return x < y ? -1 : x > y;
}
static float pct(float* v, int n, double p) { return n ? v[(int)(p * (n - 1) + 0.5)] : 0.f; }

// Summary lines (stderr): the strict check and, per swapchain with releases, the release-queue latency.
static void releaseReport(void) {
    fprintf(stderr, "xrmock: strict release check: %d images compared at their next acquire, %d changed after release\n",
            gStrictChecked, gStrictChanged);
    pthread_mutex_lock(&gPendMutex);
    for (int c = 0; c < LAT_CHAINS; c++) {
        int n = gLat[c][0].n + gLat[c][1].n;
        if (n < 10) continue;
        char part[2][96];
        float* all = (float*)malloc(sizeof(float) * (size_t)n);
        int over1 = 0, over2 = 0, over4 = 0, k = 0;
        for (int par = 0; par < 2; par++) {
            qsort(gLat[c][par].ms, (size_t)gLat[c][par].n, sizeof(float), cmpFloat);
            snprintf(part[par], sizeof(part[par]), "p50 %.2f p90 %.2f", pct(gLat[c][par].ms, gLat[c][par].n, 0.5),
                     pct(gLat[c][par].ms, gLat[c][par].n, 0.9));
            for (int j = 0; j < gLat[c][par].n; j++) {
                float v = gLat[c][par].ms[j];
                all[k++] = v;
                over1 += v > 1.f;
                over2 += v > 2.f;
                over4 += v > 4.f;
            }
        }
        qsort(all, (size_t)n, sizeof(float), cmpFloat);
        fprintf(stderr,
                "xrmock: release queue sc%d: %d releases, ms p50 %.2f p90 %.2f p99 %.2f max %.2f; >1 ms %.1f %%, >2 ms %.1f %%, "
                ">4 ms %.1f %%; even frames %s, odd frames %s\n",
                c, n, pct(all, n, 0.5), pct(all, n, 0.9), pct(all, n, 0.99), all[n - 1], 100.0 * over1 / n, 100.0 * over2 / n,
                100.0 * over4 / n, part[0], part[1]);
        free(all);
    }
    pthread_mutex_unlock(&gPendMutex);
}

static int snapshotsChecked(void) { return gChecked; }

// MOCK_XR_DUMP: inside xrReleaseSwapchainImage (the app holds its queue): wait for the queue,
// copy the image (COLOR_ATTACHMENT_OPTIMAL, as the app must leave it) into a host buffer and
// write every `step`-th pixel as a binary PPM. 8-bit RGBA / BGRA formats only.
static int dumpImage(void* images, int index, int width, int height, int64_t format, int step, const char* path) {
    if (!gQueue || !gDevice) return 0;
    int bgra = format == VK_FORMAT_B8G8R8A8_SRGB || format == VK_FORMAT_B8G8R8A8_UNORM;
    if (!bgra && format != VK_FORMAT_R8G8B8A8_SRGB && format != VK_FORMAT_R8G8B8A8_UNORM) return 0;
    VkImage image = ((VkImage*)images)[index];
    VkDeviceSize size = (VkDeviceSize)width * height * 4;
    VkBufferCreateInfo bci;
    memset(&bci, 0, sizeof(bci));
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = size;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkBuffer buf;
    if (DEV(vkCreateBuffer)(gDevice, &bci, NULL, &buf) != VK_SUCCESS) return 0;
    VkMemoryRequirements req;
    DEV(vkGetBufferMemoryRequirements)(gDevice, buf, &req);
    VkPhysicalDeviceMemoryProperties mp;
    INST(vkGetPhysicalDeviceMemoryProperties)(gPhys, &mp);
    uint32_t type = UINT32_MAX;
    const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (uint32_t t = 0; t < mp.memoryTypeCount; t++)
        if ((req.memoryTypeBits & (1u << t)) && (mp.memoryTypes[t].propertyFlags & want) == want) {
            type = t;
            break;
        }
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkMemoryAllocateInfo mai;
    memset(&mai, 0, sizeof(mai));
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = type;
    if (type == UINT32_MAX || DEV(vkAllocateMemory)(gDevice, &mai, NULL, &mem) != VK_SUCCESS) {
        DEV(vkDestroyBuffer)(gDevice, buf, NULL);
        return 0;
    }
    DEV(vkBindBufferMemory)(gDevice, buf, mem, 0);
    VkCommandPoolCreateInfo pci;
    memset(&pci, 0, sizeof(pci));
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.queueFamilyIndex = gQueueFamily;
    VkCommandPool pool;
    DEV(vkCreateCommandPool)(gDevice, &pci, NULL, &pool);
    VkCommandBufferAllocateInfo ai;
    memset(&ai, 0, sizeof(ai));
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cb;
    DEV(vkAllocateCommandBuffers)(gDevice, &ai, &cb);
    VkCommandBufferBeginInfo bi;
    memset(&bi, 0, sizeof(bi));
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    DEV(vkBeginCommandBuffer)(cb, &bi);
    VkImageMemoryBarrier b;
    memset(&b, 0, sizeof(b));
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = 1;
    DEV(vkCmdPipelineBarrier)(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    VkBufferImageCopy region;
    memset(&region, 0, sizeof(region));
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent.width = (uint32_t)width;
    region.imageExtent.height = (uint32_t)height;
    region.imageExtent.depth = 1;
    DEV(vkCmdCopyImageToBuffer)(cb, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &region);
    b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    b.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    DEV(vkCmdPipelineBarrier)(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    DEV(vkEndCommandBuffer)(cb);
    VkSubmitInfo si;
    memset(&si, 0, sizeof(si));
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    int ok = DEV(vkQueueSubmit)(gQueue, 1, &si, VK_NULL_HANDLE) == VK_SUCCESS && DEV(vkQueueWaitIdle)(gQueue) == VK_SUCCESS;
    void* map = NULL;
    if (ok && DEV(vkMapMemory)(gDevice, mem, 0, VK_WHOLE_SIZE, 0, &map) == VK_SUCCESS) {
        FILE* f = fopen(path, "wb");
        if (f) {
            int ow = (width + step - 1) / step, oh = (height + step - 1) / step;
            fprintf(f, "P6\n%d %d\n255\n", ow, oh);
            const unsigned char* px = (const unsigned char*)map;
            for (int y = 0; y < height; y += step)
                for (int x = 0; x < width; x += step) {
                    const unsigned char* q = px + ((size_t)y * width + x) * 4;
                    unsigned char rgb[3] = {bgra ? q[2] : q[0], q[1], bgra ? q[0] : q[2]};
                    fwrite(rgb, 1, 3, f);
                }
            fclose(f);
            // MOCK_XR_DUMP_ALPHA=1: the alpha channel beside it (<path without .ppm>-alpha.pgm), to
            // composite layers blended by their alpha (the menus' environment layer over the screen).
            if (getenv("MOCK_XR_DUMP_ALPHA")) {
                char apath[600];
                snprintf(apath, sizeof(apath), "%.*s-alpha.pgm", (int)(strlen(path) > 4 ? strlen(path) - 4 : strlen(path)), path);
                FILE* a = fopen(apath, "wb");
                if (a) {
                    fprintf(a, "P5\n%d %d\n255\n", ow, oh);
                    for (int y = 0; y < height; y += step)
                        for (int x = 0; x < width; x += step) fputc(px[((size_t)y * width + x) * 4 + 3], a);
                    fclose(a);
                }
            }
        } else ok = 0;
        DEV(vkUnmapMemory)(gDevice, mem);
    }
    DEV(vkDestroyCommandPool)(gDevice, pool, NULL);
    DEV(vkDestroyBuffer)(gDevice, buf, NULL);
    DEV(vkFreeMemory)(gDevice, mem, NULL);
    return ok;
}

// MOCK_XR_STAMP: the app's stamp block (see the app's vulkan_win32_interop.cpp), 64x15 pixels in the
// top left corner, one pixel a bit (rows 0-2 the display time and eye, 3-6 lane L's scene words, 7-14
// lane P's pose words), copied at the release into one of STAMP_SLOTS host-visible slots.
#define STAMP_W 64
#define STAMP_H 15
#define STAMP_SLOTS 64
static struct {
    VkFence fence;
    VkCommandBuffer cb;
    unsigned gen;
    int submitted, decoded, result, eye, scene, pose;
    uint64_t value, sceneA, sceneB, poseW[4];
} gStamp[STAMP_SLOTS];
static VkBuffer gStampBuf;
static VkDeviceMemory gStampMem;
static unsigned char* gStampMap;
static VkCommandPool gStampPool;
static int gStampNext = 0, gStampReady = -1;

static int stampSetup(void) {
    if (gStampReady >= 0) return gStampReady;
    gStampReady = 0;
    VkBufferCreateInfo bci;
    memset(&bci, 0, sizeof(bci));
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = (VkDeviceSize)STAMP_W * STAMP_H * 4 * STAMP_SLOTS;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (DEV(vkCreateBuffer)(gDevice, &bci, NULL, &gStampBuf) != VK_SUCCESS) return 0;
    VkMemoryRequirements req;
    DEV(vkGetBufferMemoryRequirements)(gDevice, gStampBuf, &req);
    VkPhysicalDeviceMemoryProperties mp;
    INST(vkGetPhysicalDeviceMemoryProperties)(gPhys, &mp);
    uint32_t type = UINT32_MAX;
    const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (uint32_t t = 0; t < mp.memoryTypeCount; t++)
        if ((req.memoryTypeBits & (1u << t)) && (mp.memoryTypes[t].propertyFlags & want) == want) {
            type = t;
            break;
        }
    VkMemoryAllocateInfo mai;
    memset(&mai, 0, sizeof(mai));
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = type;
    if (type == UINT32_MAX || DEV(vkAllocateMemory)(gDevice, &mai, NULL, &gStampMem) != VK_SUCCESS) return 0;
    DEV(vkBindBufferMemory)(gDevice, gStampBuf, gStampMem, 0);
    void* map = NULL;
    if (DEV(vkMapMemory)(gDevice, gStampMem, 0, VK_WHOLE_SIZE, 0, &map) != VK_SUCCESS) return 0;
    gStampMap = (unsigned char*)map;
    VkCommandPoolCreateInfo pci;
    memset(&pci, 0, sizeof(pci));
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = gQueueFamily;
    if (DEV(vkCreateCommandPool)(gDevice, &pci, NULL, &gStampPool) != VK_SUCCESS) return 0;
    for (int i = 0; i < STAMP_SLOTS; i++) {
        VkCommandBufferAllocateInfo ai;
        memset(&ai, 0, sizeof(ai));
        ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        ai.commandPool = gStampPool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        VkFenceCreateInfo fi;
        memset(&fi, 0, sizeof(fi));
        fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        if (DEV(vkAllocateCommandBuffers)(gDevice, &ai, &gStamp[i].cb) != VK_SUCCESS ||
            DEV(vkCreateFence)(gDevice, &fi, NULL, &gStamp[i].fence) != VK_SUCCESS)
            return 0;
    }
    gStampReady = 1;
    return 1;
}

// Decodes slot i once its copy finished: 1 valid, -1 no stamp. A cell (one pixel) is 1 when it is white
// (any colour channel above half).
static int stampBit(int i, int column, int row) {
    const unsigned char* q = gStampMap + (size_t)i * STAMP_W * STAMP_H * 4 + ((size_t)row * STAMP_W + (size_t)column) * 4;
    return q[0] > 127 || q[1] > 127 || q[2] > 127;
}
static int stampDecode(int i) {
    static const int marker[4] = {1, 0, 1, 0};
    for (int k = 0; k < 4; k++)
        if (stampBit(i, 2 + k, 2) != marker[k]) return -1;
    if (stampBit(i, 0, 2) == stampBit(i, 1, 2)) return -1;
    uint64_t value = 0;
    for (int bit = 0; bit < 64; bit++) {
        int one = stampBit(i, bit, 0);
        if (one == stampBit(i, bit, 1)) return -1;
        if (one) value |= 1ull << bit;
    }
    gStamp[i].value = value;
    gStamp[i].eye = stampBit(i, 0, 2);
    // Scene words: each bit and its complement; all-zero pairs (an app without them) read as none.
    uint64_t words[2] = {0, 0};
    int scene = 1, any = 0;
    for (int w = 0; w < 2 && scene == 1; w++)
        for (int bit = 0; bit < 64; bit++) {
            int one = stampBit(i, bit, 3 + 2 * w), other = stampBit(i, bit, 4 + 2 * w);
            any |= one | other;
            if (one == other) {
                scene = -1;
                break;
            }
            if (one) words[w] |= 1ull << bit;
        }
    gStamp[i].scene = any ? scene : 0;
    gStamp[i].sceneA = words[0];
    gStamp[i].sceneB = words[1];
    // Lane P's pose words (rows 7, 9, 11, 13 and their complements); all-zero pairs read as none.
    uint64_t pw[4] = {0, 0, 0, 0};
    int pose = 1, anyPose = 0;
    for (int w = 0; w < 4 && pose == 1; w++)
        for (int bit = 0; bit < 64; bit++) {
            int one = stampBit(i, bit, 7 + 2 * w), other = stampBit(i, bit, 8 + 2 * w);
            anyPose |= one | other;
            if (one == other) {
                pose = -1;
                break;
            }
            if (one) pw[w] |= 1ull << bit;
        }
    gStamp[i].pose = anyPose ? pose : 0;
    for (int w = 0; w < 4; w++) gStamp[i].poseW[w] = pw[w];
    return 1;
}

static int stampCapture(void* images, int index, int width, int height, unsigned* gen) {
    if (!gQueue || !gDevice || width < STAMP_W || height < STAMP_H || !stampSetup()) return -1;
    int i = gStampNext;
    gStampNext = (gStampNext + 1) % STAMP_SLOTS;
    if (gStamp[i].submitted) {
        // Reused: its copy was queued a whole ring ago and long finished.
        DEV(vkWaitForFences)(gDevice, 1, &gStamp[i].fence, VK_TRUE, 2000000000ull);
        DEV(vkResetFences)(gDevice, 1, &gStamp[i].fence);
        gStamp[i].submitted = 0;
    }
    gStamp[i].gen++;
    gStamp[i].decoded = 0;
    VkCommandBuffer cb = gStamp[i].cb;
    DEV(vkResetCommandBuffer)(cb, 0);
    VkCommandBufferBeginInfo bi;
    memset(&bi, 0, sizeof(bi));
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    DEV(vkBeginCommandBuffer)(cb, &bi);
    VkImageMemoryBarrier b;
    memset(&b, 0, sizeof(b));
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = ((VkImage*)images)[index];
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = 1;
    DEV(vkCmdPipelineBarrier)(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    VkBufferImageCopy region;
    memset(&region, 0, sizeof(region));
    region.bufferOffset = (VkDeviceSize)i * STAMP_W * STAMP_H * 4;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent.width = STAMP_W;
    region.imageExtent.height = STAMP_H;
    region.imageExtent.depth = 1;
    DEV(vkCmdCopyImageToBuffer)(cb, b.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, gStampBuf, 1, &region);
    b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    b.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    DEV(vkCmdPipelineBarrier)(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    DEV(vkEndCommandBuffer)(cb);
    VkSubmitInfo si;
    memset(&si, 0, sizeof(si));
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    if (DEV(vkQueueSubmit)(gQueue, 1, &si, gStamp[i].fence) != VK_SUCCESS) return -1;
    gStamp[i].submitted = 1;
    *gen = gStamp[i].gen;
    return i;
}

static int stampRead(int i, unsigned gen, int wait, uint64_t* value, int* eye) {
    if (i < 0 || i >= STAMP_SLOTS || gStamp[i].gen != gen) return -2;
    if (!gStamp[i].decoded) {
        if (!gStamp[i].submitted) return -2;
        VkResult r = wait ? DEV(vkWaitForFences)(gDevice, 1, &gStamp[i].fence, VK_TRUE, 2000000000ull)
                          : DEV(vkGetFenceStatus)(gDevice, gStamp[i].fence);
        if (r != VK_SUCCESS) return 0;
        DEV(vkResetFences)(gDevice, 1, &gStamp[i].fence);
        gStamp[i].submitted = 0;
        gStamp[i].result = stampDecode(i);
        gStamp[i].decoded = 1;
    }
    *value = gStamp[i].value;
    *eye = gStamp[i].eye;
    return gStamp[i].result;
}

static int stampScene(int i, unsigned gen, uint64_t* a, uint64_t* b) {
    if (i < 0 || i >= STAMP_SLOTS || gStamp[i].gen != gen || !gStamp[i].decoded || gStamp[i].result != 1) return -1;
    *a = gStamp[i].sceneA;
    *b = gStamp[i].sceneB;
    return gStamp[i].scene;
}

static int stampPose(int i, unsigned gen, uint64_t* words) {
    if (i < 0 || i >= STAMP_SLOTS || gStamp[i].gen != gen || !gStamp[i].decoded || gStamp[i].result != 1) return -1;
    for (int w = 0; w < 4; w++) words[w] = gStamp[i].poseW[w];
    return gStamp[i].pose;
}


// Lane S, MOCK_XR_DUMP_QUEUE=<slots>: dumps without stalling the app. At the release the image is blitted
// (linear filter) down to width/step x height/step into a scratch image, copied with the stamp rows of the
// full-size image into a host slot, and submitted with a fence; nothing waits. dumpFlush (when the dumps
// pause, the slots run out, or the session ends) waits for the copies and writes the PPMs and, per image,
// a line "<file> <tag> <eye> <display ns> <scene word A> <scene word B>" to <dir>/index.txt (eye -1: no stamp).
#define DQ_STAMP_ROWS 7
typedef struct {
    VkFence fence;
    VkCommandBuffer cb;
    int busy, ow, oh, bgra;
    char path[512], tag[96];
} DumpSlot;
static DumpSlot* gDq;
static int gDqSlots = -1, gDqNext, gDqUsed, gDqW, gDqH;
static VkDeviceSize gDqStride;
static VkBuffer* gDqBuf;
static VkDeviceMemory* gDqMem;
static unsigned char** gDqMap;
static VkImage gDqScratch;
static VkDeviceMemory gDqScratchMem;
static VkCommandPool gDqPool;
#define DQ_CHUNK 64
static uint32_t memType(uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags nice) {
    VkPhysicalDeviceMemoryProperties mp;
    INST(vkGetPhysicalDeviceMemoryProperties)(gPhys, &mp);
    for (int pass = 0; pass < 2; pass++)
        for (uint32_t t = 0; t < mp.memoryTypeCount; t++) {
            VkMemoryPropertyFlags f = mp.memoryTypes[t].propertyFlags;
            if ((bits & (1u << t)) && (f & want) == want && (pass || (f & nice) == nice)) return t;
        }
    return UINT32_MAX;
}
static int dumpQueueSetup(int ow, int oh, VkFormat format) {
    if (gDqSlots >= 0) return gDqSlots > 0 && ow == gDqW && oh == gDqH;
    gDqSlots = 0;
    int slots = getenv("MOCK_XR_DUMP_QUEUE") ? atoi(getenv("MOCK_XR_DUMP_QUEUE")) : 0;
    if (slots <= 0) return 0;
    slots = (slots + DQ_CHUNK - 1) / DQ_CHUNK * DQ_CHUNK;
    gDqW = ow;
    gDqH = oh;
    gDqStride = ((VkDeviceSize)ow * oh * 4 + (VkDeviceSize)STAMP_W * DQ_STAMP_ROWS * 4 + 255) & ~(VkDeviceSize)255;
    VkImageCreateInfo ici;
    memset(&ici, 0, sizeof(ici));
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = format;
    ici.extent.width = (uint32_t)ow;
    ici.extent.height = (uint32_t)oh;
    ici.extent.depth = 1;
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (DEV(vkCreateImage)(gDevice, &ici, NULL, &gDqScratch) != VK_SUCCESS) return 0;
    VkMemoryRequirements req;
    DEV(vkGetImageMemoryRequirements)(gDevice, gDqScratch, &req);
    VkMemoryAllocateInfo mai;
    memset(&mai, 0, sizeof(mai));
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = memType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
    if (mai.memoryTypeIndex == UINT32_MAX || DEV(vkAllocateMemory)(gDevice, &mai, NULL, &gDqScratchMem) != VK_SUCCESS) return 0;
    DEV(vkBindImageMemory)(gDevice, gDqScratch, gDqScratchMem, 0);
    int chunks = slots / DQ_CHUNK;
    gDqBuf = (VkBuffer*)calloc((size_t)chunks, sizeof(VkBuffer));
    gDqMem = (VkDeviceMemory*)calloc((size_t)chunks, sizeof(VkDeviceMemory));
    gDqMap = (unsigned char**)calloc((size_t)chunks, sizeof(unsigned char*));
    gDq = (DumpSlot*)calloc((size_t)slots, sizeof(DumpSlot));
    for (int c = 0; c < chunks; c++) {
        VkBufferCreateInfo bci;
        memset(&bci, 0, sizeof(bci));
        bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.size = gDqStride * DQ_CHUNK;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        if (DEV(vkCreateBuffer)(gDevice, &bci, NULL, &gDqBuf[c]) != VK_SUCCESS) return 0;
        DEV(vkGetBufferMemoryRequirements)(gDevice, gDqBuf[c], &req);
        mai.allocationSize = req.size;
        mai.memoryTypeIndex = memType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                      VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
        if (mai.memoryTypeIndex == UINT32_MAX || DEV(vkAllocateMemory)(gDevice, &mai, NULL, &gDqMem[c]) != VK_SUCCESS) return 0;
        DEV(vkBindBufferMemory)(gDevice, gDqBuf[c], gDqMem[c], 0);
        void* map = NULL;
        if (DEV(vkMapMemory)(gDevice, gDqMem[c], 0, VK_WHOLE_SIZE, 0, &map) != VK_SUCCESS) return 0;
        gDqMap[c] = (unsigned char*)map;
    }
    VkCommandPoolCreateInfo pci;
    memset(&pci, 0, sizeof(pci));
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = gQueueFamily;
    if (DEV(vkCreateCommandPool)(gDevice, &pci, NULL, &gDqPool) != VK_SUCCESS) return 0;
    for (int i = 0; i < slots; i++) {
        VkCommandBufferAllocateInfo ai;
        memset(&ai, 0, sizeof(ai));
        ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        ai.commandPool = gDqPool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        VkFenceCreateInfo fi;
        memset(&fi, 0, sizeof(fi));
        fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        if (DEV(vkAllocateCommandBuffers)(gDevice, &ai, &gDq[i].cb) != VK_SUCCESS || DEV(vkCreateFence)(gDevice, &fi, NULL, &gDq[i].fence) != VK_SUCCESS)
            return 0;
    }
    gDqSlots = slots;
    fprintf(stderr, "xrmock: dump queue: %d slots of %dx%d (%.0f MB host memory)\n", slots, ow, oh, (double)gDqStride * slots / 1048576.0);
    return 1;
}
static int dqBit(const unsigned char* st, int column, int row) {
    const unsigned char* q = st + ((size_t)row * STAMP_W + (size_t)column) * 4;
    return q[0] > 127 || q[1] > 127 || q[2] > 127;
}
static void dumpFlush(void) {
    if (gDqSlots <= 0 || !gDqUsed) return;
    static FILE* index = NULL;
    for (int k = 0; k < gDqSlots; k++) {
        int i = (gDqNext + k) % gDqSlots;  // oldest first
        DumpSlot* d = &gDq[i];
        if (!d->busy) continue;
        DEV(vkWaitForFences)(gDevice, 1, &d->fence, VK_TRUE, 5000000000ull);
        DEV(vkResetFences)(gDevice, 1, &d->fence);
        d->busy = 0;
        const unsigned char* px = gDqMap[i / DQ_CHUNK] + gDqStride * (VkDeviceSize)(i % DQ_CHUNK);
        FILE* f = fopen(d->path, "wb");
        if (f) {
            fprintf(f, "P6\n%d %d\n255\n", d->ow, d->oh);
            unsigned char* row = (unsigned char*)malloc((size_t)d->ow * 3);
            for (int y = 0; y < d->oh; y++) {
                for (int x = 0; x < d->ow; x++) {
                    const unsigned char* q = px + ((size_t)y * d->ow + x) * 4;
                    row[x * 3] = d->bgra ? q[2] : q[0];
                    row[x * 3 + 1] = q[1];
                    row[x * 3 + 2] = d->bgra ? q[0] : q[2];
                }
                fwrite(row, 1, (size_t)d->ow * 3, f);
            }
            free(row);
            fclose(f);
        }
        // The stamp (see stampDecode): row 0 the display time, 1 its complement, 2 eye and marker, 3-6 scene words.
        const unsigned char* st = px + (size_t)d->ow * d->oh * 4;
        static const int marker[4] = {1, 0, 1, 0};
        int ok = dqBit(st, 0, 2) != dqBit(st, 1, 2);
        for (int k2 = 0; k2 < 4 && ok; k2++) ok = dqBit(st, 2 + k2, 2) == marker[k2];
        uint64_t value = 0, w[2] = {0, 0};
        for (int bit = 0; bit < 64 && ok; bit++) {
            ok = dqBit(st, bit, 0) != dqBit(st, bit, 1);
            if (dqBit(st, bit, 0)) value |= 1ull << bit;
        }
        for (int ww = 0; ww < 2 && ok; ww++)
            for (int bit = 0; bit < 64; bit++)
                if (dqBit(st, bit, 3 + 2 * ww)) w[ww] |= 1ull << bit;
        if (!index) {
            char ip[600];
            const char* slash = strrchr(d->path, '/');
            snprintf(ip, sizeof(ip), "%.*s/index.txt", slash ? (int)(slash - d->path) : 1, slash ? d->path : ".");
            index = fopen(ip, "a");
        }
        if (index)
            fprintf(index, "%s %s %d %lld %llu %llu\n", strrchr(d->path, '/') ? strrchr(d->path, '/') + 1 : d->path, d->tag,
                    ok ? dqBit(st, 0, 2) : -1, ok ? (long long)value : -1ll, (unsigned long long)w[0], (unsigned long long)w[1]);
    }
    if (index) fflush(index);
    gDqUsed = 0;
}
static int dumpAsync(void* images, int index, int width, int height, int64_t format, int step, const char* path, const char* tag) {
    if (!gQueue || !gDevice) return 0;
    int bgra = format == VK_FORMAT_B8G8R8A8_SRGB || format == VK_FORMAT_B8G8R8A8_UNORM;
    if (!bgra && format != VK_FORMAT_R8G8B8A8_SRGB && format != VK_FORMAT_R8G8B8A8_UNORM) return 0;
    int ow = width / step, oh = height / step;
    if (width < STAMP_W || height < DQ_STAMP_ROWS || !dumpQueueSetup(ow, oh, (VkFormat)format)) return 0;
    if (gDq[gDqNext].busy) dumpFlush();  // out of slots: write them all now (a stall)
    int i = gDqNext;
    gDqNext = (gDqNext + 1) % gDqSlots;
    DumpSlot* d = &gDq[i];
    d->ow = ow;
    d->oh = oh;
    d->bgra = bgra;
    snprintf(d->path, sizeof(d->path), "%s", path);
    snprintf(d->tag, sizeof(d->tag), "%s", tag);
    VkImage image = ((VkImage*)images)[index];
    VkCommandBuffer cb = d->cb;
    DEV(vkResetCommandBuffer)(cb, 0);
    VkCommandBufferBeginInfo bi;
    memset(&bi, 0, sizeof(bi));
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    DEV(vkBeginCommandBuffer)(cb, &bi);
    VkImageMemoryBarrier b[2];
    memset(b, 0, sizeof(b));
    for (int k = 0; k < 2; k++) {
        b[k].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b[k].srcQueueFamilyIndex = b[k].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b[k].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        b[k].subresourceRange.levelCount = 1;
        b[k].subresourceRange.layerCount = 1;
    }
    b[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    b[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    b[0].oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    b[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b[0].image = image;
    b[1].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;  // the last dump's copy out of the scratch image
    b[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b[1].image = gDqScratch;
    DEV(vkCmdPipelineBarrier)(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 2, b);
    VkImageBlit blit;
    memset(&blit, 0, sizeof(blit));
    blit.srcSubresource.aspectMask = blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    blit.srcSubresource.layerCount = blit.dstSubresource.layerCount = 1;
    blit.srcOffsets[1].x = ow * step;
    blit.srcOffsets[1].y = oh * step;
    blit.srcOffsets[1].z = 1;
    blit.dstOffsets[1].x = ow;
    blit.dstOffsets[1].y = oh;
    blit.dstOffsets[1].z = 1;
    DEV(vkCmdBlitImage)(cb, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, gDqScratch, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                        VK_FILTER_LINEAR);
    b[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    b[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    DEV(vkCmdPipelineBarrier)(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b[1]);
    VkBufferImageCopy region;
    memset(&region, 0, sizeof(region));
    region.bufferOffset = gDqStride * (VkDeviceSize)(i % DQ_CHUNK);
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent.width = (uint32_t)ow;
    region.imageExtent.height = (uint32_t)oh;
    region.imageExtent.depth = 1;
    DEV(vkCmdCopyImageToBuffer)(cb, gDqScratch, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, gDqBuf[i / DQ_CHUNK], 1, &region);
    region.bufferOffset += (VkDeviceSize)ow * oh * 4;
    region.imageExtent.width = STAMP_W;
    region.imageExtent.height = DQ_STAMP_ROWS;
    DEV(vkCmdCopyImageToBuffer)(cb, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, gDqBuf[i / DQ_CHUNK], 1, &region);
    b[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    b[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
    b[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b[0].newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    DEV(vkCmdPipelineBarrier)(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b[0]);
    DEV(vkEndCommandBuffer)(cb);
    VkSubmitInfo si;
    memset(&si, 0, sizeof(si));
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    if (DEV(vkQueueSubmit)(gQueue, 1, &si, d->fence) != VK_SUCCESS) return 0;
    d->busy = 1;
    gDqUsed++;
    return 1;
}

const MockGfx kMockGfxVk = {"vk", ownsBinding, describeBinding, getProc, formats, formatOf, createImages, fillImages, releaseWaitMs,
                            releaseSnapshot, pollSnapshots, snapshotsChecked, dumpImage, stampCapture, stampRead, stampScene, stampPose,
                            dumpAsync, dumpFlush, acquireCheck, releaseReport};
