#pragma once

#include <stdint.h>

// Called by D_DoomLoop, sets the time for the current frame
void I_SetFrameTime();

// Called by D_DoomLoop, returns current time in tics.
int I_GetTime();

double I_GetTimeFrac(uint32_t *ms);

// like I_GetTime, except it waits for a new tic before returning
int I_WaitForTic(int);

// Freezes tic counting temporarily. While frozen, calls to I_GetTime()
// will always return the same value. This does not affect I_MSTime().
// You must also not call I_WaitForTic() while freezing time, since the
// tic will never arrive (unless it's the current one).
void I_FreezeTime(bool frozen);

// [RH] Returns millisecond-accurate time
unsigned int I_MSTime();
unsigned int I_FPSTime();
