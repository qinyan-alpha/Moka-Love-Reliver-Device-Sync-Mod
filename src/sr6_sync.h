#pragma once
#include <windows.h>
void Sr6Sync_SetConsoleWindow(HWND window);
void Sr6Sync_Start();
void Sr6Sync_Stop();
bool Sr6Sync_GetOscillationSuppression(float& deadband);
bool Sr6Sync_IsSyntheticClimaxPeakActive();
