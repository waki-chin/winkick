// WinKick - INI-block configuration model, parser and serializer
#pragma once

#include <string>
#include <vector>
#include <set>

#include <windows.h>

enum class ItemRunState {
    NotRun,
    Skipped,   // guard-skipped / not selected (final state after skipped run attempt)
    DryRun,    // simulated
    Running,   // in flight
    Ok,        // executed, exit code 0
    Failed     // executed, non-zero exit code
};

struct TaskArg {
    std::string name;          // <Name> from arg.<Name>
    std::string type;          // "txt" | "num"
    std::string defaultValue;  // as written in the .ini
    std::string currentValue;  // live value (edited in GUI / exported)
};

struct TaskItem {
    std::string category;      // from [Category | Title]
    std::string title;
    std::string desc;
    std::string cmd;           // verbatim command template
    std::string groupName;     // from tags "group:<name>" (lowercased), empty if none
    std::vector<std::string> tags;   // lowercased, group:* excluded
    std::vector<TaskArg> args;      // in file appearance order
    bool checked = false;
    bool defaultChecked = false;     // post-first-wins initial state
    int index = 0;                   // 0..N-1 source order

    // Runtime state (not serialized)
    ItemRunState state = ItemRunState::NotRun;
    unsigned long lastExitCode = 0;
    unsigned long lastElapsedMs = 0;
    std::string skipReason;          // set when state == Skipped
};

struct Config {
    std::vector<TaskItem> items;
    std::string sourceLabel;         // e.g. "Embedded (Built-in)", "File: C:\\...", "URL: https://..."

};

// Parse UTF-8 INI-block text. Always succeeds (malformed lines/sections are ignored).
Config ParseIniBlocks(const std::string& content, const std::string& sourceLabel);

// Serialize items to INI-block format (used by --export and the elevation hand-off).
std::string SerializeIniBlocks(const std::vector<TaskItem>& items);

// Load the embedded RCDATA default (res/tasks.ini). Requires g_hInstance set by main.
bool LoadEmbeddedDefaultTasksIni(std::string& out);

// Guard helpers ---------------------------------------------------------
bool ItemHasTag(const TaskItem& item, const char* tagLower);
bool OsGuardMismatch(const TaskItem& item);          // true when item has >=1 OS tag and none matches
bool NetGuardBlocked(const TaskItem& item, bool isOnline);
std::string GuardStatusText(const TaskItem& item, bool isOnline);  // for --list
bool IsGuardDisabled(const TaskItem& item, bool isOnline);         // grayed out in GUI

extern HINSTANCE g_hInstance;

void SetAppInstance(HINSTANCE h);
