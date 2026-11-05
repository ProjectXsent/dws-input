// dinput.dll -- legacy DirectInput (1..7) proxy that presents an SDL3 gamepad
// as the game's joystick. Part of DWS Input. Drop next to the game exe together
// with SDL3.dll and dws_input.ini (shared with winmm.dll / dinput8.dll).
// Keyboard/mouse (and anything else) are forwarded to the real system dinput.dll.
//
// Build: see the top-level CMakeLists.txt. Exports are listed in dinput.def.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define DIRECTINPUT_VERSION 0x0700
#include <windows.h>
#include <initguid.h>
#include <dinput.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "dws_core.h"
#include "dws_src.h"

#ifndef DIDEVTYPEJOYSTICK_GAMEPAD
#define DIDEVTYPEJOYSTICK_GAMEPAD 4
#endif

// ----------------------------------------------------------------------------
// Module / config
// ----------------------------------------------------------------------------

static HMODULE g_self = nullptr;

struct Config {
    bool log = false;
    bool hideRealPads = true;
    bool alwaysPresent = true;
    int padIndex = 0;
    char productName[128] = "SDL Gamepad";
    unsigned vid = 0x045E, pid = 0x028E;

    int stickDeadzone = 8;
    int triggerThreshold = 30;
    bool dpadAsPov = true;
    int dpadStick = 0;     // 0 = off, 1 = d-pad drives the left stick axes, 2 = right stick axes

    Src axis[6] = { A_LX, A_LY, A_RX, S_NONE, S_NONE, A_RY };
    bool invert[6] = {};
    Src button[32] = {};
    int buttonCount = 0;   // effective count

    bool rumble = true;
    int strength = 100, lowMotor = 100, highMotor = 100;
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
    ini.Open(g_dir, "DInput");
    const char* p = ini.Path().c_str();

    auto geti = [&](const char* sec, const char* key, int def) {
        return ini.GetInt(sec, key, def);
    };
    auto gets = [&](const char* sec, const char* key, const char* def, char* out, DWORD n) {
        ini.GetStr(sec, key, def, out, n);
    };
    const std::string axSec = ini.AxesSection();     // "Axes.DInput"

    Config c;
    c.log = geti("General", "Log", 0) != 0;
    c.hideRealPads = geti("General", "HideRealPads", 1) != 0;
    c.alwaysPresent = geti("General", "AlwaysPresent", 1) != 0;
    c.padIndex = std::max(0, geti("General", "PadIndex", 0));
    gets("General", "ProductName", "SDL Gamepad", c.productName, sizeof c.productName);
    gets("General", "VendorID", "045E", buf, sizeof buf);  c.vid = strtoul(buf, nullptr, 16) & 0xFFFF;
    gets("General", "ProductID", "028E", buf, sizeof buf); c.pid = strtoul(buf, nullptr, 16) & 0xFFFF;

    c.stickDeadzone = std::min(90, std::max(0, geti("Input", "StickDeadzone", 8)));
    c.triggerThreshold = std::min(100, std::max(0, geti("Input", "TriggerThreshold", 30)));
    c.dpadAsPov = geti("Input", "DPadAsPOV", 1) != 0;
    gets("Input", "DPadAsStick", "None", buf, sizeof buf);
    if (!_stricmp(buf, "Left") || !_stricmp(buf, "LeftStick")) c.dpadStick = 1;
    else if (!_stricmp(buf, "Right") || !_stricmp(buf, "RightStick")) c.dpadStick = 2;
    else c.dpadStick = 0;

    static const char* defAxis[6] = { "LeftX", "LeftY", "RightX", "None", "None", "RightY" };
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

    c.rumble = geti("Rumble", "Enabled", 1) != 0;
    c.strength = std::min(200, std::max(0, geti("Rumble", "Strength", 100)));
    c.lowMotor = std::min(200, std::max(0, geti("Rumble", "LowMotor", 100)));
    c.highMotor = std::min(200, std::max(0, geti("Rumble", "HighMotor", 100)));

    g_cfg = c;

    if (g_cfg.log) {
        std::string lp = dws::LogPath(g_dir, "DInput");
        g_logf = fopen(lp.c_str(), "w");
        Log("dinput.dll (DWS Input, SDL3) -- effective settings (%s)%s", p,
            ini.Exists() ? "" : "  [file not found, built-in defaults]");
        Log("[General] HideRealPads=%d AlwaysPresent=%d PadIndex=%d ProductName=%s VID=%04X PID=%04X",
            c.hideRealPads, c.alwaysPresent, c.padIndex, c.productName, c.vid, c.pid);
        Log("[Input] StickDeadzone=%d TriggerThreshold=%d DPadAsPOV=%d",
            c.stickDeadzone, c.triggerThreshold, c.dpadAsPov);
        Log("[Input] DPadAsStick=%s", c.dpadStick == 1 ? "Left" : c.dpadStick == 2 ? "Right" : "None");
        for (int i = 0; i < 6; i++)
            Log("[%s] Axis%d=%s Invert%d=%d", axSec.c_str(), i, kSrcNames[c.axis[i]], i, c.invert[i]);
        Log("[Buttons] ButtonCount=%d", c.buttonCount);
        for (int i = 0; i < c.buttonCount; i++) Log("  Button%d=%s", i, kSrcNames[c.button[i]]);
        Log("[Rumble] Enabled=%d Strength=%d LowMotor=%d HighMotor=%d",
            c.rumble, c.strength, c.lowMotor, c.highMotor);
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
    DWORD pov;
};

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
        SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
        if (!SDL_Init(SDL_INIT_GAMEPAD)) {
            Log("SDL_Init failed: %s", SDL_GetError());
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

    float axis(Src s) { return dpadOverride(s, axisRaw(s)); }

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

    void sample(Snap& s) {
        memset(&s, 0, sizeof s);
        s.pov = 0xFFFFFFFF;
        lock();
        ensureInit();
        if (sdlOk) {
            scan(false);
            SDL_UpdateGamepads();
        }
        if (gp) {
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
    }

    void rumble(Uint16 low, Uint16 high, Uint32 ms) {
        lock();
        if (sdlOk && gp) SDL_RumbleGamepad(gp, low, high, ms);
        unlock();
    }
} g_pad;

// ----------------------------------------------------------------------------
// Force feedback (DirectInput effects -> SDL rumble)
// ----------------------------------------------------------------------------

struct VEffect;

struct Mixer {
    CRITICAL_SECTION cs;
    bool csInit = false;
    std::vector<VEffect*> effects;
    bool paused = false;
    bool actuators = true;
    Uint16 lastLow = 0, lastHigh = 0;
    DWORD lastSent = 0;

    void lock() {
        if (!csInit) { InitializeCriticalSection(&cs); csInit = true; }
        EnterCriticalSection(&cs);
    }
    void unlock() { LeaveCriticalSection(&cs); }

    void update();   // defined after VEffect
} g_mix;

struct VEffect : IDirectInputEffect {
    LONG ref = 1;
    GUID guid{};
    bool playing = false;
    DWORD startTick = 0;
    DWORD durationMs = INFINITE;   // single iteration
    DWORD iterations = 1;
    DWORD gain = 10000;
    float mag = 0.0f;              // 0..1

    VEffect(const GUID& g) : guid(g) {
        g_mix.lock(); g_mix.effects.push_back(this); g_mix.unlock();
    }
    ~VEffect() {
        g_mix.lock();
        g_mix.effects.erase(std::remove(g_mix.effects.begin(), g_mix.effects.end(), this), g_mix.effects.end());
        g_mix.unlock();
        g_mix.update();
    }

    DWORD totalMs() const {
        if (durationMs == INFINITE || iterations == INFINITE) return INFINITE;
        unsigned long long t = (unsigned long long)durationMs * (iterations ? iterations : 1);
        return t >= 0xFFFFFFFFull ? INFINITE : (DWORD)t;
    }

    void applyParams(const DIEFFECT* e, DWORD flags) {
        if (!e) return;
        if (flags & DIEP_DURATION)
            durationMs = e->dwDuration == INFINITE ? INFINITE : e->dwDuration / 1000;
        if (flags & DIEP_GAIN)
            gain = std::min<DWORD>(e->dwGain, 10000);
        if ((flags & DIEP_TYPESPECIFICPARAMS) && e->lpvTypeSpecificParams) {
            if (IsEqualGUID(guid, GUID_ConstantForce) && e->cbTypeSpecificParams >= sizeof(DICONSTANTFORCE)) {
                LONG m = ((const DICONSTANTFORCE*)e->lpvTypeSpecificParams)->lMagnitude;
                mag = std::min(1.0f, fabsf((float)m) / 10000.0f);
            } else if (IsEqualGUID(guid, GUID_RampForce) && e->cbTypeSpecificParams >= sizeof(DIRAMPFORCE)) {
                const DIRAMPFORCE* r = (const DIRAMPFORCE*)e->lpvTypeSpecificParams;
                mag = std::min(1.0f, std::max(fabsf((float)r->lStart), fabsf((float)r->lEnd)) / 10000.0f);
            } else if (e->cbTypeSpecificParams >= sizeof(DIPERIODIC)) {
                mag = std::min(1.0f, ((const DIPERIODIC*)e->lpvTypeSpecificParams)->dwMagnitude / 10000.0f);
            }
        }
    }

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, LPVOID* ppv) override {
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, IID_IDirectInputEffect)) {
            *ppv = static_cast<IDirectInputEffect*>(this); AddRef(); return S_OK;
        }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&ref); }
    ULONG STDMETHODCALLTYPE Release() override {
        LONG r = InterlockedDecrement(&ref);
        if (r == 0) delete this;
        return r;
    }
    // IDirectInputEffect
    HRESULT STDMETHODCALLTYPE Initialize(HINSTANCE, DWORD, REFGUID) override { return DI_OK; }
    HRESULT STDMETHODCALLTYPE GetEffectGuid(LPGUID g) override { if (!g) return E_POINTER; *g = guid; return DI_OK; }
    HRESULT STDMETHODCALLTYPE GetParameters(LPDIEFFECT e, DWORD flags) override {
        if (!e) return E_POINTER;
        if (flags & DIEP_DURATION) e->dwDuration = durationMs == INFINITE ? INFINITE : durationMs * 1000;
        if (flags & DIEP_GAIN) e->dwGain = gain;
        return DI_OK;
    }
    HRESULT STDMETHODCALLTYPE SetParameters(LPCDIEFFECT e, DWORD flags) override {
        applyParams(e, flags);
        if (flags & DIEP_START) Start(1, 0);
        else g_mix.update();
        return DI_OK;
    }
    HRESULT STDMETHODCALLTYPE Start(DWORD iter, DWORD) override {
        iterations = iter;
        startTick = GetTickCount();
        playing = true;
        g_mix.update();
        return DI_OK;
    }
    HRESULT STDMETHODCALLTYPE Stop() override {
        playing = false;
        g_mix.update();
        return DI_OK;
    }
    HRESULT STDMETHODCALLTYPE GetEffectStatus(LPDWORD s) override {
        if (!s) return E_POINTER;
        g_mix.update();
        *s = playing ? DIEGES_PLAYING : 0;
        return DI_OK;
    }
    HRESULT STDMETHODCALLTYPE Download() override { return DI_OK; }
    HRESULT STDMETHODCALLTYPE Unload() override { return DI_OK; }
    HRESULT STDMETHODCALLTYPE Escape(LPDIEFFESCAPE) override { return DIERR_UNSUPPORTED; }
};

void Mixer::update()
{
    lock();
    DWORD now = GetTickCount();
    float level = 0.0f;
    for (VEffect* e : effects) {
        if (!e->playing) continue;
        DWORD tot = e->totalMs();
        if (tot != INFINITE && now - e->startTick >= tot) { e->playing = false; continue; }
        level = std::max(level, e->mag * (e->gain / 10000.0f));
    }
    if (paused || !actuators || !g_cfg.rumble) level = 0.0f;

    float s = level * g_cfg.strength / 100.0f;
    float lo = std::min(1.0f, s * g_cfg.lowMotor / 100.0f);
    float hi = std::min(1.0f, s * g_cfg.highMotor / 100.0f);
    Uint16 low = (Uint16)(lo * 65535.0f), high = (Uint16)(hi * 65535.0f);

    bool changed = low != lastLow || high != lastHigh;
    if (changed || ((low || high) && now - lastSent >= 100)) {
        lastLow = low; lastHigh = high; lastSent = now;
        unlock();
        g_pad.rumble(low, high, (low || high) ? 250 : 0);
        return;
    }
    unlock();
}

// ----------------------------------------------------------------------------
// Charset traits (A / W variants share one implementation)
// ----------------------------------------------------------------------------

static void SetStr(char* d, size_t n, const char* s) { strncpy(d, s, n - 1); d[n - 1] = 0; }
static void SetStr(wchar_t* d, size_t n, const char* s) {
    MultiByteToWideChar(CP_ACP, 0, s, -1, d, (int)n); d[n - 1] = 0;
}

struct TA {
    typedef char CH;
    typedef IDirectInputDevice7A DEV;
    typedef IDirectInputA DI1;
    typedef IDirectInput2A DI2;
    typedef IDirectInput7A DI7;
    typedef DIDEVICEINSTANCEA INST;
    typedef LPCDIDEVICEINSTANCEA CINST;
    typedef DIDEVICEOBJECTINSTANCEA OBJ;
    typedef DIEFFECTINFOA EFFINFO;
    typedef LPCDIEFFECTINFOA CEFFINFO;
    typedef LPDIENUMDEVICESCALLBACKA DEVCB;
    typedef LPDIENUMDEVICEOBJECTSCALLBACKA OBJCB;
    typedef LPDIENUMEFFECTSCALLBACKA EFFCB;
    typedef LPDIRECTINPUTDEVICEA DEVPTR;
    typedef LPCSTR CSTR;
    static bool isDI(REFIID r) { return IsEqualIID(r, IID_IDirectInputA) || IsEqualIID(r, IID_IDirectInput2A) || IsEqualIID(r, IID_IDirectInput7A); }
    static bool isDev(REFIID r) { return IsEqualIID(r, IID_IDirectInputDeviceA) || IsEqualIID(r, IID_IDirectInputDevice2A) || IsEqualIID(r, IID_IDirectInputDevice7A); }
    static const IID& iid2() { return IID_IDirectInput2A; }
    static const IID& iid7() { return IID_IDirectInput7A; }
};

struct TW {
    typedef wchar_t CH;
    typedef IDirectInputDevice7W DEV;
    typedef IDirectInputW DI1;
    typedef IDirectInput2W DI2;
    typedef IDirectInput7W DI7;
    typedef DIDEVICEINSTANCEW INST;
    typedef LPCDIDEVICEINSTANCEW CINST;
    typedef DIDEVICEOBJECTINSTANCEW OBJ;
    typedef DIEFFECTINFOW EFFINFO;
    typedef LPCDIEFFECTINFOW CEFFINFO;
    typedef LPDIENUMDEVICESCALLBACKW DEVCB;
    typedef LPDIENUMDEVICEOBJECTSCALLBACKW OBJCB;
    typedef LPDIENUMEFFECTSCALLBACKW EFFCB;
    typedef LPDIRECTINPUTDEVICEW DEVPTR;
    typedef LPCWSTR CSTR;
    static bool isDI(REFIID r) { return IsEqualIID(r, IID_IDirectInputW) || IsEqualIID(r, IID_IDirectInput2W) || IsEqualIID(r, IID_IDirectInput7W); }
    static bool isDev(REFIID r) { return IsEqualIID(r, IID_IDirectInputDeviceW) || IsEqualIID(r, IID_IDirectInputDevice2W) || IsEqualIID(r, IID_IDirectInputDevice7W); }
    static const IID& iid2() { return IID_IDirectInput2W; }
    static const IID& iid7() { return IID_IDirectInput7W; }
};

// ----------------------------------------------------------------------------
// Virtual device identity
// ----------------------------------------------------------------------------

// Instance GUID of the virtual pad (arbitrary, fixed).
static const GUID kInstanceGuid = { 0x5D1A6C0E, 0x3F4B, 0x4C7A, { 0x9E, 0x21, 0x53, 0x44, 0x4C, 0x33, 0x50, 0x41 } };

static GUID ProductGuid()
{
    GUID g = { (DWORD)((g_cfg.pid << 16) | g_cfg.vid), 0x0000, 0x0000,
               { 0x00, 0x00, 0x50, 0x49, 0x44, 0x56, 0x49, 0x44 } };   // "PIDVID"
    return g;
}

static DWORD VirtualDevType() { return DIDEVTYPE_JOYSTICK | (DIDEVTYPEJOYSTICK_GAMEPAD << 8); }

template <class T>
static void FillInstance(typename T::INST* out)
{
    DWORD sz = out->dwSize;
    typename T::INST tmp;
    memset(&tmp, 0, sizeof tmp);
    tmp.dwSize = sizeof tmp;
    tmp.guidInstance = kInstanceGuid;
    tmp.guidProduct = ProductGuid();
    tmp.dwDevType = VirtualDevType();
    SetStr(tmp.tszInstanceName, ARRAYSIZE(tmp.tszInstanceName), g_cfg.productName);
    SetStr(tmp.tszProductName, ARRAYSIZE(tmp.tszProductName), g_cfg.productName);
    tmp.wUsagePage = 1;
    tmp.wUsage = 5;
    if (sz == 0 || sz > sizeof tmp) sz = sizeof tmp;
    memcpy(out, &tmp, sz);
    out->dwSize = sz;
}

// ----------------------------------------------------------------------------
// Effects table
// ----------------------------------------------------------------------------

struct EffDef { const GUID* guid; DWORD type; const char* name; };
static const EffDef kEffects[] = {
    { &GUID_ConstantForce, DIEFT_CONSTANTFORCE, "Constant Force" },
    { &GUID_RampForce,     DIEFT_RAMPFORCE,     "Ramp" },
    { &GUID_Square,        DIEFT_PERIODIC,      "Square" },
    { &GUID_Sine,          DIEFT_PERIODIC,      "Sine" },
    { &GUID_Triangle,      DIEFT_PERIODIC,      "Triangle" },
    { &GUID_SawtoothUp,    DIEFT_PERIODIC,      "Sawtooth Up" },
    { &GUID_SawtoothDown,  DIEFT_PERIODIC,      "Sawtooth Down" },
};

static const EffDef* FindEffect(REFGUID g)
{
    for (const EffDef& e : kEffects) if (IsEqualGUID(*e.guid, g)) return &e;
    return nullptr;
}

// ----------------------------------------------------------------------------
// Virtual joystick device
// ----------------------------------------------------------------------------

struct Obj {
    GUID guid;
    DWORD type;      // DIDFT_* | instance
    DWORD defOfs;    // offset in DIJOYSTATE
    char name[32];
    int kind;        // 0 axis, 1 pov, 2 button
    int inst;
    LONG fmtOfs;     // offset in the game's data format, -1 if not mapped
};

template <class T>
struct VDev : T::DEV {
    LONG ref = 1;
    std::vector<Obj> objs;
    bool fmtSet = false;
    DWORD fmtSize = 0;
    std::vector<LONG> unmatchedPov;
    LONG rmin[6], rmax[6];
    float dz[6], sat[6];

    // buffered input (GetDeviceData)
    DWORD bufSize = 0;
    std::vector<DIDEVICEOBJECTDATA> q;
    std::vector<DWORD> prevVal;
    bool havePrev = false;
    bool overflow = false;
    DWORD seq = 0;
    unsigned lastMask = 0;
    DWORD lastPov = 0xFFFFFFFF;

    VDev() {
        static const GUID* axg[6] = { &GUID_XAxis, &GUID_YAxis, &GUID_ZAxis, &GUID_RxAxis, &GUID_RyAxis, &GUID_RzAxis };
        static const char* axn[6] = { "X Axis", "Y Axis", "Z Axis", "X Rotation", "Y Rotation", "Z Rotation" };
        for (int i = 0; i < 6; i++) {
            Obj o{};
            o.guid = *axg[i]; o.type = DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE(i);
            o.defOfs = i * 4; o.kind = 0; o.inst = i; o.fmtOfs = -1;
            SetStr(o.name, sizeof o.name, axn[i]);
            objs.push_back(o);
            rmin[i] = 0; rmax[i] = 65535; dz[i] = 0.0f; sat[i] = 1.0f;
        }
        if (g_cfg.dpadAsPov) {
            Obj o{};
            o.guid = GUID_POV; o.type = DIDFT_POV | DIDFT_MAKEINSTANCE(0);
            o.defOfs = 32; o.kind = 1; o.inst = 0; o.fmtOfs = -1;
            SetStr(o.name, sizeof o.name, "POV");
            objs.push_back(o);
        }
        for (int i = 0; i < g_cfg.buttonCount; i++) {
            Obj o{};
            o.guid = GUID_Button; o.type = DIDFT_PSHBUTTON | DIDFT_MAKEINSTANCE(i);
            o.defOfs = 48 + i; o.kind = 2; o.inst = i; o.fmtOfs = -1;
            snprintf(o.name, sizeof o.name, "Button %d", i);
            objs.push_back(o);
        }
    }

    // -- helpers -------------------------------------------------------------
    DWORD objFlags(const Obj& o) const {
        DWORD f = o.kind == 0 ? DIDOI_ASPECTPOSITION : 0;
        if (o.kind == 0 && g_cfg.rumble && o.inst < 2) f |= DIDOI_FFACTUATOR;
        return f;
    }
    LONG ofsOf(const Obj& o) const { return fmtSet ? o.fmtOfs : (LONG)o.defOfs; }

    template <class OBJT>
    void fillObj(OBJT* out, const Obj& o) {
        DWORD sz = out->dwSize;
        OBJT tmp; memset(&tmp, 0, sizeof tmp);
        tmp.dwSize = sizeof tmp;
        tmp.guidType = o.guid;
        tmp.dwOfs = ofsOf(o) < 0 ? 0 : ofsOf(o);
        tmp.dwType = o.type;
        tmp.dwFlags = objFlags(o);
        SetStr(tmp.tszName, ARRAYSIZE(tmp.tszName), o.name);
        if (sz == 0 || sz > sizeof tmp) sz = sizeof tmp;
        memcpy(out, &tmp, sz);
        out->dwSize = sz;
    }

    // Resolve a DIPROPHEADER target to a mask over objs (axes only for ranges).
    HRESULT resolve(const DIPROPHEADER* h, unsigned& axisMask) {
        axisMask = 0;
        if (h->dwHow == DIPH_DEVICE) { axisMask = 0x3F; return DI_OK; }
        for (size_t i = 0; i < objs.size(); i++) {
            const Obj& o = objs[i];
            bool hit = false;
            if (h->dwHow == DIPH_BYOFFSET) hit = ofsOf(o) >= 0 && (DWORD)ofsOf(o) == h->dwObj;
            else if (h->dwHow == DIPH_BYID) hit = (o.type & 0x00FFFFFF) == (h->dwObj & 0x00FFFFFF);
            else return DIERR_UNSUPPORTED;
            if (hit) {
                if (o.kind == 0) axisMask = 1u << o.inst;
                return DI_OK;
            }
        }
        return DIERR_OBJECTNOTFOUND;
    }

    // -- IUnknown -----------------------------------------------------------
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, LPVOID* ppv) override {
        if (IsEqualIID(riid, IID_IUnknown) || T::isDev(riid)) {
            *ppv = static_cast<typename T::DEV*>(this); AddRef(); return DI_OK;
        }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&ref); }
    ULONG STDMETHODCALLTYPE Release() override {
        LONG r = InterlockedDecrement(&ref);
        if (r == 0) delete this;
        return r;
    }

    // -- IDirectInputDevice ---------------------------------------------------
    HRESULT STDMETHODCALLTYPE GetCapabilities(LPDIDEVCAPS c) override {
        if (!c) return E_POINTER;
        DWORD sz = c->dwSize;
        DIDEVCAPS t; memset(&t, 0, sizeof t);
        t.dwSize = sizeof t;
        t.dwFlags = DIDC_ATTACHED | (g_cfg.rumble ? DIDC_FORCEFEEDBACK : 0);
        t.dwDevType = VirtualDevType();
        t.dwAxes = 6;
        t.dwButtons = g_cfg.buttonCount;
        t.dwPOVs = g_cfg.dpadAsPov ? 1 : 0;
        if (g_cfg.rumble) { t.dwFFSamplePeriod = 1000; t.dwFFMinTimeResolution = 1000; }
        if (sz == 0 || sz > sizeof t) sz = sizeof t;
        memcpy(c, &t, sz);
        c->dwSize = sz;
        return DI_OK;
    }

    HRESULT STDMETHODCALLTYPE EnumObjects(typename T::OBJCB cb, LPVOID ref, DWORD flags) override {
        if (!cb) return E_POINTER;
        for (const Obj& o : objs) {
            DWORD mask = flags & 0xFF;
            if (mask != DIDFT_ALL && !(o.type & mask)) continue;
            typename T::OBJ oi; oi.dwSize = sizeof oi;
            fillObj(&oi, o);
            if (cb(&oi, ref) == DIENUM_STOP) break;
        }
        return DI_OK;
    }

    HRESULT STDMETHODCALLTYPE GetProperty(REFGUID g, LPDIPROPHEADER h) override {
        if (!h) return E_POINTER;
        if (&g == &DIPROP_RANGE) {
            unsigned m; HRESULT hr = resolve(h, m); if (FAILED(hr)) return hr;
            if (!m) return DIERR_UNSUPPORTED;
            int i = 0; while (!(m & (1u << i))) i++;
            DIPROPRANGE* r = (DIPROPRANGE*)h; r->lMin = rmin[i]; r->lMax = rmax[i];
            return DI_OK;
        }
        DIPROPDWORD* d = (DIPROPDWORD*)h;
        if (&g == &DIPROP_DEADZONE || &g == &DIPROP_SATURATION) {
            unsigned m; HRESULT hr = resolve(h, m); if (FAILED(hr)) return hr;
            if (!m) return DIERR_UNSUPPORTED;
            int i = 0; while (!(m & (1u << i))) i++;
            d->dwData = (DWORD)((&g == &DIPROP_DEADZONE ? dz[i] : sat[i]) * 10000.0f);
            return DI_OK;
        }
        if (&g == &DIPROP_GRANULARITY) { d->dwData = 1; return DI_OK; }
        if (&g == &DIPROP_BUFFERSIZE) { d->dwData = bufSize; return DI_OK; }
        if (&g == &DIPROP_AXISMODE) { d->dwData = DIPROPAXISMODE_ABS; return DI_OK; }
        if (&g == &DIPROP_FFGAIN) { d->dwData = 10000; return DI_OK; }
        if (&g == &DIPROP_AUTOCENTER) { d->dwData = DIPROPAUTOCENTER_OFF; return DI_OK; }
        if (&g == &DIPROP_JOYSTICKID) { d->dwData = 0; return DI_OK; }
#ifdef DIPROP_VIDPID
        if (&g == &DIPROP_VIDPID) { d->dwData = MAKELONG(g_cfg.vid, g_cfg.pid); return DI_OK; }
#endif
        return DIERR_UNSUPPORTED;
    }

    HRESULT STDMETHODCALLTYPE SetProperty(REFGUID g, LPCDIPROPHEADER h) override {
        if (!h) return E_POINTER;
        if (&g == &DIPROP_RANGE) {
            unsigned m; HRESULT hr = resolve(h, m); if (FAILED(hr)) return hr;
            const DIPROPRANGE* r = (const DIPROPRANGE*)h;
            for (int i = 0; i < 6; i++) if (m & (1u << i)) { rmin[i] = r->lMin; rmax[i] = r->lMax; }
            return DI_OK;
        }
        const DIPROPDWORD* d = (const DIPROPDWORD*)h;
        if (&g == &DIPROP_DEADZONE || &g == &DIPROP_SATURATION) {
            unsigned m; HRESULT hr = resolve(h, m); if (FAILED(hr)) return hr;
            float v = std::min(10000UL, (unsigned long)d->dwData) / 10000.0f;
            for (int i = 0; i < 6; i++) if (m & (1u << i)) { if (&g == &DIPROP_DEADZONE) dz[i] = v; else sat[i] = v > 0 ? v : 1.0f; }
            return DI_OK;
        }
        Log("SetProperty prop=%p how=%lu obj=0x%lX", (const void*)&g, (unsigned long)h->dwHow, (unsigned long)h->dwObj);
        if (&g == &DIPROP_BUFFERSIZE) {
            bufSize = d->dwData;
            q.clear();
            Log("  buffer size = %lu", (unsigned long)bufSize);
            return DI_OK;
        }
        // Axis mode, autocenter, FF gain, etc.: accepted and ignored.
        if (&g == &DIPROP_BUFFERSIZE || &g == &DIPROP_AXISMODE || &g == &DIPROP_AUTOCENTER ||
            &g == &DIPROP_FFGAIN || &g == &DIPROP_GRANULARITY || &g == &DIPROP_JOYSTICKID)
            return DI_OK;
        return DIERR_UNSUPPORTED;
    }

    HRESULT STDMETHODCALLTYPE Acquire() override { LOG_ONCE("Acquire"); return DI_OK; }
    HRESULT STDMETHODCALLTYPE Unacquire() override { LOG_ONCE("Unacquire"); return DI_OK; }

    LONG axisToRange(int i, float f) const {
        float a = fabsf(f);
        if (a <= dz[i]) f = 0.0f;
        if (sat[i] < 1.0f) f = std::max(-1.0f, std::min(1.0f, f / sat[i]));
        f = std::max(-1.0f, std::min(1.0f, f));
        return rmin[i] + (LONG)((f + 1.0f) * 0.5f * (float)(rmax[i] - rmin[i]));
    }

    DWORD valueOf(const Obj& o, const Snap& s) const {
        if (o.kind == 0) return (DWORD)axisToRange(o.inst, s.ax[o.inst]);
        if (o.kind == 1) return s.pov;
        return s.btn[o.inst] ? 0x80 : 0x00;
    }

    // Diff against the previous snapshot and queue buffered events.
    void pump(const Snap& s) {
        if (prevVal.size() != objs.size()) prevVal.assign(objs.size(), 0);
        for (size_t i = 0; i < objs.size(); i++) {
            DWORD v = valueOf(objs[i], s);
            if (havePrev && v != prevVal[i] && bufSize && fmtSet && objs[i].fmtOfs >= 0) {
                if (q.size() >= bufSize) { q.erase(q.begin()); overflow = true; }
                DIDEVICEOBJECTDATA d; memset(&d, 0, sizeof d);
                d.dwOfs = (DWORD)objs[i].fmtOfs;
                d.dwData = v;
                d.dwTimeStamp = GetTickCount();
                d.dwSequence = seq++;
                q.push_back(d);
            }
            prevVal[i] = v;
        }
        havePrev = true;
    }

    void logChange(const Snap& s) {
        unsigned m = 0;
        for (int i = 0; i < 32; i++) if (s.btn[i]) m |= 1u << i;
        if (m != lastMask || s.pov != lastPov) {
            lastMask = m; lastPov = s.pov;
            Log("state: buttons=%08X pov=%lu axes=%.2f,%.2f,%.2f,%.2f,%.2f,%.2f", m, (unsigned long)s.pov,
                s.ax[0], s.ax[1], s.ax[2], s.ax[3], s.ax[4], s.ax[5]);
        }
    }

    HRESULT STDMETHODCALLTYPE GetDeviceState(DWORD cb, LPVOID data) override {
        LOG_ONCE("GetDeviceState first call (cb=%lu, gamepad=%d)", (unsigned long)cb, g_pad.gp != nullptr);
        if (!data) return E_POINTER;
        if (!fmtSet) return DIERR_INVALIDPARAM;
        Snap s; g_pad.sample(s);
        pump(s);
        logChange(s);
        g_mix.update();
        memset(data, 0, cb);
        BYTE* b = (BYTE*)data;
        for (LONG ofs : unmatchedPov)
            if (ofs >= 0 && (DWORD)ofs + 4 <= cb) *(DWORD*)(b + ofs) = 0xFFFFFFFF;
        for (const Obj& o : objs) {
            LONG ofs = o.fmtOfs;
            if (ofs < 0) continue;
            if (o.kind == 0 && (DWORD)ofs + 4 <= cb) *(LONG*)(b + ofs) = axisToRange(o.inst, s.ax[o.inst]);
            else if (o.kind == 1 && (DWORD)ofs + 4 <= cb) *(DWORD*)(b + ofs) = s.pov;
            else if (o.kind == 2 && (DWORD)ofs + 1 <= cb) b[ofs] = s.btn[o.inst] ? 0x80 : 0x00;
        }
        return DI_OK;
    }

    HRESULT STDMETHODCALLTYPE GetDeviceData(DWORD cbObj, LPDIDEVICEOBJECTDATA out, LPDWORD n, DWORD flags) override {
        LOG_ONCE("GetDeviceData first call (cbObj=%lu, bufSize=%lu)", (unsigned long)cbObj, (unsigned long)bufSize);
        if (!n) return E_POINTER;
        Snap s; g_pad.sample(s);
        pump(s);
        logChange(s);
        DWORD want = *n;
        if (!out) {                       // flush request
            *n = (DWORD)q.size();
            if (!(flags & DIGDD_PEEK)) q.clear();
            return DI_OK;
        }
        DWORD take = (DWORD)std::min<size_t>(want, q.size());
        DWORD cp = std::min<DWORD>(cbObj, sizeof(DIDEVICEOBJECTDATA));
        for (DWORD i = 0; i < take; i++) {
            memset((BYTE*)out + (size_t)i * cbObj, 0, cbObj);
            memcpy((BYTE*)out + (size_t)i * cbObj, &q[i], cp);
        }
        *n = take;
        if (!(flags & DIGDD_PEEK)) q.erase(q.begin(), q.begin() + take);
        if (overflow) { overflow = false; return DI_BUFFEROVERFLOW; }
        return DI_OK;
    }

    HRESULT STDMETHODCALLTYPE SetDataFormat(LPCDIDATAFORMAT df) override {
        if (!df) return E_POINTER;
        for (Obj& o : objs) o.fmtOfs = -1;
        unmatchedPov.clear();
        std::vector<bool> used(objs.size(), false);
        for (DWORD k = 0; k < df->dwNumObjs; k++) {
            const DIOBJECTDATAFORMAT& e = df->rgodf[k];
            DWORD cls = e.dwType & 0xFF;                       // DIDFT class bits
            bool anyInst = (e.dwType & DIDFT_ANYINSTANCE) == DIDFT_ANYINSTANCE;
            DWORD inst = DIDFT_GETINSTANCE(e.dwType);
            int found = -1;
            for (size_t i = 0; i < objs.size(); i++) {
                const Obj& o = objs[i];
                if (used[i]) continue;
                if (e.pguid) {
                    if (!IsEqualGUID(*e.pguid, o.guid)) continue;
                } else if (!(o.type & cls)) continue;
                if (cls && !(o.type & cls)) continue;
                if (!anyInst && (DWORD)o.inst != inst) continue;
                found = (int)i; break;
            }
            if (found >= 0) { objs[found].fmtOfs = (LONG)e.dwOfs; used[found] = true; }
            else if (cls & DIDFT_POV) unmatchedPov.push_back((LONG)e.dwOfs);
        }
        fmtSet = true;
        fmtSize = df->dwDataSize;
        Log("SetDataFormat: %lu objects, size %lu", (unsigned long)df->dwNumObjs, (unsigned long)df->dwDataSize);
        return DI_OK;
    }

    HRESULT STDMETHODCALLTYPE SetEventNotification(HANDLE) override { return DI_OK; }
    HRESULT STDMETHODCALLTYPE SetCooperativeLevel(HWND h, DWORD f) override {
        Log("SetCooperativeLevel hwnd=%p flags=0x%lX", (void*)h, (unsigned long)f);
        return DI_OK;
    }

    HRESULT STDMETHODCALLTYPE GetObjectInfo(typename T::OBJ* out, DWORD dwObj, DWORD how) override {
        if (!out) return E_POINTER;
        for (const Obj& o : objs) {
            bool hit = false;
            if (how == DIPH_BYOFFSET) hit = ofsOf(o) >= 0 && (DWORD)ofsOf(o) == dwObj;
            else if (how == DIPH_BYID) hit = (o.type & 0x00FFFFFF) == (dwObj & 0x00FFFFFF);
            else return DIERR_UNSUPPORTED;
            if (hit) { fillObj(out, o); return DI_OK; }
        }
        return DIERR_OBJECTNOTFOUND;
    }

    HRESULT STDMETHODCALLTYPE GetDeviceInfo(typename T::INST* out) override {
        if (!out) return E_POINTER;
        FillInstance<T>(out);
        return DI_OK;
    }
    HRESULT STDMETHODCALLTYPE RunControlPanel(HWND, DWORD) override { return DI_OK; }
    HRESULT STDMETHODCALLTYPE Initialize(HINSTANCE, DWORD, REFGUID) override { return DI_OK; }

    // -- IDirectInputDevice2 --------------------------------------------------
    HRESULT STDMETHODCALLTYPE CreateEffect(REFGUID g, LPCDIEFFECT e, LPDIRECTINPUTEFFECT* out, LPUNKNOWN outer) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (outer) return CLASS_E_NOAGGREGATION;
        if (!g_cfg.rumble) return DIERR_UNSUPPORTED;
        if (!FindEffect(g)) return DIERR_DEVICENOTREG;
        VEffect* fx = new VEffect(g);
        if (e) fx->applyParams(e, DIEP_DURATION | DIEP_GAIN | DIEP_TYPESPECIFICPARAMS);
        *out = fx;
        return DI_OK;
    }

    HRESULT STDMETHODCALLTYPE EnumEffects(typename T::EFFCB cb, LPVOID ref, DWORD type) override {
        if (!cb) return E_POINTER;
        if (!g_cfg.rumble) return DI_OK;
        for (const EffDef& d : kEffects) {
            if (type != DIEFT_ALL && (type & 0xFF) != d.type) continue;
            typename T::EFFINFO info; memset(&info, 0, sizeof info);
            info.dwSize = sizeof info; info.guid = *d.guid; info.dwEffType = d.type;
            info.dwDynamicParams = DIEP_DURATION | DIEP_GAIN | DIEP_TYPESPECIFICPARAMS | DIEP_START;
            SetStr(info.tszName, ARRAYSIZE(info.tszName), d.name);
            if (cb(&info, ref) == DIENUM_STOP) break;
        }
        return DI_OK;
    }

    HRESULT STDMETHODCALLTYPE GetEffectInfo(typename T::EFFINFO* info, REFGUID g) override {
        if (!info) return E_POINTER;
        const EffDef* d = g_cfg.rumble ? FindEffect(g) : nullptr;
        if (!d) return DIERR_DEVICENOTREG;
        DWORD sz = info->dwSize;
        typename T::EFFINFO t; memset(&t, 0, sizeof t);
        t.dwSize = sizeof t; t.guid = *d->guid; t.dwEffType = d->type;
        t.dwDynamicParams = DIEP_DURATION | DIEP_GAIN | DIEP_TYPESPECIFICPARAMS | DIEP_START;
        SetStr(t.tszName, ARRAYSIZE(t.tszName), d->name);
        if (sz == 0 || sz > sizeof t) sz = sizeof t;
        memcpy(info, &t, sz); info->dwSize = sz;
        return DI_OK;
    }

    HRESULT STDMETHODCALLTYPE GetForceFeedbackState(LPDWORD s) override {
        if (!s) return E_POINTER;
        *s = g_mix.actuators ? DIGFFS_ACTUATORSON : DIGFFS_ACTUATORSOFF;
        return DI_OK;
    }

    HRESULT STDMETHODCALLTYPE SendForceFeedbackCommand(DWORD cmd) override {
        g_mix.lock();
        if (cmd & (DISFFC_RESET | DISFFC_STOPALL))
            for (VEffect* e : g_mix.effects) e->playing = false;
        if (cmd & DISFFC_PAUSE) g_mix.paused = true;
        if (cmd & DISFFC_CONTINUE) g_mix.paused = false;
        if (cmd & DISFFC_SETACTUATORSON) g_mix.actuators = true;
        if (cmd & DISFFC_SETACTUATORSOFF) g_mix.actuators = false;
        g_mix.unlock();
        g_mix.update();
        return DI_OK;
    }

    HRESULT STDMETHODCALLTYPE EnumCreatedEffectObjects(LPDIENUMCREATEDEFFECTOBJECTSCALLBACK, LPVOID, DWORD) override { return DI_OK; }
    HRESULT STDMETHODCALLTYPE Escape(LPDIEFFESCAPE) override { return DIERR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE Poll() override {
        LOG_ONCE("Poll first call");
        Snap s; g_pad.sample(s);     // keeps SDL state fresh for rumble/hotplug
        pump(s);
        g_mix.update();
        return DI_OK;
    }
    HRESULT STDMETHODCALLTYPE SendDeviceData(DWORD, LPCDIDEVICEOBJECTDATA, LPDWORD n, DWORD) override {
        if (n) *n = 0;
        return DIERR_UNSUPPORTED;
    }

    // -- IDirectInputDevice7 --------------------------------------------------
    HRESULT STDMETHODCALLTYPE EnumEffectsInFile(typename T::CSTR, LPDIENUMEFFECTSINFILECALLBACK, LPVOID, DWORD) override { return DIERR_UNSUPPORTED; }
    HRESULT STDMETHODCALLTYPE WriteEffectToFile(typename T::CSTR, DWORD, LPDIFILEEFFECT, DWORD) override { return DIERR_UNSUPPORTED; }
};

// ----------------------------------------------------------------------------
// Real dinput.dll
// ----------------------------------------------------------------------------

typedef HRESULT(WINAPI* PFN_DICreateA)(HINSTANCE, DWORD, LPDIRECTINPUTA*, LPUNKNOWN);
typedef HRESULT(WINAPI* PFN_DICreateW)(HINSTANCE, DWORD, LPDIRECTINPUTW*, LPUNKNOWN);
typedef HRESULT(WINAPI* PFN_DICreateEx)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
typedef HRESULT(WINAPI* PFN_DllCanUnloadNow)();
typedef HRESULT(WINAPI* PFN_DllGetClassObject)(REFCLSID, REFIID, LPVOID*);
typedef HRESULT(WINAPI* PFN_DllReg)();

static HMODULE RealDll()
{
    static HMODULE h = nullptr;
    if (!h) {
        char dir[MAX_PATH];
        GetSystemDirectoryA(dir, MAX_PATH);
        std::string p = std::string(dir) + "\\dinput.dll";
        h = LoadLibraryA(p.c_str());
        Log("Real dinput.dll: %s -> %p", p.c_str(), (void*)h);
    }
    return h;
}

template <class F> static F RealFn(const char* name)
{
    HMODULE h = RealDll();
    return h ? (F)GetProcAddress(h, name) : nullptr;
}

// ----------------------------------------------------------------------------
// IDirectInput wrapper (covers IDirectInput, 2 and 7 -- they are prefixes)
// ----------------------------------------------------------------------------

template <class T>
struct VDI : T::DI7 {
    LONG ref = 1;
    typename T::DI1* r1 = nullptr;
    typename T::DI2* r2 = nullptr;
    typename T::DI7* r7 = nullptr;

    // takes ownership of one reference to 'real'
    explicit VDI(IUnknown* real) {
        r1 = (typename T::DI1*)real;
        real->QueryInterface(T::iid2(), (void**)&r2);
        real->QueryInterface(T::iid7(), (void**)&r7);
    }
    ~VDI() {
        if (r7) r7->Release();
        if (r2) r2->Release();
        if (r1) r1->Release();
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, LPVOID* ppv) override {
        if (IsEqualIID(riid, IID_IUnknown) || T::isDI(riid)) {
            *ppv = static_cast<typename T::DI7*>(this); AddRef(); return DI_OK;
        }
        return r1->QueryInterface(riid, ppv);
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&ref); }
    ULONG STDMETHODCALLTYPE Release() override {
        LONG r = InterlockedDecrement(&ref);
        if (r == 0) delete this;
        return r;
    }

    HRESULT STDMETHODCALLTYPE CreateDevice(REFGUID g, typename T::DEVPTR* out, LPUNKNOWN outer) override {
        if (IsEqualGUID(g, kInstanceGuid)) {
            if (!out) return E_POINTER;
            if (outer) return CLASS_E_NOAGGREGATION;
            *out = static_cast<typename T::DEVPTR>(new VDev<T>());
            Log("CreateDevice: virtual SDL pad");
            return DI_OK;
        }
        return r1->CreateDevice(g, out, outer);
    }

    struct EnumCtx { typename T::DEVCB cb; LPVOID ref; bool hide; };
    static BOOL CALLBACK Thunk(typename T::CINST p, LPVOID c) {
        EnumCtx* x = (EnumCtx*)c;
        if (x->hide && (LOBYTE(p->dwDevType) == DIDEVTYPE_JOYSTICK)) return DIENUM_CONTINUE;
        return x->cb(p, x->ref);
    }

    HRESULT STDMETHODCALLTYPE EnumDevices(DWORD type, typename T::DEVCB cb, LPVOID ref, DWORD flags) override {
        if (!cb) return E_POINTER;
        EnsureConfig();
        bool wantVirtual = (type == 0 || type == DIDEVTYPE_JOYSTICK);
        if (wantVirtual && (flags & DIEDFL_FORCEFEEDBACK) && !g_cfg.rumble) wantVirtual = false;
        if (wantVirtual && !g_cfg.alwaysPresent && !g_pad.present()) wantVirtual = false;
        if (wantVirtual) {
            typename T::INST inst; memset(&inst, 0, sizeof inst); inst.dwSize = sizeof inst;
            FillInstance<T>(&inst);
            if (cb(&inst, ref) == DIENUM_STOP) return DI_OK;
        }
        if (g_cfg.hideRealPads && type == DIDEVTYPE_JOYSTICK) return DI_OK;
        EnumCtx ctx{ cb, ref, g_cfg.hideRealPads };
        return r1->EnumDevices(type, &Thunk, &ctx, flags);
    }

    HRESULT STDMETHODCALLTYPE GetDeviceStatus(REFGUID g) override {
        if (IsEqualGUID(g, kInstanceGuid))
            return (g_cfg.alwaysPresent || g_pad.present()) ? DI_OK : DI_NOTATTACHED;
        return r1->GetDeviceStatus(g);
    }
    HRESULT STDMETHODCALLTYPE RunControlPanel(HWND h, DWORD f) override { return r1->RunControlPanel(h, f); }
    HRESULT STDMETHODCALLTYPE Initialize(HINSTANCE h, DWORD v) override { return r1->Initialize(h, v); }

    // IDirectInput2
    HRESULT STDMETHODCALLTYPE FindDevice(REFGUID cls, typename T::CSTR name, LPGUID inst) override {
        if (!r2) return DIERR_UNSUPPORTED;
        return r2->FindDevice(cls, name, inst);
    }
    // IDirectInput7
    HRESULT STDMETHODCALLTYPE CreateDeviceEx(REFGUID g, REFIID riid, LPVOID* out, LPUNKNOWN outer) override {
        if (IsEqualGUID(g, kInstanceGuid)) {
            if (!out) return E_POINTER;
            if (outer) return CLASS_E_NOAGGREGATION;
            VDev<T>* d = new VDev<T>();
            HRESULT hr = d->QueryInterface(riid, out);
            d->Release();
            Log("CreateDeviceEx: virtual SDL pad -> 0x%08lX", (unsigned long)hr);
            return hr;
        }
        if (!r7) return DIERR_UNSUPPORTED;
        return r7->CreateDeviceEx(g, riid, out, outer);
    }
};

// ----------------------------------------------------------------------------
// Exports
// ----------------------------------------------------------------------------

extern "C" HRESULT WINAPI DirectInputCreateA(HINSTANCE h, DWORD v, LPDIRECTINPUTA* out, LPUNKNOWN outer)
{
    EnsureConfig();
    auto fn = RealFn<PFN_DICreateA>("DirectInputCreateA");
    if (!fn) return DIERR_GENERIC;
    LPDIRECTINPUTA real = nullptr;
    HRESULT hr = fn(h, v, &real, outer);
    if (FAILED(hr) || !real) { if (out) *out = nullptr; return hr; }
    *out = static_cast<IDirectInput7A*>(new VDI<TA>(real));
    Log("DirectInputCreateA(ver=0x%lX)", (unsigned long)v);
    return DI_OK;
}

extern "C" HRESULT WINAPI DirectInputCreateW(HINSTANCE h, DWORD v, LPDIRECTINPUTW* out, LPUNKNOWN outer)
{
    EnsureConfig();
    auto fn = RealFn<PFN_DICreateW>("DirectInputCreateW");
    if (!fn) return DIERR_GENERIC;
    LPDIRECTINPUTW real = nullptr;
    HRESULT hr = fn(h, v, &real, outer);
    if (FAILED(hr) || !real) { if (out) *out = nullptr; return hr; }
    *out = static_cast<IDirectInput7W*>(new VDI<TW>(real));
    Log("DirectInputCreateW(ver=0x%lX)", (unsigned long)v);
    return DI_OK;
}

extern "C" HRESULT WINAPI DirectInputCreateEx(HINSTANCE h, DWORD v, REFIID riid, LPVOID* out, LPUNKNOWN outer)
{
    EnsureConfig();
    auto fn = RealFn<PFN_DICreateEx>("DirectInputCreateEx");
    if (!fn) return DIERR_GENERIC;
    LPVOID real = nullptr;
    HRESULT hr = fn(h, v, riid, &real, outer);
    if (FAILED(hr) || !real) { if (out) *out = nullptr; return hr; }
    if (TA::isDI(riid)) *out = static_cast<IDirectInput7A*>(new VDI<TA>((IUnknown*)real));
    else if (TW::isDI(riid)) *out = static_cast<IDirectInput7W*>(new VDI<TW>((IUnknown*)real));
    else *out = real;
    Log("DirectInputCreateEx(ver=0x%lX)", (unsigned long)v);
    return DI_OK;
}

extern "C" HRESULT WINAPI DllCanUnloadNow() { return S_FALSE; }

extern "C" HRESULT WINAPI DllGetClassObject(REFCLSID c, REFIID i, LPVOID* o)
{
    auto fn = RealFn<PFN_DllGetClassObject>("DllGetClassObject");
    return fn ? fn(c, i, o) : CLASS_E_CLASSNOTAVAILABLE;
}
extern "C" HRESULT WINAPI DllRegisterServer()
{
    auto fn = RealFn<PFN_DllReg>("DllRegisterServer");
    return fn ? fn() : E_NOTIMPL;
}
extern "C" HRESULT WINAPI DllUnregisterServer()
{
    auto fn = RealFn<PFN_DllReg>("DllUnregisterServer");
    return fn ? fn() : E_NOTIMPL;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = inst;
        DisableThreadLibraryCalls(inst);
        char p[MAX_PATH];
        GetModuleFileNameA(inst, p, MAX_PATH);
        std::string s = p;
        size_t k = s.find_last_of("\\/");
        g_dir = k == std::string::npos ? "" : s.substr(0, k + 1);
    } else if (reason == DLL_PROCESS_DETACH) {
        // Intentionally no SDL_Quit / rumble stop here: the loader lock is held and
        // the process is going away anyway.
    }
    return TRUE;
}