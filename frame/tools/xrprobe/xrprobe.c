// Copyright (C) 2026 Moonlight XR Native and GalaxyFrame contributors. GPLv3, see LICENSE.
// xrprobe: OpenXR + Vulkan probe for SteamVR pacing and frame synthesis on the
// Steam Frame (part of GalaxyFrame, not of the game build). Forked from the Moonlight XR
// Native M0 hello program, which is proven on this headset.
//
// Modes (exactly one):
//   --pace          ~14 ms of calibrated GPU work per frame, then a world-locked
//                   panel with the frame number, a hue that steps every frame and
//                   a ticker; the swapchain images are released right after
//                   vkQueueSubmit without waiting for the GPU
//   --pace=fence    the same, but vkWaitForFences before the release (control)
//   --synth         XR_EXT_frame_synthesis: R16G16B16A16_SFLOAT motion vectors
//                   + depth swapchains chained on each projection view with
//                   REQUEST_RELAXED_FRAME_INTERVAL; a moving checkerboard drawn
//                   at --fps (default 30)
//   --synth --no-mv the same frame rate and content, nothing chained
//   --depth-only    XR_KHR_composition_layer_depth only, moving checkerboard at
//                   the display rate
//   --sequence      the five cases above, in that order, in ONE session; the
//                   case number and a short label are drawn in each eye and
//                   stdout gets a "CASE n start" line per case
//   --spacewarp     XR_FB_space_warp: motion vectors (R16G16B16A16_SFLOAT,
//                   (current - previous) NDC of the object's motion, head
//                   motion left to the runtime) and depth chained on each
//                   projection view as XrCompositionLayerSpaceWarpInfoFB.
//                   New content on every second display slot (counted from
//                   predictedDisplayTime against the display's refresh: 60
//                   a second at 120 Hz); the slots between submit the last
//                   content again (same images and poses, no SpaceWarp
//                   info).  A counter strip under the checkerboard shows
//                   the content frame number.
//   --spacewarp --no-mv  the same cadence and content, nothing chained
//   --sequence-sw   cases 6 and 7: --spacewarp, then --spacewarp --no-mv, in
//                   one session ("6 SPACE", "7 no-SPC" in each eye)
// Options: --hz N (XR_FB_display_refresh_rate), --seconds N (per case, default
//   30, counted only while the session is VISIBLE or FOCUSED and rendering),
//   --log FILE (default ./xrprobe.log), --fps N, --gpu-ms N (pace load target,
//   default 14), --mv-head (motion vectors include head motion), --dry-run
//   (parse and print the configuration only).
//
// Every frame is logged to the --log file: xrWaitFrame/xrEndFrame results,
// predictedDisplayTime/Period, shouldRender, gap between xrWaitFrame returns,
// CPU time to submit, GPU time from timestamp queries and whether the GPU was
// still busy when the images were released and when xrEndFrame returned.
//
// Derived from Moonlight XR Native's hello program, which is GPLv3; this
// standalone tool keeps that licence. It is not linked into the game.

#define _GNU_SOURCE
#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <vulkan/vulkan.h>
#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#ifndef XR_EXT_frame_synthesis
#error "OpenXR headers lack XR_EXT_frame_synthesis (need 1.1.42 or newer)"
#endif

static const uint32_t kQuadVert[] =
#include "quad.vert.inc"
    ;
static const uint32_t kSceneFrag[] =
#include "scene.frag.inc"
    ;
static const uint32_t kMvFrag[] =
#include "mv.frag.inc"
    ;
static const uint32_t kFullVert[] =
#include "fullscreen.vert.inc"
    ;
static const uint32_t kHeavyFrag[] =
#include "heavy.frag.inc"
    ;

// ---------------------------------------------------------------- options

enum Mode { MODE_NONE, MODE_PACE, MODE_PACE_FENCE, MODE_SYNTH, MODE_DEPTH, MODE_SEQUENCE, MODE_SW, MODE_SEQUENCE_SW };

static struct {
    enum Mode mode;
    int noMv;
    int mvHead;
    int dryRun;
    float hz;
    double seconds;
    double fps;
    double gpuMs;
    const char* log;
} g_opt = {MODE_NONE, 0, 0, 0, 0.0f, 30.0, -1.0, 14.0, "xrprobe.log"};

static const char* modeName(void) {
    switch (g_opt.mode) {
    case MODE_PACE: return "pace";
    case MODE_PACE_FENCE: return "pace=fence";
    case MODE_SYNTH: return g_opt.noMv ? "synth-no-mv" : "synth";
    case MODE_DEPTH: return "depth-only";
    case MODE_SEQUENCE: return "sequence";
    case MODE_SW: return g_opt.noMv ? "spacewarp-no-mv" : "spacewarp";
    case MODE_SEQUENCE_SW: return "sequence-sw";
    default: return "none";
    }
}

// A case is one probe configuration; --sequence runs all five in one session.
typedef struct {
    enum Mode mode;
    int noMv;
    const char* name;  // log name
    const char* label; // drawn in each eye with seven-segment glyphs, max 8 cells
} Case;

static const Case kSequence[5] = {
    {MODE_PACE, 0, "pace", "1 PACE"},
    {MODE_PACE_FENCE, 0, "pace=fence", "2 FEnCE"},
    {MODE_SYNTH, 0, "synth", "3 SYnth"},
    {MODE_SYNTH, 1, "synth-no-mv", "4 no-SYn"},
    {MODE_DEPTH, 0, "depth-only", "5 dEPth"},
};

// --sequence-sw: XR_FB_space_warp against the same 60 fps without it.
static const Case kSequenceSw[2] = {
    {MODE_SW, 0, "spacewarp", "6 SPACE"},
    {MODE_SW, 1, "spacewarp-no-mv", "7 no-SPC"},
};

// Seven-segment mask (bits a..g) for the characters the labels use.
static uint32_t segChar(char c) {
    switch (c) {
    case '0': case 'O': case 'o': return c == 'o' ? 0x5C : 0x3F;
    case '1': return 0x06;
    case '2': return 0x5B;
    case '3': return 0x4F;
    case '4': return 0x66;
    case '5': case 'S': return 0x6D;
    case '6': return 0x7D;
    case '7': return 0x07;
    case 'A': return 0x77;
    case 'C': return 0x39;
    case 'd': return 0x5E;
    case 'E': return 0x79;
    case 'F': return 0x71;
    case 'h': return 0x74;
    case 'n': return 0x54;
    case 'P': return 0x73;
    case 't': return 0x78;
    case 'Y': return 0x6E;
    case '-': return 0x40;
    default: return 0;
    }
}

// ---------------------------------------------------------------- logging

static double nowSec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static double g_t0;
static FILE* g_log;

// LOG: stdout and the log file. FLOG: the log file only (per-frame lines).
static void logf_(int toStdout, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
static void logf_(int toStdout, const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    double t = nowSec() - g_t0;
    if (toStdout) {
        fprintf(stdout, "[%8.3f] %s\n", t, buf);
        fflush(stdout);
    }
    if (g_log) fprintf(g_log, "[%8.3f] %s\n", t, buf);
}
#define LOG(...) logf_(1, __VA_ARGS__)
#define FLOG(...) logf_(0, __VA_ARGS__)

static XrInstance g_instance = XR_NULL_HANDLE;


static const char* xrStr(XrResult r) {
    static char buf[XR_MAX_RESULT_STRING_SIZE];
    if (g_instance != XR_NULL_HANDLE && xrResultToString(g_instance, r, buf) == XR_SUCCESS) return buf;
    snprintf(buf, sizeof(buf), "XrResult %d", (int)r);
    return buf;
}

static void die(void) {
    if (g_log) fflush(g_log);
    exit(1);
}

#define XR_CHECK(call)                                                      \
    do {                                                                    \
        XrResult r_ = (call);                                               \
        if (XR_FAILED(r_)) {                                                \
            LOG("FAIL %s -> %s (line %d)", #call, xrStr(r_), __LINE__);     \
            die();                                                          \
        }                                                                   \
    } while (0)

#define VK_CHECK(call)                                                      \
    do {                                                                    \
        VkResult r_ = (call);                                               \
        if (r_ != VK_SUCCESS) {                                             \
            LOG("FAIL %s -> VkResult %d (line %d)", #call, (int)r_, __LINE__); \
            die();                                                          \
        }                                                                   \
    } while (0)

static volatile sig_atomic_t g_quit;
static void onSignal(int s) {
    (void)s;
    if (g_quit) _exit(130); // second signal: give up on the clean teardown
    g_quit = 1;
}
static void onAlarm(int s) {
    (void)s;
    static const char msg[] = "xrprobe: watchdog: no clean exit in time (headset asleep? xrWaitFrame blocked)\n";
    ssize_t w = write(2, msg, sizeof(msg) - 1);
    (void)w;
    _exit(3);
}

// ---------------------------------------------------------------- maths

typedef struct { float m[16]; } Mat4; // column major

static Mat4 matMul(const Mat4* a, const Mat4* b) {
    Mat4 r;
    for (int c = 0; c < 4; c++)
        for (int row = 0; row < 4; row++) {
            float s = 0;
            for (int k = 0; k < 4; k++) s += a->m[k * 4 + row] * b->m[c * 4 + k];
            r.m[c * 4 + row] = s;
        }
    return r;
}

// Vulkan clip space: y down, z 0..1 (near -> 0, far -> 1).
static Mat4 projectionFov(XrFovf fov, float nearZ, float farZ) {
    float l = tanf(fov.angleLeft), r = tanf(fov.angleRight);
    float d = tanf(fov.angleDown), u = tanf(fov.angleUp);
    float w = r - l, h = d - u;
    Mat4 p;
    memset(&p, 0, sizeof(p));
    p.m[0] = 2.0f / w;
    p.m[8] = (r + l) / w;
    p.m[5] = 2.0f / h;
    p.m[9] = (u + d) / h;
    p.m[10] = -farZ / (farZ - nearZ);
    p.m[14] = -(farZ * nearZ) / (farZ - nearZ);
    p.m[11] = -1.0f;
    return p;
}

static Mat4 viewFromPose(XrPosef pose) {
    float x = pose.orientation.x, y = pose.orientation.y, z = pose.orientation.z, w = pose.orientation.w;
    float R[9] = {
        1 - 2 * (y * y + z * z), 2 * (x * y + z * w),     2 * (x * z - y * w),
        2 * (x * y - z * w),     1 - 2 * (x * x + z * z), 2 * (y * z + x * w),
        2 * (x * z + y * w),     2 * (y * z - x * w),     1 - 2 * (x * x + y * y),
    };
    Mat4 v;
    memset(&v, 0, sizeof(v));
    for (int c = 0; c < 3; c++)
        for (int row = 0; row < 3; row++) v.m[c * 4 + row] = R[row * 3 + c];
    float tx = pose.position.x, ty = pose.position.y, tz = pose.position.z;
    for (int row = 0; row < 3; row++)
        v.m[12 + row] = -(v.m[0 * 4 + row] * tx + v.m[1 * 4 + row] * ty + v.m[2 * 4 + row] * tz);
    v.m[15] = 1;
    return v;
}

static Mat4 placeQuad(float x, float y, float z, float sx, float sy) {
    Mat4 t;
    memset(&t, 0, sizeof(t));
    t.m[0] = sx;
    t.m[5] = sy;
    t.m[10] = 1;
    t.m[15] = 1;
    t.m[12] = x;
    t.m[13] = y;
    t.m[14] = z;
    return t;
}

typedef struct {
    Mat4 mvp;
    Mat4 aux;
} Push;

#define NEAR_Z 0.05f
#define FAR_Z 100.0f

// The checkerboard of the synth and depth cases: 1.2 m square, 2 m ahead,
// sliding left and right (0.6 m amplitude, 4 s period) by display time.
static Mat4 checkerModel(double t) {
    float x = 0.6f * (float)sin(t * 2.0 * M_PI / 4.0);
    return placeQuad(x, 0.0f, -2.0f, 1.2f, 1.2f);
}

// ---------------------------------------------------------------- extensions

typedef struct {
    const char* name;
    int present;
    int enable;
} ExtWant;

static ExtWant g_exts[] = {
    {"XR_KHR_vulkan_enable2", 0, 1},
    {"XR_FB_display_refresh_rate", 0, 1},
    {"XR_EXT_frame_synthesis", 0, 0},          // enabled for --synth only
    {"XR_KHR_composition_layer_depth", 0, 0},  // enabled for --depth-only only
    {"XR_FB_space_warp", 0, 0},                // enabled for --spacewarp / --sequence-sw only
};
#define N_EXTS (sizeof(g_exts) / sizeof(g_exts[0]))

static ExtWant* ext(const char* name) {
    for (size_t i = 0; i < N_EXTS; i++)
        if (!strcmp(g_exts[i].name, name)) return &g_exts[i];
    return NULL;
}
static int hasExt(const char* name) { return ext(name)->present; }

// ---------------------------------------------------------------- Vulkan

static PFN_vkCmdBeginRenderingKHR pfnBeginRendering;
static PFN_vkCmdEndRenderingKHR pfnEndRendering;

typedef struct {
    VkInstance instance;
    VkPhysicalDevice phys;
    VkDevice device;
    uint32_t queueFamily;
    VkQueue queue;
    VkPhysicalDeviceMemoryProperties mem;
    float timestampPeriodNs;
    uint32_t timestampBits;
} Vk;

static void createVulkan(XrSystemId system, Vk* vk) {
    memset(vk, 0, sizeof(*vk));
    PFN_xrGetVulkanGraphicsRequirements2KHR getReq;
    PFN_xrCreateVulkanInstanceKHR createInst;
    PFN_xrGetVulkanGraphicsDevice2KHR getDev;
    PFN_xrCreateVulkanDeviceKHR createDev;
    XR_CHECK(xrGetInstanceProcAddr(g_instance, "xrGetVulkanGraphicsRequirements2KHR", (PFN_xrVoidFunction*)&getReq));
    XR_CHECK(xrGetInstanceProcAddr(g_instance, "xrCreateVulkanInstanceKHR", (PFN_xrVoidFunction*)&createInst));
    XR_CHECK(xrGetInstanceProcAddr(g_instance, "xrGetVulkanGraphicsDevice2KHR", (PFN_xrVoidFunction*)&getDev));
    XR_CHECK(xrGetInstanceProcAddr(g_instance, "xrCreateVulkanDeviceKHR", (PFN_xrVoidFunction*)&createDev));

    XrGraphicsRequirementsVulkan2KHR req = {XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR};
    XR_CHECK(getReq(g_instance, system, &req));
    LOG("vulkan api range accepted by the runtime: %u.%u .. %u.%u",
        XR_VERSION_MAJOR(req.minApiVersionSupported), XR_VERSION_MINOR(req.minApiVersionSupported),
        XR_VERSION_MAJOR(req.maxApiVersionSupported), XR_VERSION_MINOR(req.maxApiVersionSupported));

    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "xrprobe";
    app.applicationVersion = 1;
    app.pEngineName = "xrprobe";
    app.apiVersion = VK_API_VERSION_1_2; // SteamVR accepts 1.0 .. 1.2 only
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    XrVulkanInstanceCreateInfoKHR xci = {XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR};
    xci.systemId = system;
    xci.pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
    xci.vulkanCreateInfo = &ici;
    VkResult vr;
    XR_CHECK(createInst(g_instance, &xci, &vk->instance, &vr));
    VK_CHECK(vr);

    XrVulkanGraphicsDeviceGetInfoKHR gi = {XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR};
    gi.systemId = system;
    gi.vulkanInstance = vk->instance;
    XR_CHECK(getDev(g_instance, &gi, &vk->phys));
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(vk->phys, &props);
    vkGetPhysicalDeviceMemoryProperties(vk->phys, &vk->mem);
    vk->timestampPeriodNs = props.limits.timestampPeriod;
    LOG("vulkan device: %s, api %u.%u.%u, driver 0x%x, timestampPeriod %.3f ns, maxPushConstants %u",
        props.deviceName, VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion),
        VK_API_VERSION_PATCH(props.apiVersion), props.driverVersion, props.limits.timestampPeriod,
        props.limits.maxPushConstantsSize);

    uint32_t nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(vk->phys, &nq, NULL);
    VkQueueFamilyProperties qf[16];
    if (nq > 16) nq = 16;
    vkGetPhysicalDeviceQueueFamilyProperties(vk->phys, &nq, qf);
    vk->queueFamily = UINT32_MAX;
    for (uint32_t i = 0; i < nq; i++)
        if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            vk->queueFamily = i;
            break;
        }
    if (vk->queueFamily == UINT32_MAX) {
        LOG("FAIL no graphics queue family");
        die();
    }
    vk->timestampBits = qf[vk->queueFamily].timestampValidBits;
    LOG("queue family %u: %u queue(s), timestampValidBits %u", vk->queueFamily, qf[vk->queueFamily].queueCount,
        vk->timestampBits);

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = vk->queueFamily;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkPhysicalDeviceDynamicRenderingFeaturesKHR dr = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES_KHR};
    dr.dynamicRendering = VK_TRUE;
    static const char* const devExts[] = {VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = &dr;
    dci.enabledExtensionCount = 1;
    dci.ppEnabledExtensionNames = devExts;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    XrVulkanDeviceCreateInfoKHR xdci = {XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR};
    xdci.systemId = system;
    xdci.pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
    xdci.vulkanPhysicalDevice = vk->phys;
    xdci.vulkanCreateInfo = &dci;
    XR_CHECK(createDev(g_instance, &xdci, &vk->device, &vr));
    VK_CHECK(vr);
    vkGetDeviceQueue(vk->device, vk->queueFamily, 0, &vk->queue);
    pfnBeginRendering = (PFN_vkCmdBeginRenderingKHR)vkGetDeviceProcAddr(vk->device, "vkCmdBeginRenderingKHR");
    pfnEndRendering = (PFN_vkCmdEndRenderingKHR)vkGetDeviceProcAddr(vk->device, "vkCmdEndRenderingKHR");
    if (!pfnBeginRendering || !pfnEndRendering) {
        LOG("FAIL VK_KHR_dynamic_rendering entry points missing");
        die();
    }
}

static uint32_t memoryType(const Vk* vk, uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < vk->mem.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (vk->mem.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}

static VkShaderModule makeShader(VkDevice dev, const uint32_t* code, size_t bytes) {
    VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = bytes;
    ci.pCode = code;
    VkShaderModule m;
    VK_CHECK(vkCreateShaderModule(dev, &ci, NULL, &m));
    return m;
}

static VkPipeline makePipeline(VkDevice dev, VkPipelineLayout layout, const uint32_t* vsCode, size_t vsBytes,
                               const uint32_t* fsCode, size_t fsBytes, VkFormat color, VkFormat depth) {
    VkShaderModule vs = makeShader(dev, vsCode, vsBytes);
    VkShaderModule fs = makeShader(dev, fsCode, fsBytes);
    VkPipelineShaderStageCreateInfo stages[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main", NULL},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main", NULL},
    };
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo dss = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    dss.depthTestEnable = depth != VK_FORMAT_UNDEFINED;
    dss.depthWriteEnable = depth != VK_FORMAT_UNDEFINED;
    dss.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    VkPipelineColorBlendAttachmentState cba = {0};
    cba.colorWriteMask = 0xf;
    VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    ds.dynamicStateCount = 2;
    ds.pDynamicStates = dyn;
    VkPipelineRenderingCreateInfoKHR rci = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR};
    rci.colorAttachmentCount = 1;
    rci.pColorAttachmentFormats = &color;
    rci.depthAttachmentFormat = depth;

    VkGraphicsPipelineCreateInfo gp = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gp.pNext = &rci;
    gp.stageCount = 2;
    gp.pStages = stages;
    gp.pVertexInputState = &vi;
    gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vp;
    gp.pRasterizationState = &rs;
    gp.pMultisampleState = &ms;
    gp.pDepthStencilState = &dss;
    gp.pColorBlendState = &cb;
    gp.pDynamicState = &ds;
    gp.layout = layout;
    VkPipeline pipe;
    VK_CHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gp, NULL, &pipe));
    vkDestroyShaderModule(dev, vs, NULL);
    vkDestroyShaderModule(dev, fs, NULL);
    return pipe;
}

static void imageBarrier(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspect, VkImageLayout from,
                         VkImageLayout to, VkAccessFlags srcAccess, VkAccessFlags dstAccess,
                         VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage) {
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = srcAccess;
    b.dstAccessMask = dstAccess;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = (VkImageSubresourceRange){aspect, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, NULL, 0, NULL, 1, &b);
}

static const char* vkFormatName(int64_t f) {
    switch (f) {
    case VK_FORMAT_R8G8B8A8_SRGB: return "R8G8B8A8_SRGB";
    case VK_FORMAT_B8G8R8A8_SRGB: return "B8G8R8A8_SRGB";
    case VK_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
    case VK_FORMAT_B8G8R8A8_UNORM: return "B8G8R8A8_UNORM";
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return "A2B10G10R10_UNORM";
    case VK_FORMAT_R16G16B16A16_SFLOAT: return "R16G16B16A16_SFLOAT";
    case VK_FORMAT_D16_UNORM: return "D16_UNORM";
    case VK_FORMAT_D24_UNORM_S8_UINT: return "D24_UNORM_S8_UINT";
    case VK_FORMAT_D32_SFLOAT: return "D32_SFLOAT";
    case VK_FORMAT_D32_SFLOAT_S8_UINT: return "D32_SFLOAT_S8_UINT";
    case VK_FORMAT_R16G16B16A16_UNORM: return "R16G16B16A16_UNORM";
    case VK_FORMAT_B10G11R11_UFLOAT_PACK32: return "B10G11R11_UFLOAT";
    case VK_FORMAT_R32G32B32_SFLOAT: return "R32G32B32_SFLOAT";
    case VK_FORMAT_R32G32B32A32_SFLOAT: return "R32G32B32A32_SFLOAT";
    default: return "?";
    }
}

static int isDepthFormat(int64_t f) {
    return f == VK_FORMAT_D16_UNORM || f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D32_SFLOAT ||
           f == VK_FORMAT_D32_SFLOAT_S8_UINT;
}
static VkImageAspectFlags aspectOf(int64_t f) {
    if (f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D32_SFLOAT_S8_UINT)
        return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    if (isDepthFormat(f)) return VK_IMAGE_ASPECT_DEPTH_BIT;
    return VK_IMAGE_ASPECT_COLOR_BIT;
}

// ---------------------------------------------------------------- swapchains

#define MAX_IMAGES 8

typedef struct {
    XrSwapchain handle;
    int32_t width, height;
    int64_t format;
    uint32_t count;
    XrSwapchainImageVulkan2KHR images[MAX_IMAGES];
    VkImageView views[MAX_IMAGES];
    uint32_t acquired;
} Chain;

// Returns 0 on success; logs the runtime's answer either way.
static int createChain(XrSession session, VkDevice dev, const char* what, int64_t format, int32_t w, int32_t h,
                       Chain* c) {
    memset(c, 0, sizeof(*c));
    XrSwapchainCreateInfo ci = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.usageFlags = isDepthFormat(format) ? XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                                          : XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    ci.format = format;
    ci.sampleCount = 1;
    ci.width = (uint32_t)w;
    ci.height = (uint32_t)h;
    ci.faceCount = 1;
    ci.arraySize = 1;
    ci.mipCount = 1;
    XrResult r = xrCreateSwapchain(session, &ci, &c->handle);
    LOG("swapchain %s %s %dx%d -> %s", what, vkFormatName(format), w, h, XR_SUCCEEDED(r) ? "ACCEPTED" : xrStr(r));
    if (XR_FAILED(r)) {
        c->handle = XR_NULL_HANDLE;
        return -1;
    }
    c->width = w;
    c->height = h;
    c->format = format;
    XR_CHECK(xrEnumerateSwapchainImages(c->handle, 0, &c->count, NULL));
    if (c->count > MAX_IMAGES) c->count = MAX_IMAGES;
    for (uint32_t i = 0; i < c->count; i++) c->images[i] = (XrSwapchainImageVulkan2KHR){XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR};
    XR_CHECK(xrEnumerateSwapchainImages(c->handle, c->count, &c->count, (XrSwapchainImageBaseHeader*)c->images));
    for (uint32_t i = 0; i < c->count; i++) {
        VkImageViewCreateInfo vci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = c->images[i].image;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = (VkFormat)format;
        vci.subresourceRange = (VkImageSubresourceRange){aspectOf(format), 0, 1, 0, 1};
        VK_CHECK(vkCreateImageView(dev, &vci, NULL, &c->views[i]));
    }
    LOG("  %s: %u images", what, c->count);
    return 0;
}

static void destroyChain(VkDevice dev, Chain* c) {
    if (!c->handle) return;
    for (uint32_t i = 0; i < c->count; i++) vkDestroyImageView(dev, c->views[i], NULL);
    xrDestroySwapchain(c->handle);
    c->handle = XR_NULL_HANDLE;
}

static void acquireChain(Chain* c) {
    XrSwapchainImageAcquireInfo ai = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    XR_CHECK(xrAcquireSwapchainImage(c->handle, &ai, &c->acquired));
    XrSwapchainImageWaitInfo wi = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = XR_INFINITE_DURATION;
    XR_CHECK(xrWaitSwapchainImage(c->handle, &wi));
}

static void releaseChain(Chain* c) {
    XrSwapchainImageReleaseInfo ri = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    XR_CHECK(xrReleaseSwapchainImage(c->handle, &ri));
}

// ---------------------------------------------------------------- per-frame records

#define NSLOTS 3

typedef struct {
    uint64_t n;            // frame index (every xrWaitFrame)
    uint64_t drawn;        // number shown on the panel (rendered frames)
    XrTime pdt;            // predictedDisplayTime
    XrDuration period;     // predictedDisplayPeriod
    int shouldRender;
    int rendered;
    double waitMs;         // time blocked in xrWaitFrame
    double gapMs;          // time between this and the previous xrWaitFrame return
    double sleepMs;        // throttle sleep before xrWaitFrame (--fps)
    double cpuMs;          // begin of recording .. after vkQueueSubmit
    double fenceWaitMs;    // --pace=fence: vkWaitForFences before release
    double endMs;          // time in xrEndFrame
    double gpuMs;          // timestamp queries; < 0 when unknown
    int busyAtRelease;     // GPU work of this frame unfinished at xrReleaseSwapchainImage
    int busyAtEnd;         // ... and when xrEndFrame returned
    int resubmit;          // SpaceWarp cases: the last content submitted again (no new frame)
    XrResult beginRes, endRes;
    uint32_t iterations;   // heavy loop count (pace)
} FrameRec;

typedef struct {
    FrameRec* v;
    size_t n, cap;
} RecList;

static void recPush(RecList* l, const FrameRec* r) {
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 4096;
        l->v = realloc(l->v, l->cap * sizeof(FrameRec));
        if (!l->v) {
            LOG("FAIL out of memory");
            die();
        }
    }
    l->v[l->n++] = *r;
}

static void logFrame(const FrameRec* r) {
    char gpu[32];
    if (r->gpuMs >= 0) snprintf(gpu, sizeof(gpu), "%.3f", r->gpuMs);
    else snprintf(gpu, sizeof(gpu), "-");
    FLOG("F n=%llu drawn=%llu pdt=%lld period_ms=%.3f render=%d rendered=%d wait_ms=%.3f gap_ms=%.3f sleep_ms=%.3f "
         "cpu_ms=%.3f fence_ms=%.3f end_ms=%.3f gpu_ms=%s iters=%u busy_rel=%d busy_end=%d resub=%d begin=%s end=%s",
         (unsigned long long)r->n, (unsigned long long)r->drawn, (long long)r->pdt, r->period * 1e-6,
         r->shouldRender, r->rendered, r->waitMs, r->gapMs, r->sleepMs, r->cpuMs, r->fenceWaitMs, r->endMs, gpu,
         r->iterations, r->busyAtRelease, r->busyAtEnd, r->resubmit, xrStr(r->beginRes), xrStr(r->endRes));
}

static int cmpDouble(const void* a, const void* b) {
    double x = *(const double*)a, y = *(const double*)b;
    return (x > y) - (x < y);
}

static void percentiles(const double* v, size_t n, double* p50, double* p95, double* mx) {
    *p50 = *p95 = *mx = 0;
    if (!n) return;
    double* s = malloc(n * sizeof(double));
    memcpy(s, v, n * sizeof(double));
    qsort(s, n, sizeof(double), cmpDouble);
    *p50 = s[n / 2];
    *p95 = s[(size_t)((n - 1) * 0.95)];
    *mx = s[n - 1];
    free(s);
}

// ---------------------------------------------------------------- argument parsing

static void usage(FILE* f, const char* argv0) {
    fprintf(f,
            "usage: %s MODE [options]\n"
            "modes (exactly one):\n"
            "  --pace            14 ms GPU work, frame-number panel, release without waiting for the GPU\n"
            "  --pace=fence      the same, vkWaitForFences before release (control)\n"
            "  --synth           XR_EXT_frame_synthesis (MV + depth), checkerboard at --fps (30)\n"
            "  --synth --no-mv   the same frame rate, no synthesis info chained\n"
            "  --depth-only      XR_KHR_composition_layer_depth, checkerboard at display rate\n"
            "  --sequence        all five cases above in one session, --seconds each, case number\n"
            "                    and label drawn in each eye\n"
            "  --spacewarp       XR_FB_space_warp (MV + depth), new content every second display slot\n"
            "  --spacewarp --no-mv  the same frame rate, nothing chained\n"
            "  --sequence-sw     the two SpaceWarp cases (6, 7) in one session\n"
            "options:\n"
            "  --hz N            request display refresh rate N (XR_FB_display_refresh_rate)\n"
            "  --seconds N       seconds per case, counted only while the session is VISIBLE or\n"
            "                    FOCUSED and shouldRender is set; default 30\n"
            "  --log FILE        per-frame log, default ./xrprobe.log\n"
            "  --fps N           cap the app frame rate (default 30 for the synth cases, 60 for the\n"
            "                    SpaceWarp cases, display rate otherwise)\n"
            "  --gpu-ms N        GPU load target of the pace modes, default 14\n"
            "  --mv-head         motion vectors include head motion (default: object motion only)\n"
            "  --dry-run         parse, print the configuration, exit\n",
            argv0);
}

// "--name V" or "--name=V"
static const char* optVal(int argc, char** argv, int* i, const char* name) {
    size_t n = strlen(name);
    if (strncmp(argv[*i], name, n)) return NULL;
    if (argv[*i][n] == '=') return argv[*i] + n + 1;
    if (argv[*i][n] == 0 && *i + 1 < argc) return argv[++*i];
    return NULL;
}

static int parseNum(const char* s, double lo, double hi, double* out) {
    char* end;
    errno = 0;
    double v = strtod(s, &end);
    if (errno || end == s || *end || v < lo || v > hi) return -1;
    *out = v;
    return 0;
}

static int setMode(enum Mode m) {
    if (g_opt.mode != MODE_NONE && g_opt.mode != m) return -1;
    g_opt.mode = m;
    return 0;
}

static int parseArgs(int argc, char** argv) {
    for (int i = 1; i < argc; i++) {
        const char* a = argv[i];
        const char* v;
        double d;
        if (!strcmp(a, "--help") || !strcmp(a, "-h")) {
            usage(stdout, argv[0]);
            exit(0);
        } else if (!strcmp(a, "--pace")) {
            if (setMode(MODE_PACE)) goto twoModes;
        } else if (!strcmp(a, "--pace=fence")) {
            if (setMode(MODE_PACE_FENCE)) goto twoModes;
        } else if (!strcmp(a, "--synth")) {
            if (setMode(MODE_SYNTH)) goto twoModes;
        } else if (!strcmp(a, "--depth-only")) {
            if (setMode(MODE_DEPTH)) goto twoModes;
        } else if (!strcmp(a, "--sequence")) {
            if (setMode(MODE_SEQUENCE)) goto twoModes;
        } else if (!strcmp(a, "--spacewarp")) {
            if (setMode(MODE_SW)) goto twoModes;
        } else if (!strcmp(a, "--sequence-sw")) {
            if (setMode(MODE_SEQUENCE_SW)) goto twoModes;
        } else if (!strcmp(a, "--no-mv")) {
            g_opt.noMv = 1;
        } else if (!strcmp(a, "--mv-head")) {
            g_opt.mvHead = 1;
        } else if (!strcmp(a, "--dry-run")) {
            g_opt.dryRun = 1;
        } else if ((v = optVal(argc, argv, &i, "--hz"))) {
            if (parseNum(v, 1, 1000, &d)) goto bad;
            g_opt.hz = (float)d;
        } else if ((v = optVal(argc, argv, &i, "--seconds"))) {
            if (parseNum(v, 1, 86400, &d)) goto bad;
            g_opt.seconds = d;
        } else if ((v = optVal(argc, argv, &i, "--fps"))) {
            if (parseNum(v, 0, 1000, &d)) goto bad;
            g_opt.fps = d;
        } else if ((v = optVal(argc, argv, &i, "--gpu-ms"))) {
            if (parseNum(v, 0.1, 100, &d)) goto bad;
            g_opt.gpuMs = d;
        } else if ((v = optVal(argc, argv, &i, "--log"))) {
            g_opt.log = v;
        } else {
            fprintf(stderr, "xrprobe: unknown or incomplete option '%s'\n", a);
            usage(stderr, argv[0]);
            return -1;
        }
        continue;
    bad:
        fprintf(stderr, "xrprobe: bad value for '%s'\n", a);
        return -1;
    twoModes:
        fprintf(stderr, "xrprobe: give exactly one mode\n");
        return -1;
    }
    if (g_opt.mode == MODE_NONE) {
        fprintf(stderr, "xrprobe: no mode given\n");
        usage(stderr, argv[0]);
        return -1;
    }
    if (g_opt.noMv && g_opt.mode != MODE_SYNTH && g_opt.mode != MODE_SW) {
        fprintf(stderr, "xrprobe: --no-mv goes with --synth or --spacewarp\n");
        return -1;
    }
    if (g_opt.mvHead && !(g_opt.mode == MODE_SEQUENCE || g_opt.mode == MODE_SEQUENCE_SW || ((g_opt.mode == MODE_SYNTH || g_opt.mode == MODE_SW) && !g_opt.noMv))) {
        fprintf(stderr, "xrprobe: --mv-head goes with --synth (with motion vectors) or --sequence\n");
        return -1;
    }
    return 0;
}

// ---------------------------------------------------------------- GPU load calibration

typedef struct {
    Vk* vk;
    VkPipelineLayout layout;
    VkPipeline heavy;
    VkQueryPool queries;
    VkCommandPool pool;
} Gpu;

static double timestampMs(const Vk* vk, const uint64_t ts[2]) {
    uint64_t mask = vk->timestampBits >= 64 ? ~0ull : ((1ull << vk->timestampBits) - 1);
    uint64_t d = (ts[1] - ts[0]) & mask;
    return (double)d * vk->timestampPeriodNs * 1e-6;
}

static void drawHeavy(VkCommandBuffer cmd, VkPipelineLayout layout, VkPipeline heavy, int32_t w, int32_t h,
                      uint32_t iterations, float hue) {
    VkViewport vp = {0, 0, (float)w, (float)h, 0, 1};
    VkRect2D sc = {{0, 0}, {(uint32_t)w, (uint32_t)h}};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, heavy);
    Push p;
    memset(&p, 0, sizeof(p));
    p.aux.m[0] = (float)iterations;
    p.aux.m[1] = hue;
    p.aux.m[2] = 0.0f; // k
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(p), &p);
    vkCmdDraw(cmd, 3, 1, 0, 0);
}

// Times the heavy pass over two eye-sized offscreen images (both eyes of a
// frame) and scales the loop count until it lands within 5% of the target.
static uint32_t calibrate(Gpu* g, VkFormat format, int32_t w, int32_t h, double targetMs) {
    Vk* vk = g->vk;
    VkImageCreateInfo ici = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = format;
    ici.extent = (VkExtent3D){(uint32_t)w, (uint32_t)h, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage img;
    VK_CHECK(vkCreateImage(vk->device, &ici, NULL, &img));
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(vk->device, img, &mr);
    VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = memoryType(vk, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mai.memoryTypeIndex == UINT32_MAX) mai.memoryTypeIndex = memoryType(vk, mr.memoryTypeBits, 0);
    VkDeviceMemory mem;
    VK_CHECK(vkAllocateMemory(vk->device, &mai, NULL, &mem));
    VK_CHECK(vkBindImageMemory(vk->device, img, mem, 0));
    VkImageViewCreateInfo vci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = img;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = format;
    vci.subresourceRange = (VkImageSubresourceRange){VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageView view;
    VK_CHECK(vkCreateImageView(vk->device, &vci, NULL, &view));

    VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = g->pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VkCommandBuffer cmd;
    VK_CHECK(vkAllocateCommandBuffers(vk->device, &cai, &cmd));
    VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence;
    VK_CHECK(vkCreateFence(vk->device, &fci, NULL, &fence));

    uint32_t iters = 32;
    int good = 0;
    double ms = 0;
    for (int round = 0; round < 16; round++) {
        VK_CHECK(vkResetCommandBuffer(cmd, 0));
        VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VK_CHECK(vkBeginCommandBuffer(cmd, &bi));
        vkCmdResetQueryPool(cmd, g->queries, 0, 2);
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, g->queries, 0);
        for (int e = 0; e < 2; e++) {
            imageBarrier(cmd, img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                         VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                         VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
            VkRenderingAttachmentInfoKHR att = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR};
            att.imageView = view;
            att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            att.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            VkRenderingInfoKHR ri = {VK_STRUCTURE_TYPE_RENDERING_INFO_KHR};
            ri.renderArea.extent = (VkExtent2D){(uint32_t)w, (uint32_t)h};
            ri.layerCount = 1;
            ri.colorAttachmentCount = 1;
            ri.pColorAttachments = &att;
            pfnBeginRendering(cmd, &ri);
            drawHeavy(cmd, g->layout, g->heavy, w, h, iters, 0.0f);
            pfnEndRendering(cmd);
        }
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g->queries, 1);
        VK_CHECK(vkEndCommandBuffer(cmd));
        VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        VK_CHECK(vkResetFences(vk->device, 1, &fence));
        VK_CHECK(vkQueueSubmit(vk->queue, 1, &si, fence));
        VK_CHECK(vkWaitForFences(vk->device, 1, &fence, VK_TRUE, 5ull * 1000 * 1000 * 1000));
        uint64_t ts[2];
        VK_CHECK(vkGetQueryPoolResults(vk->device, g->queries, 0, 2, sizeof(ts), ts, sizeof(uint64_t),
                                       VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT));
        ms = timestampMs(vk, ts);
        LOG("calibration round %d: %u iterations -> %.3f ms GPU (two %dx%d passes)", round, iters, ms, w, h);
        if (round >= 2 && fabs(ms - targetMs) < targetMs * 0.05) {
            if (++good >= 2) break;
            continue;
        }
        good = 0;
        double scale = ms > 0.01 ? targetMs / ms : 8.0;
        if (scale > 8.0) scale = 8.0;
        double next = iters * scale;
        if (next < 1) next = 1;
        if (next > 1e6) next = 1e6;
        iters = (uint32_t)(next + 0.5);
    }
    LOG("calibration: %u iterations for %.1f ms target (last measured %.3f ms)", iters, targetMs, ms);
    vkDestroyFence(vk->device, fence, NULL);
    vkFreeCommandBuffers(vk->device, g->pool, 1, &cmd);
    vkDestroyImageView(vk->device, view, NULL);
    vkDestroyImage(vk->device, img, NULL);
    vkFreeMemory(vk->device, mem, NULL);
    return iters;
}

// ---------------------------------------------------------------- case helpers

// Creates a depth swapchain per eye: D32_SFLOAT first, then D24_UNORM_S8_UINT,
// or the format already chosen for an earlier pair.
static int createDepthPair(XrSession session, VkDevice dev, const char* what, const int32_t w[2], const int32_t h[2],
                           const int64_t* formats, uint32_t nf, int64_t* depthFormat, Chain out[2]) {
    static const int64_t depthTry[] = {VK_FORMAT_D32_SFLOAT, VK_FORMAT_D24_UNORM_S8_UINT};
    for (size_t t = 0; t < 2; t++) {
        if (*depthFormat != VK_FORMAT_UNDEFINED && depthTry[t] != *depthFormat) continue;
        int listed = 0;
        for (uint32_t i = 0; i < nf; i++) listed |= formats[i] == depthTry[t];
        if (!listed) LOG("depth format %s not in the runtime's list; trying anyway", vkFormatName(depthTry[t]));
        int ok = 1;
        for (int e = 0; e < 2 && ok; e++) {
            char name[48];
            snprintf(name, sizeof(name), "eye%d %s", e, what);
            if (createChain(session, dev, name, depthTry[t], w[e], h[e], &out[e])) ok = 0;
        }
        if (ok) {
            *depthFormat = depthTry[t];
            return 0;
        }
        for (int e = 0; e < 2; e++) destroyChain(dev, &out[e]);
    }
    return -1;
}

// The case label (e.g. "3 SYnth") above the content, world-locked. In the
// motion vector pass it is drawn with zero motion (previous = current).
static void drawLabel(VkCommandBuffer cmd, VkPipelineLayout layout, VkPipeline pipe, const Mat4* vp, const char* label,
                      int forMv) {
    Mat4 model = placeQuad(0.0f, 0.62f, -1.5f, 0.72f, 0.15f);
    Push p;
    memset(&p, 0, sizeof(p));
    p.mvp = matMul(vp, &model);
    if (forMv) {
        p.aux = p.mvp;
    } else {
        p.aux.m[0] = 2.0f; // mode: label
        for (int i = 0; i < 8 && label[i]; i++) p.aux.m[4 + i] = (float)segChar(label[i]);
    }
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(p), &p);
    vkCmdDraw(cmd, 6, 1, 0, 0);
}

// The SpaceWarp cases' content counter: a strip under the checkerboard with
// the number of the content frame (and a hue stepping with it), so a panel
// recording shows the content cadence.  In the motion vector pass it is
// drawn with zero motion.
static void drawCounter(VkCommandBuffer cmd, VkPipelineLayout layout, VkPipeline pipe, const Mat4* vp, uint64_t number,
                        int forMv) {
    Mat4 model = placeQuad(0.0f, -0.85f, -2.0f, 0.6f, 0.2f);
    Push p;
    memset(&p, 0, sizeof(p));
    p.mvp = matMul(vp, &model);
    if (forMv) {
        p.aux = p.mvp;
    } else {
        p.aux.m[0] = 1.0f;  // mode: frame-number panel
        p.aux.m[1] = (float)(number % 1000000);
        p.aux.m[2] = (float)fmod((double)number / 12.0, 1.0);
    }
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(p), &p);
    vkCmdDraw(cmd, 6, 1, 0, 0);
}

static void summarize(const char* name, RecList* recs, uint64_t endFailures) {
    size_t nr_ = 0, busyRel = 0, busyEnd = 0, longGaps = 0, ng = 0, ngap = 0, resubmits = 0;
    XrTime firstPdt = 0, lastPdt_ = 0;
    double* gpu = malloc((recs->n + 1) * sizeof(double));
    double* gap = malloc((recs->n + 1) * sizeof(double));
    double* cpu = malloc((recs->n + 1) * sizeof(double));
    double* endms = malloc((recs->n + 1) * sizeof(double));
    for (size_t i = 0; i < recs->n; i++) {
        const FrameRec* r = &recs->v[i];
        if (r->gapMs > 0) {
            gap[ngap++] = r->gapMs;
            if (r->period > 0 && r->gapMs > 1.5 * r->period * 1e-6) longGaps++;
        }
        resubmits += (size_t)r->resubmit;
        if (r->shouldRender) {
            if (!firstPdt) firstPdt = r->pdt;
            lastPdt_ = r->pdt;
        }
        if (!r->rendered) continue;
        endms[nr_] = r->endMs;
        cpu[nr_++] = r->cpuMs;
        if (r->gpuMs >= 0) gpu[ng++] = r->gpuMs;
        busyRel += (size_t)r->busyAtRelease;
        busyEnd += (size_t)r->busyAtEnd;
    }
    double a, b, c;
    LOG("SUMMARY %s: %zu frames, %zu rendered, %llu xrEndFrame failures", name, recs->n, nr_,
        (unsigned long long)endFailures);
    double span = lastPdt_ > firstPdt ? (lastPdt_ - firstPdt) * 1e-9 : 0;
    LOG("SUMMARY %s new content frames %zu (%.1f a second of display time), slots that submitted the last content again %zu",
        name, nr_, span > 0 ? nr_ / span : 0.0, resubmits);
    percentiles(gpu, ng, &a, &b, &c);
    LOG("SUMMARY %s gpu ms p50 %.3f p95 %.3f max %.3f (%zu samples)", name, a, b, c, ng);
    percentiles(cpu, nr_, &a, &b, &c);
    LOG("SUMMARY %s cpu submit ms p50 %.3f p95 %.3f max %.3f", name, a, b, c);
    percentiles(endms, nr_, &a, &b, &c);
    LOG("SUMMARY %s xrEndFrame ms p50 %.3f p95 %.3f max %.3f", name, a, b, c);
    percentiles(gap, ngap, &a, &b, &c);
    LOG("SUMMARY %s xrWaitFrame gap ms p50 %.3f p95 %.3f max %.3f; %zu gaps > 1.5 periods", name, a, b, c, longGaps);
    LOG("SUMMARY %s GPU still busy at release: %zu of %zu rendered frames; at xrEndFrame return: %zu", name, busyRel,
        nr_, busyEnd);
    // The cadence the frames went out at: display time steps between rendered
    // frames, in refreshes (the shortest predicted period of the case).
    {
        XrDuration minPeriod = 0;
        for (size_t i = 0; i < recs->n; i++)
            if (recs->v[i].period > 0 && (!minPeriod || recs->v[i].period < minPeriod)) minPeriod = recs->v[i].period;
        size_t steps[5] = {0};
        XrTime last = 0;
        for (size_t i = 0; i < recs->n && minPeriod > 0; i++) {
            const FrameRec* r = &recs->v[i];
            if (!r->rendered) continue;
            if (last) {
                long k = lround((double)(r->pdt - last) / (double)minPeriod);
                steps[k <= 0 ? 0 : k >= 4 ? 4 : k]++;
            }
            last = r->pdt;
        }
        LOG("SUMMARY %s display time steps between rendered frames (refresh %.3f ms): 1: %zu, 2: %zu, 3: %zu, more: %zu, "
            "none: %zu", name, minPeriod * 1e-6, steps[1], steps[2], steps[3], steps[4], steps[0]);
    }
    free(gpu);
    free(gap);
    free(cpu);
    free(endms);
    recs->n = 0;
}

// ---------------------------------------------------------------- main

int main(int argc, char** argv) {
    g_t0 = nowSec();
    if (parseArgs(argc, argv)) return 2;

    const int seq = g_opt.mode == MODE_SEQUENCE || g_opt.mode == MODE_SEQUENCE_SW;
    const Case single = {g_opt.mode, g_opt.noMv, modeName(), ""};
    const Case* cases = g_opt.mode == MODE_SEQUENCE ? kSequence : g_opt.mode == MODE_SEQUENCE_SW ? kSequenceSw : &single;
    const int nCases = g_opt.mode == MODE_SEQUENCE ? 5 : g_opt.mode == MODE_SEQUENCE_SW ? 2 : 1;
    int wantPace = 0, wantSynth = 0, wantDepthEye = 0, wantSw = 0;
    for (int c = 0; c < nCases; c++) {
        wantPace |= cases[c].mode == MODE_PACE || cases[c].mode == MODE_PACE_FENCE;
        wantSynth |= cases[c].mode == MODE_SYNTH && !cases[c].noMv;
        wantDepthEye |= cases[c].mode == MODE_DEPTH;
        wantSw |= cases[c].mode == MODE_SW;  // the control case too: the same session settings
    }

    char fpsStr[64];
    if (g_opt.fps >= 0) snprintf(fpsStr, sizeof(fpsStr), "%.1f", g_opt.fps);
    else snprintf(fpsStr, sizeof(fpsStr), "auto (30 synth, else display; SpaceWarp: every 2nd slot)");
    char cfg[512];
    snprintf(cfg, sizeof(cfg),
             "mode %s, %d case(s) of %.1f s, hz %s%.1f, fps cap %s, gpu target %.1f ms, mv %s, log %s",
             modeName(), nCases, g_opt.seconds, g_opt.hz > 0 ? "" : "(unchanged) ", g_opt.hz, fpsStr, g_opt.gpuMs,
             g_opt.mvHead ? "object+head" : "object only", g_opt.log);
    if (g_opt.dryRun) {
        printf("xrprobe dry run: %s\n", cfg);
        return 0;
    }

    g_log = fopen(g_opt.log, "w");
    if (!g_log) {
        fprintf(stderr, "xrprobe: cannot write log %s: %s\n", g_opt.log, strerror(errno));
        return 1;
    }
    setvbuf(g_log, NULL, _IOLBF, 0);
    LOG("xrprobe: %s", cfg);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = onSignal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sa.sa_handler = onAlarm;
    sigaction(SIGALRM, &sa, NULL);
    // Hard stop if the clean teardown never comes (xrWaitFrame can block for
    // good while the headset sleeps). The frame loop's own wall-clock cap is
    // 240 s past the case time; this is 60 s later still.
    alarm((unsigned)(nCases * g_opt.seconds) + 300);
    if (seq)
        for (int c = 0; c < nCases; c++) LOG("case %d: %s, label \"%s\"", c + 1, cases[c].name, cases[c].label);

    // ---- instance
    uint32_t n = 0;
    XrResult er = xrEnumerateInstanceExtensionProperties(NULL, 0, &n, NULL);
    if (XR_FAILED(er)) {
        LOG("FAIL no OpenXR runtime reachable (XrResult %d). Is SteamVR running, and does "
            "~/.config/openxr/1/active_runtime.json point at it?", (int)er);
        return 1;
    }
    XrExtensionProperties* props = calloc(n, sizeof(*props));
    for (uint32_t i = 0; i < n; i++) props[i].type = XR_TYPE_EXTENSION_PROPERTIES;
    XR_CHECK(xrEnumerateInstanceExtensionProperties(NULL, n, &n, props));
    for (uint32_t i = 0; i < n; i++) {
        FLOG("runtime extension %s v%u", props[i].extensionName, props[i].extensionVersion);
        ExtWant* w = ext(props[i].extensionName);
        if (w) w->present = 1;
    }
    free(props);
    {
        uint32_t nl = 0;
        xrEnumerateApiLayerProperties(0, &nl, NULL);
        XrApiLayerProperties* lp = calloc(nl ? nl : 1, sizeof(*lp));
        for (uint32_t i = 0; i < nl; i++) lp[i].type = XR_TYPE_API_LAYER_PROPERTIES;
        if (nl && XR_SUCCEEDED(xrEnumerateApiLayerProperties(nl, &nl, lp)))
            for (uint32_t i = 0; i < nl; i++) LOG("api layer available: %s (%s)", lp[i].layerName, lp[i].description);
        free(lp);
    }
    for (size_t k = 0; k < N_EXTS; k++) LOG("extension %-34s %s", g_exts[k].name, g_exts[k].present ? "present" : "ABSENT");

    if (!hasExt("XR_KHR_vulkan_enable2")) {
        LOG("FAIL runtime lacks XR_KHR_vulkan_enable2");
        return 1;
    }
    if (wantSynth) {
        if (!hasExt("XR_EXT_frame_synthesis")) {
            LOG("FAIL the synth case needs XR_EXT_frame_synthesis, which the runtime does not offer");
            return 1;
        }
        ext("XR_EXT_frame_synthesis")->enable = 1;
    }
    if (wantSw) {
        if (!hasExt("XR_FB_space_warp")) {
            LOG("FAIL the SpaceWarp cases need XR_FB_space_warp, which the runtime does not offer");
            return 1;
        }
        ext("XR_FB_space_warp")->enable = 1;
    }
    if (wantDepthEye) {
        if (!hasExt("XR_KHR_composition_layer_depth")) {
            LOG("FAIL the depth-only case needs XR_KHR_composition_layer_depth, which the runtime does not offer");
            return 1;
        }
        ext("XR_KHR_composition_layer_depth")->enable = 1;
    }

    const char* enable[N_EXTS];
    uint32_t nEnable = 0;
    for (size_t k = 0; k < N_EXTS; k++)
        if (g_exts[k].present && g_exts[k].enable) {
            enable[nEnable++] = g_exts[k].name;
            LOG("enabling %s", g_exts[k].name);
        }

    XrInstanceCreateInfo ici = {XR_TYPE_INSTANCE_CREATE_INFO};
    snprintf(ici.applicationInfo.applicationName, sizeof(ici.applicationInfo.applicationName), "xrprobe %s",
             modeName());
    ici.applicationInfo.applicationVersion = 1;
    snprintf(ici.applicationInfo.engineName, sizeof(ici.applicationInfo.engineName), "xrprobe");
    ici.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    ici.enabledExtensionCount = nEnable;
    ici.enabledExtensionNames = enable;
    XrResult cr = xrCreateInstance(&ici, &g_instance);
    if (XR_FAILED(cr)) {
        LOG("FAIL xrCreateInstance -> %s. SteamVR is probably not running (start it, then retry).", xrStr(cr));
        return 1;
    }

    XrInstanceProperties ip = {XR_TYPE_INSTANCE_PROPERTIES};
    XR_CHECK(xrGetInstanceProperties(g_instance, &ip));
    LOG("runtime: %s %u.%u.%u", ip.runtimeName, XR_VERSION_MAJOR(ip.runtimeVersion),
        XR_VERSION_MINOR(ip.runtimeVersion), XR_VERSION_PATCH(ip.runtimeVersion));

    // ---- system
    XrSystemGetInfo sgi = {XR_TYPE_SYSTEM_GET_INFO};
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrSystemId system;
    XrResult sr = xrGetSystem(g_instance, &sgi, &system);
    if (XR_FAILED(sr)) {
        LOG("FAIL xrGetSystem -> %s (SteamVR running but no headset ready?)", xrStr(sr));
        xrDestroyInstance(g_instance);
        return 1;
    }
    XrSystemProperties sp = {XR_TYPE_SYSTEM_PROPERTIES};
    XrSystemSpaceWarpPropertiesFB swp = {XR_TYPE_SYSTEM_SPACE_WARP_PROPERTIES_FB};
    if (wantSw) sp.next = &swp;
    XR_CHECK(xrGetSystemProperties(g_instance, system, &sp));
    if (wantSw)
        LOG("space warp: recommended motion vector / depth rect %ux%u", swp.recommendedMotionVectorImageRectWidth,
            swp.recommendedMotionVectorImageRectHeight);
    LOG("system: %s, max swapchain %ux%u, max layers %u", sp.systemName, sp.graphicsProperties.maxSwapchainImageWidth,
        sp.graphicsProperties.maxSwapchainImageHeight, sp.graphicsProperties.maxLayerCount);

    XrViewConfigurationView vcv[2] = {{XR_TYPE_VIEW_CONFIGURATION_VIEW}, {XR_TYPE_VIEW_CONFIGURATION_VIEW}};
    XrFrameSynthesisConfigViewEXT fsv[2] = {{XR_TYPE_FRAME_SYNTHESIS_CONFIG_VIEW_EXT},
                                            {XR_TYPE_FRAME_SYNTHESIS_CONFIG_VIEW_EXT}};
    if (wantSynth) {
        vcv[0].next = &fsv[0];
        vcv[1].next = &fsv[1];
    }
    uint32_t nvv = 0;
    XR_CHECK(xrEnumerateViewConfigurationViews(g_instance, system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &nvv,
                                               vcv));
    LOG("stereo view: recommended %ux%u (max %ux%u)", vcv[0].recommendedImageRectWidth,
        vcv[0].recommendedImageRectHeight, vcv[0].maxImageRectWidth, vcv[0].maxImageRectHeight);
    if (wantSynth)
        LOG("frame synthesis: recommended motion vector / depth rect %ux%u (left), %ux%u (right)",
            fsv[0].recommendedMotionVectorImageRectWidth, fsv[0].recommendedMotionVectorImageRectHeight,
            fsv[1].recommendedMotionVectorImageRectWidth, fsv[1].recommendedMotionVectorImageRectHeight);

    // ---- Vulkan + session
    Vk vk;
    createVulkan(system, &vk);
    XrGraphicsBindingVulkan2KHR gb = {XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR};
    gb.instance = vk.instance;
    gb.physicalDevice = vk.phys;
    gb.device = vk.device;
    gb.queueFamilyIndex = vk.queueFamily;
    gb.queueIndex = 0;
    XrSessionCreateInfo sci = {XR_TYPE_SESSION_CREATE_INFO};
    sci.next = &gb;
    sci.systemId = system;
    XrSession session;
    XR_CHECK(xrCreateSession(g_instance, &sci, &session));
    LOG("session created");

    int64_t formats[64];
    uint32_t nf = 0;
    XR_CHECK(xrEnumerateSwapchainFormats(session, 64, &nf, formats));
    {
        char line[1024] = "";
        size_t off = 0;
        for (uint32_t i = 0; i < nf && off < sizeof(line) - 64; i++)
            off += (size_t)snprintf(line + off, sizeof(line) - off, " %s(%lld)", vkFormatName(formats[i]),
                                    (long long)formats[i]);
        LOG("swapchain formats offered:%s", line);
    }
    // ---- refresh rate
    PFN_xrEnumerateDisplayRefreshRatesFB enumRates = NULL;
    PFN_xrGetDisplayRefreshRateFB getRate = NULL;
    PFN_xrRequestDisplayRefreshRateFB requestRate = NULL;
    float rates[16];
    uint32_t nr = 0;
    float rate = 0;
    if (hasExt("XR_FB_display_refresh_rate")) {
        xrGetInstanceProcAddr(g_instance, "xrEnumerateDisplayRefreshRatesFB", (PFN_xrVoidFunction*)&enumRates);
        xrGetInstanceProcAddr(g_instance, "xrGetDisplayRefreshRateFB", (PFN_xrVoidFunction*)&getRate);
        xrGetInstanceProcAddr(g_instance, "xrRequestDisplayRefreshRateFB", (PFN_xrVoidFunction*)&requestRate);
        if (enumRates) enumRates(session, 16, &nr, rates);
        if (getRate) getRate(session, &rate);
    }
    {
        char line[256] = "";
        size_t off = 0;
        for (uint32_t i = 0; i < nr && off < sizeof(line) - 16; i++)
            off += (size_t)snprintf(line + off, sizeof(line) - off, " %.2f", rates[i]);
        LOG("refresh rates offered:%s; current %.2f Hz", nr ? line : " (none)", rate);
    }
    if (g_opt.hz > 0) {
        if (requestRate) {
            XrResult rr = requestRate(session, g_opt.hz);
            LOG("requested %.2f Hz -> %s", g_opt.hz, xrStr(rr));
        } else {
            LOG("--hz ignored: XR_FB_display_refresh_rate not available");
        }
    }

    // ---- space and swapchains
    XrReferenceSpaceCreateInfo rsci = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    rsci.poseInReferenceSpace.orientation.w = 1;
    XrSpace local;
    XR_CHECK(xrCreateReferenceSpace(session, &rsci, &local));

    int64_t colorFormat = formats[0];
    for (uint32_t i = 0; i < nf; i++)
        if (formats[i] == VK_FORMAT_R8G8B8A8_SRGB || formats[i] == VK_FORMAT_B8G8R8A8_SRGB) {
            colorFormat = formats[i];
            break;
        }

    Chain eyes[2], mv[2], depthMv[2], depthEye[2];
    memset(mv, 0, sizeof(mv));
    memset(depthMv, 0, sizeof(depthMv));
    memset(depthEye, 0, sizeof(depthEye));
    for (int e = 0; e < 2; e++) {
        char what[32];
        snprintf(what, sizeof(what), "eye%d color", e);
        if (createChain(session, vk.device, what, colorFormat, (int32_t)vcv[e].recommendedImageRectWidth,
                        (int32_t)vcv[e].recommendedImageRectHeight, &eyes[e])) {
            LOG("FAIL eye swapchain refused");
            die();
        }
    }
    // All swapchains are made up front, so a --sequence session never creates
    // one mid-run.
    int64_t depthFormat = VK_FORMAT_UNDEFINED;
    if (wantSynth || wantSw) {
        int32_t dw[2], dh[2];
        for (int e = 0; e < 2; e++) {
            uint32_t rw = wantSw ? swp.recommendedMotionVectorImageRectWidth : fsv[e].recommendedMotionVectorImageRectWidth;
            uint32_t rh = wantSw ? swp.recommendedMotionVectorImageRectHeight : fsv[e].recommendedMotionVectorImageRectHeight;
            if (rw && rh) {
                dw[e] = (int32_t)rw;
                dh[e] = (int32_t)rh;
            } else {
                dw[e] = eyes[e].width / 2;
                dh[e] = eyes[e].height / 2;
                LOG("no recommended motion vector size from the runtime; using half the eye size %dx%d", dw[e], dh[e]);
            }
            char what[32];
            snprintf(what, sizeof(what), "eye%d motion", e);
            if (createChain(session, vk.device, what, VK_FORMAT_R16G16B16A16_SFLOAT, dw[e], dh[e], &mv[e])) {
                LOG("FAIL motion vector swapchain (R16G16B16A16_SFLOAT) refused");
                die();
            }
        }
        if (createDepthPair(session, vk.device, "synth depth", dw, dh, formats, nf, &depthFormat, depthMv)) {
            LOG("FAIL no depth swapchain format accepted (tried D32_SFLOAT, D24_UNORM_S8_UINT)");
            die();
        }
    }
    if (wantDepthEye) {
        int32_t dw[2] = {eyes[0].width, eyes[1].width}, dh[2] = {eyes[0].height, eyes[1].height};
        if (createDepthPair(session, vk.device, "layer depth", dw, dh, formats, nf, &depthFormat, depthEye)) {
            LOG("FAIL no depth swapchain format accepted (tried D32_SFLOAT, D24_UNORM_S8_UINT)");
            die();
        }
    }
    if (depthFormat != VK_FORMAT_UNDEFINED) LOG("depth format in use: %s", vkFormatName(depthFormat));

    // ---- pipelines, command buffers, queries
    VkPushConstantRange pcr = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Push)};
    VkPipelineLayoutCreateInfo lci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    lci.pushConstantRangeCount = 1;
    lci.pPushConstantRanges = &pcr;
    VkPipelineLayout layout;
    VK_CHECK(vkCreatePipelineLayout(vk.device, &lci, NULL, &layout));
    VkPipeline scenePipe = makePipeline(vk.device, layout, kQuadVert, sizeof(kQuadVert), kSceneFrag, sizeof(kSceneFrag),
                                        (VkFormat)colorFormat, VK_FORMAT_UNDEFINED);
    VkPipeline sceneDepthPipe = VK_NULL_HANDLE, heavyPipe = VK_NULL_HANDLE, mvPipe = VK_NULL_HANDLE;
    if (wantDepthEye)
        sceneDepthPipe = makePipeline(vk.device, layout, kQuadVert, sizeof(kQuadVert), kSceneFrag, sizeof(kSceneFrag),
                                      (VkFormat)colorFormat, (VkFormat)depthFormat);
    if (wantPace)
        heavyPipe = makePipeline(vk.device, layout, kFullVert, sizeof(kFullVert), kHeavyFrag, sizeof(kHeavyFrag),
                                 (VkFormat)colorFormat, VK_FORMAT_UNDEFINED);
    if (wantSynth || wantSw)
        mvPipe = makePipeline(vk.device, layout, kQuadVert, sizeof(kQuadVert), kMvFrag, sizeof(kMvFrag),
                              VK_FORMAT_R16G16B16A16_SFLOAT, (VkFormat)depthFormat);

    VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = vk.queueFamily;
    VkCommandPool pool;
    VK_CHECK(vkCreateCommandPool(vk.device, &pci, NULL, &pool));
    VkCommandBuffer cmds[NSLOTS];
    VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = NSLOTS;
    VK_CHECK(vkAllocateCommandBuffers(vk.device, &cai, cmds));
    VkFence fences[NSLOTS];
    for (int i = 0; i < NSLOTS; i++) {
        VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        VK_CHECK(vkCreateFence(vk.device, &fci, NULL, &fences[i]));
    }
    VkQueryPoolCreateInfo qpci = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    qpci.queryType = VK_QUERY_TYPE_TIMESTAMP;
    qpci.queryCount = 2 * NSLOTS;
    VkQueryPool queries;
    VK_CHECK(vkCreateQueryPool(vk.device, &qpci, NULL, &queries));
    int haveTimestamps = vk.timestampBits > 0;
    if (!haveTimestamps) LOG("no timestamp support on the queue: GPU times will be '-'");

    uint32_t iterations = 0;
    if (wantPace) {
        Gpu g = {&vk, layout, heavyPipe, queries, pool};
        if (haveTimestamps) {
            iterations = calibrate(&g, (VkFormat)colorFormat, eyes[0].width, eyes[0].height, g_opt.gpuMs);
        } else {
            iterations = 2000;
            LOG("no timestamps: using an uncalibrated %u iterations", iterations);
        }
    }

    // ---- frame loop
    FrameRec slotRec[NSLOTS];
    int slotPending[NSLOTS] = {0};
    RecList recs = {0};
    int running = 1, sessionRunning = 0, exitRequested = 0;
    XrSessionState sessionState = XR_SESSION_STATE_UNKNOWN;
    uint64_t frameIdx = 0, drawn = 0;
    double started = nowSec(), windowStart = started, lastWaitReturn = 0, lastPeriodSec = 1.0 / 90.0;
    uint64_t winFrames = 0, winRendered = 0, endFailures = 0;
    XrTime prevDisplayTime = 0;
    // The SpaceWarp cases: new content on every second display slot, the
    // slots between submit the last content again (same images and poses,
    // no SpaceWarp info).  Slots are counted from predictedDisplayTime.
    XrTime lastNewPdt = 0;
    XrCompositionLayerProjectionView pvLast[2];
    uint64_t content = 0;  // new content frames of the case (the counter strip)
    XrView prevViews[2];
    int havePrev = 0;
    // adaptive load: keep the measured GPU time near the target during the run
    double adaptSum = 0;
    int adaptCount = 0;
    // cases: time counts only while the session is VISIBLE or FOCUSED and the
    // runtime asks for rendering; a wall-clock cap ends a session that never
    // gets there
    int caseIdx = 0, caseStarted = 0;
    double caseTime = 0;
    const double wallCap = nCases * g_opt.seconds + 240.0;

    while (running) {
        double t = nowSec();
        int timeUp = caseIdx >= nCases || t - started > wallCap;
        if ((g_quit || timeUp) && !exitRequested) {
            const char* why = g_quit ? "signal" : caseIdx >= nCases ? "all cases done" : "wall-clock cap";
            if (sessionRunning) {
                LOG("requesting exit (%s)", why);
                xrRequestExitSession(session);
                exitRequested = 1;
            } else {
                LOG("stopping before the session started (%s)", why);
                break;
            }
        }

        XrEventDataBuffer ev = {XR_TYPE_EVENT_DATA_BUFFER};
        while (xrPollEvent(g_instance, &ev) == XR_SUCCESS) {
            switch (ev.type) {
            case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
                XrEventDataSessionStateChanged* s = (XrEventDataSessionStateChanged*)&ev;
                sessionState = s->state;
                LOG("session state %d", (int)s->state);
                if (s->state == XR_SESSION_STATE_READY) {
                    XrSessionBeginInfo bi = {XR_TYPE_SESSION_BEGIN_INFO};
                    bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                    XR_CHECK(xrBeginSession(session, &bi));
                    sessionRunning = 1;
                    started = nowSec(); // the wall-clock cap counts from the session start
                    windowStart = started;
                    if (getRate) {
                        getRate(session, &rate);
                        LOG("refresh rate at begin: %.2f Hz", rate);
                    }
                } else if (s->state == XR_SESSION_STATE_STOPPING) {
                    xrEndSession(session);
                    sessionRunning = 0;
                    running = 0;
                } else if (s->state == XR_SESSION_STATE_EXITING || s->state == XR_SESSION_STATE_LOSS_PENDING) {
                    running = 0;
                }
                break;
            }
            case XR_TYPE_EVENT_DATA_DISPLAY_REFRESH_RATE_CHANGED_FB: {
                XrEventDataDisplayRefreshRateChangedFB* r = (XrEventDataDisplayRefreshRateChangedFB*)&ev;
                LOG("display refresh rate now %.2f Hz (was %.2f)", r->toDisplayRefreshRate, r->fromDisplayRefreshRate);
                rate = r->toDisplayRefreshRate;
                break;
            }
            case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
                LOG("instance loss pending");
                running = 0;
                break;
            default:
                break;
            }
            ev = (XrEventDataBuffer){XR_TYPE_EVENT_DATA_BUFFER};
        }
        if (!running) break;
        if (!sessionRunning) {
            struct timespec ts = {0, 10 * 1000 * 1000};
            nanosleep(&ts, NULL);
            continue;
        }

        const Case* cs = &cases[caseIdx < nCases ? caseIdx : nCases - 1];
        const int pace = cs->mode == MODE_PACE || cs->mode == MODE_PACE_FENCE;
        const int sw = cs->mode == MODE_SW && !cs->noMv;
        const int synthExt = cs->mode == MODE_SYNTH && !cs->noMv;
        const int synth = synthExt || sw;  // a motion vector and depth pass
        const int depthOnly = cs->mode == MODE_DEPTH;
        const double fps = g_opt.fps >= 0 ? g_opt.fps : (cs->mode == MODE_SYNTH ? 30.0 : cs->mode == MODE_SW ? 60.0 : 0.0);
        if (!caseStarted && caseIdx < nCases) {
            caseStarted = 1;
            caseTime = 0;
            LOG("CASE %d start (%s, %s)", caseIdx + 1, cs->name, seq ? cs->label : "single");
            FLOG("=== CASE %d %s", caseIdx + 1, cs->name);
        }

        FrameRec rec;
        memset(&rec, 0, sizeof(rec));
        rec.gpuMs = -1;
        rec.n = frameIdx++;
        rec.iterations = iterations;

        // --fps (the synth cases): start the next frame no earlier than
        // 1/fps after the last xrWaitFrame return, less half a refresh.  The
        // SpaceWarp cases do not sleep: they pace by display slots (below).
        if (fps > 0 && cs->mode != MODE_SW && lastWaitReturn > 0) {
            double target = lastWaitReturn + 1.0 / fps - 0.5 * lastPeriodSec;
            double now = nowSec();
            if (target > now) {
                double s_ = target - now;
                struct timespec ts = {(time_t)s_, (long)((s_ - (time_t)s_) * 1e9)};
                nanosleep(&ts, NULL);
                rec.sleepMs = s_ * 1e3;
            }
        }

        XrFrameState fs = {XR_TYPE_FRAME_STATE};
        double w0 = nowSec();
        XR_CHECK(xrWaitFrame(session, NULL, &fs));
        double w1 = nowSec();
        rec.waitMs = (w1 - w0) * 1e3;

        rec.gapMs = lastWaitReturn > 0 ? (w1 - lastWaitReturn) * 1e3 : 0;
        if (lastWaitReturn > 0 && fs.shouldRender &&
            (sessionState == XR_SESSION_STATE_VISIBLE || sessionState == XR_SESSION_STATE_FOCUSED)) {
            double dt = w1 - lastWaitReturn;
            caseTime += dt < 0.1 ? dt : 0.1;
        }
        lastWaitReturn = w1;
        rec.pdt = fs.predictedDisplayTime;
        rec.period = fs.predictedDisplayPeriod;
        if (fs.predictedDisplayPeriod > 0) lastPeriodSec = fs.predictedDisplayPeriod * 1e-9;
        rec.shouldRender = fs.shouldRender;
        rec.beginRes = xrBeginFrame(session, NULL);
        if (XR_FAILED(rec.beginRes)) {
            LOG("FAIL xrBeginFrame -> %s", xrStr(rec.beginRes));
            die();
        }
        winFrames++;

        XrCompositionLayerProjectionView pv[2];
        XrFrameSynthesisInfoEXT fsi[2];
        XrCompositionLayerSpaceWarpInfoFB swi[2];
        XrCompositionLayerDepthInfoKHR di[2];
        XrCompositionLayerProjection proj = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        const XrCompositionLayerBaseHeader* layers[1];
        uint32_t layerCount = 0;
        int slot = -1;

        // SpaceWarp cases: new content only on every second display slot,
        // counted from predictedDisplayTime against the display's own
        // refresh; the slots between submit the last content again.
        int resubmit = 0;
        if (fs.shouldRender && cs->mode == MODE_SW && lastNewPdt) {
            if (getRate) getRate(session, &rate);
            XrDuration r = rate > 0 ? (XrDuration)(1e9 / rate) : fs.predictedDisplayPeriod;
            resubmit = fs.predictedDisplayTime < lastNewPdt + 2 * r - r / 2;
        }
        if (fs.shouldRender && resubmit) {
            pv[0] = pvLast[0];
            pv[1] = pvLast[1];
            proj.space = local;
            proj.viewCount = 2;
            proj.views = pv;
            layers[layerCount++] = (const XrCompositionLayerBaseHeader*)&proj;
            rec.resubmit = 1;
        } else if (fs.shouldRender) {
            XrViewLocateInfo vli = {XR_TYPE_VIEW_LOCATE_INFO};
            vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            vli.displayTime = fs.predictedDisplayTime;
            vli.space = local;
            XrViewState vs = {XR_TYPE_VIEW_STATE};
            XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
            uint32_t nViews = 0;
            XR_CHECK(xrLocateViews(session, &vli, &vs, 2, &nViews, views));
            int posesValid = (vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) &&
                             (vs.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT);

            for (int e = 0; e < 2; e++) {
                acquireChain(&eyes[e]);
                if (synth) {
                    acquireChain(&mv[e]);
                    acquireChain(&depthMv[e]);
                }
                if (depthOnly) acquireChain(&depthEye[e]);
            }

            // reuse the oldest slot: its fence also gives us its GPU time
            slot = (int)(rec.n % NSLOTS);
            VK_CHECK(vkWaitForFences(vk.device, 1, &fences[slot], VK_TRUE, UINT64_MAX));
            if (slotPending[slot]) {
                FrameRec* old = &slotRec[slot];
                if (haveTimestamps) {
                    uint64_t ts[2];
                    if (vkGetQueryPoolResults(vk.device, queries, (uint32_t)slot * 2, 2, sizeof(ts), ts,
                                              sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
                        old->gpuMs = timestampMs(&vk, ts);
                }
                logFrame(old);
                recPush(&recs, old);
                slotPending[slot] = 0;
                if (pace && old->gpuMs > 0) {
                    adaptSum += old->gpuMs;
                    if (++adaptCount == 30) {
                        double avg = adaptSum / adaptCount;
                        double scale = g_opt.gpuMs / avg;
                        if (fabs(scale - 1.0) > 0.05) {
                            if (scale > 1.5) scale = 1.5;
                            if (scale < 0.67) scale = 0.67;
                            uint32_t next = (uint32_t)(iterations * scale + 0.5);
                            if (next < 1) next = 1;
                            LOG("load: mean GPU %.2f ms over 30 frames, iterations %u -> %u", avg, iterations, next);
                            iterations = next;
                        }
                        adaptSum = 0;
                        adaptCount = 0;
                    }
                }
            }
            VK_CHECK(vkResetFences(vk.device, 1, &fences[slot]));
            rec.iterations = iterations;

            double c0 = nowSec();
            VkCommandBuffer cmd = cmds[slot];
            VK_CHECK(vkResetCommandBuffer(cmd, 0));
            VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            VK_CHECK(vkBeginCommandBuffer(cmd, &bi));
            if (haveTimestamps) {
                vkCmdResetQueryPool(cmd, queries, (uint32_t)slot * 2, 2);
                vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, queries, (uint32_t)slot * 2);
            }

            double tDisp = (double)fs.predictedDisplayTime * 1e-9;
            double tPrev = prevDisplayTime ? (double)prevDisplayTime * 1e-9 : tDisp;
            Mat4 modelCur = checkerModel(tDisp);
            Mat4 modelPrev = checkerModel(tPrev);
            Mat4 panel = placeQuad(0.0f, 0.0f, -1.5f, 0.9f, 0.5f);
            float hue = (float)fmod((double)drawn / 90.0, 1.0);
            VkPipeline colorPipe = depthOnly ? sceneDepthPipe : scenePipe;

            for (int e = 0; e < 2; e++) {
                Chain* eye = &eyes[e];
                VkImage img = eye->images[eye->acquired].image;
                Mat4 proj_ = projectionFov(views[e].fov, NEAR_Z, FAR_Z);
                Mat4 view_ = viewFromPose(views[e].pose);
                Mat4 vp_ = matMul(&proj_, &view_);

                // motion vectors + depth for frame synthesis (own pass at the MV size)
                if (synth) {
                    Chain* m = &mv[e];
                    Chain* d = &depthMv[e];
                    VkImage mimg = m->images[m->acquired].image, dimg = d->images[d->acquired].image;
                    imageBarrier(cmd, mimg, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                                 VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                                 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
                    imageBarrier(cmd, dimg, aspectOf(depthFormat), VK_IMAGE_LAYOUT_UNDEFINED,
                                 VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, 0,
                                 VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                                 VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                                 VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT);
                    VkRenderingAttachmentInfoKHR ca = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR};
                    ca.imageView = m->views[m->acquired];
                    ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                    ca.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
                    ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                    ca.clearValue.color = (VkClearColorValue){{0, 0, 0, 0}};
                    VkRenderingAttachmentInfoKHR da = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR};
                    da.imageView = d->views[d->acquired];
                    da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
                    da.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
                    da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                    da.clearValue.depthStencil = (VkClearDepthStencilValue){1.0f, 0};
                    VkRenderingInfoKHR ri = {VK_STRUCTURE_TYPE_RENDERING_INFO_KHR};
                    ri.renderArea.extent = (VkExtent2D){(uint32_t)m->width, (uint32_t)m->height};
                    ri.layerCount = 1;
                    ri.colorAttachmentCount = 1;
                    ri.pColorAttachments = &ca;
                    ri.pDepthAttachment = &da;
                    pfnBeginRendering(cmd, &ri);
                    if (posesValid) {
                        VkViewport vp = {0, 0, (float)m->width, (float)m->height, 0, 1};
                        VkRect2D sc = {{0, 0}, {(uint32_t)m->width, (uint32_t)m->height}};
                        vkCmdSetViewport(cmd, 0, 1, &vp);
                        vkCmdSetScissor(cmd, 0, 1, &sc);
                        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mvPipe);
                        Mat4 vpPrev = vp_;
                        if (g_opt.mvHead && havePrev) {
                            Mat4 pp = projectionFov(prevViews[e].fov, NEAR_Z, FAR_Z);
                            Mat4 pv_ = viewFromPose(prevViews[e].pose);
                            vpPrev = matMul(&pp, &pv_);
                        }
                        Push p;
                        p.mvp = matMul(&vp_, &modelCur);
                        p.aux = matMul(&vpPrev, &modelPrev);
                        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                           sizeof(p), &p);
                        vkCmdDraw(cmd, 6, 1, 0, 0);
                        if (seq) drawLabel(cmd, layout, mvPipe, &vp_, cs->label, 1);
                        if (cs->mode == MODE_SW) drawCounter(cmd, layout, mvPipe, &vp_, content, 1);
                    }
                    pfnEndRendering(cmd);
                }

                // colour pass (with the eye-sized depth attachment for --depth-only)
                imageBarrier(cmd, img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                             VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                             VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
                VkRenderingAttachmentInfoKHR att = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR};
                att.imageView = eye->views[eye->acquired];
                att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
                att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                att.clearValue.color = (VkClearColorValue){{0.02f, 0.03f, 0.05f, 1.0f}};
                VkRenderingAttachmentInfoKHR datt = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR};
                VkRenderingInfoKHR ri = {VK_STRUCTURE_TYPE_RENDERING_INFO_KHR};
                ri.renderArea.extent = (VkExtent2D){(uint32_t)eye->width, (uint32_t)eye->height};
                ri.layerCount = 1;
                ri.colorAttachmentCount = 1;
                ri.pColorAttachments = &att;
                if (depthOnly) {
                    Chain* d = &depthEye[e];
                    imageBarrier(cmd, d->images[d->acquired].image, aspectOf(depthFormat), VK_IMAGE_LAYOUT_UNDEFINED,
                                 VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, 0,
                                 VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                                 VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                                 VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT);
                    datt.imageView = d->views[d->acquired];
                    datt.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
                    datt.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
                    datt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                    datt.clearValue.depthStencil = (VkClearDepthStencilValue){1.0f, 0};
                    ri.pDepthAttachment = &datt;
                }
                pfnBeginRendering(cmd, &ri);
                if (pace) drawHeavy(cmd, layout, heavyPipe, eye->width, eye->height, iterations, hue);
                if (posesValid) {
                    VkViewport vp = {0, 0, (float)eye->width, (float)eye->height, 0, 1};
                    VkRect2D sc = {{0, 0}, {(uint32_t)eye->width, (uint32_t)eye->height}};
                    vkCmdSetViewport(cmd, 0, 1, &vp);
                    vkCmdSetScissor(cmd, 0, 1, &sc);
                    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, colorPipe);
                    Push p;
                    memset(&p, 0, sizeof(p));
                    if (pace) {
                        p.mvp = matMul(&vp_, &panel);
                        p.aux.m[0] = 1.0f;                // mode: frame-number panel
                        p.aux.m[1] = (float)(drawn % 1000000);
                        p.aux.m[2] = hue;
                    } else {
                        p.mvp = matMul(&vp_, &modelCur);
                        p.aux.m[0] = 0.0f;                // mode: checkerboard
                    }
                    vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                       sizeof(p), &p);
                    vkCmdDraw(cmd, 6, 1, 0, 0);
                    if (seq) drawLabel(cmd, layout, colorPipe, &vp_, cs->label, 0);
                    if (cs->mode == MODE_SW) drawCounter(cmd, layout, colorPipe, &vp_, content, 0);
                }
                pfnEndRendering(cmd);

                pv[e] = (XrCompositionLayerProjectionView){XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
                pv[e].pose = views[e].pose;
                pv[e].fov = views[e].fov;
                pv[e].subImage.swapchain = eye->handle;
                pv[e].subImage.imageRect.extent.width = eye->width;
                pv[e].subImage.imageRect.extent.height = eye->height;
                if (sw) {
                    // XR_FB_space_warp: motion vectors as (current - previous)
                    // NDC of the content (object motion only: the runtime
                    // takes the head's motion from the poses), depth of the
                    // same pass; LOCAL never moves here, so no app space delta.
                    swi[e] = (XrCompositionLayerSpaceWarpInfoFB){XR_TYPE_COMPOSITION_LAYER_SPACE_WARP_INFO_FB};
                    swi[e].layerFlags = 0;
                    swi[e].motionVectorSubImage.swapchain = mv[e].handle;
                    swi[e].motionVectorSubImage.imageRect.extent.width = mv[e].width;
                    swi[e].motionVectorSubImage.imageRect.extent.height = mv[e].height;
                    swi[e].appSpaceDeltaPose.orientation.w = 1;
                    swi[e].depthSubImage.swapchain = depthMv[e].handle;
                    swi[e].depthSubImage.imageRect.extent.width = depthMv[e].width;
                    swi[e].depthSubImage.imageRect.extent.height = depthMv[e].height;
                    swi[e].minDepth = 0.0f;
                    swi[e].maxDepth = 1.0f;
                    swi[e].nearZ = NEAR_Z;
                    swi[e].farZ = FAR_Z;
                    pv[e].next = &swi[e];
                } else if (synth) {
                    fsi[e] = (XrFrameSynthesisInfoEXT){XR_TYPE_FRAME_SYNTHESIS_INFO_EXT};
                    fsi[e].layerFlags = XR_FRAME_SYNTHESIS_INFO_REQUEST_RELAXED_FRAME_INTERVAL_BIT_EXT;
                    fsi[e].motionVectorSubImage.swapchain = mv[e].handle;
                    fsi[e].motionVectorSubImage.imageRect.extent.width = mv[e].width;
                    fsi[e].motionVectorSubImage.imageRect.extent.height = mv[e].height;
                    fsi[e].motionVectorScale = (XrVector4f){1, 1, 1, 1};
                    fsi[e].motionVectorOffset = (XrVector4f){0, 0, 0, 0};
                    fsi[e].appSpaceDeltaPose.orientation.w = 1; // LOCAL space never moves here
                    fsi[e].depthSubImage.swapchain = depthMv[e].handle;
                    fsi[e].depthSubImage.imageRect.extent.width = depthMv[e].width;
                    fsi[e].depthSubImage.imageRect.extent.height = depthMv[e].height;
                    fsi[e].minDepth = 0.0f;
                    fsi[e].maxDepth = 1.0f;
                    fsi[e].nearZ = NEAR_Z;
                    fsi[e].farZ = FAR_Z;
                    pv[e].next = &fsi[e];
                } else if (depthOnly) {
                    di[e] = (XrCompositionLayerDepthInfoKHR){XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR};
                    di[e].subImage.swapchain = depthEye[e].handle;
                    di[e].subImage.imageRect.extent.width = depthEye[e].width;
                    di[e].subImage.imageRect.extent.height = depthEye[e].height;
                    di[e].minDepth = 0.0f;
                    di[e].maxDepth = 1.0f;
                    di[e].nearZ = NEAR_Z;
                    di[e].farZ = FAR_Z;
                    pv[e].next = &di[e];
                }
            }
            if (haveTimestamps)
                vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, queries, (uint32_t)slot * 2 + 1);
            VK_CHECK(vkEndCommandBuffer(cmd));
            VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
            si.commandBufferCount = 1;
            si.pCommandBuffers = &cmd;
            VK_CHECK(vkQueueSubmit(vk.queue, 1, &si, fences[slot]));
            rec.cpuMs = (nowSec() - c0) * 1e3;

            // SteamVR shows a released image at once, finished or not (measured):
            // the SpaceWarp cases release only finished images.
            if (cs->mode == MODE_PACE_FENCE || cs->mode == MODE_SW) {
                double f0 = nowSec();
                VK_CHECK(vkWaitForFences(vk.device, 1, &fences[slot], VK_TRUE, UINT64_MAX));
                rec.fenceWaitMs = (nowSec() - f0) * 1e3;
            }
            rec.busyAtRelease = vkGetFenceStatus(vk.device, fences[slot]) == VK_NOT_READY;
            for (int e = 0; e < 2; e++) {
                releaseChain(&eyes[e]);
                if (synth) {
                    releaseChain(&mv[e]);
                    releaseChain(&depthMv[e]);
                }
                if (depthOnly) releaseChain(&depthEye[e]);
            }
            proj.space = local;
            proj.viewCount = 2;
            proj.views = pv;
            layers[layerCount++] = (const XrCompositionLayerBaseHeader*)&proj;
            rec.rendered = 1;
            rec.drawn = drawn++;
            winRendered++;
            if (cs->mode == MODE_SW) {
                lastNewPdt = fs.predictedDisplayTime;
                for (int e = 0; e < 2; e++) {
                    pvLast[e] = pv[e];
                    pvLast[e].next = NULL;  // the SpaceWarp info goes with new content only
                }
                content++;
            }
            prevDisplayTime = fs.predictedDisplayTime;
            prevViews[0] = views[0];
            prevViews[1] = views[1];
            havePrev = 1;
        }

        XrFrameEndInfo fei = {XR_TYPE_FRAME_END_INFO};
        fei.displayTime = fs.predictedDisplayTime;
        fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        fei.layerCount = layerCount;
        fei.layers = layers;
        double e0 = nowSec();
        rec.endRes = xrEndFrame(session, &fei);
        rec.endMs = (nowSec() - e0) * 1e3;
        if (slot >= 0) rec.busyAtEnd = vkGetFenceStatus(vk.device, fences[slot]) == VK_NOT_READY;
        if (rec.endRes != XR_SUCCESS) {
            endFailures++;
            LOG("xrEndFrame frame %llu -> %s", (unsigned long long)rec.n, xrStr(rec.endRes));
            if (rec.endRes == XR_ERROR_SESSION_LOST || rec.endRes == XR_ERROR_INSTANCE_LOST) {
                LOG("FAIL session lost");
                die();
            }
        }
        if (slot >= 0) {
            slotRec[slot] = rec;
            slotPending[slot] = 1;
        } else {
            logFrame(&rec);
            recPush(&recs, &rec);
        }

        t = nowSec();
        if (t - windowStart >= 5.0) {
            if (getRate) getRate(session, &rate);
            LOG("case %d: %.1f of %.0f s counted; last %.1f s: %llu frames, %llu rendered (%.1f fps), period %.3f ms, "
                "display %.2f Hz, state %d, xrEndFrame failures so far %llu",
                caseIdx + 1, caseTime, g_opt.seconds, t - windowStart, (unsigned long long)winFrames,
                (unsigned long long)winRendered, winRendered / (t - windowStart), lastPeriodSec * 1e3, rate,
                (int)sessionState, (unsigned long long)endFailures);
            windowStart = t;
            winFrames = winRendered = 0;
        }

        // case finished: drain its GPU work so every frame lands in its own case
        if (caseIdx < nCases && caseTime >= g_opt.seconds) {
            VK_CHECK(vkDeviceWaitIdle(vk.device));
            for (int s = 0; s < NSLOTS; s++)
                if (slotPending[s]) {
                    if (haveTimestamps) {
                        uint64_t ts[2];
                        if (vkGetQueryPoolResults(vk.device, queries, (uint32_t)s * 2, 2, sizeof(ts), ts,
                                                  sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
                            slotRec[s].gpuMs = timestampMs(&vk, ts);
                    }
                    logFrame(&slotRec[s]);
                    recPush(&recs, &slotRec[s]);
                    slotPending[s] = 0;
                }
            char name[48];
            snprintf(name, sizeof(name), "case %d %s", caseIdx + 1, cs->name);
            summarize(name, &recs, endFailures);
            LOG("CASE %d end", caseIdx + 1);
            endFailures = 0;
            caseIdx++;
            caseStarted = 0;
            prevDisplayTime = 0;
            havePrev = 0;
            adaptSum = 0;
            adaptCount = 0;
            lastWaitReturn = 0; // no throttle carry-over into the next case
            lastNewPdt = 0;
            content = 0;
        }
    }

    // ---- drain, summarise what is left (an unfinished case), tear down
    vkDeviceWaitIdle(vk.device);
    for (int s = 0; s < NSLOTS; s++)
        if (slotPending[s]) {
            if (haveTimestamps) {
                uint64_t ts[2];
                if (vkGetQueryPoolResults(vk.device, queries, (uint32_t)s * 2, 2, sizeof(ts), ts, sizeof(uint64_t),
                                          VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
                    slotRec[s].gpuMs = timestampMs(&vk, ts);
            }
            logFrame(&slotRec[s]);
            recPush(&recs, &slotRec[s]);
        }
    if (caseIdx < nCases) {
        char name[64];
        snprintf(name, sizeof(name), "case %d %s (UNFINISHED, %.1f s counted)", caseIdx + 1, cases[caseIdx].name,
                 caseTime);
        summarize(name, &recs, endFailures);
    } else if (recs.n) {
        FLOG("%zu frames after the last case (exit handshake) not summarised", recs.n);
    }
    LOG("cases completed: %d of %d", caseIdx, nCases);
    free(recs.v);

    for (int i = 0; i < NSLOTS; i++) vkDestroyFence(vk.device, fences[i], NULL);
    vkDestroyQueryPool(vk.device, queries, NULL);
    vkDestroyCommandPool(vk.device, pool, NULL);
    vkDestroyPipeline(vk.device, scenePipe, NULL);
    if (sceneDepthPipe) vkDestroyPipeline(vk.device, sceneDepthPipe, NULL);
    if (heavyPipe) vkDestroyPipeline(vk.device, heavyPipe, NULL);
    if (mvPipe) vkDestroyPipeline(vk.device, mvPipe, NULL);
    vkDestroyPipelineLayout(vk.device, layout, NULL);
    for (int e = 0; e < 2; e++) {
        destroyChain(vk.device, &eyes[e]);
        destroyChain(vk.device, &mv[e]);
        destroyChain(vk.device, &depthMv[e]);
        destroyChain(vk.device, &depthEye[e]);
    }
    xrDestroySpace(local);
    xrDestroySession(session);
    xrDestroyInstance(g_instance);
    vkDestroyDevice(vk.device, NULL);
    vkDestroyInstance(vk.instance, NULL);
    LOG("clean exit");
    fclose(g_log);
    return caseIdx >= nCases ? 0 : 4;
}
