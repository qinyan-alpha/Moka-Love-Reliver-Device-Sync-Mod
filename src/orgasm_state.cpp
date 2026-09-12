#include "orgasm_state.h"

#include <windows.h>

#include <mutex>

namespace
{
std::mutex g_orgasmMutex;
OrgasmSnapshot g_orgasm;
}

void OrgasmState_UpdateFemale(bool on, float onTime, bool after)
{
    std::lock_guard<std::mutex> lock(g_orgasmMutex);
    g_orgasm.femaleOn = on;
    g_orgasm.femaleOnTime = onTime;
    g_orgasm.femaleAfter = after;
    g_orgasm.femaleTick = GetTickCount64();
}

void OrgasmState_UpdateMaleState(bool now, float startTime, float length, void* itemInfo)
{
    std::lock_guard<std::mutex> lock(g_orgasmMutex);
    g_orgasm.maleNow = now;
    g_orgasm.maleStartTime = startTime;
    if (length > 0.0f)
        g_orgasm.maleLength = length;
    if (itemInfo)
        g_orgasm.maleItemInfo = itemInfo;
    g_orgasm.maleStateTick = GetTickCount64();
}

void OrgasmState_SetMaleLength(float length, void* itemInfo)
{
    std::lock_guard<std::mutex> lock(g_orgasmMutex);
    if (length > 0.0f)
        g_orgasm.maleLength = length;
    if (itemInfo)
        g_orgasm.maleItemInfo = itemInfo;
    g_orgasm.maleStateTick = GetTickCount64();
}

OrgasmSnapshot OrgasmState_GetSnapshot()
{
    std::lock_guard<std::mutex> lock(g_orgasmMutex);
    return g_orgasm;
}
