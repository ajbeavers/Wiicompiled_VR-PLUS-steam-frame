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
//   MOCK_XR_BUSY_MS=<ms>      Vulkan: check that each released image was finished.
//                             Exact where the images can be host visible (the
//                             Frame): the image's bytes at the release against
//                             its bytes once all work queued before the release
//                             completed (other queued work does not count). Else
//                             a release that waits longer than <ms> for the
//                             app's queue to go idle is an error.
//   MOCK_XR_MUTABLE=0         Vulkan: create images without VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT even when
//                             the app asks for XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT (the old mock)
//   MOCK_XR_EYE=<w>x<h>       recommended eye size (1728x1728, SteamVR's on
//                             the Frame)
//   MOCK_XR_TIME_OFFSET_S=<s> XrTime runs this far ahead of CLOCK_MONOTONIC
//                             (0); XR_KHR_convert_timespec_time converts
//   MOCK_XR_FOCUS_BLIP=<ms>[@<s>]  every <s> seconds (7) the session loses
//                             focus for <ms> ms (VISIBLE, then FOCUSED again),
//                             as SteamVR does now and then
//   MOCK_XR_PROFILE=<path>    the current interaction profile of both hands
//                             (e.g. /interaction_profiles/valve/frame_controller;
//                             default none); an INTERACTION_PROFILE_CHANGED
//                             event follows the first FOCUSED, and suggested
//                             bindings for it are traced
//   MOCK_XR_RATES=<hz>,...    refresh rates XR_FB_display_refresh_rate offers (72,90,120)
//   MOCK_XR_DUMP=<dir>        Vulkan: write released images as PPM files (every 4th pixel)
//                             <dir>/t<seconds>-sc<id>-<n>.ppm: at the seconds in
//                             MOCK_XR_DUMP_AT=<s>,<s>... (default every 10 s), the next
//                             MOCK_XR_DUMP_FRAMES=<n> (1) releases of every swapchain;
//                             MOCK_XR_DUMP_STEP=<n> every n-th pixel (4; 1: full resolution);
//                             MOCK_XR_DUMP_ALPHA=1 also the alpha channel (<name>-alpha.pgm)
//   MOCK_XR_STAMP=1           Vulkan, with KARTFRAME_FRAME_STAMP=1 in the app: every eye image a
//                             projection layer submits carries the display time its poses were
//                             located for (a block in its corner); compared with the locate the
//                             layer's poses came from (summary line "stamps")
//   MOCK_XR_PREDICT_JITTER=<ms>  predictedDisplayTime is off its refresh by up to +-<ms> (uniform,
//                             at most 3; default 0.02, SteamVR's on the Frame in the third headset
//                             session), as SteamVR's prediction is not always exactly one period
//                             after the last; 0 turns it off
//   MOCK_XR_LEAD=<refreshes>  predictedDisplayTime this many refreshes after xrWaitFrame's wake-up
//                             (default 3: SteamVR's lead on the Frame was 25 to 27 ms at 120 Hz)
//   MOCK_XR_FOV=<l>,<r>,<u>,<d>  the left eye's field of view in degrees (the right eye mirrors it);
//                             default the Frame's own, per eye (left -59.08,50.39,49.47,-60.00, right
//                             -50.29,59.16,49.88,-59.55), "sym" the old symmetric +-48.1
// Every xrEndFrame must carry the predictedDisplayTime of the xrWaitFrame its xrBeginFrame followed
// (ERROR otherwise; summary line "end times").
//   MOCK_XR_HANDS=1           controllers held in front of the player (left and right action
//                             spaces at chest height, pointing ahead), instead of all at the head;
//                             =high raises the right one to eye height in the middle (in front of the menu screen)
//   MOCK_XR_PRESS=<path>@<start s>/<every s>[;...]  scripted presses for the current profile
//                             (MOCK_XR_PROFILE): the action bound to <path> (e.g.
//                             /user/hand/right/input/a/click) is held for 150 ms every <every> s from
//                             <start> s on; actions of that profile then report isActive
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
// XrTime = CLOCK_MONOTONIC + gTimeOffset (MOCK_XR_TIME_OFFSET_S).
static int64_t gTimeOffset = 0;

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
    int released;  // an image was released since the last xrEndFrame
    int dumpMark, dumped;  // MOCK_XR_DUMP: the dump time this chain is on, images written for it
    int stampSlot;         // MOCK_XR_STAMP: the last released image's stamp read-back slot, -1 none
    unsigned stampGen;
    int lastRel;           // index of the last image released, -1 none
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
static int gEyeW = 1728, gEyeH = 1728;
// Focus blips (MOCK_XR_FOCUS_BLIP) and the interaction profile (MOCK_XR_PROFILE).
#define EVENT_PROFILE_CHANGED (-2)
static double gBlipMs = 0.0, gBlipEvery = 7.0;
static int64_t gNextBlip = 0, gRefocusAt = 0;
static int gFocused = 0, gBlips = 0, gProfileSent = 0;
static const char* gProfile = NULL;
static int64_t gT0 = 0, gPeriod = 8333333, gEndAt = 0;
static int64_t gJitterNs = 20000;  // MOCK_XR_PREDICT_JITTER
static int64_t gLead = 3;         // MOCK_XR_LEAD (refreshes)
// xrEndFrame against the frame's predicted display time (the one its xrBeginFrame followed).
static XrTime gWaitPredicted = 0, gBegunPredicted = 0;
static int gEndTimeMatch = 0, gEndTimeOff = 0, gEndTimeOffPeriods[3];  // [0] within a refresh, [1] a refresh earlier or more, [2] later
static double gEndTimeOffWorstMs = 0.0;
// The eyes' angles in degrees (left, right, up, down): the Frame's (its OpenXR eye fields of view, logged
// in the third headset session). MOCK_XR_FOV sets the left eye's; the right eye then mirrors it.
static float gFovDeg[2][4] = {{-59.08f, 50.39f, 49.47f, -60.00f}, {-50.29f, 59.16f, 49.88f, -59.55f}};
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
// MOCK_XR_HANDS: which hand each action space belongs to (-1: none).
static int gSpaceHand[256];
static int gHands = 0;
static const char* pathString(XrPath p);
static XrResult XRAPI_CALL mCreateActionSpace(XrSession s, const XrActionSpaceCreateInfo* ci, XrSpace* out) {
    (void)s;
    int id = gSpaces++;
    if (id < 256) {
        const char* sub = pathString(ci->subactionPath);
        gSpaceHand[id] = !sub ? -1 : strstr(sub, "/left") ? 0 : strstr(sub, "/right") ? 1 : -1;
    }
    *out = (XrSpace)(uintptr_t)(100 + id);
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL mDestroySpace(XrSpace s) { (void)s; return XR_SUCCESS; }
// Added for the KartFrame game, which calls these (the Galaxy app did not).
static XrResult XRAPI_CALL mEnumBlend(XrInstance i, XrSystemId sys, XrViewConfigurationType vct, uint32_t cap, uint32_t* count, XrEnvironmentBlendMode* modes) {
    (void)i;
    (void)sys;
    (void)vct;
    *count = 1;
    if (cap >= 1 && modes) modes[0] = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL mEnumRefSpaces(XrSession s, uint32_t cap, uint32_t* count, XrReferenceSpaceType* spaces) {
    (void)s;
    *count = 3;
    if (cap >= 3 && spaces) {
        spaces[0] = XR_REFERENCE_SPACE_TYPE_VIEW;
        spaces[1] = XR_REFERENCE_SPACE_TYPE_LOCAL;
        spaces[2] = XR_REFERENCE_SPACE_TYPE_STAGE;
    }
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL mDestroyActionSet(XrActionSet a) { (void)a; return XR_SUCCESS; }
static XrResult XRAPI_CALL mLocateSpace(XrSpace sp, XrSpace base, XrTime t, XrSpaceLocation* loc) {
    (void)base;
    (void)t;
    loc->locationFlags = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT |
                         XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
    loc->pose.orientation = (XrQuaternionf){0, 0, 0, 1};
    loc->pose.position = (XrVector3f){0, 1.6f, 0};
    int id = (int)((uintptr_t)sp - 100);
    if (gHands && id >= 0 && id < 256 && id < gSpaces && gSpaceHand[id] >= 0) {
        // Held at chest height, pointing ahead and a little up (the right one at the menu screen).
        const float pitch = 0.08f;
        loc->pose.orientation = (XrQuaternionf){sinf(pitch * 0.5f), 0, 0, cosf(pitch * 0.5f)};
        // MOCK_XR_HANDS=high: the right hand raised to eye height, in front of the menu screen.
        static int high = -1;
        if (high < 0) high = getenv("MOCK_XR_HANDS") && !strcmp(getenv("MOCK_XR_HANDS"), "high");
        loc->pose.position = (XrVector3f){gSpaceHand[id] ? (high ? 0.0f : 0.16f) : -0.2f, high && gSpaceHand[id] ? 1.55f : 1.3f, -0.32f};
    }
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
        v[e].recommendedImageRectWidth = (uint32_t)gEyeW;
        v[e].recommendedImageRectHeight = (uint32_t)gEyeH;
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
    c->stampSlot = -1;
    c->lastRel = -1;
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
static void compAcquire(int chain, int idx);
static void compRelease(int chain, int idx, int slot, unsigned gen);
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
    compAcquire(chainId(sc), (int)*idx);
    trace("acquire sc%d idx=%u", chainId(sc), *idx);
    return XR_SUCCESS;
}
static int gReleases = 0, gBusyReleases = 0;
static double gBusyMaxMs = 0.0;
static int gWaitHist[5];
static int gPoseAge[6];  // display time minus locate time in refreshes: <0, 0, 1, 2, 3, more
static int gPoseSets = 0, gPoseSameTime = 0, gPoseEarlier = 0, gPoseLater = 0, gPoseSplit = 0, gPoseUnknown = 0;
static int checkPoses(const XrCompositionLayerProjection* p, XrTime displayTime, int fresh);
static XrTime locateTime(int i);
// Frames by what they showed: a projection layer whose eye images were both released since
// the last xrEndFrame (fresh), one shown again (repeat), none.
static int gEnds = 0, gFreshSets = 0, gFreshSame = 0, gFreshEarlier = 0, gFreshLater = 0, gRepeatSets = 0, gQuadEnds = 0, gEmptyEnds = 0;
// MOCK_XR_STAMP: eye images of submitted projection layers waiting for their stamp read-back.
static int gStampOn = 0;
#define STAMP_PENDING 256
static struct {
    int slot, eye, fresh;
    unsigned gen;
    XrTime expect;  // the time the layer's poses were located for
} gStampPending[STAMP_PENDING];
static int gStampPendingCount = 0;
static int gStampChecked = 0, gStampMatch = 0, gStampOther = 0, gStampOtherEye = 0, gStampNone = 0, gStampLost = 0, gStampUnknownPose = 0;
static int gStampOff[5];  // stamp minus locate in refreshes: <=-2, -1, between, +1, >=+2
// Lane L: the scene words (the scene state each eye's draws used) of every fresh projection layer's eye
// images, reported at the end (sceneReport): eyes of one set compared, weights per eye, scene steps.
typedef struct {
    uint64_t value, a, b;
    int eye;
} SceneRec;
static SceneRec* gScene = NULL;
static int gSceneN = 0, gSceneCap = 0, gSceneBroken = 0, gSceneAbsent = 0;
static void sceneAdd(uint64_t value, int eye, int slot, unsigned gen) {
    if (!gGfx->stamp_scene) return;
    uint64_t a = 0, b = 0;
    int r = gGfx->stamp_scene(slot, gen, &a, &b);
    if (r < 0) {
        gSceneBroken++;
        trace("ERROR stamp: eye %d image's scene words are broken", eye);
        gErrors++;
        return;
    }
    if (r == 0) {
        gSceneAbsent++;
        return;
    }
    if (gSceneN == gSceneCap) {
        gSceneCap = gSceneCap ? 2 * gSceneCap : 4096;
        gScene = (SceneRec*)realloc(gScene, (size_t)gSceneCap * sizeof(SceneRec));
    }
    gScene[gSceneN++] = (SceneRec){value, a, b, eye};
}
static int sceneCmp(const void* x, const void* y) {
    const SceneRec* p = (const SceneRec*)x;
    const SceneRec* q = (const SceneRec*)y;
    if (p->value != q->value) return p->value < q->value ? -1 : 1;
    return p->eye - q->eye;
}
#define SC_VALID(a) (((a) >> 48) & 1)
#define SC_LATE(a) (((a) >> 49) & 1)
#define SC_W(a) ((double)((a) & 0xffff) / 65535.0)
#define SC_PREV(a) ((unsigned)(((a) >> 16) & 0xffff))
#define SC_CUR(a) ((unsigned)(((a) >> 32) & 0xffff))
#define SC_N(b, k) ((unsigned)(((b) >> (16 * (k))) & 0xffff))
static void sceneReport(void) {
    if (!gStampOn) return;
    if (!gSceneN) {
        fprintf(stderr, "xrmock: scene stamps: none (%d without scene words, %d broken)\n", gSceneAbsent, gSceneBroken);
        return;
    }
    qsort(gScene, (size_t)gSceneN, sizeof(SceneRec), sceneCmp);
    // Eyes of one set (same display time): weight, game-frame pair, fill kind, draw counts.
    int sets = 0, single = 0, diffW = 0, diffPair = 0, diffLate = 0, diffCounts = 0, diffAny = 0, invalid = 0;
    int whist[2][11];
    double draws[2][4];
    int lateSets = 0, sealSets = 0;
    memset(whist, 0, sizeof(whist));
    memset(draws, 0, sizeof(draws));
    // Scene position per set in display order (game frames): previous + weight x (current - previous).
    double* pos = (double*)malloc((size_t)gSceneN * sizeof(double));
    uint64_t* when = (uint64_t*)malloc((size_t)gSceneN * sizeof(uint64_t));
    double* wt = (double*)malloc((size_t)gSceneN * sizeof(double));
    int npos = 0;
    long unwrap = 0;
    unsigned lastCur = 0;
    int haveCur = 0;
    for (int i = 0; i < gSceneN;) {
        int j = i;
        while (j < gSceneN && gScene[j].value == gScene[i].value) j++;
        const SceneRec* l = NULL;
        const SceneRec* r = NULL;
        for (int k = i; k < j; k++) {
            if (gScene[k].eye == 0 && !l) l = &gScene[k];
            if (gScene[k].eye == 1 && !r) r = &gScene[k];
        }
        i = j;
        if (!l || !r) {
            single++;
            continue;
        }
        if (!SC_VALID(l->a) || !SC_VALID(r->a)) {
            invalid++;
            continue;
        }
        sets++;
        const SceneRec* e[2] = {l, r};
        for (int v = 0; v < 2; v++) {
            double w = SC_W(e[v]->a);
            whist[v][w >= 1.0 ? 10 : (int)(w * 10.0)]++;
            for (int k = 0; k < 4; k++) draws[v][k] += SC_N(e[v]->b, k);
        }
        if (SC_LATE(l->a)) lateSets++;
        else sealSets++;
        int dw = (l->a & 0xffff) != (r->a & 0xffff);
        int dp = SC_PREV(l->a) != SC_PREV(r->a) || SC_CUR(l->a) != SC_CUR(r->a);
        int dl = SC_LATE(l->a) != SC_LATE(r->a);
        int dc = l->b != r->b;
        diffW += dw;
        diffPair += dp;
        diffLate += dl;
        diffCounts += dc;
        if (dw || dp || dl || dc) {
            diffAny++;
            if (diffAny <= 10)
                trace("ERROR scene: eyes of the set for %.3f ms differ: L w=%.4f f=%u/%u n=%u/%u/%u/%u, R w=%.4f f=%u/%u n=%u/%u/%u/%u",
                      (double)l->value / 1e6, SC_W(l->a), SC_PREV(l->a), SC_CUR(l->a), SC_N(l->b, 0), SC_N(l->b, 1), SC_N(l->b, 2),
                      SC_N(l->b, 3), SC_W(r->a), SC_PREV(r->a), SC_CUR(r->a), SC_N(r->b, 0), SC_N(r->b, 1), SC_N(r->b, 2), SC_N(r->b, 3));
            gErrors++;
        }
        // Left eye's scene position, game frames unwrapped from 16 bits.
        unsigned cur = SC_CUR(l->a), prev = SC_PREV(l->a);
        if (haveCur) unwrap += (long)(short)(unsigned short)(cur - lastCur);
        else unwrap = cur;
        lastCur = cur;
        haveCur = 1;
        long prevU = unwrap - (long)(unsigned short)(cur - prev);
        pos[npos] = (double)prevU + SC_W(l->a) * (double)(unwrap - prevU);
        when[npos] = l->value;
        wt[npos] = SC_W(l->a);
        npos++;
    }
    fprintf(stderr,
            "xrmock: scene stamps: %d sets with both eyes (%d filled at the sample time, %d at the seal), %d without a scene (menus), %d one eye "
            "only, %d without scene words, %d broken\n",
            sets, lateSets, sealSets, invalid, single, gSceneAbsent, gSceneBroken);
    fprintf(stderr, "xrmock: scene stamps: eyes of one set differ: %d (weight %d, game-frame pair %d, fill %d, draw counts %d)\n", diffAny, diffW,
            diffPair, diffLate, diffCounts);
    for (int v = 0; v < 2; v++) {
        double filled = draws[v][0] + draws[v][1];
        fprintf(stderr,
                "xrmock: scene stamps %s eye: weight 0-.1 %d, .1-.2 %d, .2-.3 %d, .3-.4 %d, .4-.5 %d, .5-.6 %d, .6-.7 %d, .7-.8 %d, .8-.9 %d, "
                ".9-1 %d, 1: %d; perspective draws a set %.1f: from the pair %.1f %%, current only %.1f %%, drawn with the recorded "
                "uniform %.0f, with a range outside the set's fill %.0f\n",
                v ? "right" : "left", whist[v][0], whist[v][1], whist[v][2], whist[v][3], whist[v][4], whist[v][5], whist[v][6], whist[v][7],
                whist[v][8], whist[v][9], whist[v][10], sets ? filled / sets : 0.0, filled > 0 ? 100.0 * draws[v][0] / filled : 0.0,
                filled > 0 ? 100.0 * draws[v][1] / filled : 0.0, draws[v][2], draws[v][3]);
    }
    // Scene steps between sets one refresh apart (game frames; 0.5 is even at 120 Hz with a 60 fps game).
    int step[8], pairs = 0, alt = 0, altN = 0;
    memset(step, 0, sizeof(step));
    double sum = 0, sum2 = 0;
    for (int k = 1; k < npos; k++) {
        double refreshes = (double)(when[k] - when[k - 1]) / (double)gPeriod;
        if (fabs(refreshes - 1.0) > 0.1) continue;
        double d = pos[k] - pos[k - 1];
        pairs++;
        sum += d;
        sum2 += d * d;
        step[d < -0.05 ? 0 : d < 0.05 ? 1 : d < 0.35 ? 2 : d < 0.65 ? 3 : d < 0.95 ? 4 : d < 1.05 ? 5 : d < 1.5 ? 6 : 7]++;
        if (k >= 2 && fabs((double)(when[k - 1] - when[k - 2]) / (double)gPeriod - 1.0) < 0.1) {
            double d0 = pos[k - 1] - pos[k - 2];
            altN++;
            if (fabs(d - d0) >= 0.3) alt++;
        }
    }
    double mean = pairs ? sum / pairs : 0, sd = pairs ? sqrt(fmax(0.0, sum2 / pairs - mean * mean)) : 0;
    fprintf(stderr,
            "xrmock: scene steps (game frames, sets one refresh apart): %d, mean %.3f, sd %.3f; back %d, 0: %d, 0.05-0.35 %d, 0.35-0.65 %d, "
            "0.65-0.95 %d, 1: %d, 1.05-1.5 %d, more %d; consecutive steps differing by 0.3 or more %d of %d\n",
            pairs, mean, sd, step[0], step[1], step[2], step[3], step[4], step[5], step[6], step[7], alt, altN);
    // MOCK_XR_SCENE_DUMP=<file>: the left eye's sequence (display ns, weight, scene position) for plots.
    const char* dump = getenv("MOCK_XR_SCENE_DUMP");
    if (dump && *dump) {
        FILE* f = fopen(dump, "w");
        if (f) {
            for (int k = 0; k < npos; k++) fprintf(f, "%llu %.5f %.5f\n", (unsigned long long)when[k], wt[k], pos[k]);
            fclose(f);
        }
    }
    free(pos);
    free(when);
    free(wt);
}
static void stampPoll(int wait) {
    for (int i = 0; i < gStampPendingCount;) {
        uint64_t value = 0;
        int eye = 0;
        int r = gGfx->stamp_read(gStampPending[i].slot, gStampPending[i].gen, wait, &value, &eye);
        if (r == 0) {
            i++;
            continue;
        }
        gStampChecked++;
        if (r == -2) gStampLost++;
        else if (r < 0) gStampNone++;
        else {
            int64_t diff = (int64_t)value - (int64_t)(gStampPending[i].expect - gTimeOffset);
            int sameTime = diff > -200000 && diff < 200000;
            if (sameTime && eye == gStampPending[i].eye) {
                gStampMatch++;
                if (gStampPending[i].fresh) sceneAdd(value, eye, gStampPending[i].slot, gStampPending[i].gen);
            }
            else if (sameTime) {
                gStampOtherEye++;
                trace("ERROR stamp: eye %d image carries the other eye's stamp", gStampPending[i].eye);
                gErrors++;
            } else {
                gStampOther++;
                double k = (double)diff / (double)gPeriod;
                long n = lround(k);
                // Off by less than a refresh (n == 0, beyond the 0.2 ms tolerance) is "between", not ">=+2".
                gStampOff[fabs(k - (double)n) > 0.1 || n == 0 ? 2 : n <= -2 ? 0 : n == -1 ? 1 : n == 1 ? 3 : 4]++;
                if (gStampOther <= 20)
                    trace("ERROR stamp: eye %d image rendered for a display time %.2f ms %s the one its submitted poses were located for%s",
                          gStampPending[i].eye, fabs((double)diff) / 1e6, diff < 0 ? "before" : "after", gStampPending[i].fresh ? "" : " (repeat)");
                gErrors++;
            }
        }
        gStampPending[i] = gStampPending[--gStampPendingCount];
    }
}

// ---- Compositor model (lane J): what the panels would show ----
// The panel recording of the eighth headset session (frame/perf/PANELS.md) showed, in races only, one
// eye at a time displaying a whole image it had shown 3 to 5 refreshes before (10 % of the left eye's
// refreshes, 7 % of the right eye's, both eyes at once 0.3 %), and the race timer on the virtual screen
// going back and forth between game frames. SteamVR's compositor evidently does not show the image a
// layer was ended with but resolves each eye's image when it composites. With MOCK_XR_STAMP the mock
// logs every acquire, release (with the released image's stamp) and xrEndFrame, and at the end of the
// session replays a 120 Hz vsync grid against three models of that resolution:
//   spec    the image latched at xrEndFrame (what OpenXR specifies),
//   lazyrel each eye's swapchain's most recently released image when the eye is composited,
//   lazyacq its most recently acquired image (the one the app may be rendering into: until its
//           release it still holds what it held at its previous release).
// Frames are picked by display time (the newest frame ended before the eye is composited whose
// predicted display time is not after the vsync; MOCK_XR_COMPOSITE_SELECT=latest: the newest frame
// ended), each eye read MOCK_XR_COMPOSITE_MS=<left>,<right> ms before its vsync (default 3,1: with the
// game's two alternating swapchain pairs this reproduced the recording's per-eye pattern). For every
// eye and vsync: the refreshes between the display time the shown image was rendered for and the one
// the frame's poses were located for (0: image and poses belong together; anything else is a world
// placed by the wrong head pose, a doubled image on head turns), and how often an eye shows an image
// again that it showed 2 to 8 refreshes before (the recording's measure).
typedef struct { int chain, idx; int64_t t; } AcqEv;
typedef struct { int chain, idx; int64_t t; int slot; unsigned gen; int state; uint64_t value; } RelEv;
typedef struct { int64_t t, wake; XrTime display, pose; int chain[2], latched[2]; } EndEv;
static AcqEv* gAcqEv; static int gAcqN, gAcqCap;
static RelEv* gRelEv; static int gRelN, gRelCap, gRelDone;
static EndEv* gEndEv; static int gEndN, gEndCap;
static int64_t gLastWakeNs = 0, gBegunWakeNs = 0;
static int gEndAfterWake[7];  // xrEndFrame minus its xrWaitFrame's return: <0.5, <1, <2, <3, <4, <6, more ms
// Lane K: every xrEndFrame against the runtime's own wake-up (the refresh grid, not the moment a late
// xrWaitFrame call returned), with its display time and the time its poses were located for (0: no
// projection layer). The seventh headset session (frame/perf/H7.md): frames ended more than about 1 ms
// after SteamVR's wake-up were shown inconsistently (the image zigzagged on head turns).
typedef struct { int64_t wake, end; XrTime display, pose; } LateEv;
static LateEv* gLateEv; static int gLateN, gLateCap;
static int64_t gWaitWakeSched = 0, gBegunWakeSched = 0;
#define COMP_GROW(arr, n, cap) do { if ((n) >= (cap)) { (cap) = (cap) ? 2 * (cap) : 4096; (arr) = realloc((arr), sizeof(*(arr)) * (size_t)(cap)); } } while (0)
static void compAcquire(int chain, int idx) {
    if (!gStampOn) return;
    COMP_GROW(gAcqEv, gAcqN, gAcqCap);
    gAcqEv[gAcqN++] = (AcqEv){chain, idx, nowNs()};
}
static void compRelease(int chain, int idx, int slot, unsigned gen) {
    if (!gStampOn) return;
    COMP_GROW(gRelEv, gRelN, gRelCap);
    gRelEv[gRelN++] = (RelEv){chain, idx, nowNs(), slot, gen, slot < 0 ? -1 : 0, 0};
}
// Reads back the stamps of the released images (wait: block for the outstanding ones).
static void compResolve(int wait) {
    if (!gStampOn || !gGfx || !gGfx->stamp_read) return;
    for (int i = gRelDone; i < gRelN; i++) {
        RelEv* r = &gRelEv[i];
        if (r->state != 0) continue;
        uint64_t v = 0;
        int eye = 0;
        int got = gGfx->stamp_read(r->slot, r->gen, wait, &v, &eye);
        if (got == 1) r->state = 1, r->value = v;
        else if (got != 0) r->state = -1;
    }
    while (gRelDone < gRelN && gRelEv[gRelDone].state != 0) gRelDone++;
}
static void compEnd(const XrCompositionLayerProjection* p, XrTime display, int located) {
    int64_t now = nowNs();
    if (gBegunWakeNs) {
        double ms = (double)(now - gBegunWakeNs) / 1e6;
        gEndAfterWake[ms < 0.5 ? 0 : ms < 1 ? 1 : ms < 2 ? 2 : ms < 3 ? 3 : ms < 4 ? 4 : ms < 6 ? 5 : 6]++;
    }
    if (gBegunWakeSched && gFocused) {
        COMP_GROW(gLateEv, gLateN, gLateCap);
        gLateEv[gLateN++] = (LateEv){gBegunWakeSched, now, display,
                                     (p && p->viewCount == 2 && located >= 0) ? locateTime(located) : 0};
    }
    if (!gStampOn || !p || p->viewCount != 2 || p->views[0].subImage.swapchain == p->views[1].subImage.swapchain) return;
    COMP_GROW(gEndEv, gEndN, gEndCap);
    EndEv* e = &gEndEv[gEndN++];
    e->t = now;
    e->wake = gBegunWakeNs;
    e->display = display;
    e->pose = located >= 0 ? locateTime(located) : 0;
    for (int v = 0; v < 2; v++) {
        e->chain[v] = chainId(p->views[v].subImage.swapchain);
        e->latched[v] = chainOf(p->views[v].subImage.swapchain)->lastRel;
    }
}
// The content (stamp) image idx of chain held at time t: the stamp of its last release before t.
static int compContent(int chain, int idx, int64_t t, uint64_t* value) {
    int lo = 0, hi = gRelN;  // first release after t
    while (lo < hi) { int m = (lo + hi) / 2; if (gRelEv[m].t <= t) lo = m + 1; else hi = m; }
    for (int i = lo - 1; i >= 0 && i >= lo - 400; i--) {
        if (gRelEv[i].chain == chain && gRelEv[i].idx == idx) {
            if (gRelEv[i].state != 1) return 0;
            *value = gRelEv[i].value;
            return 1;
        }
    }
    return 0;
}
static int compLatest(int model, int chain, int64_t t) {
    if (model == 1) {
        int lo = 0, hi = gRelN;
        while (lo < hi) { int m = (lo + hi) / 2; if (gRelEv[m].t <= t) lo = m + 1; else hi = m; }
        for (int i = lo - 1; i >= 0 && i >= lo - 400; i--) if (gRelEv[i].chain == chain) return gRelEv[i].idx;
    } else {
        int lo = 0, hi = gAcqN;
        while (lo < hi) { int m = (lo + hi) / 2; if (gAcqEv[m].t <= t) lo = m + 1; else hi = m; }
        for (int i = lo - 1; i >= 0 && i >= lo - 400; i--) if (gAcqEv[i].chain == chain) return gAcqEv[i].idx;
    }
    return -1;
}
static int cmpFloat(const void* a, const void* b) {
    float x = *(const float*)a, y = *(const float*)b;
    return x < y ? -1 : x > y;
}
// Lane K: xrEndFrame after the runtime's wake-up for it, and a model of how SteamVR shows late ends.
// Model (MOCK_XR_LATE_END_MS, default 1.5): a frame ended later than that after its wake-up is taken
// for the refresh after its display time (the PrimeFrame H2 sessions: frames ended 2.5 ms or more after
// the wake-up went to a 1-3 cadence, under 1.5 ms mostly clean). Each vsync shows the newest frame taken
// for it; the step is the shown poses' locate time from the last vsync's, in refreshes. A steady stream
// steps 1 every vsync (0 where the app repeated a layer, then 2); "zigzag" counts vsyncs whose step
// differs from the one before by 2 or more (a 0 next to a 2: an image one refresh off, back and forth).
static void lateReport(void) {
    if (gLateN < 100) return;
    float* ms = malloc(sizeof(float) * (size_t)gLateN);
    int bins[8] = {0}, over1 = 0;
    for (int i = 0; i < gLateN; i++) {
        float v = (float)((double)(gLateEv[i].end - gLateEv[i].wake) / 1e6);
        ms[i] = v;
        bins[v < 0.5f ? 0 : v < 1.0f ? 1 : v < 1.5f ? 2 : v < 2.0f ? 3 : v < 4.0f ? 4 : v < 6.0f ? 5 : v < 8.3f ? 6 : 7]++;
        if (v > 1.0f) over1++;
    }
    qsort(ms, (size_t)gLateN, sizeof(float), cmpFloat);
    fprintf(stderr,
            "xrmock: end after wake-up (ms, from the runtime's wake-up): %d frames, p50 %.2f, p99 %.2f, max %.2f; <0.5 %d, 0.5-1 %d, "
            "1-1.5 %d, 1.5-2 %d, 2-4 %d, 4-6 %d, 6-8.3 %d, more %d; later than 1 ms %d (%.1f %%)\n",
            gLateN, ms[gLateN / 2], ms[(int)((gLateN - 1) * 0.99)], ms[gLateN - 1], bins[0], bins[1], bins[2], bins[3], bins[4],
            bins[5], bins[6], bins[7], over1, 100.0 * over1 / gLateN);
    free(ms);
    double limit = 1.5;
    if (getenv("MOCK_XR_LATE_END_MS")) limit = atof(getenv("MOCK_XR_LATE_END_MS"));
    int64_t limitNs = (int64_t)(limit * 1e6);
    // Vsyncs from the first frame's display time to the last's.
    XrTime first = gLateEv[0].display, last = gLateEv[gLateN - 1].display;
    int j = 0, prevStep = -99, vsyncs = 0, fresh = 0, held = 0, skipped = 0, zig = 0, unknown = 0, moved = 0;
    XrTime prevPose = 0;
    int shown = -1;
    // Frames are taken in end order; a later end replaces an earlier one taken for the same or an earlier vsync.
    int* takenFor = malloc(sizeof(int) * (size_t)gLateN);
    char* wasShown = calloc((size_t)gLateN, 1);
    for (int i = 0; i < gLateN; i++) {
        int64_t e = gLateEv[i].end - gLateEv[i].wake;
        XrTime t = gLateEv[i].display + (e > limitNs ? gPeriod : 0);
        takenFor[i] = (int)((t - first + gPeriod / 2) / gPeriod);
    }
    int nv = (int)((last - first + gPeriod / 2) / gPeriod);
    for (int v = 0; v <= nv; v++) {
        // The newest frame (in end order) taken for this vsync or an earlier one.
        // takenFor never decreases (display times step by a refresh or more): the newest is the last.
        while (j < gLateN && takenFor[j] <= v) shown = j++;
        if (shown < 0) continue;
        vsyncs++;
        wasShown[shown] = 1;
        XrTime pose = gLateEv[shown].pose;
        if (!pose || !prevPose) { unknown++; prevPose = pose; prevStep = -99; continue; }
        int step = (int)lround((double)(pose - prevPose) / (double)gPeriod);
        if (step == 1) fresh++;
        else if (step == 0) held++;
        else moved++;
        if (prevStep != -99 && abs(step - prevStep) >= 2) zig++;
        prevStep = step;
        prevPose = pose;
    }
    for (int i = 0; i < j; i++) skipped += !wasShown[i];
    free(takenFor);
    free(wasShown);
    fprintf(stderr,
            "xrmock: late-end model (ends later than %.1f ms after the wake-up shown a refresh late): %d vsyncs; poses step 1: %d, "
            "0 (held): %d, other: %d, frames replaced before shown %d, no projection %d; zigzag %d (%.1f %%)\n",
            limit, vsyncs, fresh, held, moved, skipped, unknown, zig, 100.0 * zig / (vsyncs > 0 ? vsyncs : 1));
}
static void compReport(void) {
    lateReport();
    if (!gStampOn) return;
    fprintf(stderr, "xrmock: xrEndFrame after its xrWaitFrame returned (ms): <0.5 %d, 0.5-1 %d, 1-2 %d, 2-3 %d, 3-4 %d, 4-6 %d, >6 %d\n",
            gEndAfterWake[0], gEndAfterWake[1], gEndAfterWake[2], gEndAfterWake[3], gEndAfterWake[4], gEndAfterWake[5], gEndAfterWake[6]);
    if (gEndN < 100) return;
    compResolve(1);
    double cofs[2] = {3.0, 1.0};
    const char* co = getenv("MOCK_XR_COMPOSITE_MS");
    if (co) sscanf(co, "%lf,%lf", &cofs[0], &cofs[1]);
    const char* sel = getenv("MOCK_XR_COMPOSITE_SELECT");
    int byDisplay = !(sel && !strcmp(sel, "latest"));
    static const char* names[3] = {"spec", "lazyrel", "lazyacq"};
    int64_t k0 = (gEndEv[0].display - gTimeOffset - gT0) / gPeriod + 1;
    int64_t k1 = (gEndEv[gEndN - 1].t - gT0) / gPeriod - 1;
    for (int model = 0; model < 3; model++) {
        int n[2] = {0, 0}, unknown[2] = {0, 0}, own[2] = {0, 0}, again[2] = {0, 0}, hist[2][13];
        int differ = 0, both = 0;
        memset(hist, 0, sizeof(hist));
        uint64_t prev[2][9];
        int have[2][9];
        memset(have, 0, sizeof(have));
        for (int64_t k = k0; k <= k1; k++) {
            int64_t vsync = gT0 + k * gPeriod;
            uint64_t shown[2] = {0, 0};
            int ok[2] = {0, 0};
            for (int e = 0; e < 2; e++) {
                int64_t tr = vsync - (int64_t)(cofs[e] * 1e6);
                int lo = 0, hi = gEndN;
                while (lo < hi) { int m = (lo + hi) / 2; if (gEndEv[m].t <= tr) lo = m + 1; else hi = m; }
                int f = lo - 1;
                if (byDisplay)
                    while (f >= 0 && gEndEv[f].display - gTimeOffset > vsync + gPeriod / 2) f--;
                if (f < 0) continue;
                const EndEv* ev = &gEndEv[f];
                int idx = model == 0 ? ev->latched[e] : compLatest(model, ev->chain[e], tr);
                uint64_t value = 0;
                n[e]++;
                if (idx < 0 || !ev->pose || !compContent(ev->chain[e], idx, tr, &value)) { unknown[e]++; continue; }
                ok[e] = 1;
                shown[e] = value;
                long d = lround((double)((int64_t)value - (ev->pose - gTimeOffset)) / (double)gPeriod);
                if (d == 0) own[e]++;
                hist[e][d < -6 ? 0 : d > 6 ? 12 : d + 6]++;
                if (have[e][0] && value != prev[e][0])
                    for (int j = 1; j < 8; j++)
                        if (have[e][j] && prev[e][j] == value) { again[e]++; break; }
            }
            if (ok[0] && ok[1]) {
                both++;
                if (shown[0] != shown[1]) differ++;
            }
            for (int e = 0; e < 2; e++) {
                memmove(&prev[e][1], &prev[e][0], 8 * sizeof(uint64_t));
                memmove(&have[e][1], &have[e][0], 8 * sizeof(int));
                prev[e][0] = shown[e];
                have[e][0] = ok[e];
            }
        }
        char other[2][256];
        for (int e = 0; e < 2; e++) {
            other[e][0] = 0;
            for (int b = 0; b < 13; b++) {
                if (b == 6 || !hist[e][b]) continue;
                char t[32];
                snprintf(t, sizeof(t), " %s%d:%d", b == 0 ? "<=" : b == 12 ? ">=" : "", b - 6, hist[e][b]);
                strcat(other[e], t);
            }
        }
        fprintf(stderr,
                "xrmock: compositor %s (eyes read %.1f/%.1f ms before the vsync, frames by %s): %lld vsyncs; "
                "left: %d of %d with the poses it was rendered for (%.1f %%), off by refreshes:%s; shown again from 2-8 refreshes before %d (%.1f %%), unknown %d; "
                "right: %d of %d (%.1f %%), off:%s; again %d (%.1f %%), unknown %d; eyes from different sets %d of %d (%.1f %%)\n",
                names[model], cofs[0], cofs[1], byDisplay ? "display time" : "end", (long long)(k1 - k0 + 1),
                own[0], n[0] - unknown[0], 100.0 * own[0] / (n[0] - unknown[0] > 0 ? n[0] - unknown[0] : 1), other[0][0] ? other[0] : " none",
                again[0], 100.0 * again[0] / (n[0] > 0 ? n[0] : 1), unknown[0],
                own[1], n[1] - unknown[1], 100.0 * own[1] / (n[1] - unknown[1] > 0 ? n[1] - unknown[1] : 1), other[1][0] ? other[1] : " none",
                again[1], 100.0 * again[1] / (n[1] > 0 ? n[1] : 1), unknown[1], differ, both, 100.0 * differ / (both > 0 ? both : 1));
    }
}
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
// MOCK_XR_DUMP (see the top): the image being released, written at the dump times.
static void maybeDump(Chain* c, XrSwapchain sc) {
    static const char* dir = NULL;
    static double at[64];
    static int atCount = -1, frames = 1, step = 4;
    if (atCount < 0) {
        dir = getenv("MOCK_XR_DUMP");
        atCount = 0;
        const char* list = getenv("MOCK_XR_DUMP_AT");
        for (const char* q = list; q && *q && atCount < 64;) {
            at[atCount++] = atof(q);
            q = strchr(q, ',');
            if (q) q++;
        }
        if (!atCount)
            for (; atCount < 64; atCount++) at[atCount] = 10.0 * (atCount + 1);
        if (getenv("MOCK_XR_DUMP_FRAMES")) frames = atoi(getenv("MOCK_XR_DUMP_FRAMES"));
        if (getenv("MOCK_XR_DUMP_STEP") && atoi(getenv("MOCK_XR_DUMP_STEP")) >= 1) step = atoi(getenv("MOCK_XR_DUMP_STEP"));
    }
    if (!dir || !gGfx || !gGfx->dump_image || !gT0) return;
    double t = (double)(nowNs() - gT0) / 1e9;
    int mark = 0;
    while (mark < atCount && at[mark] <= t) mark++;
    if (mark == 0) return;
    if (c->dumpMark != mark) {
        c->dumpMark = mark;
        c->dumped = 0;
    }
    if (c->dumped >= frames || t - at[mark - 1] > 5.0) return;
    int idx = ((c->next - c->acquired) % c->count + c->count) % c->count;
    char path[512];
    snprintf(path, sizeof(path), "%s/t%05.1f-sc%d-%02d.ppm", dir, at[mark - 1], chainId(sc), c->dumped);
    if (gGfx->dump_image(c->images, idx, c->w, c->h, c->format, step, path)) trace("dump sc%d %s", chainId(sc), path);
    c->dumped++;
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
    if (busyCheck && gGfx && gGfx->release_snapshot) {
        // Exact: the image's bytes at the release against its bytes once all work queued
        // before the release completed (xrmock_vk.c).  Other work in the queue does not count.
        int idx = ((c->next - c->acquired) % c->count + c->count) % c->count;
        gGfx->release_snapshot(c->images, idx);
        gReleases++;
    } else if (busyCheck && gGfx && gGfx->release_wait_ms) {
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
    maybeDump(c, sc);
    {
        int idx = ((c->next - c->acquired) % c->count + c->count) % c->count;
        if (gStampOn && gGfx && gGfx->stamp_capture) {
            c->stampSlot = gGfx->stamp_capture(c->images, idx, c->w, c->h, &c->stampGen);
            compRelease(chainId(sc), idx, c->stampSlot, c->stampGen);
        }
        c->lastRel = idx;
    }
    c->acquired--;
    c->waited--;
    c->released = 1;
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
    if (gGfx && gGfx->poll_snapshots) {
        int changed = gGfx->poll_snapshots(1);
        gErrors += changed;
        gBusyReleases += changed;
    }
    if (gGfx && gGfx->snapshots_checked)
        fprintf(stderr, "xrmock: exact release check: %d releases compared after the GPU passed them, %d changed after release\n",
                gGfx->snapshots_checked(), gBusyReleases);
    fprintf(stderr,
            "xrmock: summary: %d errors; %d releases checked for finished GPU work, %d before it finished (worst %.1f ms); "
            "%d projection layers: eyes from one locate for the display time submitted %d, for an earlier one %d, a later one %d; eyes from "
            "different locates %d, poses not from a locate %d\n",
            gErrors, gReleases, gBusyReleases, gBusyMaxMs, gPoseSets, gPoseSameTime, gPoseEarlier, gPoseLater, gPoseSplit, gPoseUnknown);
    fprintf(stderr,
            "xrmock: end times: %d frames ended with their predicted display time, %d with another (within a refresh %d, a refresh or more "
            "earlier %d, later %d; worst %.3f ms)\n",
            gEndTimeMatch, gEndTimeOff, gEndTimeOffPeriods[0], gEndTimeOffPeriods[1], gEndTimeOffPeriods[2], gEndTimeOffWorstMs);
    fprintf(stderr, "xrmock: projection layers by refreshes from the locate to the display time: later %d, 0: %d, 1: %d, 2: %d, 3: %d, more %d\n", gPoseAge[0],
            gPoseAge[1], gPoseAge[2], gPoseAge[3], gPoseAge[4], gPoseAge[5]);
    fprintf(stderr,
            "xrmock: frames: %d ended; %d showed newly released eyes (from one locate for the display time submitted %d, for an earlier one %d, "
            "a later one %d), %d repeated a projection layer, %d a quad, %d no layer; focus blips %d\n",
            gEnds, gFreshSets, gFreshSame, gFreshEarlier, gFreshLater, gRepeatSets, gQuadEnds, gEmptyEnds, gBlips);
    if (gStampOn) {
        if (gGfx && gGfx->stamp_read) stampPoll(1);
        fprintf(stderr,
                "xrmock: stamps: %d eye images of projection layers checked: %d rendered for the locate of their submitted poses, %d for "
                "another (refreshes <=-2: %d, -1: %d, between: %d, +1: %d, >=+2: %d), %d with the other eye's stamp, %d without a stamp, %d "
                "not read back, %d layers with poses from no locate\n",
                gStampChecked, gStampMatch, gStampOther, gStampOff[0], gStampOff[1], gStampOff[2], gStampOff[3], gStampOff[4], gStampOtherEye,
                gStampNone, gStampLost, gStampUnknownPose);
        sceneReport();
    }
    if (gReleases) {
        fprintf(stderr, "xrmock: queue drain time at the releases: <0.5 ms %d, 0.5-1 %d, 1-2 %d, 2-4 %d, >4 %d\n", gWaitHist[0], gWaitHist[1], gWaitHist[2],
                gWaitHist[3], gWaitHist[4]);
    }
    compReport();
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
    if (gBlipMs > 0.0 && gFocused && !gQuitSent) {
        int64_t now = nowNs();
        if (!gNextBlip) gNextBlip = now + (int64_t)(gBlipEvery * 1e9);
        if (!gRefocusAt && now >= gNextBlip) {
            pushState(XR_SESSION_STATE_VISIBLE);
            gRefocusAt = now + (int64_t)(gBlipMs * 1e6);
            gBlips++;
            trace("focus blip %.0f ms", gBlipMs);
        } else if (gRefocusAt && now >= gRefocusAt) {
            pushState(XR_SESSION_STATE_FOCUSED);
            gRefocusAt = 0;
            gNextBlip = now + (int64_t)(gBlipEvery * 1e9);
        }
    }
    if (gEventCount == 0) return XR_EVENT_UNAVAILABLE;
    int st = gEventQueue[0];
    memmove(gEventQueue, gEventQueue + 1, (size_t)(--gEventCount) * sizeof(int));
    if (st == EVENT_PROFILE_CHANGED) {
        XrEventDataInteractionProfileChanged* p = (XrEventDataInteractionProfileChanged*)ev;
        p->type = XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED;
        p->next = NULL;
        p->session = (XrSession)(uintptr_t)2;
        trace("event interactionProfileChanged");
        return XR_SUCCESS;
    }
    if (st == XR_SESSION_STATE_FOCUSED && !gFocused) {
        gFocused = 1;
        if (gProfile && !gProfileSent) {
            gProfileSent = 1;
            if (gEventCount < 16) gEventQueue[gEventCount++] = EVENT_PROFILE_CHANGED;
        }
    }
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
    gWaitWakeSched = wake;
    if (wake > now) {
        struct timespec ts = {(time_t)((wake - now) / 1000000000ll), (long)((wake - now) % 1000000000ll)};
        nanosleep(&ts, NULL);
    }
    fs->predictedDisplayTime = wake + gLead * gPeriod + gTimeOffset;
    if (gJitterNs > 0) {
        // Deterministic: the same sequence every run.
        static uint64_t lcg = 0x2545F4914F6CDD1Dull;
        lcg = lcg * 6364136223846793005ull + 1442695040888963407ull;
        double u = (double)(lcg >> 11) / 9007199254740992.0;  // [0, 1)
        fs->predictedDisplayTime += (int64_t)((2.0 * u - 1.0) * (double)gJitterNs);
    }
    fs->predictedDisplayPeriod = gPeriod;
    fs->shouldRender = XR_TRUE;
    gWaitPredicted = fs->predictedDisplayTime;
    gWaits++;
    if (gEndAt && nowNs() > gEndAt && !gQuitSent) {
        gQuitSent = 1;
        pushState(XR_SESSION_STATE_STOPPING);
    }
    gFrame++;
    gLastWakeNs = nowNs();
    trace("waitFrame");
    if (gStampOn && gGfx && gGfx->stamp_read) stampPoll(0);
    compResolve(0);
    if (gGfx && gGfx->poll_snapshots) {
        int changed = gGfx->poll_snapshots(0);
        for (int k = 0; k < changed; k++) {
            trace("ERROR an image changed after its release: released before the GPU finished");
            gErrors++;
            gBusyReleases++;
        }
    }
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL mBeginFrame(XrSession s, const XrFrameBeginInfo* bi) {
    (void)s;
    (void)bi;
    gBegunPredicted = gWaitPredicted;
    gBegunWakeNs = gLastWakeNs;
    gBegunWakeSched = gWaitWakeSched;
    trace("beginFrame");
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL mEndFrame(XrSession s, const XrFrameEndInfo* ei) {
    (void)s;
    char buf[1024] = "";
    const XrCompositionLayerProjection* eyesLayer = NULL;
    int eyesLocated = -1;
    for (uint32_t i = 0; i < ei->layerCount; i++) {
        const XrCompositionLayerBaseHeader* l = ei->layers[i];
        char t[256];
        if (l->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
            const XrCompositionLayerProjection* p = (const XrCompositionLayerProjection*)l;
            int fresh = p->viewCount == 2 && chainOf(p->views[0].subImage.swapchain)->released &&
                        chainOf(p->views[1].subImage.swapchain)->released;
            if (i == 0) {
                if (fresh) gFreshSets++;
                else gRepeatSets++;
            }
            int located = checkPoses(p, ei->displayTime, fresh);
            if (!eyesLayer && p->viewCount == 2 && p->views[0].subImage.swapchain != p->views[1].subImage.swapchain) {
                eyesLayer = p;
                eyesLocated = located;
            }
            // A layer whose eyes share one image (packed side by side) carries no stamp.
            if (gStampOn && gGfx && gGfx->stamp_read && p->viewCount == 2 &&
                p->views[0].subImage.swapchain != p->views[1].subImage.swapchain) {
                for (int v = 0; v < 2; v++) {
                    Chain* ch = chainOf(p->views[v].subImage.swapchain);
                    if (located < 0) gStampUnknownPose++;
                    else if (ch->stampSlot < 0) gStampNone++;
                    else if (gStampPendingCount < STAMP_PENDING) {
                        gStampPending[gStampPendingCount].slot = ch->stampSlot;
                        gStampPending[gStampPendingCount].gen = ch->stampGen;
                        gStampPending[gStampPendingCount].eye = v;
                        gStampPending[gStampPendingCount].fresh = fresh;
                        gStampPending[gStampPendingCount].expect = locateTime(located);
                        gStampPendingCount++;
                    } else
                        gStampLost++;
                }
            }
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
    compEnd(eyesLayer, ei->displayTime, eyesLocated);
    // Display times increase from one xrEndFrame to the next (the app keeps them so; a runtime may
    // take an older one for a frame it already showed).
    static XrTime lastDisplay = 0;
    if (ei->displayTime <= lastDisplay) {
        trace("ERROR endFrame display time %.3f ms not after the previous one", (double)(ei->displayTime - lastDisplay) / 1e6);
        gErrors++;
    }
    lastDisplay = ei->displayTime;
    // The frame's own predicted time: SteamVR matches a frame to its refresh by it; an older time showed
    // as mispresented frames and reused app frames on the headset.
    if (gBegunPredicted && ei->displayTime != gBegunPredicted) {
        const double ms = (double)(ei->displayTime - gBegunPredicted) / 1e6;
        gEndTimeOff++;
        gEndTimeOffPeriods[ms > 0 ? 2 : -ms * 1e6 >= 0.5 * (double)gPeriod ? 1 : 0]++;
        if (fabs(ms) > fabs(gEndTimeOffWorstMs)) gEndTimeOffWorstMs = ms;
        trace("ERROR endFrame display time %.3f ms off the frame's predicted time", ms);
        gErrors++;
    } else if (gBegunPredicted) {
        gEndTimeMatch++;
    }
    gBegunPredicted = 0;
    gEnds++;
    if (ei->layerCount == 0) gEmptyEnds++;
    else if (ei->layers[0]->type == XR_TYPE_COMPOSITION_LAYER_QUAD) gQuadEnds++;
    for (int k = 0; k < gChainCount; k++) gChains[k].released = 0;
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
    double t = (double)(li->displayTime - gTimeOffset - gT0) / 1e9;
    float yaw = (float)(0.3 * sin(t * 2.0 * 3.14159265 * 0.5));
    float c = cosf(yaw), sn = sinf(yaw);
    for (uint32_t e = 0; e < 2 && e < cap; e++) {
        float x = e ? 0.032f : -0.032f;
        v[e].pose.orientation = (XrQuaternionf){0, sinf(yaw * 0.5f), 0, cosf(yaw * 0.5f)};
        // The head also rises 0.1 mm a second, so no two locates share a pose: the sway alone repeats
        // (yaw at t and at a time mirrored about a sine peak), and the pose check matched such a twin
        // locate now and then ("image rendered for a display time 183 ms after...").
        v[e].pose.position = (XrVector3f){x * c, 1.6f + 0.0001f * (float)t, -x * sn};
        const float rad = 3.14159265f / 180.0f;
        v[e].fov = (XrFovf){gFovDeg[e][0] * rad, gFovDeg[e][1] * rad, gFovDeg[e][2] * rad, gFovDeg[e][3] * rad};
        gLocates[gLocateNext].pose[e] = v[e].pose;
    }
    gLocates[gLocateNext].time = li->displayTime;
    gLocateNext = (gLocateNext + 1) % LOCATES;
    return XR_SUCCESS;
}

static XrTime locateTime(int i) { return gLocates[i].time; }
static int samePose(const XrPosef* a, const XrPosef* b) { return !memcmp(a, b, sizeof(XrPosef)); }

// Both eyes of a projection layer: from one locate, and for which time.
static int checkPoses(const XrCompositionLayerProjection* p, XrTime displayTime, int fresh) {
    if (p->viewCount != 2) return -1;
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
        return -1;
    }
    XrTime t = gLocates[both].time;
    if (t == displayTime) gPoseSameTime++;
    else if (t < displayTime) gPoseEarlier++;
    else gPoseLater++;
    if (fresh) {
        if (t == displayTime) gFreshSame++;
        else if (t < displayTime) gFreshEarlier++;
        else gFreshLater++;
    }
    long k = lround((double)(displayTime - t) / (double)gPeriod);
    gPoseAge[k < 0 ? 0 : k > 3 ? 5 : k + 1]++;
    return both;
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
static XrResult XRAPI_CALL mPathToString(XrInstance i, XrPath p, uint32_t cap, uint32_t* len, char* buf);
// The current profile's suggested bindings, and MOCK_XR_PRESS's scripted presses.
static struct {
    XrAction action;
    char path[128];
} gBindings[128];
static int gBindingCount = 0;
static struct {
    char path[128];
    double start, every;
} gPresses[16];
static int gPressCount = 0;
static const char* pathString(XrPath p) {
    for (int k = 0; k < gPathCount; k++)
        if (gPaths[k] == p) return gPathStr[k];
    return NULL;
}
// Whether the action, for the hand of `sub` (0: either), is held by a scripted press now.
static int pressed(XrAction action, XrPath sub, int* active) {
    *active = gPressCount > 0 && gProfileSent;
    if (!*active || !gT0) return 0;
    const char* hand = sub ? pathString(sub) : NULL;
    double t = (double)(nowNs() - gT0) / 1e9;
    for (int b = 0; b < gBindingCount; b++) {
        if (gBindings[b].action != action || (hand && strncmp(gBindings[b].path, hand, strlen(hand)) != 0)) continue;
        for (int k = 0; k < gPressCount; k++) {
            if (strcmp(gPresses[k].path, gBindings[b].path) != 0 || t < gPresses[k].start) continue;
            double phase = fmod(t - gPresses[k].start, gPresses[k].every);
            if (phase < 0.15) return 1;
        }
    }
    return 0;
}
static XrResult XRAPI_CALL mSuggest(XrInstance i, const XrInteractionProfileSuggestedBinding* b) {
    (void)i;
    char profile[128] = "";
    uint32_t len = 0;
    if (gProfile && mPathToString(i, b->interactionProfile, sizeof(profile), &len, profile) == XR_SUCCESS && !strcmp(profile, gProfile)) {
        trace("suggestBindings %s: %u bindings accepted", profile, b->countSuggestedBindings);
        gBindingCount = 0;
        for (uint32_t k = 0; k < b->countSuggestedBindings; k++) {
            char path[128] = "";
            if (mPathToString(i, b->suggestedBindings[k].binding, sizeof(path), &len, path) == XR_SUCCESS) {
                trace("  binding %s", path);
                if (gBindingCount < 128) {
                    gBindings[gBindingCount].action = b->suggestedBindings[k].action;
                    strncpy(gBindings[gBindingCount].path, path, sizeof(gBindings[0].path) - 1);
                    gBindingCount++;
                }
            }
        }
    }
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
    int active = 0;
    o->currentState = pressed(g->action, g->subactionPath, &active) ? XR_TRUE : XR_FALSE;
    o->isActive = active ? XR_TRUE : XR_FALSE;
    o->changedSinceLastSync = XR_FALSE;
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL mGetFloat(XrSession s, const XrActionStateGetInfo* g, XrActionStateFloat* o) {
    (void)s;
    int active = 0;
    o->currentState = pressed(g->action, g->subactionPath, &active) ? 1.0f : 0.0f;
    o->isActive = active ? XR_TRUE : XR_FALSE;
    o->changedSinceLastSync = XR_FALSE;
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL mGetVec2(XrSession s, const XrActionStateGetInfo* g, XrActionStateVector2f* o) {
    (void)s;
    int active = 0;
    pressed(g->action, g->subactionPath, &active);
    o->isActive = active ? XR_TRUE : XR_FALSE;
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
    if (gProfile && gProfileSent) mStringToPath((XrInstance)(uintptr_t)1, gProfile, &o->interactionProfile);
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL mReqRefresh(XrSession s, float hz) {
    (void)s;
    // Honoured before the session begins (the frame clock is fixed after that).
    // Honoured before the session begins (the frame clock is fixed after that), unless
    // MOCK_XR_HZ fixes the rate.
    const char* fixed = getenv("MOCK_XR_HZ");
    if (!gT0 && !fixed && (hz == 72.0f || hz == 90.0f || hz == 120.0f)) gPeriod = (int64_t)(1e9 / hz);
    trace("requestRefreshRate %.0f%s", hz, fixed ? " (MOCK_XR_HZ fixes the rate)" : gT0 ? " (session running: ignored)" : "");
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL mEnumRefresh(XrSession s, uint32_t cap, uint32_t* count, float* r) {
    (void)s;
    // MOCK_XR_RATES=<hz>,<hz>... (default 72,90,120); SteamVR on the Frame offered only 90 in H1.
    float rates[8] = {72, 90, 120};
    uint32_t n = 3;
    if (getenv("MOCK_XR_RATES")) {
        n = 0;
        for (const char* q = getenv("MOCK_XR_RATES"); q && *q && n < 8;) {
            rates[n++] = (float)atof(q);
            q = strchr(q, ',');
            if (q) q++;
        }
    }
    *count = n;
    if (cap)
        for (uint32_t i = 0; i < n && i < cap; i++) r[i] = rates[i];
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL mGetRefresh(XrSession s, float* hz) {
    (void)s;
    *hz = (float)(1e9 / (double)gPeriod);
    return XR_SUCCESS;
}
struct timespec;
static XrResult XRAPI_CALL mTimespecToTime(XrInstance i, const struct timespec* ts, XrTime* t) {
    (void)i;
    *t = (XrTime)ts->tv_sec * 1000000000ll + ts->tv_nsec + gTimeOffset;
    return XR_SUCCESS;
}
static XrResult XRAPI_CALL mTimeToTimespec(XrInstance i, XrTime t, struct timespec* ts) {
    (void)i;
    int64_t ns = (int64_t)t - gTimeOffset;
    ts->tv_sec = (time_t)(ns / 1000000000ll);
    ts->tv_nsec = (long)(ns % 1000000000ll);
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
    E("xrDestroySpace", mDestroySpace), E("xrEnumerateEnvironmentBlendModes", mEnumBlend), E("xrEnumerateReferenceSpaces", mEnumRefSpaces), E("xrDestroyActionSet", mDestroyActionSet), E("xrLocateSpace", mLocateSpace), E("xrEnumerateViewConfigurationViews", mEnumViewConfigViews),
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
    E("xrConvertTimespecTimeToTimeKHR", mTimespecToTime), E("xrConvertTimeToTimespecTimeKHR", mTimeToTimespec),
};

static void init(void) {
    static int done;
    if (done) return;
    done = 1;
    const char* tp = getenv("MOCK_XR_TRACE");
    if (tp) gTrace = fopen(tp, "w");
    const char* hz = getenv("MOCK_XR_HZ");
    if (hz) gPeriod = (int64_t)(1e9 / atof(hz));
    const char* eye = getenv("MOCK_XR_EYE");
    if (eye) sscanf(eye, "%dx%d", &gEyeW, &gEyeH);
    const char* off = getenv("MOCK_XR_TIME_OFFSET_S");
    if (off) gTimeOffset = (int64_t)(atof(off) * 1e9);
    const char* blip = getenv("MOCK_XR_FOCUS_BLIP");
    if (blip) {
        gBlipMs = atof(blip);
        const char* at = strchr(blip, '@');
        if (at) gBlipEvery = atof(at + 1);
    }
    gProfile = getenv("MOCK_XR_PROFILE");
    if (gProfile && !*gProfile) gProfile = NULL;
    gStampOn = getenv("MOCK_XR_STAMP") != NULL;
    gHands = getenv("MOCK_XR_HANDS") != NULL;
    const char* presses = getenv("MOCK_XR_PRESS");
    if (presses) {
        char* copy = strdup(presses);
        for (char* tok = strtok(copy, ";"); tok && gPressCount < 16; tok = strtok(NULL, ";")) {
            char* at = strchr(tok, '@');
            if (!at) continue;
            *at = 0;
            strncpy(gPresses[gPressCount].path, tok, sizeof(gPresses[0].path) - 1);
            gPresses[gPressCount].start = atof(at + 1);
            const char* every = strchr(at + 1, '/');
            gPresses[gPressCount].every = every ? atof(every + 1) : 1.0;
            if (gPresses[gPressCount].every < 0.3) gPresses[gPressCount].every = 0.3;
            gPressCount++;
        }
        free(copy);
    }
    const char* jitter = getenv("MOCK_XR_PREDICT_JITTER");
    if (jitter) {
        double ms = atof(jitter);
        gJitterNs = 0;
        gJitterNs = (int64_t)((ms < 0 ? 0 : ms > 3 ? 3 : ms) * 1e6);
    }
    const char* fov = getenv("MOCK_XR_FOV");
    if (fov && !strcmp(fov, "sym")) {
        gFovDeg[0][0] = -48.13f, gFovDeg[0][1] = 48.13f, gFovDeg[0][2] = 48.13f, gFovDeg[0][3] = -48.13f;
    } else if (fov) {
        sscanf(fov, "%f,%f,%f,%f", &gFovDeg[0][0], &gFovDeg[0][1], &gFovDeg[0][2], &gFovDeg[0][3]);
    }
    if (fov) {
        // The right eye mirrors the left (its outer side is on the right).
        gFovDeg[1][0] = -gFovDeg[0][1], gFovDeg[1][1] = -gFovDeg[0][0], gFovDeg[1][2] = gFovDeg[0][2], gFovDeg[1][3] = gFovDeg[0][3];
    }
    const char* lead = getenv("MOCK_XR_LEAD");
    if (lead && atoi(lead) >= 1) gLead = atoi(lead);
    const char* list = getenv("MOCK_XR_EXTENSIONS");
    if (list) {
        char* copy = strdup(list);
        for (char* tok = strtok(copy, ","); tok && gExtCount < 64; tok = strtok(NULL, ",")) strcpy(gExtNames[gExtCount++], tok);
        free(copy);
    } else {
        const char* exts[] = {"XR_KHR_opengl_es_enable", "XR_MNDX_egl_enable", "XR_KHR_vulkan_enable2", "XR_FB_display_refresh_rate",
                              "XR_EXT_performance_settings", "XR_VALVE_frame_controller_interaction", "XR_KHR_composition_layer_depth", "XR_EXT_eye_gaze_interaction",
                              "XR_KHR_convert_timespec_time"};
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
