#include "runner.hpp"
#include "version.h"

#include <windows.h>
#include <tlhelp32.h>
#include <sstream>
#include <chrono>
#include <cstdio>
#include <algorithm>
#include <io.h>
#include <share.h>

namespace runner {

namespace {

constexpr size_t kMaxCapturedOutput = 1024 * 1024; // 1 MB cap, head-kept

// Child stdout may be UTF-8, or the system OEM/ANSI codepage on non-English locales.
// Validate as UTF-8 first; on failure, decode as CP_OEMCP and re-encode to UTF-8.
// Embedded NUL bytes (e.g. UTF-16-without-BOM / wmic / installer output) are stripped
// because every consumer downstream (log writer, GUI edit control) is C-string based.
std::string EnsureUtf8(const std::string& bytes) {
    std::string clean = bytes;
    clean.erase(std::remove(clean.begin(), clean.end(), '\0'), clean.end());
    if (clean.empty()) return clean;
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, clean.data(), (int)clean.size(), nullptr, 0);
    if (n > 0) return clean; // already valid UTF-8
    int m = MultiByteToWideChar(CP_OEMCP, 0, clean.data(), (int)clean.size(), nullptr, 0);
    if (m <= 0) return clean;
    std::wstring w((size_t)m, L'\0');
    MultiByteToWideChar(CP_OEMCP, 0, clean.data(), (int)clean.size(), w.data(), m);
    return env::WideToUtf8(w);
}

struct FileWriter {
    FILE* file = nullptr;
    bool open(const std::string& utf8Path) {
        if (file) return true;
        std::wstring w = env::Utf8ToWide(utf8Path);
        // Share read (but not write) so users/tailing tools can open the log during a run
        file = _wfsopen(w.c_str(), L"wb", _SH_DENYWR);
        if (!file) return false;
        return true;
    }
    void write(const std::string& line) {
        if (!file) return;
        fwrite(line.data(), 1, line.size(), file);
        fwrite("\r\n", 1, 2, file);
        fflush(file);
    }
    ~FileWriter() { if (file) fclose(file); }
};

// Spawn cmd.exe /c "<cmd>" with merged stdout+stderr into one pipe (avoids pipe-fill deadlock).
// If visible == true, launch without CREATE_NO_WINDOW and without redirection (GUI items).
bool RunChild(const std::string& cmd, std::string& captured, DWORD& exitCode, unsigned long& elapsedMs,
              bool visible, const LogSink& outputSink) {
    auto start = std::chrono::steady_clock::now();
    captured.clear();
    exitCode = 127;

    std::wstring wideCmd = env::Utf8ToWide(cmd);
    const std::wstring cmdExe = env::System32Path(L"cmd.exe");
    // lpApplicationName must be a full path so a planted cmd.exe next to
    // winkick.exe cannot execute elevated instead of the real one.
    // /d suppresses unrelated HKCU/HKLM Command Processor AutoRun hooks.
    // /s makes the outer quoting deterministic, including quoted executable paths.
    std::wstring cmdline = L"\"" + cmdExe + L"\" /d /s /c \"" + wideCmd + L"\"";

    if (visible) {
        STARTUPINFOW si{}; si.cb = sizeof(si);
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_SHOWNORMAL;
        PROCESS_INFORMATION pi{};
        if (!CreateProcessW(cmdExe.c_str(), cmdline.data(), nullptr, nullptr, FALSE,
                            0, nullptr, nullptr, &si, &pi)) {
            exitCode = 127;
            captured = "Failed to create process (error " + std::to_string(GetLastError()) + ").";
            auto end = std::chrono::steady_clock::now();
            elapsedMs = (unsigned long)std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
            return false;
        }
        WaitForSingleObject(pi.hProcess, INFINITE);
        GetExitCodeProcess(pi.hProcess, &exitCode);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        captured = "(not captured - launched visibly)";
        auto end = std::chrono::steady_clock::now();
        elapsedMs = (unsigned long)std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
        return exitCode == 0;
    }

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE hRead = nullptr, hWrite = nullptr;
    if (!CreatePipe(&hRead, &hWrite, &sa, 0)) {
        captured = "CreatePipe failed (error " + std::to_string(GetLastError()) + ").";
        auto end = std::chrono::steady_clock::now();
        elapsedMs = (unsigned long)std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
        return false;
    }
    SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0); // parent side must not be inherited

    // Child gets NUL as stdin: any accidental prompt reads EOF instead of hanging
    SECURITY_ATTRIBUTES saIn{};
    saIn.nLength = sizeof(saIn);
    saIn.bInheritHandle = TRUE;
    HANDLE hNul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              &saIn, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

    STARTUPINFOW si{}; si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdInput  = (hNul != INVALID_HANDLE_VALUE) ? hNul : nullptr;
    si.hStdOutput = hWrite;
    si.hStdError  = hWrite; // merged: prevents cross-pipe deadlock

    PROCESS_INFORMATION pi{};
    BOOL created = CreateProcessW(cmdExe.c_str(), cmdline.data(), nullptr, nullptr, TRUE,
                                  CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    DWORD createError = created ? ERROR_SUCCESS : GetLastError();
    // Parent must close the write handle immediately so ReadFile can observe EOF.
    CloseHandle(hWrite);
    if (hNul != INVALID_HANDLE_VALUE) CloseHandle(hNul);

    if (!created) {
        CloseHandle(hRead);
        captured = "Failed to create process (error " + std::to_string(createError) + ").";
        auto end = std::chrono::steady_clock::now();
        elapsedMs = (unsigned long)std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
        return false;
    }

    const size_t cap = kMaxCapturedOutput;
    bool truncated = false;
    size_t streamOffset = 0;
    std::string streamLine;
    bool crPending = false;
    // Emit complete logical lines while the child is still running. Keep an incomplete
    // UTF-8 line/frame between reads; a CR overwrite is resolved before emitting it.
    auto streamAvailable = [&]() {
        while (streamOffset < captured.size()) {
            char c = captured[streamOffset++];
            if (c == '\r') { crPending = true; continue; }
            if (c == '\n') {
                outputSink(EnsureUtf8(streamLine));
                streamLine.clear();
                crPending = false;
                continue;
            }
            if (crPending) { streamLine.clear(); crPending = false; }
            streamLine += c;
        }
    };
    for (;;) {
        char buf[4096];
        DWORD rd = 0;
        if (!ReadFile(hRead, buf, sizeof(buf), &rd, nullptr) || rd == 0) break;
        if (captured.size() < cap) {
            size_t room = cap - captured.size();
            captured.append(buf, std::min<size_t>(rd, room));
            if (rd > room) truncated = true;
        } else {
            truncated = true;
        }
        streamAvailable();
    }
    // If we truncated mid-UTF-8-sequence, drop the incomplete tail before downstream validation
    size_t beforeTrim = captured.size();
    if (truncated) {
        // Count trailing continuation bytes backwards until a non-continuation byte
        size_t sz = captured.size();
        size_t seqStart = sz, contCount = 0;
        while (seqStart > 0 && (captured[seqStart - 1] & 0xC0) == 0x80) { --seqStart; ++contCount; }
        if (seqStart > 0) {
            unsigned char lead = (unsigned char)captured[seqStart - 1];
            size_t seqLen = 1; // plain ASCII
            if ((lead & 0xE0) == 0xC0) seqLen = 2; else if ((lead & 0xF0) == 0xE0) seqLen = 3; else if ((lead & 0xF8) == 0xF0) seqLen = 4;
            if (1 + contCount < seqLen) captured.resize(seqStart - 1); // incomplete sequence at tail
        } else {
            captured.clear(); // pathological: all continuation bytes
        }
    }
    // The only removed bytes are an incomplete UTF-8 suffix in the pending line.
    size_t dropped = beforeTrim - captured.size();
    if (dropped <= streamLine.size()) streamLine.resize(streamLine.size() - dropped);
    streamOffset = captured.size();
    if (truncated) {
        captured += "\n[... output truncated at 1 MB ...]";
        streamAvailable();
    }
    if (!streamLine.empty()) outputSink(EnsureUtf8(streamLine));
    CloseHandle(hRead);

    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    auto end = std::chrono::steady_clock::now();
    elapsedMs = (unsigned long)std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    return exitCode == 0;
}

} // namespace

// ============================================================================
std::string NormalizeCarriageReturns(const std::string& s) {
    std::string out;
    std::string currentLine;
    bool crPending = false;   // last char was a lone CR; next printable char starts a fresh frame
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c == '\r') {
            // CRLF pair is treated as a single newline; lone CR starts an overwrite
            if (i + 1 < s.size() && s[i + 1] == '\n') {
                out += currentLine; out += '\n'; currentLine.clear(); ++i;
                crPending = false;
                continue;
            }
            crPending = true;
            continue;
        }
        if (c == '\n') {
            out += currentLine; out += '\n'; currentLine.clear(); crPending = false;
            continue;
        }
        if (crPending) { currentLine.clear(); crPending = false; } // frame switch
        currentLine += c;
    }
    out += currentLine; // keep the final line even if it ended with CR
    return out;
}

std::string SubstituteArgs(const std::string& cmd, const std::vector<TaskArg>& args) {
    // Single left-to-right pass: substitute each {Name} token directly.
    // Never re-scans already-substituted text, so {A} -> "{B}" cannot cascade.
    std::string out;
    out.reserve(cmd.size() + 64);
    for (size_t i = 0; i < cmd.size();) {
        char c = cmd[i];
        if (c == '{') {
            size_t close = cmd.find('}', i);
            if (close != std::string::npos) {
                std::string name = cmd.substr(i + 1, close - i - 1);
                bool matched = false;
                for (const auto& arg : args) {
                    if (arg.name == name) { out += arg.currentValue; matched = true; break; }
                }
                if (matched) { i = close + 1; continue; }
                if (name.find('{') != std::string::npos) {
                    // Not a placeholder; emit only this '{' so a literal '{' before the real {Arg} survives
                    out += cmd[i];
                    ++i;
                    continue;
                }
                out.append(cmd, i, close - i + 1); // leave unknown placeholder verbatim
                i = close + 1;
                continue;
            }
        }
        out += c;
        ++i;
    }
    return out;
}

// ============================================================================
// RestartExplorer intentionally never launches explorer.exe from an elevated token (a High-integrity
// desktop shell is a bad outcome); it relies on Windows' AutoRestartShell to respawn the shell.
// If the admin/policy has AutoRestartShell disabled, do NOT terminate the shell at all - kill it
// and the user loses their desktop with no recourse. Skip the restart instead and say so.
//
// Termination is done in-process with exit code 0 (never via taskkill /f: taskkill's exit code 1
// is treated by winlogon as a deliberate kill - Task Manager / taskkill - and AutoRestartShell is
// suppressed on all supported Windows 10/11 builds). Exit code 0 lets winlogon respawn the shell.
// Return the PID of this session's shell (the process owning Shell_TrayWnd), or 0 if there is none.
static DWORD ShellProcessId() {
    HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (!tray) return 0;
    DWORD pid = 0;
    GetWindowThreadProcessId(tray, &pid);
    return pid;
}

// Terminate a process with exit code 0, but only within our own session.
// Returns false (without touching the process) when the session check or OpenProcess fails.
static bool TerminateProcessById(DWORD pid) {
    DWORD thisSession = 0, shellSession = 0;
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &thisSession)) return false;
    if (!ProcessIdToSessionId(pid, &shellSession) || shellSession != thisSession) return false;
    HANDLE h = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid);
    if (!h) return false;
    BOOL ok = TerminateProcess(h, 0);   // exit code 0, unlike taskkill /f (exit code 1)
    if (ok) WaitForSingleObject(h, 10000);
    CloseHandle(h);
    return ok != FALSE;
}

// Terminate every same-session explorer.exe EXCEPT the shell itself, with exit code 0.
// The shell is returned for the caller to terminate LAST: winlogon is free to respawn the desktop
// the moment the shell (tray) exits, and the respawn can appear within milliseconds - so nothing
// may terminate anything after the tray kill, or the fresh respawn would be killed too (observed:
// 2 processes terminated and the shell never came back). This preserves taskkill /im's
// "close folder windows too" semantics without the exit code 1 and without the race.
static int TerminateNonShellSessionExplorers(DWORD trayPid) {
    int killed = 0;
    DWORD thisSession = 0;
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &thisSession)) return 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe{};
        pe.dwSize = sizeof(pe);
        if (Process32FirstW(snap, &pe)) {
            do {
                if (pe.th32ProcessID == trayPid) continue;   // caller terminates the shell LAST
                if (CompareStringOrdinal(pe.szExeFile, -1, L"explorer.exe", -1, TRUE) != CSTR_EQUAL) continue;
                DWORD s = 0;
                if (ProcessIdToSessionId(pe.th32ProcessID, &s) && s == thisSession) {
                    if (TerminateProcessById(pe.th32ProcessID)) ++killed;
                }
            } while (Process32NextW(snap, &pe));
        }
        CloseHandle(snap);
    }
    return killed;
}

static bool IsAutoRestartShellEnabled() {
    DWORD value = 1, size = sizeof(value), type = 0;
    LSTATUS lr = RegGetValueW(HKEY_LOCAL_MACHINE,
                              L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon",
                              L"AutoRestartShell", RRF_RT_REG_DWORD, &type, &value, &size);
    if (lr != ERROR_SUCCESS) return true;   // key absent -> documented default is 1
    return value != 0;
}

void RestartExplorer(const LogSink& sink) {
    sink("Restarting explorer.exe ...");
    if (!IsAutoRestartShellEnabled()) {
        sink("WARNING: AutoRestartShell is disabled — restart-explorer NOT executed (killing the shell would strand the desktop). Start explorer.exe manually (Win+R -> explorer.exe) or re-enable AutoRestartShell.");
        return;
    }
    // Remember the existing tray window's PID so we can tell a NEW shell from the one being torn down
    DWORD trayPidBefore = ShellProcessId();
    if (trayPidBefore == 0) {
        // No shell window at all: nothing to restart.
        sink("WARNING: Shell_TrayWnd not found - explorer.exe left alone (no shell to restart).");
        return;
    }
    // Terminate with exit code 0 so winlogon's AutoRestartShell can respawn the desktop shell at
    // Medium integrity. Folder windows etc. go first (legacy taskkill /im parity); the shell goes
    // LAST so nothing can kill the respawn winlogon launches in response to the shell's death.
    // Never fall back to taskkill on failure: its exit code 1 would suppress the respawn and
    // strand the desktop.
    TerminateNonShellSessionExplorers(trayPidBefore);
    if (!TerminateProcessById(trayPidBefore)) {
        sink("WARNING: could not terminate the shell (OpenProcess/TerminateProcess failed) - desktop shell left running.");
        return;
    }
    // Windows auto-restarts the shell (AutoRestartShell); poll briefly rather than sleep a fixed
    // interval so slow machines don't get a false-positive warning and fast machines don't stall.
    // Let teardown settle first: an immediately-found Shell_TrayWnd is the OLD window draining out.
    Sleep(250);
    bool shellBack = false;
    for (int waited = 250; waited < 5000; waited += 250) {
        HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
        if (tray) {
            DWORD pid = 0;
            GetWindowThreadProcessId(tray, &pid);
            // A tray belonging to a different PID than before the kill is a freshly auto-restarted shell
            if (trayPidBefore == 0 || pid != trayPidBefore) { shellBack = true; break; }
        }
        Sleep(250);
    }
    if (shellBack) {
        sink("Shell auto-restarted; explorer.exe left alone.");
        return;
    }
    // Never spawn explorer.exe elevated: the shell would run with a High-integrity token.
    // Tell the user to start it manually (Win+R -> explorer.exe) instead.
    sink("WARNING: explorer.exe did not auto-restart. Start it manually (Win+R, explorer.exe) to restore the shell.");
}

bool AttemptReboot(std::string& resultMessage) {
    HANDLE token = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
        TOKEN_PRIVILEGES tp{};
        LUID luid{};
        if (LookupPrivilegeValueW(nullptr, SE_SHUTDOWN_NAME, &luid)) {
            tp.PrivilegeCount = 1;
            tp.Privileges[0].Luid = luid;
            tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
            AdjustTokenPrivileges(token, FALSE, &tp, sizeof(tp), nullptr, nullptr);
        }
        CloseHandle(token);
    }
    if (ExitWindowsEx(EWX_REBOOT, 0)) {
        resultMessage = "Reboot initiated via ExitWindowsEx.";
        return true;
    }
    DWORD err = GetLastError();
    // Fallback: shutdown.exe /r /t 0 — but wait for it and check its exit code so we
    // don't claim success when the reboot is refused (SeShutdownPrivilege / pending shutdown).
    std::wstring sdExe = env::System32Path(L"shutdown.exe");
    std::wstring sdArgs = L"\"" + sdExe + L"\" /r /t 0";
    STARTUPINFOW si{}; si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(sdExe.c_str(), sdArgs.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 15000);
        DWORD code = ~0u;
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        if (code == 0) {
            resultMessage = "Reboot initiated via shutdown.exe.";
            return true;
        }
        resultMessage = "Reboot request failed (shutdown.exe exit code " + std::to_string(code) +
                        "; last Win32 error before the fallback was " + std::to_string(err) + ").";
        return false;
    }
    resultMessage = "Reboot failed (error " + std::to_string(err) + ").";
    return false;
}

// ============================================================================
Stats RunAll(std::vector<TaskItem>& items, const Options& opts,
             const LogSink& sink, const ItemStateSink& stateSink) {
    Stats stats;
    auto runStart = std::chrono::steady_clock::now();

    FileWriter logFile;
    bool fileLogEnabled = !opts.logPath.empty();
    if (fileLogEnabled && !logFile.open(opts.logPath)) {
        // GUI: warn in the log drawer. CLI: warn on stderr once (stdout sink would double-print in non-quiet runs).
        if (opts.guiMode) sink("WARNING: could not open log file '" + opts.logPath + "' for writing.");
        else env::PrintUtf8Err("WARNING: could not open log file '" + opts.logPath + "' for writing.");
        fileLogEnabled = false;
    }
    stats.logFileOpened = fileLogEnabled;

    auto Log = [&](const std::string& line) {
        sink(line);
        if (fileLogEnabled) logFile.write(line);
    };

    const int n = (int)items.size();
    auto elapsedTotal = [&]() {
        return (unsigned long long)std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - runStart).count();
    };

    // ---- Run header ----
    Log("WinKick " WINKICK_VERSION_STRING " - Run Log");
    Log("Time: " + env::NowTimestamp());
    Log("Mode: " + opts.modeLabel);
    Log(std::string("Admin: ") + (opts.admin ? "Yes" : "No"));
    Log(std::string("Network: ") + (opts.isOnline ? "Online" : "Offline"));
    Log("OS: " + env::OsDisplayString());
    Log("Config Source: " + opts.configSourceLabel);
    Log("");

    // ---- Per item ----
    for (int i = 0; i < n; ++i) {
        TaskItem& item = items[i];
        std::string headLine = std::string("[") + env::ClockNow() + "] [" +
                               std::to_string(i + 1) + "/" + std::to_string(n) + "] [" +
                               item.category + "] " + item.title;

        auto skip = [&](const std::string& reason) {
            item.state = ItemRunState::Skipped;
            item.skipReason = reason;
            stats.skipped++;
            Log(headLine);
            Log("  RES: SKIPPED (" + reason + ")");
            if (stateSink) stateSink(i, ItemRunState::Skipped, 0, 0);
        };

        // Check guards first so blocked items log their reason — this also keeps the
        // reason accurate across the UAC elevation hand-off (serialized items arrive check:0).
        if (NetGuardBlocked(item, opts.isOnline)) { skip("Offline"); continue; }
        if (OsGuardMismatch(item)) { skip("OS mismatch"); continue; }
        if (!item.checked) { skip("Not selected"); continue; }
        if (opts.silent && ItemHasTag(item, "gui")) { skip("Interactive GUI required"); continue; }

        std::string substituted = SubstituteArgs(item.cmd, item.args);
        if (env::Trim(substituted).empty()) {
            // Selected but empty cmd: a config error in real execution (exit=127). In dry-run the
            // job is to simulate — record it as simulated, don't fail the whole run.
            Log(headLine);
            if (opts.dryRun) {
                Log("  RES: DRY-RUN (empty cmd)");
                Log("");
                stats.dryRun++;
                item.state = ItemRunState::DryRun;
                if (stateSink) stateSink(i, ItemRunState::DryRun, 0, 0);
                continue;
            }
            item.state = ItemRunState::Failed;
            item.lastExitCode = 127;
            stats.failed++;
            stats.failedItems.push_back({i, item.category, item.title, 127});
            Log("  RES: ERR (exit=127, empty cmd)");
            Log("");
            if (stateSink) stateSink(i, ItemRunState::Failed, 0, 127);
            continue;
        }
        std::string fullCmd = std::string("cmd.exe /d /s /c \"") + substituted + "\"";

        Log(headLine);
        Log("  CMD: " + fullCmd);

        if (opts.dryRun) {
            item.state = ItemRunState::DryRun;
            stats.dryRun++;
            Log("  RES: DRY-RUN (not executed)");
            Log("");
            if (stateSink) stateSink(i, ItemRunState::DryRun, 0, 0);
            continue;
        }

        item.state = ItemRunState::Running;
        if (stateSink) stateSink(i, ItemRunState::Running, 0, 0);

        std::string captured;
        DWORD exitCode = 0;
        unsigned long elapsedMs = 0;
        bool visible = opts.guiMode && ItemHasTag(item, "gui");
        bool outputStreamed = false;
        LogSink outputSink = [&](const std::string& line) {
            Log((outputStreamed ? "       " : "  OUT: ") + line);
            outputStreamed = true;
        };
        RunChild(substituted, captured, exitCode, elapsedMs, visible, outputSink);

        // Visible launches and process-creation errors do not use the pipe sink.
        if (!outputStreamed) {
            std::string normalized = NormalizeCarriageReturns(EnsureUtf8(captured));
            if (normalized.empty()) {
                Log("  OUT: (no output)");
            } else {
                std::istringstream iss(normalized);
                std::string line;
                while (std::getline(iss, line)) outputSink(line);
            }
        }

        char resBuf[128];
        snprintf(resBuf, sizeof(resBuf), "  RES: %s (exit=%lu, %lu ms)",
                 exitCode == 0 ? "OK" : "ERR", (unsigned long)exitCode, elapsedMs);
        Log(resBuf);
        Log("");

        item.lastExitCode = (unsigned long)exitCode;
        item.lastElapsedMs = elapsedMs;
        item.state = (exitCode == 0) ? ItemRunState::Ok : ItemRunState::Failed;
        if (exitCode == 0) {
            stats.succeeded++;
            if (ItemHasTag(item, "restart-explorer")) stats.anyRestartExplorer = true;
            if (ItemHasTag(item, "reboot")) stats.anyReboot = true;
        } else {
            stats.failed++;
            stats.failedItems.push_back({i, item.category, item.title, (unsigned long)exitCode});
        }
        if (stateSink) stateSink(i, item.state, elapsedMs, (unsigned long)exitCode);
    }

    // ---- Post-run: explorer restart (reboot decision is left to caller) ----
    if (!opts.dryRun && stats.anyRestartExplorer && !opts.noExplorerRestart) {
        RestartExplorer(Log);
        Log("");
    }
    stats.totalMs = elapsedTotal();   // wall clock, consistent with the log footer

    // ---- Footer summary ----
    Log("---- Summary --------------------------------------------------");
    Log("Succeeded: " + std::to_string(stats.succeeded) +
        " | Failed: " + std::to_string(stats.failed) +
        " | Skipped: " + std::to_string(stats.skipped) +
        " | Dry-run: " + std::to_string(stats.dryRun) +
        " | Total: " + std::to_string(stats.totalMs) + " ms");
    Log("FAILED ITEMS:");
    if (stats.failedItems.empty()) {
        Log("  (none)");
    } else {
        for (const auto& f : stats.failedItems) {
            Log("  [" + std::to_string(f.index + 1) + "] [" + f.category + "] " +
                f.title + " - exit=" + std::to_string(f.exitCode));
        }
    }

    return stats;
}

} // namespace runner
