#include "cli.hpp"
#include "env.hpp"
#include "version.h"

#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <sstream>
#include <set>

namespace {

// ---------- small helpers ----------
std::vector<std::string> SplitCsvLower(const std::string& csv) {
    std::vector<std::string> out;
    std::stringstream ss(csv);
    std::string token;
    while (std::getline(ss, token, ',')) {
        std::string t = env::ToLower(env::Trim(token));
        if (!t.empty()) out.push_back(t);
    }
    return out;
}

std::string WideArgToUtf8(const std::wstring& w) { return env::WideToUtf8(w); }

// For optional [path] args: accept the next token as the path only when it does
// not start with '-' AND it either exists, has an existing parent directory, or
// is a bare filename (no directory portion). Otherwise the default is used.
bool LooksLikePathArg(const std::wstring& tok) {
    if (tok.empty()) return false;
    if (tok.find_first_not_of(L" \t") == std::wstring::npos) return false;  // whitespace-only: not a path
    if (tok[0] == L'-') return false;
    if (env::FileExistsW(tok)) return true;
    // An existing directory is not a valid output file
    DWORD attrs = GetFileAttributesW(tok.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY)) return false;
    return env::EnsureParentDirectoryExists(tok);
}

struct FlagDef {
    const wchar_t* longName;   // without dashes, lowercase
    const wchar_t* shortName;  // nullptr if none (without dashes)
    enum class Kind { Bool, RequiredValue, OptionalValue } kind;
};

const FlagDef kFlags[] = {
    { L"help",            L"h", FlagDef::Kind::Bool },
    { L"version",         L"v", FlagDef::Kind::Bool },
    { L"config",          L"c", FlagDef::Kind::RequiredValue },
    { L"url",             L"u", FlagDef::Kind::RequiredValue },
    { L"offline",         L"o", FlagDef::Kind::Bool },
    { L"dump-config",     L"d", FlagDef::Kind::OptionalValue },
    { L"export",          L"e", FlagDef::Kind::OptionalValue },
    { L"tags",            L"t", FlagDef::Kind::RequiredValue },
    { L"exclude",         L"x", FlagDef::Kind::RequiredValue },
    { L"pick",            L"p", FlagDef::Kind::RequiredValue },
    { L"skip",            L"k", FlagDef::Kind::RequiredValue },
    { L"all",             L"a", FlagDef::Kind::Bool },
    { L"silent",          L"s", FlagDef::Kind::Bool },
    { L"dry-run",         L"n", FlagDef::Kind::Bool },
    { L"log",             L"l", FlagDef::Kind::OptionalValue },
    { L"quiet",           L"q", FlagDef::Kind::Bool },
    { L"reboot",          L"r", FlagDef::Kind::Bool },
    { L"no-reboot",       nullptr, FlagDef::Kind::Bool },
    { L"no-explorer-restart", nullptr, FlagDef::Kind::Bool },
    { L"list",            nullptr, FlagDef::Kind::Bool },
    // Internal UAC hand-off (not documented in --help)
    { L"auto-run",        nullptr, FlagDef::Kind::Bool },
    { L"cleanup-config",  nullptr, FlagDef::Kind::Bool },
};

std::wstring ToLowerW(const std::wstring& s) {
    std::wstring out = s;
    for (auto& c : out) if (c >= L'A' && c <= L'Z') c = (wchar_t)(c - L'A' + L'a');
    return out;
}

} // namespace

bool IsConsoleInvocation(const Options& o) {
    return o.showHelp || o.showVersion || o.listRequested || o.dumpConfig ||
           o.exportRequested || o.silent || o.dryRun;
}

bool ParseCommandLine(const std::wstring& cmdLine, Options& out, std::string& outError) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(cmdLine.c_str(), &argc);
    if (!argv) { outError = "Failed to parse command line."; return false; }

    auto fail = [&](const std::string& msg) { outError = msg; LocalFree(argv); return false; };

    int i = 1; // skip program name
    while (i < argc) {
        std::wstring token = argv[i];
        ++i;

        if (token.empty()) return fail("Unexpected empty argument ''. See --help.");
        if (token[0] != L'-') {
            return fail("Unexpected argument '" + WideArgToUtf8(token) + "'. See --help.");
        }

        std::wstring body = token.substr(1);
        if (!body.empty() && body[0] == L'-') body = body.substr(1); // strip second dash

        // split at first '=' (value attached)
        std::wstring name, attachedVal;
        bool hasAttachedVal = false;
        size_t eq = body.find(L'=');
        if (eq != std::wstring::npos) {
            name = body.substr(0, eq);
            attachedVal = body.substr(eq + 1);
            hasAttachedVal = true;
        } else {
            name = body;
        }
        std::wstring lowerName = ToLowerW(name);
        if (lowerName.empty()) return fail("Empty flag '-'. See --help.");

        // Find flag definition (match long or short name)
        const FlagDef* def = nullptr;
        for (const auto& f : kFlags) {
            if (lowerName == f.longName) { def = &f; break; }
            if (f.shortName && lowerName == f.shortName) { def = &f; break; }
        }
        if (!def) return fail("Unknown flag '--" + WideArgToUtf8(lowerName) + "'. See --help.");

        // Attached values only make sense for value-taking flags; whitespace-only is empty
        if (hasAttachedVal && def->kind == FlagDef::Kind::Bool)
            return fail("Flag '--" + WideArgToUtf8(lowerName) + "' does not take a value.");
        if (hasAttachedVal && attachedVal.find_first_not_of(L" \t") == std::wstring::npos)
            return fail("Empty value for '--" + WideArgToUtf8(lowerName) + "'.");

        // Required values: reject the next token when it looks like a flag (users who
        // genuinely want a leading dash can use --flag=-value); empty is also an error.
        auto takeRequiredValue = [&](std::wstring& outVal) -> bool {
            if (hasAttachedVal) {
                // attachedVal is known non-whitespace here (checked at parse time above)
                outVal = attachedVal;
                return true;
            }
            if (i < argc) {
                std::wstring v = argv[i];
                if (v.empty() || v.find_first_not_of(L" \t") == std::wstring::npos) return false; // would eat a flag/blank
                if (!v.empty() && v[0] == L'-') return false; // would eat a flag; report missing value
                ++i;
                outVal = v;
                return true;
            }
            return false;
        };
        auto takeOptionalPath = [&](std::wstring& pathOut, const std::string& flagForErr) -> bool {
            if (hasAttachedVal) { pathOut = attachedVal; return true; }
            if (i < argc) {
                std::wstring nxt = argv[i];
                if (nxt.empty() || nxt.find_first_not_of(L" \t") == std::wstring::npos) {
                    return fail("Empty value for --" + flagForErr + ".");
                }
                if (LooksLikePathArg(nxt)) { pathOut = nxt; ++i; }
            }
            return true;
        };

        const std::wstring& ln = lowerName;
        if (ln == L"help" || ln == L"h") out.showHelp = true;
        else if (ln == L"version" || ln == L"v") out.showVersion = true;
        else if (ln == L"config" || ln == L"c") {
            std::wstring v;
            if (!takeRequiredValue(v) || v.empty()) return fail("Missing value for --config <path>.");
            out.configPath = v; out.hasConfigPath = true;
        }
        else if (ln == L"url" || ln == L"u") {
            std::wstring v;
            if (!takeRequiredValue(v) || v.empty()) return fail("Missing value for --url <url>.");
            out.url = v; out.hasUrl = true;
        }
        else if (ln == L"offline" || ln == L"o") out.offline = true;
        else if (ln == L"dump-config" || ln == L"d") {
            out.dumpConfig = true;
            if (!takeOptionalPath(out.dumpConfigPath, "dump-config")) return false;
        }
        else if (ln == L"export" || ln == L"e") {
            out.exportRequested = true;
            if (!takeOptionalPath(out.exportPath, "export")) return false;
        }
        else if (ln == L"tags" || ln == L"t") {
            std::wstring v;
            if (!takeRequiredValue(v) || v.empty()) return fail("Missing value for --tags <t1,t2,...>.");
            auto items = SplitCsvLower(WideArgToUtf8(v));
            if (items.empty()) return fail("--tags: no non-empty tokens in '" + WideArgToUtf8(v) + "'.");
            out.tags.insert(out.tags.end(), items.begin(), items.end());
        }
        else if (ln == L"exclude" || ln == L"x") {
            std::wstring v;
            if (!takeRequiredValue(v) || v.empty()) return fail("Missing value for --exclude <t1,t2,...>.");
            auto items = SplitCsvLower(WideArgToUtf8(v));
            if (items.empty()) return fail("--exclude: no non-empty tokens in '" + WideArgToUtf8(v) + "'.");
            out.excludeTags.insert(out.excludeTags.end(), items.begin(), items.end());
        }
        else if (ln == L"pick" || ln == L"p") {
            std::wstring v;
            if (!takeRequiredValue(v) || v.empty()) return fail("Missing value for --pick <s1,s2,...>.");
            auto items = SplitCsvLower(WideArgToUtf8(v));
            if (items.empty()) return fail("--pick: no non-empty tokens in '" + WideArgToUtf8(v) + "'.");
            out.picks.insert(out.picks.end(), items.begin(), items.end());
        }
        else if (ln == L"skip" || ln == L"k") {
            std::wstring v;
            if (!takeRequiredValue(v) || v.empty()) return fail("Missing value for --skip <s1,s2,...>.");
            auto items = SplitCsvLower(WideArgToUtf8(v));
            if (items.empty()) return fail("--skip: no non-empty tokens in '" + WideArgToUtf8(v) + "'.");
            out.skips.insert(out.skips.end(), items.begin(), items.end());
        }
        else if (ln == L"all" || ln == L"a") out.selectAll = true;
        else if (ln == L"silent" || ln == L"s") out.silent = true;
        else if (ln == L"dry-run" || ln == L"n") out.dryRun = true;
        else if (ln == L"log" || ln == L"l") {
            out.logEnabled = true;
            if (!takeOptionalPath(out.logPath, "log")) return false;
        }
        else if (ln == L"quiet" || ln == L"q") out.quiet = true;
        else if (ln == L"reboot" || ln == L"r") out.reboot = true;
        else if (ln == L"no-reboot") out.noReboot = true;
        else if (ln == L"no-explorer-restart") out.noExplorerRestart = true;
        else if (ln == L"list") out.listRequested = true;
        else if (ln == L"auto-run") out.autoRun = true;
        else if (ln == L"cleanup-config") out.cleanupConfig = true;
        else return fail("Unhandled flag. See --help.");
    }

    LocalFree(argv);
    return true;
}

// ============================================================================
// Selection & filter pipeline
// ============================================================================
namespace {

bool ItemMatchesTagFilter(const TaskItem& item, const std::string& filterLower) {
    if (filterLower.empty()) return false;
    if (env::StartsWithNoCase(filterLower, "group:")) {
        return !item.groupName.empty() && env::ToLower(item.groupName) == env::Trim(filterLower.substr(6));
    }
    for (auto& t : item.tags) if (t == filterLower) return true;
    return false;
}

std::string DisplayNameLower(const TaskItem& item) {
    return env::ToLower(item.category + " | " + item.title);
}

bool DisplayNameContains(const TaskItem& item, const std::string& needleLower) {
    if (needleLower.empty()) return false;
    return DisplayNameLower(item).find(needleLower) != std::string::npos;
}

} // namespace

void ApplySelectionFilters(std::vector<TaskItem>& items, const Options& opts, bool isOnline) {
    const bool hasPositiveFilter = !opts.tags.empty() || !opts.picks.empty();

    // 1) Base state
    if (opts.selectAll) {
        for (auto& item : items) item.checked = true;
    } else if (hasPositiveFilter) {
        for (auto& item : items) {
            bool match = false;
            for (auto& t : opts.tags) if (ItemMatchesTagFilter(item, t)) { match = true; break; }
            if (!match) for (auto& p : opts.picks) if (DisplayNameContains(item, p)) { match = true; break; }
            item.checked = match;
        }
    }
    // else: keep config defaults

    // 2) Exclusions always win
    for (auto& item : items) {
        if (!item.checked) continue;
        bool excluded = false;
        for (auto& t : opts.excludeTags) if (ItemMatchesTagFilter(item, t)) { excluded = true; break; }
        if (!excluded) for (auto& s : opts.skips) if (DisplayNameContains(item, s)) { excluded = true; break; }
        if (excluded) item.checked = false;
    }

    // 3) Guards: net + OS
    for (auto& item : items) {
        if (!item.checked) continue;
        if (NetGuardBlocked(item, isOnline)) item.checked = false;
        else if (OsGuardMismatch(item))      item.checked = false;
    }

    // 4) group:<name> first-wins over remaining selected items (file order)
    std::set<std::string> pickedGroups;
    for (auto& item : items) {
        if (!item.checked || item.groupName.empty()) continue;
        std::string g = env::ToLower(item.groupName);
        if (pickedGroups.count(g)) item.checked = false;
        else pickedGroups.insert(g);
    }
}

namespace {
// Pad right with spaces; always leave at least one space as a column separator
std::string PadRight(const std::string& s, size_t width) {
    if (s.size() >= width) return s + ' ';
    return s + std::string(width - s.size(), ' ');
}
} // namespace

// ============================================================================
// --list
// ============================================================================
void PrintItemTable(const std::vector<TaskItem>& items, bool isOnline) {
    std::ostringstream oss;
    oss << PadRight("Idx", 4) << PadRight("Category", 22) << PadRight("Title", 46)
        << PadRight("Default", 8) << PadRight("Tags", 24) << "Guard\n";
    oss << PadRight("----", 4) << PadRight("----------------------", 22)
        << PadRight("----------------------------------------------", 46)
        << PadRight("--------", 8) << PadRight("------------------------", 24)
        << "-------------------\n";
    for (const auto& item : items) {
        std::string tags;
        for (size_t i = 0; i < item.tags.size(); ++i) { if (i) tags += ", "; tags += item.tags[i]; }
        if (!item.groupName.empty()) { if (!tags.empty()) tags += ", "; tags += "group:" + item.groupName; }
        oss << PadRight(std::to_string(item.index), 4)
            << PadRight(item.category, 22)
            << PadRight(item.title, 46)
            << PadRight(item.defaultChecked ? "yes" : "no", 8)
            << PadRight(tags, 24)
            << GuardStatusText(item, isOnline) << '\n';
    }
    env::PrintUtf8(oss.str());
}

// ============================================================================
void PrintVersionText() {
    env::PrintUtf8("WinKick " WINKICK_VERSION_STRING);
}

void PrintHelpText() {
    env::PrintUtf8(
"WinKick " WINKICK_VERSION_STRING " - Modular Windows Setup & Task Utility (Kickstart-style)\n"
"\n"
"Usage:\n"
"  winkick.exe [options]\n"
"  (no options) launches the GUI.\n"
"\n"
"Source options:\n"
"  -c, --config <path>       Load configuration from a local .ini file\n"
"  -u, --url <url>           Fetch configuration from a raw URL (HTTPS)\n"
"  -o, --offline             Skip network; use the built-in embedded tasks.ini\n"
"  -d, --dump-config [path]  Write the embedded tasks.ini to disk (default: tasks.ini)\n"
"  -e, --export [path]       Write the resolved selection to tasks-export.ini and exit\n"
"\n"
"Selection & filter options (usable in GUI pre-selection and --silent):\n"
"  -t, --tags <t1,t2,...>    Select items carrying any listed tag (case-insensitive)\n"
"  -x, --exclude <t1,t2,...> Deselect items carrying any listed tag\n"
"  -p, --pick <s1,s2,...>    Select items whose 'Category | Title' contains a substring\n"
"  -k, --skip <s1,s2,...>    Deselect items whose 'Category | Title' contains a substring\n"
"  -a, --all                 Select all compatible items (respects exclusions/guards/groups)\n"
"\n"
"Execution & output options:\n"
"  -s, --silent              Run headless. Requires elevation (no prompts in headless mode)\n"
"  -n, --dry-run             Simulate the run; print/log commands without executing\n"
"  -l, --log [path]          Write a UTF-8 log (default: log.txt)\n"
"  -q, --quiet               Suppress stdout messages during --silent / --dry-run (and suppress modal fatal dialogs)\n"
"  -r, --reboot              In --silent mode, reboot if an executed item has the 'reboot' tag\n"
"      --no-reboot           Suppress auto-reboot and the GUI reboot prompt\n"
"      --no-explorer-restart Suppress the explorer.exe restart at the end of the run\n"
"      --list                Print the item table (Index/Category/Title/Default/Tags/Guard)\n"
"  -h, --help                Show this help\n"
"  -v, --version             Show version\n"
"\n"
"Notes:\n"
"  * Default tasks are embedded in this build. Remote task loading requires --url.\n"
"    A connectivity probe may still run unless --offline is specified.\n"
"  * All flags support --flag=value and --flag value forms.\n"
"  * Guards: 'net' (needs connectivity), 'win7'/'win10'/'win11' (OS match, OR logic),\n"
"    'gui' (skipped in --silent). 'group:<name>' = mutual exclusion (first checked wins).\n"
"  * 'reboot' and 'restart-explorer' tags trigger end-of-run lifecycle actions.\n");
}



