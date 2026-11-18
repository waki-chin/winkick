#include "config.hpp"
#include "env.hpp"
#include "resource_ids.h"

#include <set>
#include <sstream>
#include <algorithm>

HINSTANCE g_hInstance = nullptr;
void SetAppInstance(HINSTANCE h) { g_hInstance = h; }

namespace {

const wchar_t* kEmbeddedDefaultTasksId = MAKEINTRESOURCEW(IDR_DEFAULT_TASKS);

std::vector<std::string> SplitChar(const std::string& s, char delim) {
    std::vector<std::string> out;
    size_t start = 0;
    for (;;) {
        size_t pos = s.find(delim, start);
        if (pos == std::string::npos) { out.push_back(s.substr(start)); break; }
        out.push_back(s.substr(start, pos - start));
        start = pos + 1;
    }
    return out;
}

// LTrim of spaces + tabs only (config values may legitimately keep trailing ws;
// CR/LF are stripped line-wise by the caller)
std::string LTrimWs(const std::string& s) {
    size_t b = s.find_first_not_of(" \t");
    return (b == std::string::npos) ? "" : s.substr(b);
}

// Strip CR/LF embedded in a value so serialization can't split a line on re-parse
std::string StripCrLf(const std::string& s) {
    std::string out = s;
    out.erase(std::remove(out.begin(), out.end(), '\r'), out.end());
    out.erase(std::remove(out.begin(), out.end(), '\n'), out.end());
    return out;
}

// First appearance order preserving dedupe (lowercase compare)
void AddTagUnique(std::vector<std::string>& tags, const std::string& tag) {
    for (auto& t : tags) if (t == tag) return;
    tags.push_back(tag);
}

void FinalizeItem(TaskItem& item, size_t index) {
    item.index = (int)index;
    item.state = ItemRunState::NotRun;
    item.lastExitCode = 0;
    item.lastElapsedMs = 0;
    item.skipReason.clear();
}

} // namespace

// ============================================================================
// Parser
// ============================================================================
Config ParseIniBlocks(const std::string& content, const std::string& sourceLabel) {
    Config cfg;
    cfg.sourceLabel = sourceLabel;

    // Tolerate a leading UTF-8 BOM (Windows editors often add one)
    std::string text = content;
    if (text.size() >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB &&
        (unsigned char)text[2] == 0xBF) {
        text.erase(0, 3);
    }

    // Normalize: split into lines, tolerate \r\n
    std::vector<std::string> lines = SplitChar(text, '\n');

    TaskItem cur;
    bool inSection = false;

    auto flush = [&]() {
        if (!inSection) return;
        // Title fallback
        if (env::Trim(cur.title).empty()) cur.title = "Untitled";
        if (env::Trim(cur.category).empty()) cur.category = "General";
        cfg.items.push_back(cur);
        cur = TaskItem{};
        inSection = false;
    };

    for (const std::string& rawLine : lines) {
        std::string line = rawLine;
        // strip trailing \r and \n only — value whitespace on other columns is preserved below
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();

        // Structural view: no leading spaces/tabs, trailing kept for value extraction
        size_t ls = line.find_first_not_of(" \t");
        std::string body = (ls == std::string::npos) ? "" : line.substr(ls);
        if (body.empty()) continue;                      // blank line
        // Both-ends trim is used ONLY for comment/section detection (safe there)
        std::string trimmed = env::Trim(body);
        if (trimmed.empty()) continue; // e.g. a malformed line containing CR followed by spaces
        if (trimmed[0] == '#' || trimmed[0] == ';') continue;   // comment line

        if (trimmed.front() == '[' && trimmed.back() == ']') {
            // [Category | Title]
            flush();
            std::string inner = env::Trim(trimmed.substr(1, trimmed.size() - 2));
            size_t sep = inner.find('|');
            if (sep == std::string::npos) {
                cur.category = "General";
                cur.title = env::Trim(inner);
            } else {
                cur.category = env::Trim(inner.substr(0, sep));
                cur.title = env::Trim(inner.substr(sep + 1));
            }
            if (cur.category.empty()) cur.category = "General";
            if (cur.title.empty()) cur.title = "Untitled";
            inSection = true;
            continue;
        }

        if (!inSection) continue;                     // key before first section: ignore

        size_t eq = body.find('=');                 // index is valid in body (left ws stripped only)
        if (eq == std::string::npos) continue;        // malformed line: ignore
        std::string key = env::Trim(body.substr(0, eq));
        std::string value = LTrimWs(body.substr(eq + 1));   // leading ws stripped; trailing ws kept
        std::string keyLower = env::ToLower(key);

        if (keyLower == "param") {
            for (const std::string& tokRaw : SplitChar(value, ',')) {
                std::string tok = env::ToLower(env::Trim(tokRaw));
                if (tok == "check:1") cur.checked = true;
                else if (tok == "check:0") cur.checked = false;
                // unknown tokens ignored
            }
        } else if (keyLower == "tags") {
            for (const std::string& tokRaw : SplitChar(value, ',')) {
                std::string tok = env::ToLower(env::Trim(tokRaw));
                if (tok.empty()) continue;
                if (env::StartsWithNoCase(tok, "group:")) {
                    std::string g = env::Trim(tok.substr(6));
                    if (!g.empty()) cur.groupName = g;   // empty group: must not clobber a prior group
                } else {
                    AddTagUnique(cur.tags, tok);
                }
            }
        } else if (keyLower == "desc") {
            cur.desc = value;
        } else if (keyLower == "cmd") {
            cur.cmd = value;
        } else if (env::StartsWithNoCase(keyLower, "arg.")) {
            std::string argName = env::Trim(key.substr(4));
            if (argName.empty()) continue;
            // split on FIRST ':' only (preserve drive letters / paths in the value)
            size_t colon = value.find(':');
            std::string typePart, valPart;
            if (colon == std::string::npos) { typePart = "txt"; valPart = value; }
            // Split on the first ':' only; the default is preserved verbatim (a user
            // typing a leading space in an arg edit box round-trips through export/elevation)
            else { typePart = env::ToLower(env::Trim(value.substr(0, colon))); valPart = value.substr(colon + 1); }
            TaskArg arg;
            arg.name = argName;
            arg.type = (typePart == "num") ? "num" : "txt";   // txt/text/anything else -> txt
            arg.defaultValue = valPart;
            arg.currentValue = valPart;
            cur.args.push_back(std::move(arg));
        }
        // unknown keys ignored
    }
    flush();

    // group:<name> first-wins precedence across the whole file (parse order)
    std::set<std::string> seenGroupDefault;
    for (auto& item : cfg.items) {
        if (item.groupName.empty() || !item.checked) continue;
        if (seenGroupDefault.count(item.groupName)) item.checked = false;
        else seenGroupDefault.insert(item.groupName);
    }

    for (size_t i = 0; i < cfg.items.size(); ++i) {
        FinalizeItem(cfg.items[i], i);
        cfg.items[i].defaultChecked = cfg.items[i].checked;
    }
    return cfg;
}

// ============================================================================
// Serializer (export)
// ============================================================================
std::string SerializeIniBlocks(const std::vector<TaskItem>& items) {
    std::ostringstream out;
    out << "# WinKick exported configuration\n";
    out << "# Generated: " << env::NowTimestamp() << "\n\n";
    for (const auto& item : items) {
        out << "[" << item.category << " | " << item.title << "]\n";
        out << "param = check:" << (item.checked ? "1" : "0") << "\n";
        // tags: rejoin, append group:<name> if present
        if (!item.tags.empty() || !item.groupName.empty()) {
            out << "tags  = ";
            for (size_t i = 0; i < item.tags.size(); ++i) {
                if (i) out << ", ";
                out << item.tags[i];
            }
            if (!item.groupName.empty()) {
                if (!item.tags.empty()) out << ", ";
                out << "group:" << item.groupName;
            }
            out << "\n";
        }
        if (!item.desc.empty()) out << "desc  = " << StripCrLf(item.desc) << "\n";
        for (const auto& arg : item.args) {
            out << "arg." << arg.name << " = " << (arg.type == "num" ? "num:" : "txt:") << StripCrLf(arg.currentValue) << "\n";
        }
        out << "cmd   = " << StripCrLf(item.cmd) << "\n\n";
    }
    return out.str();
}

// ============================================================================
// Embedded default (RCDATA)
// ============================================================================
bool LoadEmbeddedDefaultTasksIni(std::string& out) {
    HRSRC res = FindResourceW(g_hInstance, kEmbeddedDefaultTasksId, RT_RCDATA);
    if (!res) return false;
    DWORD size = SizeofResource(g_hInstance, res);
    if (size == 0) return false;
    HGLOBAL mem = LoadResource(g_hInstance, res);
    if (!mem) return false;
    const char* data = (const char*)LockResource(mem);
    if (!data) return false;
    out.assign(data, size);
    // Strip a leading UTF-8 BOM if present so parsing, --dump-config, and round-trips stay consistent
    if (out.size() >= 3 && (unsigned char)out[0] == 0xEF && (unsigned char)out[1] == 0xBB &&
        (unsigned char)out[2] == 0xBF) {
        out.erase(0, 3);
    }
    return true;
}

// ============================================================================
// Guards
// ============================================================================
bool ItemHasTag(const TaskItem& item, const char* tagLower) {
    std::string t = env::ToLower(tagLower ? tagLower : "");
    for (auto& tag : item.tags) if (tag == t) return true;
    return false;
}

bool OsGuardMismatch(const TaskItem& item) {
    bool hasOs = false, anyMatch = false;
    for (auto& tag : item.tags) {
        if (tag == "win7" || tag == "win10" || tag == "win11") {
            hasOs = true;
            if (env::IsOsTagCompatible(tag)) anyMatch = true;
        }
    }
    return hasOs && !anyMatch;
}

bool NetGuardBlocked(const TaskItem& item, bool isOnline) {
    return ItemHasTag(item, "net") && !isOnline;
}

bool IsGuardDisabled(const TaskItem& item, bool isOnline) {
    return NetGuardBlocked(item, isOnline) || OsGuardMismatch(item);
}

std::string GuardStatusText(const TaskItem& item, bool isOnline) {
    std::string out;
    if (NetGuardBlocked(item, isOnline)) out += "net: offline";
    if (OsGuardMismatch(item)) { if (!out.empty()) out += ", "; out += "os: mismatch"; }
    if (ItemHasTag(item, "gui")) { if (!out.empty()) out += ", "; out += "gui (silent: skip)"; }
    if (out.empty()) out = "OK";
    return out;
}
