#pragma once

enum class MotionInputSource
{
    None,
    Live2DPlayable
};

struct MotionInputSnapshot
{
    bool inserted = false;
    // Legacy x/y remain available for Intiface and the existing GUI.
    float x = 0.0f;
    float y = 0.0f;
    float xPb = 0.0f;
    float yPb = 0.0f;
    // Normalized TCode axes. L0 uses the TCode direction (0=down/deep,
    // 0.999=up/out); every other axis is centered at 0.5 when unavailable.
    float l0 = 0.999f;
    float l1 = 0.5f;
    float l2 = 0.5f;
    float r0 = 0.5f;
    float r1 = 0.5f;
    float r2 = 0.5f;
    MotionInputSource source = MotionInputSource::None;
    bool oscillationSuppressed = false;
    unsigned long long ageMs = 0;
};

struct Live2DAxisInput
{
    float insertionRatio = 0.0f;
    float l1 = 0.5f;
    float l2 = 0.5f;
    float r0 = 0.5f;
    float r1 = 0.5f;
    float r2 = 0.5f;
    bool oscillationSuppressed = false;
};

const char* MotionInputSourceName(MotionInputSource source);
void MotionInput_SetLookaheadMs(unsigned int lookaheadMs);
unsigned int MotionInput_GetLookaheadMs();
void MotionInput_UpdateLive2D(const Live2DAxisInput& input);
MotionInputSnapshot MotionInput_GetSelected();
