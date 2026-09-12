#include <windows.h>

#include <MinHook.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>

#include "generated_motion_curves.h"
#include "motion_input.h"
#include "sr6_sync.h"

namespace
{
constexpr std::uintptr_t kRvaEpisode1UpdateMotionState = 0x421020;
constexpr std::uintptr_t kRvaEpisode2UpdateMotionState = 0x459B50;
constexpr std::uintptr_t kRvaPlayableHandleGetTimeInjected = 0x1C8AFC0;
constexpr std::uintptr_t kRvaPlayableHandleIsValidInjected = 0x1C8B120;

constexpr unsigned char kUpdateMotionStateSignature[] = {
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x60
};

using UpdateMotionStateFn = void(__fastcall*)(void* controller, void* motionState, void* methodInfo);
using GetPlayableTimeInjectedFn = double(__fastcall*)(const void* playableHandle);
using IsPlayableValidInjectedFn = bool(__fastcall*)(const void* playableHandle);

UpdateMotionStateFn g_episode1Original = nullptr;
UpdateMotionStateFn g_episode2Original = nullptr;
GetPlayableTimeInjectedFn g_getPlayableTime = nullptr;
IsPlayableValidInjectedFn g_isPlayableValid = nullptr;

std::atomic<int> g_lastEpisode1Motion{-1};
std::atomic<int> g_lastEpisode2Motion{-1};
std::ofstream g_log;
std::mutex g_logMutex;

struct SuppressedCurveCache
{
    const Live2DParameterCurve* curve = nullptr;
    float duration = 0.0f;
    int thresholdKey = -1;
    std::vector<float> times;
    std::vector<float> values;
    std::vector<float> slopes;
};

std::vector<SuppressedCurveCache> g_suppressedCurves;
std::mutex g_suppressedCurvesMutex;

struct MotionSetDataValue
{
    std::int32_t layer;
    std::int32_t motionId;
    void* startClip;
    void* loopClip;
    float blendTime;
    float startToLoopBlendTime;
    float startPlaySpeed;
    float loopPlaySpeed;
};

static_assert(sizeof(MotionSetDataValue) == 40, "MotionSetData layout changed");

std::string GetGameDirectory()
{
    char path[MAX_PATH]{};
    const DWORD length = GetModuleFileNameA(nullptr, path, MAX_PATH);
    if (!length || length >= MAX_PATH)
        return ".";
    std::string result(path, length);
    const std::size_t slash = result.find_last_of("\\/");
    return slash == std::string::npos ? "." : result.substr(0, slash);
}

void Log(const std::string& message)
{
    std::lock_guard<std::mutex> lock(g_logMutex);
    std::cout << message << std::endl;
    if (g_log.is_open())
    {
        g_log << message << std::endl;
        g_log.flush();
    }
}

void InitConsoleAndLog()
{
    AllocConsole();
    SetConsoleTitleW(L"Moka Love Relive - Live2D Motion Sync");
    SetConsoleOutputCP(CP_UTF8);

    FILE* stream = nullptr;
    freopen_s(&stream, "CONOUT$", "w", stdout);
    freopen_s(&stream, "CONOUT$", "w", stderr);
    freopen_s(&stream, "CONIN$", "r", stdin);
    std::ios::sync_with_stdio(true);

    Sr6Sync_SetConsoleWindow(GetConsoleWindow());
    g_log.open(GetGameDirectory() + "\\MokaLoveReliveMotionSync.log", std::ios::out | std::ios::app);

    Log("============================================================");
    Log(" Moka Love Relive - exact Live2D playable curve motion sync");
    Log(" Episode 1: PistonMain depth + PinstonAngle roll when present");
    Log(" Episode 2/3: BodyAngleUD3 depth + body pose axes when present");
    Log(" Serial output: TCode L0/L1/L2/R0/R1/R2 (unobservable axes centered)");
    Log("============================================================");
}

bool SignatureMatches(const unsigned char* address, const unsigned char* expected, std::size_t size)
{
    return address && std::equal(expected, expected + size, address);
}

float EvaluateCurve(const Live2DParameterCurve& curve, float time)
{
    if (!curve.keys || !curve.keyCount)
        return 0.0f;

    std::size_t lo = 0;
    std::size_t hi = curve.keyCount;
    while (lo < hi)
    {
        const std::size_t mid = lo + (hi - lo) / 2;
        if (curve.keys[mid].time <= time)
            lo = mid + 1;
        else
            hi = mid;
    }

    const Live2DCurveKey& key = curve.keys[lo == 0 ? 0 : lo - 1];
    const float delta = std::max(0.0f, time - key.time);
    return ((key.a * delta + key.b) * delta + key.c) * delta + key.d;
}

float EndpointPchipSlope(float h0, float h1, float d0, float d1)
{
    const float denominator = h0 + h1;
    float slope = denominator <= 0.000001f
        ? d0
        : ((2.0f * h0 + h1) * d0 - h0 * d1) / denominator;
    if (slope * d0 <= 0.0f)
        return 0.0f;
    if (d0 * d1 < 0.0f && std::abs(slope) > std::abs(3.0f * d0))
        return 3.0f * d0;
    return slope;
}

std::vector<float> BuildPchipSlopes(
    const std::vector<float>& times,
    const std::vector<float>& values)
{
    if (times.size() != values.size() || times.size() < 2)
        return {};

    const std::size_t count = times.size();
    std::vector<float> slopes(count, 0.0f);
    std::vector<float> h(count - 1, 0.0f);
    std::vector<float> delta(count - 1, 0.0f);
    for (std::size_t i = 0; i + 1 < count; ++i)
    {
        h[i] = std::max(0.000001f, times[i + 1] - times[i]);
        delta[i] = (values[i + 1] - values[i]) / h[i];
    }

    slopes.front() = EndpointPchipSlope(
        h.front(), h.size() > 1 ? h[1] : h.front(),
        delta.front(), delta.size() > 1 ? delta[1] : delta.front());
    slopes.back() = EndpointPchipSlope(
        h.back(), h.size() > 1 ? h[h.size() - 2] : h.back(),
        delta.back(), delta.size() > 1 ? delta[delta.size() - 2] : delta.back());

    for (std::size_t i = 1; i + 1 < count; ++i)
    {
        if (delta[i - 1] * delta[i] <= 0.0f)
            continue;
        const float w1 = 2.0f * h[i] + h[i - 1];
        const float w2 = h[i] + 2.0f * h[i - 1];
        slopes[i] = (w1 + w2) / (w1 / delta[i - 1] + w2 / delta[i]);
    }
    return slopes;
}

void BuildSuppressedCurve(
    SuppressedCurveCache& cache,
    const Live2DParameterCurve& curve,
    float duration,
    float threshold)
{
    cache.curve = &curve;
    cache.duration = duration;
    cache.thresholdKey = static_cast<int>(std::lround(threshold * 1000.0f));
    cache.times.clear();
    cache.values.clear();
    cache.slopes.clear();

    // Fixed cadence makes the extrema set independent of the render rate.
    const int sampleCount = std::max(2, static_cast<int>(std::ceil(duration * 120.0f)));
    std::vector<float> sampleTimes(static_cast<std::size_t>(sampleCount) + 1);
    std::vector<float> sampleValues(static_cast<std::size_t>(sampleCount) + 1);
    for (int i = 0; i <= sampleCount; ++i)
    {
        const float time = duration * static_cast<float>(i) / static_cast<float>(sampleCount);
        sampleTimes[static_cast<std::size_t>(i)] = time;
        sampleValues[static_cast<std::size_t>(i)] = EvaluateCurve(curve, time);
    }

    const auto minmax = std::minmax_element(sampleValues.begin(), sampleValues.end());
    const float span = *minmax.second - *minmax.first;
    if (span <= 0.000001f)
    {
        cache.times = { 0.0f, duration };
        cache.values = { sampleValues.front(), sampleValues.back() };
        cache.slopes = BuildPchipSlopes(cache.times, cache.values);
        return;
    }

    std::vector<std::size_t> points{ 0 };
    int previousSign = 0;
    for (std::size_t i = 1; i < sampleValues.size(); ++i)
    {
        const float delta = sampleValues[i] - sampleValues[i - 1];
        if (std::abs(delta) <= span * 0.001f)
            continue;
        const int sign = delta > 0.0f ? 1 : -1;
        if (previousSign != 0 && sign != previousSign && points.back() != i - 1)
            points.push_back(i - 1);
        previousSign = sign;
    }
    if (points.back() != sampleValues.size() - 1)
        points.push_back(sampleValues.size() - 1);

    const float minimumTurn = span * std::clamp(threshold, 0.0f, 0.5f);
    bool changed = false;
    do
    {
        changed = false;
        for (std::size_t i = 1; i + 1 < points.size(); ++i)
        {
            const float left = sampleValues[points[i - 1]];
            const float current = sampleValues[points[i]];
            const float right = sampleValues[points[i + 1]];
            const bool monotonic =
                (current >= left && current <= right) ||
                (current <= left && current >= right);
            const bool smallTurn = std::min(
                std::abs(current - left),
                std::abs(right - current)) < minimumTurn;
            if (!monotonic && !smallTurn)
                continue;
            points.erase(points.begin() + static_cast<std::ptrdiff_t>(i));
            changed = true;
            break;
        }
    } while (changed && points.size() > 2);

    cache.times.reserve(points.size());
    cache.values.reserve(points.size());
    for (const std::size_t point : points)
    {
        cache.times.push_back(sampleTimes[point]);
        cache.values.push_back(sampleValues[point]);
    }
    cache.slopes = BuildPchipSlopes(cache.times, cache.values);
}

float EvaluateSuppressedCurve(
    const Live2DParameterCurve& curve,
    float duration,
    float time,
    float threshold)
{
    const int thresholdKey = static_cast<int>(std::lround(threshold * 1000.0f));
    std::lock_guard<std::mutex> lock(g_suppressedCurvesMutex);
    auto found = std::find_if(
        g_suppressedCurves.begin(),
        g_suppressedCurves.end(),
        [&](const SuppressedCurveCache& item) { return item.curve == &curve; });
    if (found == g_suppressedCurves.end())
    {
        g_suppressedCurves.emplace_back();
        found = g_suppressedCurves.end() - 1;
    }
    if (found->thresholdKey != thresholdKey || std::abs(found->duration - duration) > 0.0001f)
        BuildSuppressedCurve(*found, curve, duration, threshold);

    if (found->times.size() < 2 || found->slopes.size() != found->times.size())
        return EvaluateCurve(curve, time);

    const auto upper = std::upper_bound(found->times.begin(), found->times.end(), time);
    const std::size_t segment = upper == found->times.begin()
        ? 0
        : std::min(static_cast<std::size_t>(upper - found->times.begin() - 1), found->times.size() - 2);
    const float x0 = found->times[segment];
    const float x1 = found->times[segment + 1];
    const float h = std::max(0.000001f, x1 - x0);
    const float t = std::clamp((time - x0) / h, 0.0f, 1.0f);
    const float t2 = t * t;
    const float t3 = t2 * t;
    const float value =
        (2.0f * t3 - 3.0f * t2 + 1.0f) * found->values[segment] +
        (t3 - 2.0f * t2 + t) * h * found->slopes[segment] +
        (-2.0f * t3 + 3.0f * t2) * found->values[segment + 1] +
        (t3 - t2) * h * found->slopes[segment + 1];
    const float low = std::min(found->values[segment], found->values[segment + 1]);
    const float high = std::max(found->values[segment], found->values[segment + 1]);
    return std::clamp(value, low, high);
}

float EvaluateCenteredParameter(
    const Live2DMotionCurve& motion,
    std::uint32_t pathHash,
    float clipTime,
    bool* available = nullptr)
{
    const Live2DParameterCurve* curve = FindLive2DParameterCurve(motion, pathHash);
    if (!curve || curve->maximum <= curve->minimum)
    {
        if (available)
            *available = false;
        return 0.0f;
    }

    if (available)
        *available = true;
    const float value = EvaluateCurve(*curve, clipTime);
    const float unit = std::clamp(
        (value - curve->minimum) / (curve->maximum - curve->minimum),
        0.0f,
        1.0f);
    return unit * 2.0f - 1.0f;
}

void SampleMotionState(void* motionState, int episode, std::atomic<int>& lastMotion)
{
    if (!motionState || !g_getPlayableTime || !g_isPlayableValid)
        return;

    const auto* bytes = static_cast<const unsigned char*>(motionState);
    const auto* motion = reinterpret_cast<const MotionSetDataValue*>(bytes + 0x50);
    if (motion->layer != 0)
        return;

    const Live2DMotionCurve* motionCurve = FindLive2DMotionCurve(episode, motion->motionId);
    if (!motionCurve || motionCurve->duration <= 0.0f)
        return;

    const Live2DParameterCurve* primary = FindLive2DParameterCurve(
        *motionCurve,
        episode == 1 ? kParamPistonMain : kParamBodyAngleUD3);
    if (!primary && episode != 1)
        primary = FindLive2DParameterCurve(*motionCurve, kParamBodyAngleY);
    if (!primary || primary->maximum <= primary->minimum)
        return;

    // Every target motion is a loop-only MotionSetData. Live2DAnimController.n/cs
    // stores its AnimationClipPlayable (and therefore PlayableHandle) at +0x30.
    const void* loopPlayableHandle = bytes + 0x30;
    if (!g_isPlayableValid(loopPlayableHandle))
        return;

    const double playableTime = g_getPlayableTime(loopPlayableHandle);
    if (!std::isfinite(playableTime) || playableTime < 0.0)
        return;

    // Stream the position for the next device update, not the already-past
    // position at this render frame. TCode then interpolates toward that
    // future point and reaches it just before the animation does.
    const double playbackSpeed = std::isfinite(motion->loopPlaySpeed)
        ? std::max(0.0, static_cast<double>(motion->loopPlaySpeed))
        : 1.0;
    const double lookaheadSeconds =
        static_cast<double>(MotionInput_GetLookaheadMs()) / 1000.0;
    const double targetPlayableTime = playableTime + lookaheadSeconds * playbackSpeed;
    const float clipTime = static_cast<float>(
        std::fmod(targetPlayableTime, static_cast<double>(motionCurve->duration)));
    float suppressionDeadband = 0.0f;
    const bool suppressionApplied =
        Sr6Sync_GetOscillationSuppression(suppressionDeadband) &&
        !Sr6Sync_IsSyntheticClimaxPeakActive();
    const float value = suppressionApplied
        ? EvaluateSuppressedCurve(*primary, motionCurve->duration, clipTime, suppressionDeadband)
        : EvaluateCurve(*primary, clipTime);

    // Insert transitions in the shipped assets run ParamPistonMain from +2 to -1.
    // Thus the lower parameter endpoint is the deeper physical position.
    const float insertion = std::clamp(
        (primary->maximum - value) / (primary->maximum - primary->minimum),
        0.0f,
        1.0f);

    Live2DAxisInput axes{};
    axes.insertionRatio = insertion;
    axes.oscillationSuppressed = suppressionApplied;

    // Live2D is a 2D rig, so only parameters that describe the body/piston are
    // allowed to drive secondary SR6 axes. Missing degrees of freedom stay at
    // the TCode centre instead of being synthesized from hair/face physics.
    if (episode == 1)
    {
        bool pistonAngleAvailable = false;
        const float pistonAngle = EvaluateCenteredParameter(
            *motionCurve,
            kParamPinstonAngle,
            clipTime,
            &pistonAngleAvailable);
        if (pistonAngleAvailable)
            axes.r1 = std::clamp(0.5f + pistonAngle * 0.28f, 0.0f, 1.0f);
    }
    else
    {
        bool bodyUdAvailable = false;
        bool bodyZAvailable = false;
        const float bodyUd = EvaluateCenteredParameter(
            *motionCurve, kParamBodyAngleUD, clipTime, &bodyUdAvailable);
        const float bodyZ = EvaluateCenteredParameter(
            *motionCurve, kParamBodyAngleZ, clipTime, &bodyZAvailable);

        if (bodyUdAvailable)
            axes.l1 = std::clamp(0.5f + bodyUd * 0.10f, 0.0f, 1.0f);
        if (bodyZAvailable)
            axes.r1 = std::clamp(0.5f + bodyZ * 0.18f, 0.0f, 1.0f);

        // BodyAngleY is also the insertion fallback for a few clips. Do not use
        // the same curve twice in those clips; keep pitch centered instead.
        if (primary->pathHash != kParamBodyAngleY)
        {
            bool bodyYAvailable = false;
            const float bodyY = EvaluateCenteredParameter(
                *motionCurve, kParamBodyAngleY, clipTime, &bodyYAvailable);
            if (bodyYAvailable)
                axes.r2 = std::clamp(0.5f + bodyY * 0.18f, 0.0f, 1.0f);
        }
    }
    MotionInput_UpdateLive2D(axes);

    const int previous = lastMotion.exchange(motion->motionId);
    if (previous != motion->motionId)
    {
        char line[320]{};
        std::snprintf(
            line,
            sizeof(line),
            "[Motion] episode=%d id=%d clip=%s duration=%.3fs speed=%.3f",
            episode,
            motion->motionId,
            motionCurve->clipName,
            motionCurve->duration,
            motion->loopPlaySpeed);
        Log(line);
    }
}

void __fastcall HookEpisode1UpdateMotionState(void* controller, void* motionState, void* methodInfo)
{
    g_episode1Original(controller, motionState, methodInfo);
    SampleMotionState(motionState, 1, g_lastEpisode1Motion);
}

void __fastcall HookEpisode2UpdateMotionState(void* controller, void* motionState, void* methodInfo)
{
    g_episode2Original(controller, motionState, methodInfo);
    SampleMotionState(motionState, 2, g_lastEpisode2Motion);
}

bool InstallHook(void* target, void* detour, void** original, const char* name)
{
    const MH_STATUS createStatus = MH_CreateHook(target, detour, original);
    if (createStatus != MH_OK)
    {
        Log(std::string("[Hook] create failed: ") + name + " status=" + std::to_string(createStatus));
        return false;
    }
    const MH_STATUS enableStatus = MH_EnableHook(target);
    if (enableStatus != MH_OK)
    {
        Log(std::string("[Hook] enable failed: ") + name + " status=" + std::to_string(enableStatus));
        return false;
    }
    Log(std::string("[Hook] installed: ") + name);
    return true;
}

DWORD WINAPI InitializeMod(void*)
{
    InitConsoleAndLog();
    Log("[Init] waiting for GameAssembly.dll...");

    HMODULE gameAssembly = nullptr;
    for (int i = 0; i < 600 && !gameAssembly; ++i)
    {
        gameAssembly = GetModuleHandleW(L"GameAssembly.dll");
        if (!gameAssembly)
            Sleep(100);
    }
    if (!gameAssembly)
    {
        Log("[Init] GameAssembly.dll was not loaded");
        return 1;
    }

    const auto base = reinterpret_cast<std::uintptr_t>(gameAssembly);
    auto* episode1Target = reinterpret_cast<unsigned char*>(base + kRvaEpisode1UpdateMotionState);
    auto* episode2Target = reinterpret_cast<unsigned char*>(base + kRvaEpisode2UpdateMotionState);
    if (!SignatureMatches(episode1Target, kUpdateMotionStateSignature, sizeof(kUpdateMotionStateSignature)) ||
        !SignatureMatches(episode2Target, kUpdateMotionStateSignature, sizeof(kUpdateMotionStateSignature)))
    {
        Log("[Init] GameAssembly signature mismatch; hooks were not installed (game update guard)");
        return 2;
    }

    g_getPlayableTime = reinterpret_cast<GetPlayableTimeInjectedFn>(base + kRvaPlayableHandleGetTimeInjected);
    g_isPlayableValid = reinterpret_cast<IsPlayableValidInjectedFn>(base + kRvaPlayableHandleIsValidInjected);

    if (MH_Initialize() != MH_OK)
    {
        Log("[Init] MH_Initialize failed");
        return 3;
    }

    const bool episode1Ok = InstallHook(
        episode1Target,
        reinterpret_cast<void*>(&HookEpisode1UpdateMotionState),
        reinterpret_cast<void**>(&g_episode1Original),
        "Live2DAnimController.bgx RVA 0x421020");
    const bool episode2Ok = InstallHook(
        episode2Target,
        reinterpret_cast<void*>(&HookEpisode2UpdateMotionState),
        reinterpret_cast<void**>(&g_episode2Original),
        "Live2DAnimController_2.cdw RVA 0x459B50");

    if (!episode1Ok || !episode2Ok)
    {
        Log("[Init] one or more hooks failed; SR6 output was not started");
        return 4;
    }

    Sr6Sync_Start();
    Log("[Init] ready; supported exact curves: " +
        std::to_string(sizeof(kLive2DMotionCurves) / sizeof(kLive2DMotionCurves[0])));
    return 0;
}
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(module);
        HANDLE thread = CreateThread(nullptr, 0, InitializeMod, nullptr, 0, nullptr);
        if (thread)
            CloseHandle(thread);
    }
    return TRUE;
}
