// WinKick - entry point: CLI output modes attach to parent console; otherwise GUI.
#include "env.hpp"
#include "cli.hpp"
#include "config.hpp"
#include "net.hpp"
#include "runner.hpp"
#include "gui.hpp"

#include <windows.h>
#include <shellapi.h>

#include <string>
#include <atomic>

namespace {

// Set true as soon as we know (even before full parse) that this invocation is headless,
// so fatal errors never open a modal MessageBoxW (Task Scheduler/Intune/WinRM/Run dialog
// would otherwise hang an unattended process forever).
std::atomic<bool> g_headlessSilent{false};
// Where to append a fatal error line when --log was supplied (empty otherwise).
std::wstring g_fatalLogPath;

// Cheap raw-token scan used before the real parser; case-insensitive. Matches not only
// -s/-n/--silent/--dry-run but any console-style or attached-value form (--silent=1) and
// the other console invocations (--list, --export, --dump-config, --version, --help, --quiet),
// so no failure path of a console invocation can ever open a modal MessageBoxW.
bool CommandLineHasHeadlessFlag(const std::wstring& cmdLine) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(cmdLine.c_str(), &argc);
    if (!argv) return false;
    auto isHeadless = [](const std::wstring& token) {
        std::wstring t = token;
        for (auto& c : t) if (c >= L'A' && c <= L'Z') c = (wchar_t)(c - L'A' + L'a');
        // Same normalization as the real parser (src/cli.cpp strips up to 2 leading dashes)
        int dashes = 0;
        while (dashes < 2 && dashes < (int)t.size() && t[dashes] == L'-') ++dashes;
        if (dashes == 0) return false;
        std::wstring base = t.substr(dashes);
        size_t eq = base.find(L'=');
        if (eq != std::wstring::npos) base = base.substr(0, eq);
        return base == L"silent" || base == L"s" || base == L"dry-run" || base == L"n" ||
               base == L"list" || base == L"version" || base == L"v" ||
               base == L"help" || base == L"h" || base == L"quiet" || base == L"q" ||
               base == L"export" || base == L"e" || base == L"dump-config" || base == L"d";
    };
    bool found = false;
    for (int i = 1; i < argc; ++i) {
        if (isHeadless(argv[i])) found = true;
    }
    LocalFree(argv);
    return found;
}

// Best-effort deletion of the %TEMP%\WKJxxxx.tmp job file on early fatal paths.
// The elevated child normally deletes it right after loading; if parsing or source
// resolution fails first, this avoids leaking it into %TEMP%. Only files directly
// inside %TEMP% whose name starts with "WKJ" are eligible.
void DeleteElevatedJobFileIfRequested(const std::wstring& cmdLine) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(cmdLine.c_str(), &argc);
    if (!argv) return;
    bool wantsCleanup = false;
    std::wstring cfgPath;
    for (int i = 1; i < argc; ++i) {
        std::wstring raw = argv[i];
        // Same normalization as ParseCommandLine: 1-2 leading dashes, optional '=' tail
        std::wstring t = raw;
        for (auto& c : t) if (c >= L'A' && c <= L'Z') c = (wchar_t)(c - L'A' + L'a');
        int dashes = 0;
        while (dashes < 2 && dashes < (int)t.size() && t[dashes] == L'-') ++dashes;
        if (dashes == 0) continue;
        std::wstring body = t.substr(dashes);
        size_t eq = body.find(L'=');
        std::wstring name = (eq == std::wstring::npos) ? body : body.substr(0, eq);
        std::wstring attachVal;
        if (eq != std::wstring::npos) attachVal = raw.substr(dashes + eq + 1);
        if (name == L"cleanup-config") wantsCleanup = true;
        else if (name == L"config" || name == L"c") {
            if (eq != std::wstring::npos) cfgPath = attachVal;
            else if (i + 1 < argc) { cfgPath = argv[i + 1]; ++i; }   // consume the value token
        }
    }
    LocalFree(argv);
    if (!wantsCleanup || cfgPath.empty()) return;
    if (!env::IsTempJobFilePath(cfgPath)) return;   // strictly a WKJ*.tmp directly in a Temp dir
    DeleteFileW(cfgPath.c_str());
}

void ReportFatal(const std::string& utf8Message) {
    std::wstring wide = env::Utf8ToWide(utf8Message);
    if (env::ConsoleAttached()) {
        env::PrintUtf8Err(utf8Message);
    } else if (g_headlessSilent.load()) {
        // Detached unattended context: never open a modal dialog. Try to attach to a
        // parent console anyway so interactive terminal users still see the error;
        // if there is no console to attach to, print nowhere (that's the point).
        env::AttachParentConsoleAndUtf8();
        if (env::ConsoleAttached()) env::PrintUtf8Err(utf8Message);
    } else {
        MessageBoxW(nullptr, wide.c_str(), L"WinKick", MB_OK | MB_ICONERROR);
    }
    // Best-effort: also record the fatal error in the requested log file
    if (!g_fatalLogPath.empty()) {
        HANDLE h = CreateFileW(g_fatalLogPath.c_str(), FILE_APPEND_DATA,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                               OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            env::PrintUtf8Err("WARNING: could not open log file '" + env::WideToUtf8(g_fatalLogPath) + "' for writing.");
        } else {
            DWORD w = 0;
            std::string line = "[fatal] " + utf8Message + "\r\n";
            WriteFile(h, line.data(), (DWORD)line.size(), &w, nullptr);
            CloseHandle(h);
        }
    }
}

std::string NarrowPath(const std::wstring& w) { return env::WideToUtf8(w); }

} // namespace

int WINAPI wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE, _In_ PWSTR, _In_ int) {
    // Harden DLL resolution before ANYTHING else (network/COM/bcrypt all load lazily):
    // never search the app's own directory for WinHTTP/WinINet/BCrypt (not KnownDLLs) — otherwise
    // a planted bcrypt.dll beside winkick.exe would run before main(), including elevated via Apply.
    {
        typedef BOOL(WINAPI* SetDefaultDllDirectoriesFn)(DWORD);
        HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
        auto sdpdd = (SetDefaultDllDirectoriesFn)(k32 ? GetProcAddress(k32, "SetDefaultDllDirectories") : nullptr);
        if (sdpdd) {
            sdpdd(0x00000800);          // LOAD_LIBRARY_SEARCH_SYSTEM32 — System32 only, never the app directory
        } else {
            SetDllDirectoryW(L"");      // pre-KB2533623 fallback: drops the CWD from the search path (does not remove the app directory)
        }
    }
    SetAppInstance(hInstance);
    // Restore the parent console's output codepage on process exit when we attached to it
    struct CpRestoreGuard { ~CpRestoreGuard() { env::RestoreConsoleOutputCP(); } } cpRestoreGuard;

    // Pre-parse: raw headless-flag scan so even parse failures never open a modal dialog
    g_headlessSilent.store(CommandLineHasHeadlessFlag(GetCommandLineW()));

    Options opts;
    std::string parseError;
    if (!ParseCommandLine(GetCommandLineW(), opts, parseError)) {
        DeleteElevatedJobFileIfRequested(GetCommandLineW());   // sweep hand-off job file on fatal exit
        env::AttachParentConsoleAndUtf8();
        ReportFatal("Error: " + parseError);
        return 2;
    }
    g_headlessSilent.store(IsConsoleInvocation(opts) || opts.quiet);
    if (opts.logEnabled) g_fatalLogPath = opts.logPath;

    const bool consoleNeeded = IsConsoleInvocation(opts);
    if (consoleNeeded) env::AttachParentConsoleAndUtf8();

    if (opts.showHelp)    { PrintHelpText();    return 0; }
    if (opts.showVersion) { PrintVersionText(); return 0; }

    // --dump-config: write embedded default and exit (no source resolution needed)
    if (opts.dumpConfig) {
        std::string data;
        if (!LoadEmbeddedDefaultTasksIni(data)) {
            ReportFatal("Error: embedded default configuration resource is missing.");
            return 2;
        }
        if (!env::WriteFileUtf8(opts.dumpConfigPath, data)) {
            ReportFatal("Error: cannot write '" + NarrowPath(opts.dumpConfigPath) + "'.");
            return 2;
        }
        env::PrintUtf8("Wrote embedded default configuration to " + NarrowPath(opts.dumpConfigPath));
        return 0;
    }

    // Source resolution (respects --config/--offline/--url/default)
    net::SourceOptions sourceOpts;
    sourceOpts.hasConfigPath = opts.hasConfigPath;
    sourceOpts.configPath = opts.configPath;
    sourceOpts.offline = opts.offline;
    sourceOpts.hasUrl = opts.hasUrl;
    sourceOpts.url = opts.url;
    sourceOpts.cleanupConfig = opts.cleanupConfig;   // UAC hand-off: unelevated side already established connectivity

    bool isOnline = false;
    net::SourceResolution src = net::ResolveSource(sourceOpts, isOnline);
    if (!src.ok) {
        DeleteElevatedJobFileIfRequested(GetCommandLineW());   // sweep hand-off job file on fatal exit
        ReportFatal("Error: " + src.error);
        return 2;
    }

    // UAC hand-off: the elevated instance deletes the temporary job file right after loading it.
    // Defense in depth: only ever delete WKJ*.tmp files directly in a Temp dir, never a user's file.
    if (opts.cleanupConfig && opts.hasConfigPath && env::IsTempJobFilePath(opts.configPath)) {
        DeleteFileW(opts.configPath.c_str());
    }

    Config cfg = ParseIniBlocks(src.configText, env::WideToUtf8(src.sourceLabel));

    // --list
    if (opts.listRequested) {
        PrintItemTable(cfg.items, isOnline);
        return 0;
    }

    // --export: resolve selection/filters, serialize, exit without executing
    if (opts.exportRequested) {
        Config exportCfg = cfg; // copy: resolve selection on a fresh snapshot
        ApplySelectionFilters(exportCfg.items, opts, isOnline);
        std::string ini = SerializeIniBlocks(exportCfg.items);
        if (!env::WriteFileUtf8(opts.exportPath, ini)) {
            ReportFatal("Error: cannot write '" + NarrowPath(opts.exportPath) + "'.");
            return 2;
        }
        env::PrintUtf8("Exported " + std::to_string(exportCfg.items.size()) +
                       " items to " + NarrowPath(opts.exportPath));
        return 0;
    }

    // --silent / --dry-run: headless execution
    if (opts.silent || opts.dryRun) {
        if (opts.silent && !opts.dryRun && !env::IsRunningAsAdmin()) {
            const char* msg = "Error: Administrator privileges required for --silent mode.";
            env::PrintUtf8Err(msg);
            if (opts.logEnabled) { // also record it in the log file when -l was given
                HANDLE h = CreateFileW(opts.logPath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                       nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (h == INVALID_HANDLE_VALUE) {
                    env::PrintUtf8Err("WARNING: could not open log file '" + env::WideToUtf8(opts.logPath) + "' for writing.");
                } else {
                    DWORD w = 0;
                    std::string line = std::string(msg) + "\r\n";
                    WriteFile(h, line.data(), (DWORD)line.size(), &w, nullptr);
                    CloseHandle(h);
                }
            }
            return 1;
        }
        ApplySelectionFilters(cfg.items, opts, isOnline);

        runner::Options ropt;
        ropt.dryRun = opts.dryRun;
        // Headless semantics per spec §5.1/§5.2: 'gui' items are skipped ONLY under --silent.
        // --dry-run without --silent simulates them (CMD/DRY-RUN lines, exact substituted commands).
        ropt.silent = opts.silent;
        ropt.guiMode = false;
        ropt.noReboot = opts.noReboot;
        ropt.noExplorerRestart = opts.noExplorerRestart;
        ropt.logPath = opts.logEnabled ? env::WideToUtf8(opts.logPath) : "";
        ropt.modeLabel = opts.dryRun ? "Dry-Run" : "Silent";
        ropt.admin = env::IsRunningAsAdmin();
        ropt.isOnline = isOnline;
        ropt.configSourceLabel = cfg.sourceLabel;

        runner::LogSink sink = [&opts](const std::string& line) {
            if (!opts.quiet) env::PrintUtf8(line);
        };
        runner::Stats stats = runner::RunAll(cfg.items, ropt, sink, nullptr);

        if (!opts.dryRun && stats.anyReboot && !opts.noReboot) {
            // Append the reboot status to the log file as well (spec: "print/log").
            // Only if RunAll actually opened the log — never create an orphaned one-line file.
            auto appendLog = [&opts, &stats](const std::string& line) {
                if (!opts.logEnabled || !stats.logFileOpened) return;
                HANDLE h = CreateFileW(opts.logPath.c_str(), FILE_APPEND_DATA,
                                       FILE_SHARE_READ | FILE_SHARE_WRITE,
                                       nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (h == INVALID_HANDLE_VALUE) return;
                DWORD w = 0;
                std::string l = line + "\r\n";
                WriteFile(h, l.data(), (DWORD)l.size(), &w, nullptr);
                CloseHandle(h);
            };
            if (opts.reboot) {
                std::string msg;
                bool ok = runner::AttemptReboot(msg);
                if (ok) {
                    if (!opts.quiet) env::PrintUtf8(msg);
                } else {
                    env::PrintUtf8Err("Error: " + msg);   // failure always on stderr, consistent with other error paths
                }
                appendLog(msg);
                if (!ok) {
                    return 1; // reboot requested but failed -> non-zero exit
                }
            } else {
                if (!opts.quiet) env::PrintUtf8("Reboot required for some applied changes.");
                appendLog("Reboot required for some applied changes.");
            }
        }
        return stats.failed > 0 ? 1 : 0;
    }

    // GUI path
    return gui::RunGui(std::move(cfg), isOnline, opts);
}
