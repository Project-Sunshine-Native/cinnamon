#ifndef CINNAMON_PROFILER_H
#define CINNAMON_PROFILER_H

#include <stdint.h>

#ifndef CINNAMON_PROFILE_REPORT_EVERY
#define CINNAMON_PROFILE_REPORT_EVERY 120
#endif

#ifndef CINNAMON_PROFILE_SPIKE_MS
#define CINNAMON_PROFILE_SPIKE_MS 25.0
#endif

typedef enum {
    CINNAMON_PROFILE_INPUT = 0,
    CINNAMON_PROFILE_STEP,
    CINNAMON_PROFILE_AUDIO,
    CINNAMON_PROFILE_RENDER,
    CINNAMON_PROFILE_PRESENT,
    CINNAMON_PROFILE_THROTTLE,
    CINNAMON_PROFILE_SECTION_COUNT
} CinnamonProfileSection;

#if defined(CINNAMON_PROFILE)
void CinnamonProfiler_init(uint32_t reportEveryFrames, double spikeFrameMs);
void CinnamonProfiler_beginFrame(uint64_t frameIndex);
void CinnamonProfiler_beginSection(CinnamonProfileSection section);
void CinnamonProfiler_endSection(CinnamonProfileSection section);
void CinnamonProfiler_endFrame(void);
#else
static inline void CinnamonProfiler_init(uint32_t reportEveryFrames, double spikeFrameMs) {
    (void) reportEveryFrames;
    (void) spikeFrameMs;
}

static inline void CinnamonProfiler_beginFrame(uint64_t frameIndex) {
    (void) frameIndex;
}

static inline void CinnamonProfiler_beginSection(CinnamonProfileSection section) {
    (void) section;
}

static inline void CinnamonProfiler_endSection(CinnamonProfileSection section) {
    (void) section;
}

static inline void CinnamonProfiler_endFrame(void) {
}
#endif

#endif
