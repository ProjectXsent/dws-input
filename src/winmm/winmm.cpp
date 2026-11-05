// winmm.dll -- proxy that presents an SDL3 gamepad through the legacy winmm
// joystick API (joyGetNumDevs / joyGetDevCapsA/W / joyGetPos / joyGetPosEx).
// Part of DWS Input. Drop next to the game exe together with SDL3.dll and
// dws_input.ini (shared with dinput.dll / dinput8.dll).
//
// Every other winmm export (waveOut, mmio, mci, timeGetTime, ...) is a linker
// forwarder to the real C:\Windows\System32\winmm.dll, so no code runs for them.
//
// Build: see the top-level CMakeLists.txt (winmm.def is attached to the target).
// Do NOT link winmm.lib.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define WINMMAPI            // we define the joy* exports ourselves, so no dllimport
#include <windows.h>
#include <mmsystem.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "dws_core.h"
#include "dws_src.h"

// ----------------------------------------------------------------------------
// Forwarders to the real winmm.dll (everything we do not implement)
// ----------------------------------------------------------------------------

#define FWD(n) __pragma(comment(linker, "/export:" #n "=C:\\Windows\\System32\\winmm." #n))

FWD(CloseDriver) FWD(DefDriverProc) FWD(DriverCallback) FWD(DrvGetModuleHandle)
FWD(GetDriverModuleHandle) FWD(NotifyCallbackData) FWD(OpenDriver)
FWD(PlaySound) FWD(PlaySoundA) FWD(PlaySoundW) FWD(SendDriverMessage)
FWD(WOW32DriverCallback) FWD(WOW32ResolveMultiMediaHandle) FWD(WOWAppExit)
FWD(aux32Message) FWD(auxGetDevCapsA) FWD(auxGetDevCapsW) FWD(auxGetNumDevs)
FWD(auxGetVolume) FWD(auxOutMessage) FWD(auxSetVolume)
FWD(joy32Message) FWD(joyConfigChanged) FWD(joyGetThreshold) FWD(joyReleaseCapture)
FWD(joySetCapture) FWD(joySetThreshold)
FWD(mci32Message) FWD(mciDriverNotify) FWD(mciDriverYield) FWD(mciExecute)
FWD(mciFreeCommandResource) FWD(mciGetCreatorTask) FWD(mciGetDeviceIDA)
FWD(mciGetDeviceIDFromElementIDA) FWD(mciGetDeviceIDFromElementIDW) FWD(mciGetDeviceIDW)
FWD(mciGetDriverData) FWD(mciGetErrorStringA) FWD(mciGetErrorStringW) FWD(mciGetYieldProc)
FWD(mciLoadCommandResource) FWD(mciSendCommandA) FWD(mciSendCommandW) FWD(mciSendStringA)
FWD(mciSendStringW) FWD(mciSetDriverData) FWD(mciSetYieldProc)
FWD(mid32Message) FWD(midiConnect) FWD(midiDisconnect) FWD(midiInAddBuffer) FWD(midiInClose)
FWD(midiInGetDevCapsA) FWD(midiInGetDevCapsW) FWD(midiInGetErrorTextA) FWD(midiInGetErrorTextW)
FWD(midiInGetID) FWD(midiInGetNumDevs) FWD(midiInMessage) FWD(midiInOpen)
FWD(midiInPrepareHeader) FWD(midiInReset) FWD(midiInStart) FWD(midiInStop)
FWD(midiInUnprepareHeader) FWD(midiOutCacheDrumPatches) FWD(midiOutCachePatches)
FWD(midiOutClose) FWD(midiOutGetDevCapsA) FWD(midiOutGetDevCapsW) FWD(midiOutGetErrorTextA)
FWD(midiOutGetErrorTextW) FWD(midiOutGetID) FWD(midiOutGetNumDevs) FWD(midiOutGetVolume)
FWD(midiOutLongMsg) FWD(midiOutMessage) FWD(midiOutOpen) FWD(midiOutPrepareHeader)
FWD(midiOutReset) FWD(midiOutSetVolume) FWD(midiOutShortMsg) FWD(midiOutUnprepareHeader)
FWD(midiStreamClose) FWD(midiStreamOpen) FWD(midiStreamOut) FWD(midiStreamPause)
FWD(midiStreamPosition) FWD(midiStreamProperty) FWD(midiStreamRestart) FWD(midiStreamStop)
FWD(mixerClose) FWD(mixerGetControlDetailsA) FWD(mixerGetControlDetailsW) FWD(mixerGetDevCapsA)
FWD(mixerGetDevCapsW) FWD(mixerGetID) FWD(mixerGetLineControlsA) FWD(mixerGetLineControlsW)
FWD(mixerGetLineInfoA) FWD(mixerGetLineInfoW) FWD(mixerGetNumDevs) FWD(mixerMessage)
FWD(mixerOpen) FWD(mixerSetControlDetails)
FWD(mmDrvInstall) FWD(mmGetCurrentTask) FWD(mmTaskBlock) FWD(mmTaskCreate) FWD(mmTaskSignal)
FWD(mmTaskYield)
FWD(mmioAdvance) FWD(mmioAscend) FWD(mmioClose) FWD(mmioCreateChunk) FWD(mmioDescend)
FWD(mmioFlush) FWD(mmioGetInfo) FWD(mmioInstallIOProcA) FWD(mmioInstallIOProcW) FWD(mmioOpenA)
FWD(mmioOpenW) FWD(mmioRead) FWD(mmioRenameA) FWD(mmioRenameW) FWD(mmioSendMessage)
FWD(mmioSeek) FWD(mmioSetBuffer) FWD(mmioSetInfo) FWD(mmioStringToFOURCCA)
FWD(mmioStringToFOURCCW) FWD(mmioWrite)
FWD(mod32Message) FWD(mxd32Message) FWD(sndPlaySoundA) FWD(sndPlaySoundW) FWD(tid32Message)
FWD(timeBeginPeriod) FWD(timeEndPeriod) FWD(timeGetDevCaps) FWD(timeGetSystemTime)
FWD(timeGetTime) FWD(timeKillEvent) FWD(timeSetEvent)
FWD(waveInAddBuffer) FWD(waveInClose) FWD(waveInGetDevCapsA) FWD(waveInGetDevCapsW)
FWD(waveInGetErrorTextA) FWD(waveInGetErrorTextW) FWD(waveInGetID) FWD(waveInGetNumDevs)
FWD(waveInMessage) FWD(waveInOpen) FWD(waveInPrepareHeader) FWD(waveInReset) FWD(waveInStart)
FWD(waveInStop) FWD(waveInUnprepareHeader)
FWD(waveOutBreakLoop) FWD(waveOutClose) FWD(waveOutGetDevCapsA) FWD(waveOutGetDevCapsW)
FWD(waveOutGetErrorTextA) FWD(waveOutGetErrorTextW) FWD(waveOutGetID) FWD(waveOutGetNumDevs)
FWD(waveOutGetPitch) FWD(waveOutGetPlaybackRate) FWD(waveOutGetPosition) FWD(waveOutGetVolume)
FWD(waveOutMessage) FWD(waveOutOpen) FWD(waveOutPause) FWD(waveOutPrepareHeader)
FWD(waveOutReset) FWD(waveOutRestart) FWD(waveOutSetPitch) FWD(waveOutSetPlaybackRate)
FWD(waveOutSetVolume) FWD(waveOutUnprepareHeader) FWD(waveOutWrite)
FWD(wid32Message) FWD(wod32Message)

// ----------------------------------------------------------------------------
// Module / config
// ----------------------------------------------------------------------------

struct Config {
    bool log = false;
    bool hideRealPads = true;
    bool alwaysPresent = true;
    int padIndex = 0;       // which SDL gamepad
    int joyId = 0;          // which winmm joystick ID (joyGetPosEx uJoyID) the SDL pad occupies
    char productName[128] = "SDL Gamepad";
    unsigned vid = 0x045E, pid = 0x028E;

    int stickDeadzone = 8;
    int triggerThreshold = 30;
    bool dpadAsPov = true;
    int dpadStick = 1;     // 0 = off, 1 = d-pad drives the left stick axes, 2 = right stick axes

    // winmm axis order: X, Y, Z, R, U, V
    Src axis[6] = { A_LX, A_LY, A_TRIGGERS, A_RY, A_RX, S_NONE };
    bool invert[6] = {};
    Src button[32] = {};
    int buttonCount = 0;   // effective count
} g_cfg;

static std::string g_dir;   // folder containing this dll

static FILE* g_logf = nullptr;
static void Log(const char* fmt, ...)
{
    if (!g_logf) return;
    va_list ap; va_start(ap, fmt);
    vfprintf(g_logf, fmt, ap);
    va_end(ap);
    fputc('\n', g_logf);
    fflush(g_logf);
}

// Logs only the first time a given call site is reached.
#define LOG_ONCE(...) do { static bool _once = false; if (!_once) { _once = true; Log(__VA_ARGS__); } } while (0)

static void LoadConfig()
{
    char buf[256];
    dws::Ini ini;
    ini.Open(g_dir, "WinMM");
    const char* p = ini.Path().c_str();

    auto geti = [&](const char* sec, const char* key, int def) {
        return ini.GetInt(sec, key, def);
    };
    auto gets = [&](const char* sec, const char* key, const char* def, char* out, DWORD n) {
        ini.GetStr(sec, key, def, out, n);
    };
    const std::string axSec = ini.AxesSection();     // "Axes.WinMM"

    Config c;
    c.log = geti("General", "Log", 0) != 0;
    c.hideRealPads = geti("General", "HideRealPads", 1) != 0;
    c.alwaysPresent = geti("General", "AlwaysPresent", 1) != 0;
    c.padIndex = std::max(0, geti("General", "PadIndex", 0));
    c.joyId = std::min(15, std::max(0, geti("General", "JoystickID", 0)));   // usually set in [WinMM]
    gets("General", "ProductName", "SDL Gamepad", c.productName, sizeof c.productName);
    gets("General", "VendorID", "045E", buf, sizeof buf);  c.vid = strtoul(buf, nullptr, 16) & 0xFFFF;
    gets("General", "ProductID", "028E", buf, sizeof buf); c.pid = strtoul(buf, nullptr, 16) & 0xFFFF;

    c.stickDeadzone = std::min(90, std::max(0, geti("Input", "StickDeadzone", 8)));
    c.triggerThreshold = std::min(100, std::max(0, geti("Input", "TriggerThreshold", 30)));
    c.dpadAsPov = geti("Input", "DPadAsPOV", 1) != 0;
    gets("Input", "DPadAsStick", "Left", buf, sizeof buf);
    if (!_stricmp(buf, "Left") || !_stricmp(buf, "LeftStick")) c.dpadStick = 1;
    else if (!_stricmp(buf, "Right") || !_stricmp(buf, "RightStick")) c.dpadStick = 2;
    else c.dpadStick = 0;

    static const char* defAxis[6] = { "LeftX", "LeftY", "Triggers", "RightY", "RightX", "None" };
    for (int i = 0; i < 6; i++) {
        char key[32];
        snprintf(key, sizeof key, "Axis%d", i);
        gets(axSec.c_str(), key, defAxis[i], buf, sizeof buf);
        c.axis[i] = ParseSrc(buf, true);
        snprintf(key, sizeof key, "Invert%d", i);
        c.invert[i] = geti(axSec.c_str(), key, 0) != 0;
    }

    static const char* defBtn[12] = { "A", "B", "X", "Y", "LeftShoulder", "RightShoulder",
        "LeftTrigger", "RightTrigger", "Back", "Start", "LeftStick", "RightStick" };
    int highest = -1;
    for (int i = 0; i < 32; i++) {
        char key[32];
        snprintf(key, sizeof key, "Button%d", i);
        gets("Buttons", key, i < 12 ? defBtn[i] : "None", buf, sizeof buf);
        c.button[i] = ParseSrc(buf, false);
        if (c.button[i] != S_NONE) highest = i;
    }
    int forced = geti("Buttons", "ButtonCount", 0);
    c.buttonCount = forced > 0 ? std::min(32, forced) : highest + 1;

    g_cfg = c;

    if (g_cfg.log) {
        std::string lp = dws::LogPath(g_dir, "WinMM");
        g_logf = fopen(lp.c_str(), "w");
        Log("winmm.dll (DWS Input, SDL3) -- effective settings (%s)%s", p,
            ini.Exists() ? "" : "  [file not found, built-in defaults]");
        Log("[General] HideRealPads=%d AlwaysPresent=%d PadIndex=%d JoystickID=%d ProductName=%s VID=%04X PID=%04X",
            c.hideRealPads, c.alwaysPresent, c.padIndex, c.joyId, c.productName, c.vid, c.pid);
        Log("[Input] StickDeadzone=%d TriggerThreshold=%d DPadAsPOV=%d",
            c.stickDeadzone, c.triggerThreshold, c.dpadAsPov);
        Log("[Input] DPadAsStick=%s", c.dpadStick == 1 ? "Left" : c.dpadStick == 2 ? "Right" : "None");
        static const char* axn[6] = { "X", "Y", "Z", "R", "U", "V" };
        for (int i = 0; i < 6; i++)
            Log("[%s] Axis%d(%s)=%s Invert%d=%d", axSec.c_str(), i, axn[i], kSrcNames[c.axis[i]], i, c.invert[i]);
        Log("[Buttons] ButtonCount=%d", c.buttonCount);
        for (int i = 0; i < c.buttonCount; i++) Log("  Button%d=%s", i, kSrcNames[c.button[i]]);
    }
}

static void EnsureConfig()
{
    static bool done = false;
    if (done) return;
    done = true;
    LoadConfig();
}

// ----------------------------------------------------------------------------
// SDL3 pad
// ----------------------------------------------------------------------------

struct Snap {
    float ax[6];
    bool btn[32];
    DWORD pov;      // hundredths of a degree, 0xFFFF = centered (JOY_POVCENTERED)
};

// Thread currently inside SDL initialisation. SDL (or something it loads) may call
// winmm joy* itself; those calls must go to the real winmm instead of re-entering us.
static volatile DWORD g_initTid = 0;
static bool InInit() { return g_initTid != 0 && g_initTid == GetCurrentThreadId(); }

struct Pad {
    CRITICAL_SECTION cs;
    bool csInit = false;
    bool sdlOk = false;
    SDL_Gamepad* gp = nullptr;
    DWORD lastScan = 0;
    int lastCount = -1;
    int lastJoyCount = -1;

    void lock() {
        if (!csInit) { InitializeCriticalSection(&cs); csInit = true; }
        EnterCriticalSection(&cs);
    }
    void unlock() { LeaveCriticalSection(&cs); }

    // call with lock held
    void ensureInit() {
        if (sdlOk) return;
        EnsureConfig();
        g_initTid = GetCurrentThreadId();
        SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
        if (!SDL_Init(SDL_INIT_GAMEPAD)) {
            Log("SDL_Init failed: %s", SDL_GetError());
            g_initTid = 0;
            return;
        }
        // We poll state ourselves; don't let events pile up in the queue.
        SDL_SetGamepadEventsEnabled(false);
        SDL_SetJoystickEventsEnabled(false);
        sdlOk = true;
        {
            int v = SDL_GetVersion();
            Log("SDL_Init ok (SDL %d.%d.%d)", SDL_VERSIONNUM_MAJOR(v), SDL_VERSIONNUM_MINOR(v), SDL_VERSIONNUM_MICRO(v));
        }
        {
            // Optional extra mappings, e.g. from https://github.com/mdqinc/SDL_GameControllerDB
            std::string db = g_dir + "gamecontrollerdb.txt";
            if (GetFileAttributesA(db.c_str()) != INVALID_FILE_ATTRIBUTES) {
                int n = SDL_AddGamepadMappingsFromFile(db.c_str());
                if (n >= 0) Log("gamecontrollerdb.txt: %d mapping(s) loaded", n);
                else Log("gamecontrollerdb.txt: load failed: %s", SDL_GetError());
            } else {
                Log("gamecontrollerdb.txt not found next to the dll (optional)");
            }
        }
        scan(true);
        // Device detection can complete asynchronously; give it a moment on first init.
        for (int i = 0; i < 15 && !gp; i++) { Sleep(20); scan(true); }
        g_initTid = 0;
    }

    void scan(bool force) {
        DWORD now = GetTickCount();
        if (!force && now - lastScan < 1000) return;
        lastScan = now;
        SDL_UpdateGamepads();
        {
            // Diagnostics: list every joystick SDL sees, including ones with no gamepad mapping.
            int jn = 0;
            SDL_JoystickID* jids = SDL_GetJoysticks(&jn);
            if (!jids) jn = 0;
            if (jn != lastJoyCount) {
                lastJoyCount = jn;
                Log("SDL sees %d joystick(s)", jn);
                for (int k = 0; k < jn; k++) {
                    const char* nm = SDL_GetJoystickNameForID(jids[k]);
                    char guid[40] = "";
                    SDL_GUIDToString(SDL_GetJoystickGUIDForID(jids[k]), guid, sizeof guid);
                    bool isGp = SDL_IsGamepad(jids[k]);
                    Log("  joy[%d] '%s' VID=%04X PID=%04X GUID=%s isGamepad=%d", k, nm ? nm : "?",
                        SDL_GetJoystickVendorForID(jids[k]), SDL_GetJoystickProductForID(jids[k]),
                        guid, isGp ? 1 : 0);
                    if (!isGp) Log("    no mapping for this GUID -- add a line for it to gamecontrollerdb.txt");
                }
            }
            SDL_free(jids);
        }
        if (gp && !SDL_GamepadConnected(gp)) {
            Log("Gamepad lost");
            SDL_CloseGamepad(gp);
            gp = nullptr;
        }
        if (!gp) {
            int n = 0;
            SDL_JoystickID* ids = SDL_GetGamepads(&n);
            if (!ids) n = 0;
            if (n != lastCount) {
                lastCount = n;
                Log("SDL sees %d gamepad(s)", n);
                for (int k = 0; k < n; k++) Log("  [%d] %s", k, SDL_GetGamepadNameForID(ids[k]));
            }
            if (ids && n > 0) {
                int idx = g_cfg.padIndex < n ? g_cfg.padIndex : 0;
                gp = SDL_OpenGamepad(ids[idx]);
                if (gp) Log("Opened gamepad: %s", SDL_GetGamepadName(gp));
                else Log("SDL_OpenGamepad failed: %s", SDL_GetError());
            }
            SDL_free(ids);
        }
    }

    bool present() {
        lock();
        ensureInit();
        if (sdlOk) scan(false);
        bool r = gp != nullptr;
        unlock();
        return r;
    }

    static float stick(Sint16 v, float dz) {
        float f = v < 0 ? v / 32768.0f : v / 32767.0f;
        float a = fabsf(f);
        if (a <= dz) return 0.0f;
        float r = (a - dz) / (1.0f - dz);
        return f < 0 ? -r : r;
    }
    static float trig(Sint16 v) {
        float f = v / 32767.0f;
        return f < 0 ? 0 : (f > 1 ? 1 : f);
    }

    bool button(Src s) {
        switch (s) {
        case B_A: return SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_SOUTH);
        case B_B: return SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_EAST);
        case B_X: return SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_WEST);
        case B_Y: return SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_NORTH);
        case B_BACK: return SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_BACK);
        case B_START: return SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_START);
        case B_GUIDE: return SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_GUIDE);
        case B_LSTICK: return SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_LEFT_STICK);
        case B_RSTICK: return SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_RIGHT_STICK);
        case B_LSHOULDER: return SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
        case B_RSHOULDER: return SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
        case B_DUP: return SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_DPAD_UP);
        case B_DDOWN: return SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_DPAD_DOWN);
        case B_DLEFT: return SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_DPAD_LEFT);
        case B_DRIGHT: return SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
        case B_MISC1: return SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_MISC1);
        case B_TOUCHPAD: return SDL_GetGamepadButton(gp, SDL_GAMEPAD_BUTTON_TOUCHPAD);
        case B_LTRIGGER: return trig(SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFT_TRIGGER)) * 100.0f >= g_cfg.triggerThreshold;
        case B_RTRIGGER: return trig(SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)) * 100.0f >= g_cfg.triggerThreshold;
        default: return false;
        }
    }

    // While a d-pad direction is held, it overrides the chosen stick's axis with full deflection.
    float dpadOverride(Src s, float v) {
        int st = g_cfg.dpadStick;
        if (!st) return v;
        bool left = (st == 1);
        bool isX = left ? (s == A_LX) : (s == A_RX);
        bool isY = left ? (s == A_LY) : (s == A_RY);
        if (isX) {
            bool l = button(B_DLEFT), r = button(B_DRIGHT);
            if (l != r) return l ? -1.0f : 1.0f;
        } else if (isY) {
            bool u = button(B_DUP), d = button(B_DDOWN);
            if (u != d) return u ? -1.0f : 1.0f;
        }
        return v;
    }

    float axisRaw(Src s) {
        float dz = g_cfg.stickDeadzone / 100.0f;
        switch (s) {
        case A_LX: return stick(SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFTX), dz);
        case A_LY: return stick(SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFTY), dz);
        case A_RX: return stick(SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_RIGHTX), dz);
        case A_RY: return stick(SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_RIGHTY), dz);
        case A_LT: return -1.0f + 2.0f * trig(SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFT_TRIGGER));
        case A_RT: return -1.0f + 2.0f * trig(SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER));
        case A_TRIGGERS:
            return trig(SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER))
                 - trig(SDL_GetGamepadAxis(gp, SDL_GAMEPAD_AXIS_LEFT_TRIGGER));
        default: return 0.0f;
        }
    }

    float axis(Src s) { return dpadOverride(s, axisRaw(s)); }

    // Returns true if a gamepad is currently connected. 's' is always filled (neutral if not).
    bool sample(Snap& s) {
        memset(&s, 0, sizeof s);
        s.pov = JOY_POVCENTERED;
        lock();
        ensureInit();
        if (sdlOk) {
            scan(false);
            SDL_UpdateGamepads();
        }
        bool have = gp != nullptr;
        if (have) {
            for (int i = 0; i < 6; i++) {
                float f = axis(g_cfg.axis[i]);
                if (g_cfg.invert[i]) f = -f;
                s.ax[i] = f;
            }
            for (int i = 0; i < g_cfg.buttonCount; i++)
                s.btn[i] = button(g_cfg.button[i]);
            if (g_cfg.dpadAsPov) {
                bool u = button(B_DUP), d = button(B_DDOWN), l = button(B_DLEFT), r = button(B_DRIGHT);
                if (u && d) u = d = false;
                if (l && r) l = r = false;
                if (u && r) s.pov = 4500;
                else if (r && d) s.pov = 13500;
                else if (d && l) s.pov = 22500;
                else if (l && u) s.pov = 31500;
                else if (u) s.pov = 0;
                else if (r) s.pov = 9000;
                else if (d) s.pov = 18000;
                else if (l) s.pov = 27000;
            }
        }
        unlock();
        return have;
    }
} g_pad;

// ----------------------------------------------------------------------------
// Real winmm.dll (for non-virtual joystick IDs and re-entrant calls)
// ----------------------------------------------------------------------------

typedef UINT(WINAPI* PFN_joyGetNumDevs)();
typedef MMRESULT(WINAPI* PFN_joyGetDevCapsA)(UINT_PTR, LPJOYCAPSA, UINT);
typedef MMRESULT(WINAPI* PFN_joyGetDevCapsW)(UINT_PTR, LPJOYCAPSW, UINT);
typedef MMRESULT(WINAPI* PFN_joyGetPos)(UINT, LPJOYINFO);
typedef MMRESULT(WINAPI* PFN_joyGetPosEx)(UINT, LPJOYINFOEX);

static HMODULE RealDll()
{
    static HMODULE h = nullptr;
    if (!h) {
        char dir[MAX_PATH];
        GetSystemDirectoryA(dir, MAX_PATH);
        std::string p = std::string(dir) + "\\winmm.dll";
        h = LoadLibraryA(p.c_str());
        Log("Real winmm.dll: %s -> %p", p.c_str(), (void*)h);
    }
    return h;
}

template <class F> static F RealFn(const char* name)
{
    HMODULE h = RealDll();
    return h ? (F)GetProcAddress(h, name) : nullptr;
}

// ----------------------------------------------------------------------------
// Joystick API
// ----------------------------------------------------------------------------

static void SetStr(char* d, size_t n, const char* s) { strncpy(d, s, n - 1); d[n - 1] = 0; }
static void SetStr(wchar_t* d, size_t n, const char* s) {
    MultiByteToWideChar(CP_ACP, 0, s, -1, d, (int)n); d[n - 1] = 0;
}

static bool IsVirtualId(UINT_PTR id) { EnsureConfig(); return id == (UINT_PTR)g_cfg.joyId; }

// Should the virtual pad answer at all right now?
static bool VirtualAvailable()
{
    return g_cfg.alwaysPresent || g_pad.present();
}

static DWORD AxisToWinmm(float f)
{
    f = std::max(-1.0f, std::min(1.0f, f));
    return (DWORD)((f + 1.0f) * 0.5f * 65535.0f + 0.5f);
}

template <class C>
static void FillCaps(C* out, UINT cb)
{
    C c; memset(&c, 0, sizeof c);
    c.wMid = (WORD)g_cfg.vid;
    c.wPid = (WORD)g_cfg.pid;
    SetStr(c.szPname, ARRAYSIZE(c.szPname), g_cfg.productName);
    c.wXmin = 0; c.wXmax = 65535;
    c.wYmin = 0; c.wYmax = 65535;
    c.wZmin = 0; c.wZmax = 65535;
    c.wRmin = 0; c.wRmax = 65535;
    c.wUmin = 0; c.wUmax = 65535;
    c.wVmin = 0; c.wVmax = 65535;
    c.wNumButtons = (UINT)g_cfg.buttonCount;
    c.wMaxButtons = 32;
    c.wPeriodMin = 10;
    c.wPeriodMax = 1000;
    c.wMaxAxes = 6;
    UINT axes = 2;
    UINT caps = 0;
    if (g_cfg.axis[2] != S_NONE) { caps |= JOYCAPS_HASZ; axes++; }
    if (g_cfg.axis[3] != S_NONE) { caps |= JOYCAPS_HASR; axes++; }
    if (g_cfg.axis[4] != S_NONE) { caps |= JOYCAPS_HASU; axes++; }
    if (g_cfg.axis[5] != S_NONE) { caps |= JOYCAPS_HASV; axes++; }
    if (g_cfg.dpadAsPov) caps |= JOYCAPS_HASPOV | JOYCAPS_POV4DIR | JOYCAPS_POVCTS;
    c.wNumAxes = axes;
    c.wCaps = caps;
    UINT n = std::min<UINT>(cb, sizeof c);
    memcpy(out, &c, n);
}

extern "C" UINT WINAPI joyGetNumDevs(void)
{
    EnsureConfig();
    UINT n = 16;
    if (!g_cfg.hideRealPads || InInit()) {
        auto fn = RealFn<PFN_joyGetNumDevs>("joyGetNumDevs");
        if (fn) n = fn();
        if (InInit()) return n;
    } else {
        n = (UINT)g_cfg.joyId + 1;
    }
    if (n < (UINT)g_cfg.joyId + 1) n = (UINT)g_cfg.joyId + 1;
    return n;
}

extern "C" MMRESULT WINAPI joyGetDevCapsA(UINT_PTR id, LPJOYCAPSA caps, UINT cb)
{
    EnsureConfig();
    if (!caps) return JOYERR_PARMS;
    if (InInit() || id == (UINT_PTR)-1 || (!IsVirtualId(id) && !g_cfg.hideRealPads)) {
        auto fn = RealFn<PFN_joyGetDevCapsA>("joyGetDevCapsA");
        return fn ? fn(id, caps, cb) : MMSYSERR_NODRIVER;
    }
    if (!IsVirtualId(id)) return JOYERR_UNPLUGGED;
    if (!VirtualAvailable()) return JOYERR_UNPLUGGED;
    LOG_ONCE("joyGetDevCapsA(id=%u, cb=%u)", (unsigned)id, cb);
    FillCaps(caps, cb);
    return JOYERR_NOERROR;
}

extern "C" MMRESULT WINAPI joyGetDevCapsW(UINT_PTR id, LPJOYCAPSW caps, UINT cb)
{
    EnsureConfig();
    if (!caps) return JOYERR_PARMS;
    if (InInit() || id == (UINT_PTR)-1 || (!IsVirtualId(id) && !g_cfg.hideRealPads)) {
        auto fn = RealFn<PFN_joyGetDevCapsW>("joyGetDevCapsW");
        return fn ? fn(id, caps, cb) : MMSYSERR_NODRIVER;
    }
    if (!IsVirtualId(id)) return JOYERR_UNPLUGGED;
    if (!VirtualAvailable()) return JOYERR_UNPLUGGED;
    LOG_ONCE("joyGetDevCapsW(id=%u, cb=%u)", (unsigned)id, cb);
    FillCaps(caps, cb);
    return JOYERR_NOERROR;
}

static void LogChange(const Snap& s)
{
    static unsigned lastMask = 0;
    static DWORD lastPov = 0xFFFFFFFF;
    unsigned m = 0;
    for (int i = 0; i < 32; i++) if (s.btn[i]) m |= 1u << i;
    if (m != lastMask || s.pov != lastPov) {
        lastMask = m; lastPov = s.pov;
        Log("state: buttons=%08X pov=%lu axes=%.2f,%.2f,%.2f,%.2f,%.2f,%.2f", m, (unsigned long)s.pov,
            s.ax[0], s.ax[1], s.ax[2], s.ax[3], s.ax[4], s.ax[5]);
    }
}

extern "C" MMRESULT WINAPI joyGetPosEx(UINT id, LPJOYINFOEX pji)
{
    EnsureConfig();
    if (!pji) return JOYERR_PARMS;
    if (InInit() || (!IsVirtualId(id) && !g_cfg.hideRealPads)) {
        auto fn = RealFn<PFN_joyGetPosEx>("joyGetPosEx");
        return fn ? fn(id, pji) : MMSYSERR_NODRIVER;
    }
    if (!IsVirtualId(id)) return JOYERR_UNPLUGGED;
    if (pji->dwSize < sizeof(JOYINFOEX)) return JOYERR_PARMS;

    Snap s;
    bool have = g_pad.sample(s);
    if (!have && !g_cfg.alwaysPresent) return JOYERR_UNPLUGGED;
    LOG_ONCE("joyGetPosEx first call (id=%u, flags=0x%lX, gamepad=%d)", id, (unsigned long)pji->dwFlags, have);
    LogChange(s);

    DWORD fl = pji->dwFlags;
    bool centered = (fl & JOY_RETURNCENTERED) != 0;
    auto ax = [&](int i) -> DWORD { return centered ? 32767 : AxisToWinmm(s.ax[i]); };

    pji->dwXpos = pji->dwYpos = pji->dwZpos = 0;
    pji->dwRpos = pji->dwUpos = pji->dwVpos = 0;
    pji->dwButtons = 0;
    pji->dwButtonNumber = 0;
    pji->dwPOV = JOY_POVCENTERED;
    pji->dwReserved1 = pji->dwReserved2 = 0;

    if (fl & JOY_RETURNX) pji->dwXpos = ax(0);
    if (fl & JOY_RETURNY) pji->dwYpos = ax(1);
    if (fl & JOY_RETURNZ) pji->dwZpos = ax(2);
    if (fl & JOY_RETURNR) pji->dwRpos = ax(3);
    if (fl & JOY_RETURNU) pji->dwUpos = ax(4);
    if (fl & JOY_RETURNV) pji->dwVpos = ax(5);

    if (fl & JOY_RETURNBUTTONS) {
        DWORD mask = 0, num = 0;
        for (int i = 0; i < g_cfg.buttonCount && i < 32; i++) {
            if (s.btn[i]) { mask |= 1u << i; if (!num) num = (DWORD)i + 1; }
        }
        pji->dwButtons = mask;
        pji->dwButtonNumber = num;
    }

    if (fl & (JOY_RETURNPOV | JOY_RETURNPOVCTS)) {
        DWORD pov = s.pov;
        if (!(fl & JOY_RETURNPOVCTS) && pov != JOY_POVCENTERED && pov % 9000 != 0) {
            // Discrete (4-direction) request: snap diagonals to the vertical direction.
            pov = (pov < 9000 || pov > 27000) ? 0 : 18000;
        }
        pji->dwPOV = pov;
    }
    return JOYERR_NOERROR;
}

extern "C" MMRESULT WINAPI joyGetPos(UINT id, LPJOYINFO pji)
{
    EnsureConfig();
    if (!pji) return JOYERR_PARMS;
    if (InInit() || (!IsVirtualId(id) && !g_cfg.hideRealPads)) {
        auto fn = RealFn<PFN_joyGetPos>("joyGetPos");
        return fn ? fn(id, pji) : MMSYSERR_NODRIVER;
    }
    if (!IsVirtualId(id)) return JOYERR_UNPLUGGED;

    Snap s;
    bool have = g_pad.sample(s);
    if (!have && !g_cfg.alwaysPresent) return JOYERR_UNPLUGGED;
    LOG_ONCE("joyGetPos first call (id=%u, gamepad=%d)", id, have);

    pji->wXpos = AxisToWinmm(s.ax[0]);
    pji->wYpos = AxisToWinmm(s.ax[1]);
    pji->wZpos = AxisToWinmm(s.ax[2]);
    UINT b = 0;
    for (int i = 0; i < 4 && i < g_cfg.buttonCount; i++)
        if (s.btn[i]) b |= (JOY_BUTTON1 << i);
    pji->wButtons = b;
    return JOYERR_NOERROR;
}

// ----------------------------------------------------------------------------

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        char p[MAX_PATH];
        GetModuleFileNameA(inst, p, MAX_PATH);
        std::string s = p;
        size_t k = s.find_last_of("\\/");
        g_dir = k == std::string::npos ? "" : s.substr(0, k + 1);
    }
    // No SDL_Quit on detach: the loader lock is held and the process is going away anyway.
    return TRUE;
}
