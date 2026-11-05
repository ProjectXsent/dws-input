// DWS Input -- shared core used by all three proxy DLLs (winmm, dinput, dinput8).
//
// All three DLLs read ONE config file, dws_input.ini, from the folder they live in.
//
// Lookup rule for every key: the DLL's own override section is checked first
// ([WinMM], [DInput] or [DInput8]), then the normal section. So any key can be
// shared by default and overridden for a single DLL.
//
// Axis mappings are the exception: the APIs order axes differently, so they have
// their own sections, [Axes.WinMM] (X,Y,Z,R,U,V) and [Axes.DInput] (X,Y,Z,Rx,Ry,Rz;
// used by both dinput.dll and dinput8.dll). AxesSection() picks the right one.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <string>

namespace dws {

static const char* const kIniName = "dws_input.ini";

// Folder containing the given module, with trailing '\'.
inline std::string ModuleDir(HMODULE m)
{
    char p[MAX_PATH] = {};
    GetModuleFileNameA(m, p, MAX_PATH);
    std::string s = p;
    size_t k = s.find_last_of("\\/");
    return k == std::string::npos ? std::string() : s.substr(0, k + 1);
}

// e.g. LogPath("C:\\game\\", "DInput8") -> "C:\\game\\dws_input_dinput8.log"
// One log per DLL, because a game can load several of the proxies at once.
inline std::string LogPath(const std::string& dir, const char* tag)
{
    std::string t = tag;
    for (auto& c : t) c = (char)std::tolower((unsigned char)c);
    return dir + "dws_input_" + t + ".log";
}

class Ini {
public:
    void Open(const std::string& dir, const char* tag)
    {
        path_ = dir + kIniName;
        tag_ = tag;
        exists_ = GetFileAttributesA(path_.c_str()) != INVALID_FILE_ATTRIBUTES;
    }

    bool Exists() const { return exists_; }
    const std::string& Path() const { return path_; }
    const std::string& Tag() const { return tag_; }

    // Section that holds this DLL's axis mapping.
    std::string AxesSection() const { return tag_ == "WinMM" ? "Axes.WinMM" : "Axes.DInput"; }

    // String value; trailing "; comment" and surrounding blanks are removed.
    void GetStr(const char* sec, const char* key, const char* def, char* out, size_t n) const
    {
        char buf[512];
        bool found = false;
        if (!tag_.empty() && tag_ != sec) found = Raw(tag_.c_str(), key, buf, sizeof buf);
        if (!found) found = Raw(sec, key, buf, sizeof buf);
        if (!found) { Copy(out, n, def); return; }

        if (char* sc = strchr(buf, ';')) *sc = 0;
        int e = (int)strlen(buf) - 1;
        while (e >= 0 && (buf[e] == ' ' || buf[e] == '\t')) buf[e--] = 0;
        const char* s = buf;
        while (*s == ' ' || *s == '\t') s++;
        Copy(out, n, s);
    }

    int GetInt(const char* sec, const char* key, int def) const
    {
        char buf[64];
        GetStr(sec, key, "", buf, sizeof buf);
        if (!buf[0]) return def;
        char* end = nullptr;
        long v = strtol(buf, &end, 10);
        return end == buf ? def : (int)v;
    }

private:
    bool Raw(const char* sec, const char* key, char* out, size_t n) const
    {
        static const char kMissing[] = "\x01<dws-missing>\x01";
        GetPrivateProfileStringA(sec, key, kMissing, out, (DWORD)n, path_.c_str());
        return strcmp(out, kMissing) != 0;
    }
    static void Copy(char* d, size_t n, const char* s)
    {
        if (!n) return;
        strncpy(d, s, n - 1);
        d[n - 1] = 0;
    }

    std::string path_, tag_;
    bool exists_ = false;
};

} // namespace dws
