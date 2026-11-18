// WinKick - execution engine: child processes, guards, logging, lifecycle
#pragma once

#include "config.hpp"
#include "env.hpp"

#include <string>
#include <vector>
#include <functional>

namespace runner {

// Reduce "\r-spinner" output to clean lines (keep only the final frame per overwrite).
std::string NormalizeCarriageReturns(const std::string& s);

// Replace every {Name} placeholder in cmd with the arg's currentValue.
std::string SubstituteArgs(const std::string& cmd, const std::vector<TaskArg>& args);

struct Options {
    bool dryRun = false;
    bool silent = false;          // headless execution; 'gui'-tagged items are skipped
    bool guiMode = false;         // true when running inside the GUI (visible gui items)
    bool noReboot = false;
    bool noExplorerRestart = false;
    std::string logPath;          // empty => no file log
    std::string modeLabel;        // "GUI" | "Silent" | "Dry-Run"
    bool admin = false;
    bool isOnline = false;
    std::string configSourceLabel;
};

struct FailedEntry {
    int index = 0;
    std::string category, title;
    unsigned long exitCode = 0;
};

struct Stats {
    int succeeded = 0, failed = 0, skipped = 0, dryRun = 0;
    unsigned long long totalMs = 0;
    std::vector<FailedEntry> failedItems;
    bool anyRestartExplorer = false;
    bool anyReboot = false;
    bool logFileOpened = false;   // true only when RunAll's FileWriter actually opened opts.logPath
};

// Sink for every log line (no trailing newline). Console/GUI/file all go through here.
using LogSink = std::function<void(const std::string& line)>;
// Optional per-item state callback (index, state, exitCode, elapsedMs).
using ItemStateSink = std::function<void(int index, ItemRunState state, unsigned long elapsedMs, unsigned long exitCode)>;

// Runs items in source order, 0..N-1. Applies guards, substitution, logging.
// Handles explorer restart at the end (reboot is left to the caller: GUI prompts, silent honors -r).
Stats RunAll(std::vector<TaskItem>& items, const Options& opts,
             const LogSink& sink, const ItemStateSink& stateSink);

// Post-run reboot helpers.
// Silent mode policy is decided by the caller; these perform the actual action.
bool AttemptReboot(std::string& resultMessage);   // enables SeShutdownPrivilege + ExitWindowsEx, falls back to shutdown.exe
void RestartExplorer(const LogSink& sink);        // terminates same-session Explorer; OS auto-restarts shell (no elevated explorer spawn)

} // namespace runner
