// WinKick - environment & encoding helpers
#pragma once

#include <string>
#include <windows.h>

namespace env {

// ---- UTF-8 <-> UTF-16 conversion (CP_UTF8 only) ----
std::wstring Utf8ToWide(const std::string& s);
std::string  WideToUtf8(const std::wstring& s);

// ---- string helpers ----
std::string Trim(const std::string& s);        // trims space, tab, CR, LF
std::string ToLower(const std::string& s);     // ASCII lowercase
bool        StartsWithNoCase(const std::string& s, const char* prefix);

// ---- file helpers (UTF-8, no BOM) ----
bool FileExistsW(const std::wstring& path);
bool ReadFileUtf8(const std::wstring& path, std::string& out);
bool WriteFileUtf8(const std::wstring& path, const std::string& data);
bool EnsureParentDirectoryExists(const std::wstring& path); // true if dir portion exists or no dir portion

// ---- OS info ----
// DPI-aware system helpers. Resolved dynamically (GetDpiForWindow/GetDpiForSystem
// were introduced in Windows 10 1607, and we also want a non-zero fallback).
UINT DpiForWindowSafe(HWND h);   // GetDpiForWindow, falling back to GetDeviceCaps(LOGPIXELSX)/96
UINT DpiForSystemSafe();         // GetDpiForSystem, falling back to 96
// GetSystemMetricsForDpi (Win10 1607+), falling back to GetSystemMetrics (system-DPI metrics)
int  GetSystemMetricsForDpiSafe(int nIndex, UINT dpi);

// Full path to an executable in %SystemRoot%\System32 (or fallback name on failure).
std::wstring System32Path(const std::wstring& exeName);

// True only when `path` looks like the one-time job file the app deliberately writes on the elevation
// hand-off: `WKJ<1..32 hex>.tmp` directly inside the resolved %TEMP% (or the launching user's
// AppData\Local\Temp in Over-the-Shoulder UAC) — 128-bit BCryptGenRandom's output, no traversal.
// Deleting only files passing this check is the sole sanctioned use of --cleanup-config / job-file sweep.
bool IsTempJobFilePath(const std::wstring& path);

struct OsVersion { DWORD major = 10, minor = 0, build = 0; };
OsVersion   GetOsVersion();          // via RtlGetVersion (unmaterialized by shims)
std::string OsDisplayString();       // e.g. "Windows 11 Build 22631"
bool        IsOsTagCompatible(const std::string& osTag); // win7/win10/win11
bool        IsRunningAsAdmin();      // CheckTokenMembership(Administrators)

// Message font for a specific DPI. Prefers SystemParametersInfoForDpi (Windows 10 1607+)
// resolved dynamically; falls back to SPI_GETNONCLIENTMETRICS.
bool GetMessageFontForDpi(LOGFONTW* out, UINT dpi);

// ---- clock ----
std::string NowTimestamp();          // "2026-11-14 14:22:10"
std::string ClockNow();              // "14:22:11"

// ---- console ----
void AttachParentConsoleAndUtf8();   // AttachConsole(ATTACH_PARENT_PROCESS) + reopen stdout/stderr + UTF-8 CP
// Restores the console output codepage that AttachParentConsoleAndUtf8 previously saved.
// No-op when no console was attached / already restored. Call once at process exit.
void RestoreConsoleOutputCP();
void PrintUtf8(const std::string& line);  // writes to stdout if console attached
void PrintUtf8Err(const std::string& line);
bool ConsoleAttached();

} // namespace env
