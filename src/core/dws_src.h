// DWS Input -- control-source names shared by the winmm and dinput proxies
// (the two that use a flat enum). dinput8 has its own richer Src struct.
// Identical to the copies that used to live in winmm_sdl3.cpp / dinput_sdl3.cpp.
#pragma once

#include <cstring>

enum Src {
    S_NONE,
    // axis sources
    A_LX, A_LY, A_RX, A_RY, A_LT, A_RT, A_TRIGGERS,
    // button sources
    B_A, B_B, B_X, B_Y, B_BACK, B_START, B_GUIDE, B_LSTICK, B_RSTICK,
    B_LSHOULDER, B_RSHOULDER, B_DUP, B_DDOWN, B_DLEFT, B_DRIGHT,
    B_MISC1, B_TOUCHPAD, B_LTRIGGER, B_RTRIGGER,
    S_COUNT
};

static const char* kSrcNames[S_COUNT] = {
    "None",
    "LeftX", "LeftY", "RightX", "RightY", "LeftTrigger", "RightTrigger", "Triggers",
    "A", "B", "X", "Y", "Back", "Start", "Guide", "LeftStick", "RightStick",
    "LeftShoulder", "RightShoulder", "DPadUp", "DPadDown", "DPadLeft", "DPadRight",
    "Misc1", "Touchpad", "LeftTrigger", "RightTrigger"
};

static Src ParseSrc(const char* s, bool axisCtx)
{
    for (int i = 0; i < S_COUNT; i++) {
        if (_stricmp(s, kSrcNames[i]) != 0) continue;
        // "LeftTrigger"/"RightTrigger" exist both as axis and button sources.
        if (axisCtx) {
            if (i == B_LTRIGGER) return A_LT;
            if (i == B_RTRIGGER) return A_RT;
            if (i >= A_LX && i <= A_TRIGGERS) return (Src)i;
            return S_NONE;
        } else {
            if (i == A_LT) return B_LTRIGGER;
            if (i == A_RT) return B_RTRIGGER;
            if (i >= B_A && i < S_COUNT) return (Src)i;
            return S_NONE;
        }
    }
    return S_NONE;
}
