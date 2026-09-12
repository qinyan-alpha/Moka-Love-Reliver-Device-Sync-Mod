#include "motion_input.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <mutex>

namespace
{
constexpr unsigned long long kFreshInputWindowMs = 500;

struct MotionInputState
{
    bool valid = false;
    Live2DAxisInput input{};
    unsigned long long tick = 0;
};

std::mutex g_motionMutex;
MotionInputState g_live2DInput;
std::atomic<unsigned int> g_lookaheadMs{100};

float Clamp01(float value)
{
    return std::max(0.0f, std::min(1.0f, value));
}
}

const char* MotionInputSourceName(MotionInputSource source)
{
    return source == MotionInputSource::Live2DPlayable ? "Live2D playable curve" : "none";
}

void MotionInput_SetLookaheadMs(unsigned int lookaheadMs)
{
    g_lookaheadMs.store(std::min(lookaheadMs, 500u), std::memory_order_relaxed);
}

unsigned int MotionInput_GetLookaheadMs()
{
    return g_lookaheadMs.load(std::memory_order_relaxed);
}

void MotionInput_UpdateLive2D(const Live2DAxisInput& input)
{
    std::lock_guard<std::mutex> lock(g_motionMutex);
    g_live2DInput.valid = true;
    g_live2DInput.input.insertionRatio = Clamp01(input.insertionRatio);
    g_live2DInput.input.l1 = Clamp01(input.l1);
    g_live2DInput.input.l2 = Clamp01(input.l2);
    g_live2DInput.input.r0 = Clamp01(input.r0);
    g_live2DInput.input.r1 = Clamp01(input.r1);
    g_live2DInput.input.r2 = Clamp01(input.r2);
    g_live2DInput.tick = GetTickCount64();
}

MotionInputSnapshot MotionInput_GetSelected()
{
    const unsigned long long now = GetTickCount64();
    std::lock_guard<std::mutex> lock(g_motionMutex);

    if (!g_live2DInput.valid || now < g_live2DInput.tick || now - g_live2DInput.tick > kFreshInputWindowMs)
        return MotionInputSnapshot{};

    const Live2DAxisInput axes = g_live2DInput.input;
    const float ratio = axes.insertionRatio;
    MotionInputSnapshot snapshot{};
    snapshot.inserted = true;
    snapshot.x = ratio * 2.0f - 1.0f;
    snapshot.y = ratio;
    snapshot.xPb = snapshot.x;
    snapshot.yPb = snapshot.y;
    snapshot.l0 = Clamp01(1.0f - ratio);
    snapshot.l1 = axes.l1;
    snapshot.l2 = axes.l2;
    snapshot.r0 = axes.r0;
    snapshot.r1 = axes.r1;
    snapshot.r2 = axes.r2;
    snapshot.source = MotionInputSource::Live2DPlayable;
    snapshot.oscillationSuppressed = axes.oscillationSuppressed;
    snapshot.ageMs = now - g_live2DInput.tick;
    return snapshot;
}
