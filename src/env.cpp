#include "env.hpp"

#include <cstdio>
#include <vector>

namespace env {

UINT DpiForWindowSafe(HWND h) {
    typedef UINT(WINAPI *GetDpiForWindow_t)(HWND);
    static GetDpiForWindow_t fn =
        (GetDpiForWindow_t)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
    if (fn) {
        UINT dpi = fn(h);
        if (dpi) return dpi;
    }
    HDC hdc = GetDC(h);
    if (!hdc) return 96;
    UINT dpi = (UINT)GetDeviceCaps(hdc, LOGPIXELSX);
    ReleaseDC(h, hdc);
    return dpi ? dpi : 96;
}

UINT DpiForSystemSafe() {
    typedef UINT(WINAPI *GetDpiForSystem_t)();
    static GetDpiForSystem_t fn =
        (GetDpiForSystem_t)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForSystem");
    UINT dpi = fn ? fn() : 96;
    return dpi ? dpi : 96;
}

// GetSystemMetricsForDpi exists only on Windows 10 1607+. Resolve dynamically and fall back to
// GetSystemMetrics (system-DPI metrics) so the binary still loads on Windows 7 / Win10 pre-1607.
int GetSystemMetricsForDpiSafe(int nIndex, UINT dpi) {
    typedef int(WINAPI *GetSystemMetricsForDpi_t)(int, UINT);
    static GetSystemMetricsForDpi_t fn =
        (GetSystemMetricsForDpi_t)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetSystemMetricsForDpi");
    if (fn) {
        int v = fn(nIndex, dpi);
        if (v > 0) return v;
    }
    return GetSystemMetrics(nIndex);
}
std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    if (n <= 0) return L"";
    std::wstring out((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), out.data(), n);
    return out;
}

std::string WideToUtf8(const std::wstring& s) {
    if (s.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return "";
    std::string out((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), out.data(), n, nullptr, nullptr);
    return out;
}

// ---------- string helpers ----------
std::string Trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) --e;
    return s.substr(b, e - b);
}

std::string ToLower(const std::string& s) {
    std::string out = s;
    for (auto& c : out) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return out;
}



bool StartsWithNoCase(const std::string& s, const char* prefix) {
    size_t n = 0; while (prefix[n]) ++n;
    if (s.size() < n) return false;
    for (size_t i = 0; i < n; ++i) {
        char a = s[i], b = prefix[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b) return false;
    }
    return true;
}

// ---------- file helpers ----------
bool FileExistsW(const std::wstring& path) {
    DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

bool ReadFileUtf8(const std::wstring& path, std::string& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(h, &size) || size.QuadPart < 0 || size.QuadPart > 64LL * 1024 * 1024) { CloseHandle(h); return false; }
    out.clear();
    out.reserve((size_t)size.QuadPart);
    char buf[8192];
    DWORD rd = 0;
    bool readOk = true;
    for (;;) {
        if (!ReadFile(h, buf, sizeof(buf), &rd, nullptr)) { readOk = false; break; }
        if (rd == 0) break;
        // The file is shared for writing and can grow after GetFileSizeEx.
        if (out.size() + rd > 64u * 1024 * 1024) { readOk = false; break; }
        out.append(buf, rd);
    }
    CloseHandle(h);
    if (!readOk) { out.clear(); return false; }
    // Strip UTF-8 BOM if present
    if (out.size() >= 3 && (unsigned char)out[0] == 0xEF && (unsigned char)out[1] == 0xBB && (unsigned char)out[2] == 0xBF)
        out.erase(0, 3);
    return true;
}

bool WriteFileUtf8(const std::wstring& path, const std::string& data) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    const char* p = data.data();
    size_t left = data.size();
    while (left > 0) {
        DWORD wrote = 0;
        DWORD chunk = (DWORD)(left > 65536 ? 65536 : left);
        if (!WriteFile(h, p, chunk, &wrote, nullptr) || wrote == 0) {
            CloseHandle(h);
            DeleteFileW(path.c_str());   // never leave a truncated file behind
            return false;
        }
        p += wrote; left -= wrote;
    }
    CloseHandle(h);
    return true;
}

bool EnsureParentDirectoryExists(const std::wstring& path) {
    size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return true;         // bare filename -> CWD
    std::wstring dir = path.substr(0, slash);
    if (dir.empty()) return true;                          // root
    if (dir.size() == 2 && dir[1] == L':') dir += L'\\';   // "C:" means the per-drive CWD, not the root
    DWORD attrs = GetFileAttributesW(dir.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring System32Path(const std::wstring& exeName) {
    wchar_t dir[MAX_PATH + 1] = {};
    UINT n = GetSystemDirectoryW(dir, MAX_PATH);
    if (n > 0 && n < MAX_PATH) return std::wstring(dir) + L"\\" + exeName;
    return exeName; // worst-case fallback
}

// Normalize for comparison: lowercase, '/' -> '\\', strip trailing backslashes (but keep "c:\")
static std::wstring NormalizeDir(const std::wstring& dir) {
    std::wstring s = dir;
    for (auto& c : s) {
        if (c >= L'A' && c <= L'Z') c = (wchar_t)(c - L'A' + L'a');
        if (c == L'/') c = L'\\';
    }
    if (s.size() == 2 && s[1] == L':') s += L'\\';          // "C:" means the drive root for comparison
    while (s.size() > 3 && s.back() == L'\\') s.pop_back();
    return s;
}

bool IsTempJobFilePath(const std::wstring& path) {
    if (path.empty()) return false;

    // Require a rooted path (drive letter or UNC share) — nothing relative is ever a job file
    bool rooted = (path.size() >= 3 && path[1] == L':' && (path[2] == L'\\' || path[2] == L'/')) ||
                  (path.size() >= 2 && path[0] == L'\\' && path[1] == L'\\');
    if (!rooted) return false;

    // Split into components and reject any literal ".." component (a raw substring check
    // would false-positive on legitimate names like "j..doe")
    std::vector<std::wstring> parts;
    size_t i = 0;
    while (i < path.size()) {
        size_t j = path.find_first_of(L"\\/", i);
        std::wstring p = path.substr(i, j == std::wstring::npos ? std::wstring::npos : j - i);
        parts.push_back(p);
        if (j == std::wstring::npos) break;
        i = j + 1;
    }
    if (parts.empty()) return false;
    for (const auto& p : parts) if (p == L"..") return false;

    auto lower = [](std::wstring s) {
        for (auto& c : s) if (c >= L'A' && c <= L'Z') c = (wchar_t)(c - L'A' + L'a');
        return s;
    };
    std::wstring name = lower(parts.back());

    // Filename must look like the WinKick job file produced by MakeRandomJobPath:
    // wkj + 1..32 hex + ".tmp" (matches both 16-bit legacy names and 128-bit random names)
    if (name.size() < 4 + 3 || name.compare(0, 3, L"wkj") != 0 ||
        name.compare(name.size() - 4, 4, L".tmp") != 0) {
        return false;
    }
    std::wstring mid = name.substr(3, name.size() - 3 - 4);
    if (mid.empty() || mid.size() > 32) return false;
    for (wchar_t c : mid) {
        if (!((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f'))) return false;
    }

    size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return false;
    std::wstring parent = path.substr(0, slash);
    std::wstring parentLow = NormalizeDir(parent);

    // Parent must be a temp directory. Preferred test: exact match with the process's resolved
    // temp dir (covers relocated/custom %TEMP% such as D:\Scratch, C:\, and RDS per-session dirs).
    wchar_t tempDir[MAX_PATH + 1] = {};
    DWORD tn = GetTempPathW(MAX_PATH, tempDir);
    if (tn > 0 && tn < MAX_PATH) {
        if (NormalizeDir(tempDir) == parentLow) return true;
    }

    // Fallback for Over-the-Shoulder UAC (admin's %TEMP% differs from the user's) — ONLY the
    // standard per-user AppData\Local\Temp pattern (with optional RDS <digits> subdir) and only
    // on local drive paths (UNC network shares are never trusted as delete/online-bypass targets).
    // Arbitrary user-named "Temp" folders are intentionally NOT accepted.
    if (path.size() >= 3 && path[1] == L':' && (path[2] == L'\\' || path[2] == L'/')) {
        const wchar_t* kSuffix = L"\\appdata\\local\\temp";
        size_t sl = (sizeof(L"\\appdata\\local\\temp") / sizeof(wchar_t)) - 1;
        if (parentLow.size() >= sl && parentLow.compare(parentLow.size() - sl, sl, kSuffix) == 0) {
            return true;   // <...>\AppData\Local\Temp
        }
        size_t lastBS = parentLow.find_last_of(L'\\');
        if (lastBS != std::wstring::npos && lastBS < parentLow.size() - 1) {
            std::wstring last = parentLow.substr(lastBS + 1);
            std::wstring up = parentLow.substr(0, lastBS);
            bool digits = !last.empty();
            for (wchar_t c : last) if (c < L'0' || c > L'9') { digits = false; break; }
            if (digits && up.size() >= sl && up.compare(up.size() - sl, sl, kSuffix) == 0) {
                return true;   // <...>\AppData\Local\Temp\<digits> (RDS per-session)
            }
        }
    }
    return false;
}

// ---------- OS info ----------
struct RtlOsVersionInfo {
    ULONG dwOSVersionInfoSize;
    ULONG dwMajorVersion, dwMinorVersion, dwBuildNumber, dwPlatformId;
    WCHAR szCSDVersion[128];
};
typedef LONG(WINAPI* RtlGetVersionFn)(RtlOsVersionInfo*);

// ============================================================================
// DPI-aware message font
// ============================================================================
bool GetMessageFontForDpi(LOGFONTW* out, UINT dpi) {
    if (!out) return false;
    typedef BOOL (WINAPI *SysParamsInfoForDpi_t)(UINT, UINT, PVOID, UINT, UINT);
    static SysParamsInfoForDpi_t fn =
        (SysParamsInfoForDpi_t)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SystemParametersInfoForDpi");

    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof(ncm);
    if (fn && fn(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, dpi)) {
        *out = ncm.lfMessageFont;
        return true;
    }
    // Fallback: system-wide metrics (not dpi-aware, but functional)
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0)) {
        *out = ncm.lfMessageFont;
        return true;
    }
    return false;
}

OsVersion GetOsVersion() {
    OsVersion v{};
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll) {
        auto fn = (RtlGetVersionFn)GetProcAddress(ntdll, "RtlGetVersion");
        if (fn) {
            RtlOsVersionInfo info{};
            info.dwOSVersionInfoSize = sizeof(info);
            if (fn(&info) == 0 /*STATUS_SUCCESS*/) {
                v.major = info.dwMajorVersion;
                v.minor = info.dwMinorVersion;
                v.build = info.dwBuildNumber;
                return v;
            }
        }
    }
    v.major = 10; v.minor = 0; v.build = 0; // best effort
    return v;
}

std::string OsDisplayString() {
    OsVersion v = GetOsVersion();
    char buf[128];
    if (v.major == 10 && v.minor == 0) {
        if (v.build >= 22000) snprintf(buf, sizeof(buf), "Windows 11 Build %lu", v.build);
        else                  snprintf(buf, sizeof(buf), "Windows 10 Build %lu", v.build);
    } else {
        snprintf(buf, sizeof(buf), "Windows %lu.%lu Build %lu", v.major, v.minor, v.build);
    }
    return buf;
}

bool IsOsTagCompatible(const std::string& osTag) {
    OsVersion v = GetOsVersion();
    std::string t = ToLower(osTag);
    if (t == "win7")  return v.major == 6 && v.minor == 1;
    if (t == "win10") return v.major == 10 && v.minor == 0 && v.build < 22000;
    if (t == "win11") return v.major == 10 && v.minor == 0 && v.build >= 22000;
    return true; // unknown OS tags: not restrictive
}

bool IsRunningAsAdmin() {
    SID_IDENTIFIER_AUTHORITY ntAuthority = SECURITY_NT_AUTHORITY;
    PSID adminsGroup = nullptr;
    if (!AllocateAndInitializeSid(&ntAuthority, 2,
            SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS,
            0, 0, 0, 0, 0, 0, &adminsGroup)) {
        return false;
    }
    BOOL member = FALSE;
    BOOL ok = CheckTokenMembership(nullptr, adminsGroup, &member);
    FreeSid(adminsGroup);
    return ok && member;
}

// ---------- clock ----------
static std::string FormatTime(const SYSTEMTIME& st, bool withDate) {
    char buf[64];
    if (withDate)
        snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
                 st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    else
        snprintf(buf, sizeof(buf), "%02d:%02d:%02d", st.wHour, st.wMinute, st.wSecond);
    return buf;
}

std::string NowTimestamp() { SYSTEMTIME st; GetLocalTime(&st); return FormatTime(st, true); }
std::string ClockNow()     { SYSTEMTIME st; GetLocalTime(&st); return FormatTime(st, false); }

// ---------- console ----------
static bool g_consoleAttached = false;
static bool g_outUsable = false;          // stdout is writable (console or redirected)
static bool g_errUsable = false;          // stderr is writable (console or redirected)
static UINT g_prevConsoleOutputCP = 0;   // saved so RestoreConsoleOutputCP can put it back

void AttachParentConsoleAndUtf8() {
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    HANDLE hErr = GetStdHandle(STD_ERROR_HANDLE);
    // If the parent shell redirected our stdout/stderr (pipe or file), keep them:
    // this makes `winkick --help > out.txt` and `... | findstr` work correctly.
    const bool outUsable = (hOut != nullptr && hOut != INVALID_HANDLE_VALUE);
    const bool errUsable = (hErr != nullptr && hErr != INVALID_HANDLE_VALUE);

    bool attached = false;
    if (!outUsable || !errUsable) {
        attached = AttachConsole(ATTACH_PARENT_PROCESS) != FALSE;
        if (attached) {
            if (!outUsable) {
                FILE* fp = nullptr;
                if (freopen_s(&fp, "CONOUT$", "w", stdout) == 0) g_outUsable = true;
            }
            if (!errUsable) {
                FILE* fp = nullptr;
                if (freopen_s(&fp, "CONOUT$", "w", stderr) == 0) g_errUsable = true;
            }
        }
    }
    // Console is usable as long as at least one output path works
    // (a valid stdout pipe with a broken stderr pipe must not silence output)
    if (outUsable) g_outUsable = true;
    if (errUsable) g_errUsable = true;
    g_consoleAttached = g_outUsable || g_errUsable;
    if (g_prevConsoleOutputCP == 0) g_prevConsoleOutputCP = GetConsoleOutputCP();
    SetConsoleOutputCP(CP_UTF8);
}

void RestoreConsoleOutputCP() {
    if (g_prevConsoleOutputCP != 0) {
        SetConsoleOutputCP(g_prevConsoleOutputCP);
        g_prevConsoleOutputCP = 0;   // never restore twice
    }
}

bool ConsoleAttached() { return g_consoleAttached; }

void PrintUtf8(const std::string& line) {
    if (!g_outUsable) return;   // gated on stdout's own sink; stderr-only redirect must not break stdout
    fwrite(line.data(), 1, line.size(), stdout);
    fputc('\n', stdout);
    fflush(stdout);
}

void PrintUtf8Err(const std::string& line) {
    if (!g_consoleAttached) return;
    // If stderr has no writable sink but stdout does (e.g. detached parent redirected only
    // stdout), deliver the error line on stdout instead of losing it entirely.
    FILE* target = g_errUsable ? stderr : (g_outUsable ? stdout : stderr);
    fwrite(line.data(), 1, line.size(), target);
    fputc('\n', target);
    fflush(target);
}

} // namespace env
