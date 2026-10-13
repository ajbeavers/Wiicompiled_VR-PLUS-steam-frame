// A mock OpenXR runtime for testing the app's frame loop (xr_app.cpp)
// without a headset.  Loaded by the OpenXR loader through XR_RUNTIME_JSON
// (see runtime.json.in), in the app's process.
//
// It keeps the call discipline of a real runtime where the app can get it
// wrong (swapchain acquire / release order, images in flight) and a frame
// clock, and writes a trace of the calls that decide how the app paces:
// swapchain creation, acquire, release, frame begin / end with the layers
// (swapchains, image rects, eye visibility, SpaceWarp info).  The trace is
// deterministic as far as the app is: poses, fovs and times are not in it.
// tools/xrmock/trace-compare.sh runs two builds and diffs their traces.
//
// Environment:
//   MOCK_XR_TRACE=<file>      trace output (none: no trace)
//   MOCK_XR_SECONDS=<s>       run time, then the session stops (20)
//   MOCK_XR_HZ=<hz>           display refresh (120; the app pairs its eyes
//                             over two refreshes at 120 Hz, and renders both
//                             eyes each refresh at other rates)
//   MOCK_XR_SPACEWARP=1       offer XR_FB_space_warp (the app then renders
//                             motion vectors and depth with each frame)
//   MOCK_XR_EXTENSIONS=a,b    replace the list of extensions offered
//   MOCK_XR_SKIP=1            a frame begun late loses refreshes (the display
//                             times jump), as on a headset
//   MOCK_XR_BUSY_MS=<ms>      Vulkan: a release that waits longer than this
//                             for the app's queue to go idle is an error
//                             (off unless set: the app submits its next set
//                             before releasing the last)
// The graphics API is chosen by the app's binding (xrmock_gfx.h).
//
// Checks (ERROR lines in the trace, and a summary on stderr when the session
// ends): acquire / wait / release order per swapchain (one image waited for
// at a time, a release only of a waited image, no more acquired than the
// swapchain has); with Vulkan, that the
// app's GPU work had finished at each release (SteamVR shows a released image
// at once); that both eyes of a projection layer come from one xrLocateViews
// (the views turn with time, so views located for different times differ),
// and for which display time that locate was made.
#define _GNU_SOURCE
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <openxr/openxr.h>
#include <openxr/openxr_loader_negotiation.h>

#include "xrmock_gfx.h"

static FILE* gTrace;
static int gFrame = 0;
static void trace(const char* fmt, ...) {
    if (!gTrace) return;
    va_list ap;
    va_start(ap, fmt);
    fprintf(gTrace, "f%d ", gFrame);
    vfprintf(gTrace, fmt, ap);
    fputc('\n', gTrace);
    va_end(ap);
    fflush(gTrace);
}

static int64_t nowNs(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000ll + ts.tv_nsec;
}

// The graphics tables, tried in order for the session's binding.  Lane D2
// adds the Vulkan one here.
static const MockGfx* const kGfxTables[] = {&kMockGfxGles, &kMockGfxVk};
static const MockGfx* gGfx = NULL;  // the one the session is bound to

#define MAXSC 64
typedef struct {
    int w, h, mips, count;
    int64_t format;
    void* images;
    int acquired, next;
    int waited;  // of the images acquired, how many were waited for
} Chain;
static Chain gChains[MAXSC];
static int gErrors = 0;  // ERROR lines in the trace
static int gChainCount = 0;

static char gExtNames[64][XR_MAX_EXTENSION_NAME_SIZE];
static int gExtCount = 0;

static XrPath gPaths[512];
static char gPathStr[512][128];
static int gPathCount = 0;

static int gEventQueue[16], gEventCount = 0;
static int64_t gT0 = 0, gPeriod = 8333333, gEndAt = 0;
static int gQuitSent = 0;

static void pushState(int s) {
    if (gEventCount < 16) gEventQueue[gEventCount++] = s;
}

static XrResult XRAPI_CALL mEnumExt(const char* layer, uint32_t cap, uint32_t* count, XrExtensionProperties* props) {
    (void)layer;
    *count = (uint32_t)gExtCount;
    if (cap == 0) return XR_SUCCESS;
    if (cap < (uint32_t)gExtCount) return XR_ERROR_SIZE_INSUFFICIENT;
    for (int i = 0; i < gExtCount; i++) {
        memset(props[i].extensionName, 0, sizeof(props[i].extensionName));
        strcpy(props[i].extensionName, gExtNames[i]);
        props[i].extensionVersion = 1;
    }
    return XR_SUCCESS;
}

static XrResult XRAPI_CALL mCreateInstance(const XrInstanceCreateInfo* ci, XrInstance* out) {
    char line[2048] = "";
    for (uint32_t i = 0; i < ci->enabledExtensionCount; i++) {
        strcat(line, " ");
        strcat(line, ci->enabledExtensionNames[i]);
    }
    trace("createInstance extensions:%s", line);
    *out = (XrInstance)(uintptr_t)1;
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL mDestroyInstance(XrInstance i) { (void)i; return XR_SUCCESS; }
static XrResult XRAPI_CALL mResultToString(XrInstance i, XrResult r, char buf[XR_MAX_RESULT_STRING_SIZE]) {
    (void)i;
    snprintf(buf, XR_MAX_RESULT_STRING_SIZE, "XrResult_%d", (int)r);
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL mGetInstanceProps(XrInstance i, XrInstanceProperties* p) {
    (void)i;
    p->runtimeVersion = XR_MAKE_VERSION(0, 1, 0);
    strcpy(p->runtimeName, "xrmock");
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL mGetSystem(XrInstance i, const XrSystemGetInfo* gi, XrSystemId* id) {
    (void)i;
    (void)gi;
    *id = 1;
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL mGetSystemProps(XrInstance i, XrSystemId s, XrSystemProperties* p) {
    (void)i;
    (void)s;
    strcpy(p->systemName, "mock frame");
    for (XrBaseOutStructure* n = (XrBaseOutStructure*)p->next; n; n = n->next) {
        if (n->type == XR_TYPE_SYSTEM_EYE_GAZE_INTERACTION_PROPERTIES_EXT) {
            ((XrSystemEyeGazeInteractionPropertiesEXT*)n)->supportsEyeGazeInteraction = XR_TRUE;
        }
        if (n->type == XR_TYPE_SYSTEM_SPACE_WARP_PROPERTIES_FB) {
            XrSystemSpaceWarpPropertiesFB* w = (XrSystemSpaceWarpPropertiesFB*)n;
            w->recommendedMotionVectorImageRectWidth = 420;
            w->recommendedMotionVectorImageRectHeight = 440;
        }
    }
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL mCreateSession(XrInstance i, const XrSessionCreateInfo* ci, XrSession* out) {
    (void)i;
    for (const XrBaseInStructure* n = (const XrBaseInStructure*)ci->next; n && !gGfx; n = n->next) {
        for (unsigned t = 0; t < sizeof(kGfxTables) / sizeof(kGfxTables[0]); t++) {
            if (kGfxTables[t]->owns_binding(n->type)) {
                char desc[256];
                gGfx = kGfxTables[t];
                gGfx->describe_binding(n, desc, sizeof(desc));
                trace("createSession %s", desc);
                break;
            }
        }
    }
    if (!gGfx) {
        trace("createSession: no graphics binding the mock knows");
        return XR_ERROR_GRAPHICS_DEVICE_INVALID;
    }
    *out = (XrSession)(uintptr_t)2;
    pushState(XR_SESSION_STATE_IDLE);
    pushState(XR_SESSION_STATE_READY);
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL mDestroySession(XrSession s) { (void)s; return XR_SUCCESS; }
static int gSpaces = 0;
static XrResult XRAPI_CALL mCreateRefSpace(XrSession s, const XrReferenceSpaceCreateInfo* ci, XrSpace* out) {
    (void)s;
    (void)ci;
    *out = (XrSpace)(uintptr_t)(100 + gSpaces++);
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL mCreateActionSpace(XrSession s, const XrActionSpaceCreateInfo* ci, XrSpace* out) {
    (void)s;
    (void)ci;
    *out = (XrSpace)(uintptr_t)(100 + gSpaces++);
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL mDestroySpace(XrSpace s) { (void)s; return XR_SUCCESS; }
static XrResult XRAPI_CALL mLocateSpace(XrSpace sp, XrSpace base, XrTime t, XrSpaceLocation* loc) {
    (void)sp;
    (void)base;
    (void)t;
    loc->locationFlags = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT |
                         XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
    loc->pose.orientation = (XrQuaternionf){0, 0, 0, 1};
    loc->pose.position = (XrVector3f){0, 1.6f, 0};
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL mEnumViewConfigViews(XrInstance i, XrSystemId s, XrViewConfigurationType t, uint32_t cap, uint32_t* count,
                                                XrViewConfigurationView* v) {
    (void)i;
    (void)s;
    (void)t;
    *count = 2;
    if (cap == 0) return XR_SUCCESS;
    for (int e = 0; e < 2; e++) {
        v[e].recommendedImageRectWidth = 1680;
        v[e].recommendedImageRectHeight = 1760;
        v[e].maxImageRectWidth = 4096;
        v[e].maxImageRectHeight = 4096;
        v[e].recommendedSwapchainSampleCount = 1;
        v[e].maxSwapchainSampleCount = 1;
    }
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL mEnumFormats(XrSession s, uint32_t cap, uint32_t* count, int64_t* f) {
    (void)s;
    const int64_t* list;
    int n = gGfx->formats(&list);
    *count = (uint32_t)n;
    if (cap == 0) return XR_SUCCESS;
    for (int i = 0; i < n && i < (int)cap; i++) f[i] = list[i];
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL mCreateSwapchain(XrSession s, const XrSwapchainCreateInfo* ci, XrSwapchain* out) {
    (void)s;
    if (gChainCount >= MAXSC) return XR_ERROR_LIMIT_REACHED;
    Chain* c = &gChains[gChainCount];
    c->w = (int)ci->width;
    c->h = (int)ci->height;
    c->mips = (int)ci->mipCount;
    c->format = ci->format;
    c->count = 3;
    c->images = gGfx->create_images(c->format, ci->usageFlags, c->w, c->h, c->mips, c->count);
    trace("createSwapchain sc%d %dx%d mips=%d format=0x%llx usage=0x%llx", gChainCount, c->w, c->h, c->mips, (long long)c->format,
          (unsigned long long)ci->usageFlags);
    if (gGfx == &kMockGfxVk && ci->mipCount > 1) {
        // SteamVR on the Frame allocates a Vulkan swapchain image's memory for
        // level 0 only: levels below it are written past the image.
        trace("ERROR createSwapchain sc%d with %u mip levels: SteamVR allocates one", gChainCount, ci->mipCount);
        gErrors++;
    }
    *out = (XrSwapchain)(uintptr_t)(1000 + gChainCount);
    gChainCount++;
    return XR_SUCCESS;
}
static Chain* chainOf(XrSwapchain sc) { return &gChains[(uintptr_t)sc - 1000]; }
static XrResult XRAPI_CALL mDestroySwapchain(XrSwapchain sc) {
    (void)sc;
    return XR_SUCCESS;
}
static int chainId(XrSwapchain sc) { return (int)((uintptr_t)sc - 1000); }
static XrResult XRAPI_CALL mEnumSwapImages(XrSwapchain sc, uint32_t cap, uint32_t* count, XrSwapchainImageBaseHeader* im) {
    Chain* c = chainOf(sc);
    *count = (uint32_t)c->count;
    if (cap == 0) return XR_SUCCESS;
    return gGfx->fill_images(c->images, c->count < (int)cap ? c->count : (int)cap, im);
}
static XrResult XRAPI_CALL mAcquire(XrSwapchain sc, const XrSwapchainImageAcquireInfo* ai, uint32_t* idx) {
    (void)ai;
    Chain* c = chainOf(sc);
    if (c->acquired >= c->count) {
        trace("ERROR acquire sc%d with all images acquired", chainId(sc));
        return XR_ERROR_CALL_ORDER_INVALID;
    }
    *idx = (uint32_t)c->next;
    c->next = (c->next + 1) % c->count;
    c->acquired++;
    trace("acquire sc%d idx=%u", chainId(sc), *idx);
    return XR_SUCCESS;
}
static int gReleases = 0, gBusyReleases = 0;
static double gBusyMaxMs = 0.0;
static int gWaitHist[5];
static int gPoseAge[6];  // display time minus locate time in refreshes: <0, 0, 1, 2, 3, more
static int gPoseSets = 0, gPoseSameTime = 0, gPoseEarlier = 0, gPoseLater = 0, gPoseSplit = 0, gPoseUnknown = 0;
static void checkPoses(const XrCompositionLayerProjection* p, XrTime displayTime);
static XrResult XRAPI_CALL mWaitImage(XrSwapchain sc, const XrSwapchainImageWaitInfo* wi) {
    (void)wi;
    Chain* c = chainOf(sc);
    if (c->waited >= c->acquired) {
        trace("ERROR wait sc%d without an image acquired", chainId(sc));
        gErrors++;
        return XR_ERROR_CALL_ORDER_INVALID;
    }
    // OpenXR: one image of a swapchain waited for at a time (SteamVR returns
    // XR_ERROR_CALL_ORDER_INVALID, and the game aborted on it in round 3).
    if (c->waited > 0) {
        trace("ERROR wait sc%d with an image already waited for and not released", chainId(sc));
        gErrors++;
        return XR_ERROR_CALL_ORDER_INVALID;
    }
    c->waited++;
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL mRelease(XrSwapchain sc, const XrSwapchainImageReleaseInfo* ri) {
    (void)ri;
    Chain* c = chainOf(sc);
    if (c->acquired <= 0) {
        trace("ERROR release sc%d without acquire", chainId(sc));
        return XR_ERROR_CALL_ORDER_INVALID;
    }
    // The image released is the oldest acquired, and it must have been waited
    // for: with one waited image at most, acquired must not exceed the
    // waited one plus those acquired after it.
    if (c->waited <= 0) {
        trace("ERROR release sc%d of an image not waited for", chainId(sc));
        gErrors++;
        return XR_ERROR_CALL_ORDER_INVALID;
    }
    // Opt-in (MOCK_XR_BUSY_MS): the app's frame loop now submits the next
    // set before it releases the last, so the queue is rarely idle here.
    static int busyCheck = -1;
    if (busyCheck < 0) busyCheck = getenv("MOCK_XR_BUSY_MS") != NULL;
    if (busyCheck && gGfx && gGfx->release_wait_ms) {
        // The wait includes other processes' GPU work (the desktop's
        // compositor) ahead of the mock's own check: up to 4 ms seen
        // with the app's work all done.  The eyes' work takes 10 ms and more.
        static double limit = -1.0;
        if (limit < 0.0) limit = getenv("MOCK_XR_BUSY_MS") ? atof(getenv("MOCK_XR_BUSY_MS")) : 4.0;
        double ms = gGfx->release_wait_ms();
        gReleases++;
        gWaitHist[ms < 0.5 ? 0 : ms < 1.0 ? 1 : ms < 2.0 ? 2 : ms < 4.0 ? 3 : 4]++;
        if (ms > limit) {
            gBusyReleases++;
            if (ms > gBusyMaxMs) gBusyMaxMs = ms;
            trace("ERROR release sc%d before the GPU finished", chainId(sc));
            gErrors++;
        }
    }
    c->acquired--;
    c->waited--;
    trace("release sc%d", chainId(sc));
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL mBeginSession(XrSession s, const XrSessionBeginInfo* bi) {
    (void)s;
    (void)bi;
    gT0 = nowNs();
    const char* secs = getenv("MOCK_XR_SECONDS");
    gEndAt = gT0 + (int64_t)((secs ? atof(secs) : 20.0) * 1e9);
    pushState(XR_SESSION_STATE_SYNCHRONIZED);
    pushState(XR_SESSION_STATE_VISIBLE);
    pushState(XR_SESSION_STATE_FOCUSED);
    trace("beginSession");
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL mEndSession(XrSession s) {
    (void)s;
    fprintf(stderr,
            "xrmock: summary: %d errors; %d releases checked for finished GPU work, %d before it finished (worst %.1f ms); "
            "%d projection layers: eyes from one locate for the display time submitted %d, for an earlier one %d, a later one %d; eyes from "
            "different locates %d, poses not from a locate %d\n",
            gErrors, gReleases, gBusyReleases, gBusyMaxMs, gPoseSets, gPoseSameTime, gPoseEarlier, gPoseLater, gPoseSplit, gPoseUnknown);
    fprintf(stderr, "xrmock: projection layers by refreshes from the locate to the display time: later %d, 0: %d, 1: %d, 2: %d, 3: %d, more %d\n", gPoseAge[0],
            gPoseAge[1], gPoseAge[2], gPoseAge[3], gPoseAge[4], gPoseAge[5]);
    if (gReleases) {
        fprintf(stderr, "xrmock: queue drain time at the releases: <0.5 ms %d, 0.5-1 %d, 1-2 %d, 2-4 %d, >4 %d\n", gWaitHist[0], gWaitHist[1], gWaitHist[2],
                gWaitHist[3], gWaitHist[4]);
    }
    pushState(XR_SESSION_STATE_EXITING);
    trace("endSession");
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL mRequestExit(XrSession s) {
    (void)s;
    pushState(XR_SESSION_STATE_STOPPING);
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL mPollEvent(XrInstance i, XrEventDataBuffer* ev) {
    (void)i;
    if (gEventCount == 0) return XR_EVENT_UNAVAILABLE;
    int st = gEventQueue[0];
    memmove(gEventQueue, gEventQueue + 1, (size_t)(--gEventCount) * sizeof(int));
    XrEventDataSessionStateChanged* e = (XrEventDataSessionStateChanged*)ev;
    e->type = XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED;
    e->next = NULL;
    e->session = (XrSession)(uintptr_t)2;
    e->state = (XrSessionState)st;
    e->time = 0;
    trace("event sessionState=%d", st);
    return XR_SUCCESS;
}

static int gWaits = 0;
static XrResult XRAPI_CALL mWaitFrame(XrSession s, const XrFrameWaitInfo* wi, XrFrameState* fs) {
    (void)s;
    (void)wi;
    int64_t now = nowNs();
    // MOCK_XR_SKIP=1: a frame loop that falls behind loses refreshes, as on a
    // headset (the next frame waits for the next refresh to come).
    static int skip = -1;
    if (skip < 0) skip = getenv("MOCK_XR_SKIP") != NULL;
    if (skip && gT0 && now > gT0 + (int64_t)gWaits * gPeriod + gPeriod / 2) {
        gWaits = (int)((now - gT0) / gPeriod) + 1;
    }
    int64_t wake = gT0 + (int64_t)gWaits * gPeriod;
    if (wake > now) {
        struct timespec ts = {(time_t)((wake - now) / 1000000000ll), (long)((wake - now) % 1000000000ll)};
        nanosleep(&ts, NULL);
    }
    fs->predictedDisplayTime = wake + 2 * gPeriod;
    fs->predictedDisplayPeriod = gPeriod;
    fs->shouldRender = XR_TRUE;
    gWaits++;
    if (gEndAt && nowNs() > gEndAt && !gQuitSent) {
        gQuitSent = 1;
        pushState(XR_SESSION_STATE_STOPPING);
    }
    gFrame++;
    trace("waitFrame");
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL mBeginFrame(XrSession s, const XrFrameBeginInfo* bi) {
    (void)s;
    (void)bi;
    trace("beginFrame");
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL mEndFrame(XrSession s, const XrFrameEndInfo* ei) {
    (void)s;
    char buf[1024] = "";
    for (uint32_t i = 0; i < ei->layerCount; i++) {
        const XrCompositionLayerBaseHeader* l = ei->layers[i];
        char t[256];
        if (l->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
            const XrCompositionLayerProjection* p = (const XrCompositionLayerProjection*)l;
            checkPoses(p, ei->displayTime);
            snprintf(t, sizeof(t), " proj[flags=%llx", (unsigned long long)p->layerFlags);
            strcat(buf, t);
            for (uint32_t v = 0; v < p->viewCount; v++) {
                const XrCompositionLayerProjectionView* pv = &p->views[v];
                int warp = 0;
                for (const XrBaseInStructure* n = (const XrBaseInStructure*)pv->next; n; n = n->next) warp |= n->type == XR_TYPE_COMPOSITION_LAYER_SPACE_WARP_INFO_FB;
                snprintf(t, sizeof(t), " sc%d:%dx%d%s", chainId(pv->subImage.swapchain), pv->subImage.imageRect.extent.width,
                         pv->subImage.imageRect.extent.height, warp ? "+warp" : "");
                strcat(buf, t);
            }
            strcat(buf, "]");
        } else if (l->type == XR_TYPE_COMPOSITION_LAYER_QUAD) {
            const XrCompositionLayerQuad* q = (const XrCompositionLayerQuad*)l;
            snprintf(t, sizeof(t), " quad[sc%d:%dx%d eye=%d]", chainId(q->subImage.swapchain), q->subImage.imageRect.extent.width,
                     q->subImage.imageRect.extent.height, (int)q->eyeVisibility);
            strcat(buf, t);
        } else {
            snprintf(t, sizeof(t), " layer%d", (int)l->type);
            strcat(buf, t);
        }
    }
    trace("endFrame layers=%u%s", ei->layerCount, buf);
    return XR_SUCCESS;
}
// The head turns slowly from side to side, so views located for different
// times differ.  The last locates are kept for the pose check in mEndFrame.
#define LOCATES 32
static struct {
    XrTime time;
    XrPosef pose[2];
} gLocates[LOCATES];
static int gLocateNext = 0;
static XrResult XRAPI_CALL mLocateViews(XrSession s, const XrViewLocateInfo* li, XrViewState* vs, uint32_t cap, uint32_t* count, XrView* v) {
    (void)s;
    vs->viewStateFlags = XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT | XR_VIEW_STATE_ORIENTATION_TRACKED_BIT |
                         XR_VIEW_STATE_POSITION_TRACKED_BIT;
    *count = 2;
    double t = (double)(li->displayTime - gT0) / 1e9;
    float yaw = (float)(0.3 * sin(t * 2.0 * 3.14159265 * 0.5));
    float c = cosf(yaw), sn = sinf(yaw);
    for (uint32_t e = 0; e < 2 && e < cap; e++) {
        float x = e ? 0.032f : -0.032f;
        v[e].pose.orientation = (XrQuaternionf){0, sinf(yaw * 0.5f), 0, cosf(yaw * 0.5f)};
        v[e].pose.position = (XrVector3f){x * c, 1.6f, -x * sn};
        v[e].fov = (XrFovf){-0.84f, 0.84f, 0.84f, -0.84f};
        gLocates[gLocateNext].pose[e] = v[e].pose;
    }
    gLocates[gLocateNext].time = li->displayTime;
    gLocateNext = (gLocateNext + 1) % LOCATES;
    return XR_SUCCESS;
}

static int samePose(const XrPosef* a, const XrPosef* b) { return !memcmp(a, b, sizeof(XrPosef)); }

// Both eyes of a projection layer: from one locate, and for which time.
static void checkPoses(const XrCompositionLayerProjection* p, XrTime displayTime) {
    if (p->viewCount != 2) return;
    gPoseSets++;
    int left = -1, both = -1;
    for (int i = 0; i < LOCATES; i++) {
        if (!gLocates[i].time) continue;
        if (samePose(&gLocates[i].pose[0], &p->views[0].pose)) {
            left = i;
            if (samePose(&gLocates[i].pose[1], &p->views[1].pose)) both = i;
        }
    }
    if (both < 0) {
        if (left >= 0) {
            gPoseSplit++;
            trace("ERROR projection layer eyes from different locates");
            gErrors++;
        } else {
            gPoseUnknown++;
        }
        return;
    }
    XrTime t = gLocates[both].time;
    if (t == displayTime) gPoseSameTime++;
    else if (t < displayTime) gPoseEarlier++;
    else gPoseLater++;
    long k = lround((double)(displayTime - t) / (double)gPeriod);
    gPoseAge[k < 0 ? 0 : k > 3 ? 5 : k + 1]++;
}
static XrResult XRAPI_CALL mStringToPath(XrInstance i, const char* s, XrPath* p) {
    (void)i;
    for (int k = 0; k < gPathCount; k++)
        if (!strcmp(gPathStr[k], s)) {
            *p = gPaths[k];
            return XR_SUCCESS;
        }
    strncpy(gPathStr[gPathCount], s, 127);
    gPaths[gPathCount] = (XrPath)(5000 + gPathCount);
    *p = gPaths[gPathCount++];
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL mPathToString(XrInstance i, XrPath p, uint32_t cap, uint32_t* len, char* buf) {
    (void)i;
    for (int k = 0; k < gPathCount; k++)
        if (gPaths[k] == p) {
            strncpy(buf, gPathStr[k], cap);
            *len = (uint32_t)strlen(gPathStr[k]) + 1;
            return XR_SUCCESS;
        }
    return XR_ERROR_PATH_INVALID;
}
static XrResult XRAPI_CALL mCreateActionSet(XrInstance i, const XrActionSetCreateInfo* ci, XrActionSet* o) {
    (void)i;
    (void)ci;
    *o = (XrActionSet)(uintptr_t)3;
    return XR_SUCCESS;
}
static int gActions = 0;
static XrResult XRAPI_CALL mCreateAction(XrActionSet s, const XrActionCreateInfo* ci, XrAction* o) {
    (void)s;
    (void)ci;
    *o = (XrAction)(uintptr_t)(200 + gActions++);
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL mSuggest(XrInstance i, const XrInteractionProfileSuggestedBinding* b) {
    (void)i;
    (void)b;
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL mAttach(XrSession s, const XrSessionActionSetsAttachInfo* a) {
    (void)s;
    (void)a;
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL mSync(XrSession s, const XrActionsSyncInfo* a) {
    (void)s;
    (void)a;
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL mGetBool(XrSession s, const XrActionStateGetInfo* g, XrActionStateBoolean* o) {
    (void)s;
    (void)g;
    o->isActive = XR_FALSE;
    o->currentState = XR_FALSE;
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL mGetFloat(XrSession s, const XrActionStateGetInfo* g, XrActionStateFloat* o) {
    (void)s;
    (void)g;
    o->isActive = XR_FALSE;
    o->currentState = 0;
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL mGetVec2(XrSession s, const XrActionStateGetInfo* g, XrActionStateVector2f* o) {
    (void)s;
    (void)g;
    o->isActive = XR_FALSE;
    o->currentState.x = o->currentState.y = 0;
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL mHaptic(XrSession s, const XrHapticActionInfo* h, const XrHapticBaseHeader* b) {
    (void)s;
    (void)h;
    (void)b;
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL mHapticStop(XrSession s, const XrHapticActionInfo* h) {
    (void)s;
    (void)h;
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL mGetProfile(XrSession s, XrPath p, XrInteractionProfileState* o) {
    (void)s;
    (void)p;
    o->interactionProfile = XR_NULL_PATH;
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL mReqRefresh(XrSession s, float hz) {
    (void)s;
    trace("requestRefreshRate %.0f", hz);
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL mEnumRefresh(XrSession s, uint32_t cap, uint32_t* count, float* r) {
    (void)s;
    *count = 3;
    if (cap) {
        r[0] = 72;
        r[1] = 90;
        r[2] = 120;
    }
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL mGetRefresh(XrSession s, float* hz) {
    (void)s;
    *hz = (float)(1e9 / (double)gPeriod);
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL mPerfLevel(XrSession s, XrPerfSettingsDomainEXT d, XrPerfSettingsLevelEXT l) {
    (void)s;
    (void)d;
    (void)l;
    return XR_SUCCESS;
}

typedef struct {
    const char* name;
    PFN_xrVoidFunction fn;
} Entry;
#define E(n, f) {n, (PFN_xrVoidFunction)f}
static const Entry kTable[] = {
    E("xrEnumerateInstanceExtensionProperties", mEnumExt), E("xrCreateInstance", mCreateInstance), E("xrDestroyInstance", mDestroyInstance), E("xrGetInstanceProperties", mGetInstanceProps), E("xrResultToString", mResultToString),
    E("xrGetSystem", mGetSystem), E("xrGetSystemProperties", mGetSystemProps), E("xrCreateSession", mCreateSession),
    E("xrDestroySession", mDestroySession), E("xrCreateReferenceSpace", mCreateRefSpace), E("xrCreateActionSpace", mCreateActionSpace),
    E("xrDestroySpace", mDestroySpace), E("xrLocateSpace", mLocateSpace), E("xrEnumerateViewConfigurationViews", mEnumViewConfigViews),
    E("xrEnumerateSwapchainFormats", mEnumFormats), E("xrCreateSwapchain", mCreateSwapchain), E("xrDestroySwapchain", mDestroySwapchain), E("xrEnumerateSwapchainImages", mEnumSwapImages),
    E("xrAcquireSwapchainImage", mAcquire), E("xrWaitSwapchainImage", mWaitImage), E("xrReleaseSwapchainImage", mRelease),
    E("xrBeginSession", mBeginSession), E("xrEndSession", mEndSession), E("xrRequestExitSession", mRequestExit), E("xrPollEvent", mPollEvent),
    E("xrWaitFrame", mWaitFrame), E("xrBeginFrame", mBeginFrame), E("xrEndFrame", mEndFrame), E("xrLocateViews", mLocateViews),
    E("xrStringToPath", mStringToPath), E("xrPathToString", mPathToString), E("xrCreateActionSet", mCreateActionSet),
    E("xrCreateAction", mCreateAction), E("xrSuggestInteractionProfileBindings", mSuggest), E("xrAttachSessionActionSets", mAttach),
    E("xrSyncActions", mSync), E("xrGetActionStateBoolean", mGetBool), E("xrGetActionStateFloat", mGetFloat), E("xrGetActionStateVector2f", mGetVec2),
    E("xrApplyHapticFeedback", mHaptic), E("xrStopHapticFeedback", mHapticStop), E("xrGetCurrentInteractionProfile", mGetProfile),
    E("xrRequestDisplayRefreshRateFB", mReqRefresh), E("xrEnumerateDisplayRefreshRatesFB", mEnumRefresh), E("xrGetDisplayRefreshRateFB", mGetRefresh),
    E("xrPerfSettingsSetPerformanceLevelEXT", mPerfLevel),
};

static void init(void) {
    static int done;
    if (done) return;
    done = 1;
    const char* tp = getenv("MOCK_XR_TRACE");
    if (tp) gTrace = fopen(tp, "w");
    const char* hz = getenv("MOCK_XR_HZ");
    if (hz) gPeriod = (int64_t)(1e9 / atof(hz));
    const char* list = getenv("MOCK_XR_EXTENSIONS");
    if (list) {
        char* copy = strdup(list);
        for (char* tok = strtok(copy, ","); tok && gExtCount < 64; tok = strtok(NULL, ",")) strcpy(gExtNames[gExtCount++], tok);
        free(copy);
    } else {
        const char* exts[] = {"XR_KHR_opengl_es_enable", "XR_MNDX_egl_enable", "XR_KHR_vulkan_enable2", "XR_FB_display_refresh_rate",
                              "XR_EXT_performance_settings", "XR_VALVE_frame_controller_interaction", "XR_KHR_composition_layer_depth", "XR_EXT_eye_gaze_interaction"};
        for (unsigned i = 0; i < sizeof(exts) / sizeof(exts[0]); i++) strcpy(gExtNames[gExtCount++], exts[i]);
        if (getenv("MOCK_XR_SPACEWARP")) strcpy(gExtNames[gExtCount++], "XR_FB_space_warp");
    }
}

static XRAPI_ATTR XrResult XRAPI_CALL mockGetInstanceProcAddr(XrInstance instance, const char* name, PFN_xrVoidFunction* fn) {
    (void)instance;
    init();
    for (unsigned i = 0; i < sizeof(kTable) / sizeof(kTable[0]); i++) {
        if (!strcmp(kTable[i].name, name)) {
            *fn = kTable[i].fn;
            return XR_SUCCESS;
        }
    }
    // The graphics extension's own functions: the app asks for them before
    // the session exists, so any table may answer.
    for (unsigned t = 0; t < sizeof(kGfxTables) / sizeof(kGfxTables[0]); t++) {
        PFN_xrVoidFunction f = kGfxTables[t]->get_proc(name);
        if (f) {
            *fn = f;
            return XR_SUCCESS;
        }
    }
    *fn = NULL;
    return XR_ERROR_FUNCTION_UNSUPPORTED;
}

__attribute__((visibility("default"))) XrResult XRAPI_CALL xrNegotiateLoaderRuntimeInterface(const XrNegotiateLoaderInfo* li,
                                                                                            XrNegotiateRuntimeRequest* rr) {
    (void)li;
    init();
    rr->runtimeInterfaceVersion = 1;
    rr->runtimeApiVersion = XR_CURRENT_API_VERSION;
    rr->getInstanceProcAddr = mockGetInstanceProcAddr;
    return XR_SUCCESS;
}
