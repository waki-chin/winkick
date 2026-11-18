// WinKick - CLI: flag parser, selection precedence pipeline, --list, --help
#pragma once

#include <string>
#include <vector>

#include "config.hpp"

struct Options {
    // Source
    std::wstring configPath;
    bool hasConfigPath = false;
    std::wstring url;
    bool hasUrl = false;
    bool offline = false;

    bool dumpConfig = false;
    std::wstring dumpConfigPath = L"tasks.ini";

    bool exportRequested = false;
    std::wstring exportPath = L"tasks-export.ini";

    // Selection & filters
    std::vector<std::string> tags;        // -t (lowercased)
    std::vector<std::string> excludeTags; // -x (lowercased)
    std::vector<std::string> picks;       // -p (lowercased substrings)
    std::vector<std::string> skips;       // -k (lowercased substrings)
    bool selectAll = false;               // -a

    // Execution
    bool silent = false;
    bool dryRun = false;
    bool quiet = false;
    bool reboot = false;                  // -r: auto-reboot in silent mode
    bool logEnabled = false;
    std::wstring logPath = L"log.txt";
    bool noReboot = false;
    bool noExplorerRestart = false;

    // Output modes
    bool listRequested = false;
    bool showHelp = false;
    bool showVersion = false;

    // Internal (UAC hand-off)
    bool autoRun = false;
    bool cleanupConfig = false;
};

// Parse command line. Returns false and fills outError on any problem.
bool ParseCommandLine(const std::wstring& cmdLine, Options& out, std::string& outError);

// True when the invocation only needs non-GUI console output.
bool IsConsoleInvocation(const Options& opts);

// Selection & filter pipeline (spec 5.2): base -> exclusions -> guards -> groups.
// Mutates items[].checked in place. 'gui' items are NOT removed here.
void ApplySelectionFilters(std::vector<TaskItem>& items, const Options& opts, bool isOnline);

// --list output
void PrintItemTable(const std::vector<TaskItem>& items, bool isOnline);

// --help / --version
void PrintHelpText();
void PrintVersionText();
