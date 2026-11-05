// DInput8 SDL Wrapper: a DirectInput8 proxy (dinput8.dll) for DirectInput8 games.
// Presents an SDL3 gamepad to the game as a DirectInput joystick.
// Both the ANSI (IDirectInput8A) and Unicode (IDirectInput8W) interfaces are wrapped.
//
//  * Real joystick/gamepad devices are hidden from the game; keyboard and
//    mouse pass straight through to the real DirectInput.
//  * gamecontrollerdb.txt is loaded from the proxy's folder, the exe's folder
//    and the working directory (see LoadMappingDBs).
//  * Part of DWS Input. Configuration: dws_input.ini next to dinput8.dll, shared with
//    winmm.dll / dinput.dll (button/axis/trigger mapping, deadzone, rumble, device
//    hiding, AlwaysPresent). Missing file = built-in defaults.
//  * Log: dws_input_dinput8.log next to dinput8.dll (rewritten every run).

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#define DIRECTINPUT_VERSION 0x0800

#include <windows.h>
// The SDK declares DirectInput8Create itself; rename that declaration so it
// cannot clash with our exported definition below.
#define DirectInput8Create SDKDirectInput8Create
#include <dinput.h>
#undef DirectInput8Create

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <type_traits>
#include <vector>

#include "dws_core.h"

#if defined(_MSC_VER)
#include <intrin.h>
#define CALLER_ADDRESS() _ReturnAddress()
#else
#define CALLER_ADDRESS() __builtin_return_address(0)
#endif

// DIPROP_* are small integers disguised as GUID references: compare addresses only.
#define IS_PROP(p, id) (&(p) == &(id))

// ============================================================================
// Module state + logging
// ============================================================================
static char g_dir[MAX_PATH];   // folder containing this DLL, with trailing '\'

// ============================================================================
// Configuration (dws_input.ini, section [DInput8] overrides)
// ============================================================================
enum SrcKind { K_NONE, K_AXIS, K_TRIG, K_TRIGCOMBINED, K_BTN, K_TRIGBTN };
struct Src { SrcKind kind = K_NONE; int id = 0; };  // id: SDL axis / SDL button / 0=left,1=right trigger

struct SrcName { const char* name; Src src; };
static const SrcName kSrcNames[] = {
    { "None",          { K_NONE, 0 } },
    { "LeftX",         { K_AXIS, SDL_GAMEPAD_AXIS_LEFTX } },
    { "LeftY",         { K_AXIS, SDL_GAMEPAD_AXIS_LEFTY } },
    { "RightX",        { K_AXIS, SDL_GAMEPAD_AXIS_RIGHTX } },
    { "RightY",        { K_AXIS, SDL_GAMEPAD_AXIS_RIGHTY } },
    { "LeftTrigger",   { K_TRIG, 0 } },
    { "RightTrigger",  { K_TRIG, 1 } },
    { "Triggers",      { K_TRIGCOMBINED, 0 } },   // axis only: LT up, RT down
    { "A",             { K_BTN, SDL_GAMEPAD_BUTTON_SOUTH } },
    { "B",             { K_BTN, SDL_GAMEPAD_BUTTON_EAST } },
    { "X",             { K_BTN, SDL_GAMEPAD_BUTTON_WEST } },
    { "Y",             { K_BTN, SDL_GAMEPAD_BUTTON_NORTH } },
    { "Back",          { K_BTN, SDL_GAMEPAD_BUTTON_BACK } },
    { "Start",         { K_BTN, SDL_GAMEPAD_BUTTON_START } },
    { "Guide",         { K_BTN, SDL_GAMEPAD_BUTTON_GUIDE } },
    { "LeftStick",     { K_BTN, SDL_GAMEPAD_BUTTON_LEFT_STICK } },
    { "RightStick",    { K_BTN, SDL_GAMEPAD_BUTTON_RIGHT_STICK } },
    { "LeftShoulder",  { K_BTN, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER } },
    { "RightShoulder", { K_BTN, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER } },
    { "DPadUp",        { K_BTN, SDL_GAMEPAD_BUTTON_DPAD_UP } },
    { "DPadDown",      { K_BTN, SDL_GAMEPAD_BUTTON_DPAD_DOWN } },
    { "DPadLeft",      { K_BTN, SDL_GAMEPAD_BUTTON_DPAD_LEFT } },
    { "DPadRight",     { K_BTN, SDL_GAMEPAD_BUTTON_DPAD_RIGHT } },
    { "Misc1",         { K_BTN, SDL_GAMEPAD_BUTTON_MISC1 } },
    { "Touchpad",      { K_BTN, SDL_GAMEPAD_BUTTON_TOUCHPAD } },
};

static const int kMaxButtons = 32;
static const char* kDefAxis[6] = { "LeftX", "LeftY", "RightX", "None", "None", "RightY" };   // = shipped dws_input.ini
static const char* kDefBtn[12] = { "A", "B", "X", "Y", "LeftShoulder", "RightShoulder",
                                   "LeftTrigger", "RightTrigger", "Back", "Start", "LeftStick", "RightStick" };

struct Config {
    bool log = false, hideReal = true, rumble = true, dpadPOV = true;
    int  dpadStick = 0;           // 0 = off, 1 = d-pad drives the left stick axes, 2 = right stick axes
    bool alwaysPresent = true;    // report the virtual pad even when no controller is connected
    int  padIndex = 0;
    int  stickDeadzone = 8;       // percent
    int  triggerThreshold = 30;   // percent (trigger used as a button)
    int  rumbleScale = 100, rumbleLow = 100, rumbleHigh = 100;   // percent
    std::string name = "SDL Gamepad";
    unsigned vid = 0x045E, pid = 0x028E;   // fixed USB IDs reported to the game (Xbox 360 pad)
    Src  axis[6];
    bool invert[6] = {};
    Src  button[kMaxButtons];
    int  buttonCount = 12;
};
static Config g_cfg;

static void Log(const char* fmt, ...) {
    if (!g_cfg.log) return;
    static FILE* f = nullptr;
    static bool tried = false;
    if (!tried) {
        tried = true;
        std::string p = dws::LogPath(g_dir, "DInput8");
        f = fopen(p.c_str(), "w");
    }
    if (!f) return;
    va_list a; va_start(a, fmt); vfprintf(f, fmt, a); va_end(a);
    fputc('\n', f); fflush(f);
}

static Src ParseSrc(const char* v, bool forButton, const char* key) {
    for (auto& n : kSrcNames) {
        if (_stricmp(n.name, v) != 0) continue;
        Src r = n.src;
        if (forButton) {
            if (r.kind == K_TRIG) r.kind = K_TRIGBTN;                       // trigger as a button
            else if (r.kind == K_AXIS || r.kind == K_TRIGCOMBINED) break;   // not valid for a button
        } else {
            if (r.kind == K_BTN || r.kind == K_TRIGBTN) break;              // not valid for an axis
        }
        return r;
    }
    Log("config: invalid value '%s' for %s, using None", v, key);
    return Src{};
}

static int Clamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static void LoadConfig() {
    dws::Ini ini;
    ini.Open(g_dir, "DInput8");
    const char* iniPath = ini.Path().c_str();
    auto I = [&](const char* sec, const char* key, int def) { return ini.GetInt(sec, key, def); };
    auto S = [&](const char* sec, const char* key, const char* def, char* out, size_t n) {
        ini.GetStr(sec, key, def, out, n);
    };
    const std::string axSec = ini.AxesSection();     // "Axes.DInput"

    g_cfg.log = I("General", "Log", 0) != 0;
    Log("config: %s%s", iniPath, ini.Exists() ? "" : "  (not found, using built-in defaults)");

    g_cfg.hideReal = I("General", "HideRealPads", 1) != 0;
    g_cfg.padIndex = std::max(0, I("General", "PadIndex", 0));
    g_cfg.alwaysPresent = I("General", "AlwaysPresent", 1) != 0;
    char buf[128];
    S("General", "ProductName", "SDL Gamepad", buf, sizeof buf);
    g_cfg.name = buf[0] ? buf : "SDL Gamepad";

    // Fixed VID/PID (hex). The game stores its controller settings against these,
    // so they must not change when a different physical pad is connected.
    S("General", "VendorID", "045E", buf, sizeof buf);
    g_cfg.vid = strtoul(buf, nullptr, 16) & 0xFFFF;
    S("General", "ProductID", "028E", buf, sizeof buf);
    g_cfg.pid = strtoul(buf, nullptr, 16) & 0xFFFF;

    g_cfg.stickDeadzone    = Clamp(I("Input", "StickDeadzone", 8), 0, 90);
    g_cfg.triggerThreshold = Clamp(I("Input", "TriggerThreshold", 30), 1, 99);
    g_cfg.dpadPOV          = I("Input", "DPadAsPOV", 1) != 0;
    S("Input", "DPadAsStick", "None", buf, sizeof buf);
    if (!_stricmp(buf, "Left") || !_stricmp(buf, "LeftStick")) g_cfg.dpadStick = 1;
    else if (!_stricmp(buf, "Right") || !_stricmp(buf, "RightStick")) g_cfg.dpadStick = 2;
    else g_cfg.dpadStick = 0;

    g_cfg.rumble      = I("Rumble", "Enabled", 1) != 0;
    g_cfg.rumbleScale = Clamp(I("Rumble", "Strength", 100), 0, 200);
    g_cfg.rumbleLow   = Clamp(I("Rumble", "LowMotor", 100), 0, 200);
    g_cfg.rumbleHigh  = Clamp(I("Rumble", "HighMotor", 100), 0, 200);

    char key[32];
    for (int i = 0; i < 6; i++) {
        snprintf(key, sizeof key, "Axis%d", i);
        S(axSec.c_str(), key, kDefAxis[i], buf, sizeof buf);
        g_cfg.axis[i] = ParseSrc(buf, false, key);
        snprintf(key, sizeof key, "Invert%d", i);
        g_cfg.invert[i] = I(axSec.c_str(), key, 0) != 0;
    }

    int last = -1;
    for (int i = 0; i < kMaxButtons; i++) {
        snprintf(key, sizeof key, "Button%d", i);
        S("Buttons", key, i < 12 ? kDefBtn[i] : "None", buf, sizeof buf);
        g_cfg.button[i] = ParseSrc(buf, true, key);
        if (g_cfg.button[i].kind != K_NONE) last = i;
    }
    int forced = I("Buttons", "ButtonCount", 0);
    g_cfg.buttonCount = forced > 0 ? std::min(kMaxButtons, forced) : std::max(1, last + 1);

    Log("config: hideReal=%d alwaysPresent=%d padIndex=%d deadzone=%d%% dpadPOV=%d dpadStick=%d buttons=%d rumble=%d (strength %d%%, low %d%%, high %d%%)",
        g_cfg.hideReal, g_cfg.alwaysPresent, g_cfg.padIndex, g_cfg.stickDeadzone, g_cfg.dpadPOV, g_cfg.dpadStick, g_cfg.buttonCount,
        g_cfg.rumble, g_cfg.rumbleScale, g_cfg.rumbleLow, g_cfg.rumbleHigh);
}

// ============================================================================
// SDL
// ============================================================================
static SDL_Gamepad* g_pad = nullptr;
static DWORD g_ffGain = 10000;

static void StopRumble() { if (g_pad) SDL_RumbleGamepad(g_pad, 0, 0, 0); }

static bool PadHasRumble() {
    if (!g_cfg.rumble) return false;
    if (!g_pad) return g_cfg.alwaysPresent;   // placeholder pad: advertise rumble so FF-only enumeration still lists it
    return SDL_GetBooleanProperty(SDL_GetGamepadProperties(g_pad),
                                  SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, false);
}

// Find every gamecontrollerdb.txt we know about (proxy dir, exe dir, cwd).
static std::vector<std::string> LoadMappingDBs() {
    char exe[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    if (char* s = strrchr(exe, '\\')) s[1] = 0;

    char cwd[MAX_PATH] = {};
    GetCurrentDirectoryA(MAX_PATH, cwd);
    size_t n = strlen(cwd);
    if (n && cwd[n - 1] != '\\' && n + 1 < MAX_PATH) { cwd[n] = '\\'; cwd[n + 1] = 0; }

    const char* dirs[3] = { g_dir, exe, cwd };
    std::vector<std::string> found;
    for (const char* d : dirs) {
        if (!d[0]) continue;
        std::string path = std::string(d) + "gamecontrollerdb.txt";
        if (GetFileAttributesA(path.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        bool dup = false;
        for (auto& f : found) if (_stricmp(f.c_str(), path.c_str()) == 0) dup = true;
        if (!dup) found.push_back(path);
    }
    return found;
}

static bool SdlInit() {
    static int state = 0;   // 0 = untried, 1 = ok, -1 = failed
    if (state) return state > 0;

    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");

    // The first gamecontrollerdb.txt found is handed to SDL before init, so
    // already-connected pads are mapped during SDL's first device scan.
    // (If the user set the SDL_GAMECONTROLLERCONFIG_FILE environment variable,
    // that takes priority over this hint, which is what you'd want.)
    std::vector<std::string> dbs = LoadMappingDBs();
    if (!dbs.empty()) {
        SDL_SetHint(SDL_HINT_GAMECONTROLLERCONFIG_FILE, dbs[0].c_str());
        Log("gamecontrollerdb: using %s", dbs[0].c_str());
    } else {
        Log("gamecontrollerdb: no gamecontrollerdb.txt found (looked in proxy dir, exe dir, cwd)");
    }

    if (!SDL_Init(SDL_INIT_GAMEPAD)) {
        Log("SDL_Init failed: %s", SDL_GetError());
        state = -1;
        return false;
    }
    int v = SDL_GetVersion();
    Log("SDL %d.%d.%d initialised", SDL_VERSIONNUM_MAJOR(v), SDL_VERSIONNUM_MINOR(v), SDL_VERSIONNUM_MICRO(v));

    // Any further databases (other folders) are layered on top.
    for (size_t i = 1; i < dbs.size(); i++) {
        int c = SDL_AddGamepadMappingsFromFile(dbs[i].c_str());
        if (c < 0) Log("gamecontrollerdb: failed to load %s: %s", dbs[i].c_str(), SDL_GetError());
        else       Log("gamecontrollerdb: added %d mappings from %s", c, dbs[i].c_str());
    }

    state = 1;
    return true;
}

static void SdlPoll() {
    if (!SdlInit()) return;
    SDL_UpdateGamepads();

    if (g_pad && !SDL_GamepadConnected(g_pad)) {
        Log("gamepad disconnected");
        SDL_CloseGamepad(g_pad);
        g_pad = nullptr;
    }
    if (g_pad) return;

    // No pad open: rescan, but not more than twice a second.
    static DWORD last = 0;
    static int lastJoyCount = -1;
    DWORD now = GetTickCount();
    if (last && now - last < 500) return;
    last = now;

    int n = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&n);
    if (ids && n > 0) {
        g_pad = SDL_OpenGamepad(ids[g_cfg.padIndex < n ? g_cfg.padIndex : 0]);
        if (g_pad) {
            char* mapping = SDL_GetGamepadMapping(g_pad);
            Log("gamepad opened: %s (rumble=%d)\n  mapping: %s",
                SDL_GetGamepadName(g_pad), PadHasRumble() ? 1 : 0, mapping ? mapping : "(none)");
            SDL_free(mapping);
        } else {
            Log("SDL_OpenGamepad failed: %s", SDL_GetError());
        }
    }
    SDL_free(ids);

    // Joysticks that SDL sees but has no gamepad mapping for: tell the user the GUID
    // so they can add a line to gamecontrollerdb.txt.
    if (!g_pad) {
        int jn = 0;
        SDL_JoystickID* js = SDL_GetJoysticks(&jn);
        if (jn != lastJoyCount) {
            lastJoyCount = jn;
            for (int i = 0; i < jn; i++) {
                char guid[64];
                SDL_GUIDToString(SDL_GetJoystickGUIDForID(js[i]), guid, sizeof guid);
                Log("joystick present but not a mapped gamepad: \"%s\" GUID=%s\n"
                    "  -> add a mapping for this GUID to gamecontrollerdb.txt",
                    SDL_GetJoystickNameForID(js[i]), guid);
            }
        }
        SDL_free(js);
    }
}

// {B7A3C1D2-5E4F-4A6B-9C8D-1E2F3A4B5C6D}  (our fake device)
static const GUID GUID_FakePad =
    { 0xb7a3c1d2, 0x5e4f, 0x4a6b, { 0x9c, 0x8d, 0x1e, 0x2f, 0x3a, 0x4b, 0x5c, 0x6d } };

// ============================================================================
// Force feedback effects
// ============================================================================
struct EffDef { const GUID* g; DWORD type; const char* name; };
static const EffDef kEffs[] = {
    { &GUID_ConstantForce, DIEFT_CONSTANTFORCE, "Constant Force" },
    { &GUID_RampForce,     DIEFT_RAMPFORCE,     "Ramp Force"     },
    { &GUID_Square,        DIEFT_PERIODIC,      "Square"         },
    { &GUID_Sine,          DIEFT_PERIODIC,      "Sine"           },
    { &GUID_Triangle,      DIEFT_PERIODIC,      "Triangle"       },
};
static const EffDef* FindEff(REFGUID g) {
    for (auto& e : kEffs) if (g == *e.g) return &e;
    return nullptr;
}

class FakeEffect : public IDirectInputEffect {
    LONG  ref = 1;
    GUID  type;
    DWORD magnitude  = 0;        // 0..10000
    DWORD gain       = 10000;    // per-effect gain, 0..10000
    DWORD durationUs = INFINITE; // DIEFFECT.dwDuration is in microseconds
    DWORD endTick    = 0;        // 0 = not playing
    bool  infinite   = false;
public:
    explicit FakeEffect(REFGUID g) : type(g) {}

    void Apply(const DIEFFECT* e, DWORD flags) {
        if (!e) return;
        if (flags & DIEP_DURATION) durationUs = e->dwDuration;
        if (flags & DIEP_GAIN)     gain = e->dwGain;
        if ((flags & DIEP_TYPESPECIFICPARAMS) && e->lpvTypeSpecificParams) {
            if (type == GUID_ConstantForce && e->cbTypeSpecificParams >= sizeof(DICONSTANTFORCE)) {
                magnitude = (DWORD)abs(((DICONSTANTFORCE*)e->lpvTypeSpecificParams)->lMagnitude);
            } else if (type == GUID_RampForce && e->cbTypeSpecificParams >= sizeof(DIRAMPFORCE)) {
                auto* r = (DIRAMPFORCE*)e->lpvTypeSpecificParams;
                magnitude = (DWORD)std::max(abs(r->lStart), abs(r->lEnd));
            } else if (e->cbTypeSpecificParams >= sizeof(DIPERIODIC)) {
                magnitude = ((DIPERIODIC*)e->lpvTypeSpecificParams)->dwMagnitude;
            }
        }
        if (magnitude > 10000) magnitude = 10000;
    }

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID r, void** o) override {
        if (r == IID_IUnknown || r == IID_IDirectInputEffect) { *o = this; AddRef(); return S_OK; }
        *o = nullptr; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&ref); }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = InterlockedDecrement(&ref);
        if (!r) delete this;
        return r;
    }
    // IDirectInputEffect
    HRESULT STDMETHODCALLTYPE Initialize(HINSTANCE, DWORD, REFGUID) override { return DI_OK; }
    HRESULT STDMETHODCALLTYPE GetEffectGuid(LPGUID g) override { *g = type; return DI_OK; }
    HRESULT STDMETHODCALLTYPE GetParameters(LPDIEFFECT e, DWORD) override {
        e->dwDuration = durationUs; e->dwGain = gain; return DI_OK;
    }
    HRESULT STDMETHODCALLTYPE SetParameters(LPCDIEFFECT e, DWORD flags) override {
        Apply(e, flags);
        if (flags & DIEP_START) Start(1, 0);
        return DI_OK;
    }
    HRESULT STDMETHODCALLTYPE Start(DWORD iterations, DWORD) override {
        if (!PadHasRumble()) return DI_OK;
        double amp = (magnitude / 10000.0) * (gain / 10000.0) * (g_ffGain / 10000.0);
        amp *= g_cfg.rumbleScale / 100.0;
        auto motor = [&](int pct) { return (Uint16)(std::min(amp * pct / 100.0, 1.0) * 65535.0); };
        Uint16 lo = motor(g_cfg.rumbleLow), hi = motor(g_cfg.rumbleHigh);
        infinite = (durationUs == INFINITE || iterations == INFINITE);
        Uint32 ms = infinite ? 0xFFFFFFFFu
                             : (Uint32)((ULONGLONG)(durationUs / 1000) * (iterations ? iterations : 1));
        endTick = infinite ? 0xFFFFFFFFu : (GetTickCount() + ms);
        // SDL has a single rumble slot: the last-started effect wins.
        // Motor weights come from [Rumble] LowMotor / HighMotor in the ini.
        if (g_pad) SDL_RumbleGamepad(g_pad, lo, hi, ms);
        return DI_OK;
    }
    HRESULT STDMETHODCALLTYPE Stop() override { StopRumble(); endTick = 0; return DI_OK; }
    HRESULT STDMETHODCALLTYPE GetEffectStatus(LPDWORD s) override {
        bool playing = endTick && (infinite || (LONG)(endTick - GetTickCount()) > 0);
        *s = playing ? DIEGES_PLAYING : 0;
        return DI_OK;
    }
    HRESULT STDMETHODCALLTYPE Download() override { return DI_OK; }
    HRESULT STDMETHODCALLTYPE Unload() override { return Stop(); }
    HRESULT STDMETHODCALLTYPE Escape(LPDIEFFESCAPE) override { return DIERR_UNSUPPORTED; }
};

// ============================================================================
// Fake gamepad device
// ============================================================================
// Copy text into the fixed-size buffers of the ANSI (char) and Unicode (wchar_t)
// DirectInput structures.
template <size_t N> static void PutText(char (&d)[N], const char* s) {
    strncpy(d, s, N - 1);
    d[N - 1] = 0;
}
template <size_t N> static void PutText(wchar_t (&d)[N], const char* s) {
    MultiByteToWideChar(CP_ACP, 0, s, -1, d, (int)N);
    d[N - 1] = 0;
}
template <class C, size_t N> static void CopyName(C (&d)[N]) { PutText(d, g_cfg.name.c_str()); }

// The DirectInput8 interfaces and structures come in an ANSI (A) and a Unicode (W)
// flavour. FakePadT / ProxyDI8T are written once and instantiated for both.
template <bool W> struct DIT;
template <> struct DIT<false> {
    using Device    = IDirectInputDevice8A;
    using DI8       = IDirectInput8A;
    using DevInst   = DIDEVICEINSTANCEA;
    using ObjInst   = DIDEVICEOBJECTINSTANCEA;
    using EffInfo   = DIEFFECTINFOA;
    using ActFmt    = DIACTIONFORMATA;
    using CfgParams = DICONFIGUREDEVICESPARAMSA;
    using ImgHdr    = DIDEVICEIMAGEINFOHEADERA;
    using EnumDevCb = LPDIENUMDEVICESCALLBACKA;
    using EnumObjCb = LPDIENUMDEVICEOBJECTSCALLBACKA;
    using EnumEffCb = LPDIENUMEFFECTSCALLBACKA;
    using SemCb     = LPDIENUMDEVICESBYSEMANTICSCBA;
    using Str       = LPCSTR;
    static const IID& DevIid() { return IID_IDirectInputDevice8A; }
    static const IID& DI8Iid() { return IID_IDirectInput8A; }
};
template <> struct DIT<true> {
    using Device    = IDirectInputDevice8W;
    using DI8       = IDirectInput8W;
    using DevInst   = DIDEVICEINSTANCEW;
    using ObjInst   = DIDEVICEOBJECTINSTANCEW;
    using EffInfo   = DIEFFECTINFOW;
    using ActFmt    = DIACTIONFORMATW;
    using CfgParams = DICONFIGUREDEVICESPARAMSW;
    using ImgHdr    = DIDEVICEIMAGEINFOHEADERW;
    using EnumDevCb = LPDIENUMDEVICESCALLBACKW;
    using EnumObjCb = LPDIENUMDEVICEOBJECTSCALLBACKW;
    using EnumEffCb = LPDIENUMEFFECTSCALLBACKW;
    using SemCb     = LPDIENUMDEVICESBYSEMANTICSCBW;
    using Str       = LPCWSTR;
    static const IID& DevIid() { return IID_IDirectInputDevice8W; }
    static const IID& DI8Iid() { return IID_IDirectInput8W; }
};

// DPadAsStick: while a d-pad direction is held it overrides the chosen stick's
// axis with full deflection (v is -1..1, up/left negative). Only call with g_pad open.
static double DpadOverride(int sdlAxis, double v) {
    int st = g_cfg.dpadStick;
    if (!st) return v;
    bool left = (st == 1);
    if (sdlAxis == (left ? SDL_GAMEPAD_AXIS_LEFTX : SDL_GAMEPAD_AXIS_RIGHTX)) {
        bool l = SDL_GetGamepadButton(g_pad, SDL_GAMEPAD_BUTTON_DPAD_LEFT);
        bool r = SDL_GetGamepadButton(g_pad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
        if (l != r) return l ? -1.0 : 1.0;
    } else if (sdlAxis == (left ? SDL_GAMEPAD_AXIS_LEFTY : SDL_GAMEPAD_AXIS_RIGHTY)) {
        bool u = SDL_GetGamepadButton(g_pad, SDL_GAMEPAD_BUTTON_DPAD_UP);
        bool d = SDL_GetGamepadButton(g_pad, SDL_GAMEPAD_BUTTON_DPAD_DOWN);
        if (u != d) return u ? -1.0 : 1.0;
    }
    return v;
}

// Axis source value, 0..1 (0.5 = centre). Only call with g_pad open.
static double SrcAxis(const Src& s) {
    auto trig = [](SDL_GamepadAxis a) {
        double t = SDL_GetGamepadAxis(g_pad, a) / 32767.0;
        return t < 0.0 ? 0.0 : t > 1.0 ? 1.0 : t;
    };
    switch (s.kind) {
    case K_AXIS: {
        double v  = SDL_GetGamepadAxis(g_pad, (SDL_GamepadAxis)s.id) / 32768.0;
        double dz = g_cfg.stickDeadzone / 100.0, a = fabs(v);
        v = (a <= dz) ? 0.0 : (v < 0 ? -1.0 : 1.0) * std::min(1.0, (a - dz) / (1.0 - dz));
        v = DpadOverride(s.id, v);
        return 0.5 + 0.5 * v;
    }
    case K_TRIG:
        return trig(s.id == 0 ? SDL_GAMEPAD_AXIS_LEFT_TRIGGER : SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
    case K_TRIGCOMBINED:
        return 0.5 + (trig(SDL_GAMEPAD_AXIS_LEFT_TRIGGER) - trig(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)) * 0.5;
    default:
        return 0.5;
    }
}

static bool SrcButton(const Src& s) {
    switch (s.kind) {
    case K_BTN:
        return SDL_GetGamepadButton(g_pad, (SDL_GamepadButton)s.id);
    case K_TRIGBTN:
        return SDL_GetGamepadAxis(g_pad, s.id == 0 ? SDL_GAMEPAD_AXIS_LEFT_TRIGGER
                                                   : SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)
               > g_cfg.triggerThreshold * 32767 / 100;
    default:
        return false;
    }
}

template <bool W> class FakePadT : public DIT<W>::Device {
    using T = DIT<W>;
    LONG  ref = 1;
    LONG  rmin[6], rmax[6];
    DWORD bufSize = 0, seq = 0;
    bool  overflow = false, prevValid = false;
    DIJOYSTATE2 prev{};
    std::deque<DIDEVICEOBJECTDATA> q;

    // -1 = whole device, -2 = not an axis, 0..5 = that axis
    static int AxisIdx(const DIPROPHEADER* h) {
        if (h->dwHow == DIPH_DEVICE) return -1;
        if (h->dwHow == DIPH_BYOFFSET) return h->dwObj < 24 ? (int)(h->dwObj / 4) : -2;
        if (h->dwHow == DIPH_BYID) {
            DWORD i = DIDFT_GETINSTANCE(h->dwObj);
            return ((DIDFT_GETTYPE(h->dwObj) & DIDFT_AXIS) && i < 6) ? (int)i : -2;
        }
        return -2;
    }
    LONG Scale(double n, int i) { return (LONG)(rmin[i] + n * (rmax[i] - rmin[i])); }  // n: 0..1

    void Fill(DIJOYSTATE2& s) {
        memset(&s, 0, sizeof(s));
        for (int i = 0; i < 4; i++) s.rgdwPOV[i] = (DWORD)-1;
        LONG* ax = &s.lX;                       // lX,lY,lZ,lRx,lRy,lRz are contiguous
        if (!g_pad) { for (int i = 0; i < 6; i++) ax[i] = Scale(0.5, i); return; }

        for (int i = 0; i < 6; i++) {
            double v = SrcAxis(g_cfg.axis[i]);
            if (g_cfg.invert[i]) v = 1.0 - v;
            ax[i] = Scale(v, i);
        }
        for (int i = 0; i < g_cfg.buttonCount; i++)
            if (SrcButton(g_cfg.button[i])) s.rgbButtons[i] = 0x80;

        if (g_cfg.dpadPOV) {
            bool u = SDL_GetGamepadButton(g_pad, SDL_GAMEPAD_BUTTON_DPAD_UP);
            bool d = SDL_GetGamepadButton(g_pad, SDL_GAMEPAD_BUTTON_DPAD_DOWN);
            bool l = SDL_GetGamepadButton(g_pad, SDL_GAMEPAD_BUTTON_DPAD_LEFT);
            bool r = SDL_GetGamepadButton(g_pad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
            DWORD pov = (DWORD)-1;
            if (u)      pov = r ? 4500 : l ? 31500 : 0;
            else if (d) pov = r ? 13500 : l ? 22500 : 18000;
            else if (r) pov = 9000;
            else if (l) pov = 27000;
            s.rgdwPOV[0] = pov;
        }
    }

    // Diff the current state against the previous one and queue buffered events.
    void Pump() {
        if (!bufSize) return;
        SdlPoll();
        DIJOYSTATE2 cur; Fill(cur);
        if (!prevValid) { prev = cur; prevValid = true; return; }
        DWORD t = GetTickCount();
        auto push = [&](DWORD ofs, DWORD data) {
            if (q.size() >= bufSize) { overflow = true; return; }
            DIDEVICEOBJECTDATA d{};
            d.dwOfs = ofs; d.dwData = data; d.dwTimeStamp = t; d.dwSequence = seq++;
            q.push_back(d);
        };
        const LONG* pa = &prev.lX; const LONG* ca = &cur.lX;
        for (int i = 0; i < 6; i++) if (pa[i] != ca[i]) push(i * 4, (DWORD)ca[i]);
        if (prev.rgdwPOV[0] != cur.rgdwPOV[0])
            push((DWORD)offsetof(DIJOYSTATE2, rgdwPOV), cur.rgdwPOV[0]);
        for (int i = 0; i < g_cfg.buttonCount; i++)
            if (prev.rgbButtons[i] != cur.rgbButtons[i])
                push((DWORD)offsetof(DIJOYSTATE2, rgbButtons) + i, cur.rgbButtons[i]);
        prev = cur;
    }

public:
    FakePadT() { for (int i = 0; i < 6; i++) { rmin[i] = 0; rmax[i] = 65535; } }
    ~FakePadT() { StopRumble(); }

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID r, void** o) override {
        if (r == IID_IUnknown || r == T::DevIid()) { *o = this; AddRef(); return S_OK; }
        *o = nullptr; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&ref); }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = InterlockedDecrement(&ref);
        if (!r) delete this;
        return r;
    }

    HRESULT STDMETHODCALLTYPE GetCapabilities(LPDIDEVCAPS c) override {
        DWORD sz = c->dwSize; memset(c, 0, sz); c->dwSize = sz;
        c->dwFlags = DIDC_ATTACHED | (PadHasRumble() ? DIDC_FORCEFEEDBACK : 0);
        c->dwDevType = DI8DEVTYPE_GAMEPAD | (DI8DEVTYPEGAMEPAD_STANDARD << 8);
        c->dwAxes = 6; c->dwButtons = g_cfg.buttonCount; c->dwPOVs = g_cfg.dpadPOV ? 1 : 0;
        if (PadHasRumble() && sz >= sizeof(DIDEVCAPS)) {
            c->dwFFSamplePeriod = 1000; c->dwFFMinTimeResolution = 1000;
        }
        return DI_OK;
    }

    HRESULT STDMETHODCALLTYPE EnumObjects(typename T::EnumObjCb cb, LPVOID ctx, DWORD flags) override {
        bool all = (flags == DIDFT_ALL);
        if (all || (flags & DIDFT_AXIS)) {
            const GUID* g[6] = { &GUID_XAxis, &GUID_YAxis, &GUID_ZAxis, &GUID_RxAxis, &GUID_RyAxis, &GUID_RzAxis };
            for (int i = 0; i < 6; i++) {
                typename T::ObjInst o = { sizeof(o) };
                o.guidType = *g[i]; o.dwOfs = i * 4;
                o.dwType = DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE(i);
                o.dwFlags = DIDOI_ASPECTPOSITION | (PadHasRumble() ? DIDOI_FFACTUATOR : 0);
                if (PadHasRumble()) o.dwFFMaxForce = 10000;
                char nm[32]; snprintf(nm, sizeof nm, "Axis %d", i); PutText(o.tszName, nm);
                if (cb(&o, ctx) == DIENUM_STOP) return DI_OK;
            }
        }
        if (all || (flags & DIDFT_BUTTON)) {
            for (int i = 0; i < g_cfg.buttonCount; i++) {
                typename T::ObjInst o = { sizeof(o) };
                o.guidType = GUID_Button;
                o.dwOfs = (DWORD)offsetof(DIJOYSTATE2, rgbButtons) + i;
                o.dwType = DIDFT_PSHBUTTON | DIDFT_MAKEINSTANCE(i);
                char nm[32]; snprintf(nm, sizeof nm, "Button %d", i); PutText(o.tszName, nm);
                if (cb(&o, ctx) == DIENUM_STOP) return DI_OK;
            }
        }
        if (g_cfg.dpadPOV && (all || (flags & DIDFT_POV))) {
            typename T::ObjInst o = { sizeof(o) };
            o.guidType = GUID_POV; o.dwOfs = (DWORD)offsetof(DIJOYSTATE2, rgdwPOV);
            o.dwType = DIDFT_POV | DIDFT_MAKEINSTANCE(0);
            PutText(o.tszName, "POV");
            cb(&o, ctx);
        }
        return DI_OK;
    }

    HRESULT STDMETHODCALLTYPE GetProperty(REFGUID p, LPDIPROPHEADER h) override {
        if (IS_PROP(p, DIPROP_RANGE)) {
            int i = AxisIdx(h);
            if (i == -2) return DIERR_OBJECTNOTFOUND;
            if (i < 0) i = 0;
            auto* r = (DIPROPRANGE*)h; r->lMin = rmin[i]; r->lMax = rmax[i];
            return DI_OK;
        }
        if (IS_PROP(p, DIPROP_BUFFERSIZE)) { ((DIPROPDWORD*)h)->dwData = bufSize;  return DI_OK; }
        if (IS_PROP(p, DIPROP_FFGAIN))     { ((DIPROPDWORD*)h)->dwData = g_ffGain; return DI_OK; }
        // The game identifies the controller by USB VID/PID and keeps its saved
        // settings against them. Always report the same configured IDs, whatever
        // physical pad is connected, so the settings survive switching controllers.
        if (IS_PROP(p, DIPROP_VIDPID)) {
            ((DIPROPDWORD*)h)->dwData = MAKELONG(g_cfg.vid, g_cfg.pid);
            Log("GetProperty VIDPID -> %04x:%04x", g_cfg.vid, g_cfg.pid);
            return DI_OK;
        }
        if (IS_PROP(p, DIPROP_PRODUCTNAME) || IS_PROP(p, DIPROP_INSTANCENAME)) {
            auto* str = (DIPROPSTRING*)h;
            MultiByteToWideChar(CP_ACP, 0, g_cfg.name.c_str(), -1, str->wsz, MAX_PATH);
            str->wsz[MAX_PATH - 1] = 0;
            return DI_OK;
        }
        if (IS_PROP(p, DIPROP_JOYSTICKID))  { ((DIPROPDWORD*)h)->dwData = 0;     return DI_OK; }
        if (IS_PROP(p, DIPROP_GRANULARITY)) { ((DIPROPDWORD*)h)->dwData = 1;     return DI_OK; }
        if (IS_PROP(p, DIPROP_DEADZONE))    { ((DIPROPDWORD*)h)->dwData = 0;     return DI_OK; }
        if (IS_PROP(p, DIPROP_SATURATION))  { ((DIPROPDWORD*)h)->dwData = 10000; return DI_OK; }
        if (IS_PROP(p, DIPROP_AXISMODE))    { ((DIPROPDWORD*)h)->dwData = DIPROPAXISMODE_ABS; return DI_OK; }
        if (IS_PROP(p, DIPROP_AUTOCENTER))  { ((DIPROPDWORD*)h)->dwData = DIPROPAUTOCENTER_OFF; return DI_OK; }
        if (IS_PROP(p, DIPROP_FFLOAD))      { ((DIPROPDWORD*)h)->dwData = 0;     return DI_OK; }
        Log("GetProperty unhandled (id=%p)", (const void*)&p);
        return DIERR_UNSUPPORTED;
    }

    HRESULT STDMETHODCALLTYPE SetProperty(REFGUID p, LPCDIPROPHEADER h) override {
        if (IS_PROP(p, DIPROP_RANGE)) {
            auto* r = (const DIPROPRANGE*)h; int i = AxisIdx(h);
            Log("SetProperty RANGE how=%lu obj=%lx %ld..%ld", h->dwHow, h->dwObj, r->lMin, r->lMax);
            if (i == -2) return DIERR_OBJECTNOTFOUND;
            for (int k = 0; k < 6; k++) if (i < 0 || i == k) { rmin[k] = r->lMin; rmax[k] = r->lMax; }
            return DI_OK;
        }
        if (IS_PROP(p, DIPROP_BUFFERSIZE)) {
            bufSize = ((const DIPROPDWORD*)h)->dwData; q.clear(); prevValid = false;
            Log("SetProperty BUFFERSIZE %lu", bufSize);
            return DI_OK;
        }
        if (IS_PROP(p, DIPROP_FFGAIN)) { g_ffGain = ((const DIPROPDWORD*)h)->dwData; return DI_OK; }
        return DI_OK;   // DEADZONE, SATURATION, AXISMODE, AUTOCENTER...: accept silently
    }

    HRESULT STDMETHODCALLTYPE Acquire() override   { Log("Acquire"); SdlPoll(); prevValid = false; return DI_OK; }
    HRESULT STDMETHODCALLTYPE Unacquire() override { StopRumble(); q.clear(); prevValid = false; return DI_OK; }
    HRESULT STDMETHODCALLTYPE Poll() override      { SdlPoll(); Pump(); return DI_OK; }

    HRESULT STDMETHODCALLTYPE GetDeviceState(DWORD cb, LPVOID data) override {
        SdlPoll();
        static bool logged = false;
        if (!logged) { logged = true; Log("GetDeviceState: first call, size %lu", cb); }
        DIJOYSTATE2 s; Fill(s);
        if (cb == sizeof(DIJOYSTATE2) || cb == sizeof(DIJOYSTATE)) { memcpy(data, &s, cb); return DI_OK; }
        Log("GetDeviceState: unexpected size %lu (custom data format?)", cb);
        return DIERR_INVALIDPARAM;
    }

    HRESULT STDMETHODCALLTYPE GetDeviceData(DWORD cb, LPDIDEVICEOBJECTDATA out, LPDWORD n, DWORD flags) override {
        if (!n) return E_POINTER;
        if (!bufSize) { *n = 0; return DIERR_NOTBUFFERED; }
        if (cb != sizeof(DIDEVICEOBJECTDATA) && cb != sizeof(DIDEVICEOBJECTDATA_DX3)) return DIERR_INVALIDPARAM;
        Pump();
        bool peek = (flags & DIGDD_PEEK) != 0;
        HRESULT hr = overflow ? DI_BUFFEROVERFLOW : DI_OK;
        if (!out) {                                   // flush
            *n = (DWORD)q.size();
            if (!peek) q.clear();
        } else {
            DWORD cnt = std::min<DWORD>(*n, (DWORD)q.size());
            for (DWORD i = 0; i < cnt; i++) memcpy((BYTE*)out + i * cb, &q[i], cb);   // DX3 struct is a prefix
            *n = cnt;
            if (!peek) q.erase(q.begin(), q.begin() + cnt);
        }
        if (!peek) overflow = false;
        return hr;
    }

    HRESULT STDMETHODCALLTYPE SetDataFormat(LPCDIDATAFORMAT f) override {
        Log("SetDataFormat dwDataSize=%lu numObjs=%lu", f->dwDataSize, f->dwNumObjs);
        return DI_OK;
    }
    HRESULT STDMETHODCALLTYPE SetEventNotification(HANDLE) override { return DI_OK; }
    HRESULT STDMETHODCALLTYPE SetCooperativeLevel(HWND, DWORD f) override { Log("SetCooperativeLevel %lx", f); return DI_OK; }
    HRESULT STDMETHODCALLTYPE GetObjectInfo(typename T::ObjInst*, DWORD, DWORD) override { return DIERR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE GetDeviceInfo(typename T::DevInst* i) override {
        DWORD sz = i->dwSize; memset(i, 0, sz); i->dwSize = sz;
        i->guidInstance = i->guidProduct = GUID_FakePad;
        i->dwDevType = DI8DEVTYPE_GAMEPAD | (DI8DEVTYPEGAMEPAD_STANDARD << 8);
        CopyName(i->tszInstanceName);
        CopyName(i->tszProductName);
        return DI_OK;
    }
    HRESULT STDMETHODCALLTYPE RunControlPanel(HWND, DWORD) override { return DI_OK; }
    HRESULT STDMETHODCALLTYPE Initialize(HINSTANCE, DWORD, REFGUID) override { return DI_OK; }

    // ---- force feedback ----
    HRESULT STDMETHODCALLTYPE CreateEffect(REFGUID g, LPCDIEFFECT e, LPDIRECTINPUTEFFECT* out, LPUNKNOWN) override {
        const EffDef* d = FindEff(g);
        Log("CreateEffect %s", d ? d->name : "unknown");
        if (!PadHasRumble() || !d) return DIERR_UNSUPPORTED;
        auto* fx = new FakeEffect(g);
        if (e) fx->Apply(e, DIEP_ALLPARAMS);
        *out = fx;
        return DI_OK;
    }
    HRESULT STDMETHODCALLTYPE EnumEffects(typename T::EnumEffCb cb, LPVOID ctx, DWORD type) override {
        if (!PadHasRumble()) return DI_OK;
        for (auto& e : kEffs) {
            if (type != DIEFT_ALL && !(type & e.type)) continue;
            typename T::EffInfo info = { sizeof(info) };
            info.guid = *e.g; info.dwEffType = e.type; PutText(info.tszName, e.name);
            if (cb(&info, ctx) == DIENUM_STOP) break;
        }
        return DI_OK;
    }
    HRESULT STDMETHODCALLTYPE GetEffectInfo(typename T::EffInfo* info, REFGUID g) override {
        const EffDef* e = FindEff(g);
        if (!e) return DIERR_DEVICENOTREG;
        DWORD sz = info->dwSize; memset(info, 0, sz); info->dwSize = sz;
        info->guid = *e->g; info->dwEffType = e->type; PutText(info->tszName, e->name);
        return DI_OK;
    }
    HRESULT STDMETHODCALLTYPE GetForceFeedbackState(LPDWORD s) override {
        *s = DIGFFS_ACTUATORSON | DIGFFS_POWERON;
        return DI_OK;
    }
    HRESULT STDMETHODCALLTYPE SendForceFeedbackCommand(DWORD cmd) override {
        if (cmd == DISFFC_RESET || cmd == DISFFC_STOPALL) StopRumble();
        return DI_OK;
    }
    HRESULT STDMETHODCALLTYPE EnumCreatedEffectObjects(LPDIENUMCREATEDEFFECTOBJECTSCALLBACK, LPVOID, DWORD) override { return DI_OK; }
    HRESULT STDMETHODCALLTYPE Escape(LPDIEFFESCAPE) override { return DIERR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE SendDeviceData(DWORD, LPCDIDEVICEOBJECTDATA, LPDWORD, DWORD) override { return DIERR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE EnumEffectsInFile(typename T::Str, LPDIENUMEFFECTSINFILECALLBACK, LPVOID, DWORD) override { return DI_OK; }
    HRESULT STDMETHODCALLTYPE WriteEffectToFile(typename T::Str, DWORD, LPDIFILEEFFECT, DWORD) override { return DIERR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE BuildActionMap(typename T::ActFmt*, typename T::Str, DWORD) override { return DIERR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE SetActionMap(typename T::ActFmt*, typename T::Str, DWORD) override { return DIERR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE GetImageInfo(typename T::ImgHdr*) override { return DIERR_UNSUPPORTED; }
};
using FakePadA = FakePadT<false>;
using FakePadW = FakePadT<true>;

// ============================================================================
// IDirectInput8A / IDirectInput8W wrapper
// ============================================================================
template <bool W> class ProxyDI8T : public DIT<W>::DI8 {
    using T = DIT<W>;
    typename T::DI8* real;
    LONG ref = 1;

    struct Ctx { typename T::EnumDevCb cb; LPVOID user; bool stop; };
    static BOOL CALLBACK Filter(const typename T::DevInst* i, LPVOID p) {
        auto* c = (Ctx*)p;
        BYTE t = GET_DIDEVICE_TYPE(i->dwDevType);
        if (t == DI8DEVTYPE_KEYBOARD || t == DI8DEVTYPE_MOUSE) {
            BOOL r = c->cb(i, c->user);
            if (r == DIENUM_STOP) c->stop = true;
            return r;
        }
        if (!g_cfg.hideReal) {
            BOOL r = c->cb(i, c->user);
            if (r == DIENUM_STOP) c->stop = true;
            return r;
        }
        if constexpr (W) Log("hiding real device: %ls", i->tszProductName);
        else             Log("hiding real device: %s", i->tszProductName);
        return DIENUM_CONTINUE;
    }

public:
    explicit ProxyDI8T(typename T::DI8* r) : real(r) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID r, void** o) override {
        if (r == IID_IUnknown || r == T::DI8Iid()) { *o = this; AddRef(); return S_OK; }
        return real->QueryInterface(r, o);
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&ref); }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = InterlockedDecrement(&ref);
        if (!r) { real->Release(); delete this; }
        return r;
    }

    HRESULT STDMETHODCALLTYPE CreateDevice(REFGUID g, typename T::Device** o, LPUNKNOWN u) override {
        if (g == GUID_FakePad) { *o = new FakePadT<W>(); Log("CreateDevice: fake pad"); return DI_OK; }
        return real->CreateDevice(g, o, u);
    }

    HRESULT STDMETHODCALLTYPE EnumDevices(DWORD type, typename T::EnumDevCb cb, LPVOID user, DWORD flags) override {
        Log("EnumDevices type=%lu flags=%lx", type, flags);
        Ctx c = { cb, user, false };
        real->EnumDevices(type, Filter, &c, flags);
        if (c.stop) return DI_OK;

        bool wantsPads = (type == DI8DEVCLASS_ALL || type == DI8DEVCLASS_GAMECTRL);
        if (!wantsPads) Log("EnumDevices: type %lu is not a pad query, fake pad not offered", type);
        if (wantsPads) {
            SdlPoll();
            bool ffOk = !(flags & DIEDFL_FORCEFEEDBACK) || PadHasRumble();
            Log("EnumDevices: pad=%d alwaysPresent=%d ffOk=%d -> %s", g_pad ? 1 : 0,
                g_cfg.alwaysPresent ? 1 : 0, ffOk ? 1 : 0,
                ((g_pad || g_cfg.alwaysPresent) && ffOk) ? "reporting fake pad" : "NOT reporting fake pad");
            if ((g_pad || g_cfg.alwaysPresent) && ffOk) {
                typename T::DevInst i = { sizeof(i) };
                i.guidInstance = i.guidProduct = GUID_FakePad;
                i.dwDevType = DI8DEVTYPE_GAMEPAD | (DI8DEVTYPEGAMEPAD_STANDARD << 8);
                CopyName(i.tszInstanceName);
                CopyName(i.tszProductName);
                cb(&i, user);
            }
        }
        return DI_OK;
    }

    HRESULT STDMETHODCALLTYPE GetDeviceStatus(REFGUID g) override {
        if (g == GUID_FakePad) { SdlPoll(); return (g_pad || g_cfg.alwaysPresent) ? DI_OK : S_FALSE; }
        return real->GetDeviceStatus(g);
    }
    HRESULT STDMETHODCALLTYPE RunControlPanel(HWND h, DWORD f) override { return real->RunControlPanel(h, f); }
    HRESULT STDMETHODCALLTYPE Initialize(HINSTANCE h, DWORD v) override { return real->Initialize(h, v); }
    HRESULT STDMETHODCALLTYPE FindDevice(REFGUID a, typename T::Str b, LPGUID c) override { return real->FindDevice(a, b, c); }
    HRESULT STDMETHODCALLTYPE EnumDevicesBySemantics(typename T::Str, typename T::ActFmt*, typename T::SemCb, LPVOID, DWORD) override { return DIERR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE ConfigureDevices(LPDICONFIGUREDEVICESCALLBACK, typename T::CfgParams*, DWORD, LPVOID) override { return DIERR_UNSUPPORTED; }
};
using ProxyDI8A = ProxyDI8T<false>;
using ProxyDI8W = ProxyDI8T<true>;

// ============================================================================
// Exported entry point + DllMain
// ============================================================================
static HMODULE LoadRealDInput8() {
    static HMODULE real = nullptr;
    if (!real) {
        char p[MAX_PATH];
        GetSystemDirectoryA(p, MAX_PATH);   // SysWOW64 for a 32-bit process on 64-bit Windows
        strcat(p, "\\dinput8.dll");
        real = LoadLibraryA(p);
        if (!real) Log("failed to load %s (error %lu)", p, GetLastError());
    }
    return real;
}

static HMODULE g_self = nullptr;   // this DLL

// Wrap every DirectInput8Create call EXCEPT the ones SDL makes itself while it
// enumerates controllers (SDL3.dll, or SDL statically linked into this DLL): SDL
// must see the real, unfiltered DirectInput. Everything else is wrapped, including
// calls made by wrapper/mod DLLs on the game's behalf and calls from exe code that
// is not inside a normal module (packed/protected exes). `who` receives the caller's
// module name for the log.
static bool ShouldWrap(void* addr, char* who, size_t n) {
    HMODULE m = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)addr, &m) || !m) {
        snprintf(who, n, "(not inside any module)");
        return true;
    }
    char path[MAX_PATH] = {};
    GetModuleFileNameA(m, path, MAX_PATH);
    const char* base = strrchr(path, '\\');
    snprintf(who, n, "%s%s", base ? base + 1 : path, m == GetModuleHandleA(nullptr) ? " (game exe)" : "");
    if (m == g_self) return false;
    if (_strnicmp(base ? base + 1 : path, "SDL3", 4) == 0) return false;
    return true;
}

extern "C" HRESULT WINAPI DirectInput8Create(HINSTANCE h, DWORD ver, REFIID riid, LPVOID* out, LPUNKNOWN outer) {
    void* caller = CALLER_ADDRESS();
    HMODULE real = LoadRealDInput8();
    if (!real) return E_FAIL;

    using Fn = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
    auto fn = (Fn)GetProcAddress(real, "DirectInput8Create");
    if (!fn) return E_FAIL;

    HRESULT hr = fn(h, ver, riid, out, outer);
    char who[MAX_PATH + 32];
    bool wrap = ShouldWrap(caller, who, sizeof who);
    Log("DirectInput8Create ver=%lx hr=%lx caller=%s wrap=%d", ver, hr, who, wrap ? 1 : 0);

    if (SUCCEEDED(hr) && wrap) {
        if (riid == IID_IDirectInput8A)
            *out = new ProxyDI8A((IDirectInput8A*)*out);
        else if (riid == IID_IDirectInput8W)
            *out = new ProxyDI8W((IDirectInput8W*)*out);
    }
    return hr;
}

BOOL APIENTRY DllMain(HMODULE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        g_self = h;
        GetModuleFileNameA(h, g_dir, MAX_PATH);
        if (char* s = strrchr(g_dir, '\\')) s[1] = 0;
        LoadConfig();
    }
    // No SDL_Quit on detach: running it under the loader lock is unsafe.
    return TRUE;
}