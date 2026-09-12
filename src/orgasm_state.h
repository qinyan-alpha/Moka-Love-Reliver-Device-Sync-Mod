#pragma once

struct OrgasmSnapshot
{
    bool femaleOn = false;
    bool femaleAfter = false;
    float femaleOnTime = 0.0f;
    unsigned long long femaleTick = 0;
    bool maleNow = false;
    float maleStartTime = 0.0f;
    float maleLength = 0.0f;
    void* maleItemInfo = nullptr;
    unsigned long long maleStateTick = 0;
};

void OrgasmState_UpdateFemale(bool on, float onTime, bool after);
void OrgasmState_UpdateMaleState(bool now, float startTime, float length, void* itemInfo);
void OrgasmState_SetMaleLength(float length, void* itemInfo);
OrgasmSnapshot OrgasmState_GetSnapshot();
