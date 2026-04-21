#include "profiler.h"

#if defined(CINNAMON_PROFILE)

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#if defined(__3DS__)
#include <3ds.h>
#else
#include <sys/time.h>
#endif

typedef struct {
    const char* name;
    bool active;
    double startMs;
    double frameMs;
    double windowTotalMs;
    double windowMaxMs;
    uint64_t windowCalls;
} ProfileSectionState;

typedef struct {
    bool initialized;
    uint32_t reportEveryFrames;
    double spikeFrameMs;

    uint64_t frameIndex;
    double frameStartMs;
    double frameDurationMs;
    double windowFrameTotalMs;
    double windowFrameMaxMs;
    uint32_t windowFrameCount;

    ProfileSectionState sections[CINNAMON_PROFILE_SECTION_COUNT];
} ProfileState;

static ProfileState gProfile;

enum {
    CINNAMON_PROFILE_SPIKE_LOG_EVERY_FRAMES = 10,
};

static double profilerNowMs(void) {
#if defined(__3DS__)
    return (double) osGetTime();
#else
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return ((double) tv.tv_sec * 1000.0) + ((double) tv.tv_usec / 1000.0);
#endif
}

static void resetFrameSections(void) {
    for (int i = 0; i < CINNAMON_PROFILE_SECTION_COUNT; ++i) {
        gProfile.sections[i].active = false;
        gProfile.sections[i].startMs = 0.0;
        gProfile.sections[i].frameMs = 0.0;
    }
}

static bool shouldEmitSpikeLog(uint64_t frameIndex) {
    return (frameIndex % CINNAMON_PROFILE_SPIKE_LOG_EVERY_FRAMES) == 0;
}

static void ensureInitialized(void) {
    if (gProfile.initialized) return;

    memset(&gProfile, 0, sizeof(gProfile));
    gProfile.initialized = true;
    gProfile.reportEveryFrames = (uint32_t) CINNAMON_PROFILE_REPORT_EVERY;
    gProfile.spikeFrameMs = (double) CINNAMON_PROFILE_SPIKE_MS;

    gProfile.sections[CINNAMON_PROFILE_INPUT].name = "input";
    gProfile.sections[CINNAMON_PROFILE_STEP].name = "step";
    gProfile.sections[CINNAMON_PROFILE_AUDIO].name = "audio";
    gProfile.sections[CINNAMON_PROFILE_RENDER].name = "render";
    gProfile.sections[CINNAMON_PROFILE_PRESENT].name = "present";
    gProfile.sections[CINNAMON_PROFILE_THROTTLE].name = "throttle";

    fprintf(stderr,
            "Profiler: enabled (reportEvery=%u frames, spikeThreshold=%.2f ms)\n",
            gProfile.reportEveryFrames,
            gProfile.spikeFrameMs);
}

void CinnamonProfiler_init(uint32_t reportEveryFrames, double spikeFrameMs) {
    ensureInitialized();

    if (reportEveryFrames > 0) {
        gProfile.reportEveryFrames = reportEveryFrames;
    }
    if (spikeFrameMs > 0.0) {
        gProfile.spikeFrameMs = spikeFrameMs;
    }
}

void CinnamonProfiler_beginFrame(uint64_t frameIndex) {
    ensureInitialized();

    gProfile.frameIndex = frameIndex;
    gProfile.frameStartMs = profilerNowMs();
    gProfile.frameDurationMs = 0.0;

    resetFrameSections();
}

void CinnamonProfiler_beginSection(CinnamonProfileSection section) {
    if (section < 0 || section >= CINNAMON_PROFILE_SECTION_COUNT) return;
    if (gProfile.sections[section].active) return;

    gProfile.sections[section].active = true;
    gProfile.sections[section].startMs = profilerNowMs();
}

void CinnamonProfiler_endSection(CinnamonProfileSection section) {
    if (section < 0 || section >= CINNAMON_PROFILE_SECTION_COUNT) return;
    if (!gProfile.sections[section].active) return;

    double nowMs = profilerNowMs();
    double elapsedMs = nowMs - gProfile.sections[section].startMs;
    if (elapsedMs < 0.0) elapsedMs = 0.0;

    gProfile.sections[section].active = false;
    gProfile.sections[section].frameMs += elapsedMs;
}

static void printSpikeIfNeeded(void) {
    if (gProfile.frameDurationMs < gProfile.spikeFrameMs) return;
    if (!shouldEmitSpikeLog(gProfile.frameIndex)) return;

    fprintf(stderr,
            "Profiler: frame=%llu spike=%.2fms",
            (unsigned long long) gProfile.frameIndex,
            gProfile.frameDurationMs);

    for (int i = 0; i < CINNAMON_PROFILE_SECTION_COUNT; ++i) {
        double ms = gProfile.sections[i].frameMs;
        if (ms < 0.2) continue;
        fprintf(stderr, " %s=%.2f", gProfile.sections[i].name, ms);
    }

    fputc('\n', stderr);
}

static void printWindowReportIfNeeded(void) {
    if (gProfile.windowFrameCount == 0) return;
    if (gProfile.windowFrameCount < gProfile.reportEveryFrames) return;

    double avgFrame = gProfile.windowFrameTotalMs / (double) gProfile.windowFrameCount;
    double fps = avgFrame > 0.0 ? 1000.0 / avgFrame : 0.0;

    fprintf(stderr,
            "Profiler: avg over %u frames -> frame=%.2fms (%.1f FPS), max=%.2fms",
            gProfile.windowFrameCount,
            avgFrame,
            fps,
            gProfile.windowFrameMaxMs);

    for (int i = 0; i < CINNAMON_PROFILE_SECTION_COUNT; ++i) {
        ProfileSectionState* section = &gProfile.sections[i];
        double avgSection = section->windowTotalMs / (double) gProfile.windowFrameCount;
        double pct = avgFrame > 0.0 ? (avgSection * 100.0 / avgFrame) : 0.0;
        fprintf(stderr,
                " | %s avg=%.2fms(%.0f%%) max=%.2fms",
                section->name,
                avgSection,
                pct,
                section->windowMaxMs);

        section->windowTotalMs = 0.0;
        section->windowMaxMs = 0.0;
        section->windowCalls = 0;
    }

    fputc('\n', stderr);

    gProfile.windowFrameTotalMs = 0.0;
    gProfile.windowFrameMaxMs = 0.0;
    gProfile.windowFrameCount = 0;
}

void CinnamonProfiler_endFrame(void) {
    double frameEndMs = profilerNowMs();

    for (int i = 0; i < CINNAMON_PROFILE_SECTION_COUNT; ++i) {
        if (gProfile.sections[i].active) {
            CinnamonProfiler_endSection((CinnamonProfileSection) i);
        }
    }

    gProfile.frameDurationMs = frameEndMs - gProfile.frameStartMs;
    if (gProfile.frameDurationMs < 0.0) gProfile.frameDurationMs = 0.0;

    gProfile.windowFrameTotalMs += gProfile.frameDurationMs;
    if (gProfile.frameDurationMs > gProfile.windowFrameMaxMs) {
        gProfile.windowFrameMaxMs = gProfile.frameDurationMs;
    }
    gProfile.windowFrameCount++;

    for (int i = 0; i < CINNAMON_PROFILE_SECTION_COUNT; ++i) {
        ProfileSectionState* section = &gProfile.sections[i];
        section->windowTotalMs += section->frameMs;
        if (section->frameMs > section->windowMaxMs) {
            section->windowMaxMs = section->frameMs;
        }
        section->windowCalls++;
    }

    printSpikeIfNeeded();
    printWindowReportIfNeeded();
}

#endif