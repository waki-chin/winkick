// WinKick - GUI implementation
#include "gui.hpp"
#include "version.h"

#include "env.hpp"
#include "net.hpp"
#include "runner.hpp"
#include "resource_ids.h"

#include <windows.h>
#include <bcrypt.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <windowsx.h>
#include <objbase.h>

#include <string>
#include <vector>
#include <thread>
#include <algorithm>
#include <set>
#include <cstring>
#include <deque>
#include <mutex>
#include <memory>
#include <cstdint>
#include <atomic>

// WM_APP_* payloads pack (generation << 32) | idx into WPARAM, and WM_APP_RUN_DONE passes a 64-bit
// runGen there directly — only safe when WPARAM is 64-bit. The product targets x64 exclusively
// (description.md §1), so fail the build loudly if that ever changes.
static_assert(sizeof(WPARAM) >= 8 && sizeof(void*) == 8, "WinKick requires an x64 target.");

#define WM_APP_LOG_WAKE    (WM_APP + 1)
#define WM_APP_ITEM_STATUS (WM_APP + 2)
#define WM_APP_AUTORUN     (WM_APP + 4)
#define WM_APP_LV_CHANGED  (WM_APP + 5)   // deferred: checkbox state-image change
#define WM_APP_LV_SELECT   (WM_APP + 6)   // deferred: selection change -> inspector
#define WM_APP_RUN_DONE    (WM_APP + 7)   // worker completed; strictly ordered after all WM_APP_ITEM_STATUS
#define WM_APP_URL_LOADED  (WM_APP + 8)   // remote fetch finished; payload = UrlFetchResult*

#define IDC_SOURCE_COMBO  1001
#define IDC_TAG_COMBO     1002
#define IDC_LOAD_BTN      1003
#define IDC_EXPORT_BTN    1004
#define IDC_SELECT_BTN    1005
#define IDC_DESELECT_BTN  1006
#define IDC_APPLY_BTN     1007
#define IDC_DRYRUN_CHK    1008
#define IDC_SAVELOG_CHK   1009
#define IDC_LISTVIEW      1010
#define IDC_DESC_STATIC   1011
#define IDC_LOG_EDIT      1012
#define IDC_SOURCE_LABEL  1013
#define IDC_TAG_LABEL     1014
#define IDC_ARG_LABEL_0   1100
#define IDC_ARG_EDIT_0    1110
#define MENU_LOAD_FROM_FILE 2001
#define MENU_LOAD_FROM_URL  2002
#define IDC_CTX_SELECT_CAT   2101
#define IDC_CTX_DESELECT_CAT 2102

namespace {

constexpr int kArgSlots = 6;
constexpr int kMaxLogChars = 400000;

// Status/completion payloads must not be dropped when a large dry-run fills the
// 10,000-message queue. Throttle only the worker; the UI continues draining it.
bool PostWorkerMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    while (!PostMessageW(hwnd, msg, wParam, lParam)) {
        if (GetLastError() != ERROR_NOT_ENOUGH_QUOTA || !IsWindow(hwnd)) return false;
        Sleep(1);
    }
    return true;
}

struct SourceEntry {
    std::wstring label;   // shown in combo
    int kind = 0;         // 0 = Embedded, 1 = File, 2 = URL
    std::wstring detail;  // path or url (empty for embedded)
};

struct ItemStatusMsg {
    ItemRunState state;
    unsigned long exitCode;
    unsigned long elapsedMs;
    uint64_t runGen = 0;           // run generation; stale messages from an earlier run are dropped
};

// Result of an asynchronous remote INI fetch (+ Load... -> From URL..., or a combo reload)
struct UrlFetchResult {
    SourceEntry entry;             // kind == 2 (URL); entry.label = "URL: <url>"
    bool isNewSource = false;      // came from LoadSourceFromUrl (append if absent)
    bool ok = false;               // transport + INI sanity check passed
    std::string body, err;
};

// Coalesced log pump: the runner thread pushes here; the UI thread drains in a batch.
// Bounds the cross-thread message count regardless of output volume.
struct LogPump {
    std::mutex m;
    std::deque<std::string> pending;
    std::atomic<bool> wakeQueued{false};
    HWND h = nullptr;

    void Push(const std::string& line) {
        {
            std::lock_guard<std::mutex> lk(m);
            pending.push_back(line);
        }
        if (!wakeQueued.exchange(true) && h) {
            if (!PostMessageW(h, WM_APP_LOG_WAKE, 0, 0)) wakeQueued = false;  // don't wedge the pump
        }
    }
};

struct GuiState {
    Config config;
    bool isOnline = false;
    Options opts;                  // CLI options (read-only reference to invocation)
    std::wstring sourceLabel;      // label of the clean source the state derives from

    HWND hwnd = nullptr;
    HWND sourceCombo = nullptr;
    HWND sourceLabelCtl = nullptr;
    HWND tagLabelCtl = nullptr;
    HWND tagCombo = nullptr;
    HWND loadBtn = nullptr;
    HWND exportBtn = nullptr;
    HWND selectBtn = nullptr;
    HWND deselectBtn = nullptr;
    HWND applyBtn = nullptr;
    HWND dryRunChk = nullptr;
    HWND saveLogChk = nullptr;
    HWND listView = nullptr;
    HWND descStatic = nullptr;
    HWND logEdit = nullptr;
    HWND argLabels[kArgSlots] = {};
    HWND argEdits[kArgSlots] = {};

    HFONT uiFont = nullptr;
    HFONT monoFont = nullptr;

    int dpi = 96;

    std::vector<SourceEntry> sources;  // clean sources (marker not included)
    bool modified = false;             // current state diverges from any clean source
    bool running = false;              // worker thread active
    bool suppressSourceEvents = false; // programmatic combo rebuild in flight
    bool suppressArgEvents = false;    // programmatic inspector refill in flight
    bool enforcingGroup = false;       // group uncheck recursion guard
    bool inBulkOp = false;             // bulk select/deselect guard

    int inspectIndex = -1;             // item index shown in the inspector
    int inspectArgCount = 0;

    bool lastRunDryRun = false;
    std::string pendingContextCategory; // set before TrackPopupMenu, consumed by WM_COMMAND
    bool suppressListEvents = false;    // true while (re)populating the ListView

    std::shared_ptr<LogPump> logPump;   // coalesced worker->UI log pump
    int activeSourceIndex = 0;          // index of the clean source the current state derives from
    uint32_t configGen = 0;             // bumped on every config (re)load; stale LV_CHANGED msgs carry old gen (32-bit: packed into wParam's high half)
    uint64_t runGen = 0;                // bumped on every run; stale ITEM_STATUS/RUN_DONE msgs carry old gen
    bool lastRunSaveLog = false;        // Save Log checkbox was on for the last run
    std::string lastRunLogPath;         // log path used by the last run's worker ("" when none)
    bool fetchInFlight = false;         // async remote fetch running; controls disabled meanwhile
    bool closeWhenDone = false;          // user asked to close mid-operation; exit after the run/fetch completes
    bool elevating = false;              // UAC hand-off pending (ShellExecuteExW blocks); controls disabled, Apply/close re-entry blocked
};

} // namespace

// single-window application state (owned by RunGui)
void RestoreFocusAfterBusy(GuiState* s);  // defined just before FinishRun (file scope, below)
namespace {
void LayoutControls(GuiState* s, int clientW, int clientH);  // defined in the layout section below
void EnableInteractiveControls(GuiState* s, bool enable);    // defined in the layout section below
} // namespace
static void ReplaceFonts(GuiState* s);

namespace {

void ApplyFontTo(GuiState* s, HWND h) {
    if (h && s->uiFont) SendMessageW(h, WM_SETFONT, (WPARAM)s->uiFont, TRUE);
}

// ---------- small helpers ----------
std::wstring Widen(const std::string& utf8) { return env::Utf8ToWide(utf8); }
std::string Narrow(const std::wstring& wide) { return env::WideToUtf8(wide); }

void AppendLogUtf8Batch(GuiState* s, const std::vector<std::string>& lines);
std::wstring StatusTextForState(ItemRunState st, unsigned long exitCode); // fwd

void AppendLogUtf8(GuiState* s, const std::string& line) {
    AppendLogUtf8Batch(s, std::vector<std::string>{line});
}

void AppendLogUtf8Batch(GuiState* s, const std::vector<std::string>& lines) {
    if (!s->logEdit || lines.empty()) return;
    // Build the text to append, trimming a single over-cap batch to the cap
    std::wstring w;
    size_t total = 0;
    for (const auto& line : lines) total += line.size() + 2;
    w.reserve(total);
    for (const auto& line : lines) { w += Widen(line); w += L"\r\n"; }
    // EM_REPLACESEL stops at L'\0'; strip any embedded NULs
    w.erase(std::remove(w.begin(), w.end(), L'\0'), w.end());
    if ((int)w.size() > kMaxLogChars) {
        // Keep the tail — but never split a surrogate pair or a CRLF boundary
        size_t keepFrom = w.size() - (size_t)kMaxLogChars;
        if (keepFrom > 0 && keepFrom < w.size()) {
            // If we would start at a low surrogate, advance past the complete pair
            if (w[keepFrom] >= 0xDC00 && w[keepFrom] <= 0xDFFF) ++keepFrom;
            // If we split between \r and \n, start past the \n
            if (keepFrom < w.size() && w[keepFrom - 1] == L'\r' && w[keepFrom] == L'\n') ++keepFrom;
        }
        w = w.substr(keepFrom);
    }

    // Make room: delete enough leading characters so len + w stays under the cap
    int len = GetWindowTextLengthW(s->logEdit);
    int overflow = (len + (int)w.size()) - kMaxLogChars;
    if (overflow > 0) {
        int cut = std::min(overflow + 1024 /* slack */, len);
        // Snap the cut to a UTF-16 boundary so we never split a surrogate pair or a CRLF.
        // Reading the control text here is only done on this rare overflow path.
        if (cut > 0 && cut < len && (len <= (int)(kMaxLogChars + 65536))) {
            std::wstring text((size_t)len + 1, L'\0');
            int got = GetWindowTextW(s->logEdit, text.data(), len + 1);
            text.resize((size_t)got);
            if (cut < (int)text.size()) {
                // Split inside a surrogate pair -> move cut after the low surrogate
                if (text[cut] >= 0xDC00 && text[cut] <= 0xDFFF) ++cut;
                // Split between \r and \n -> move cut past the \n
                if (cut < (int)text.size() && text[cut - 1] == L'\r' && text[cut] == L'\n') ++cut;
                if (cut > len) cut = len;
            }
        }
        SendMessageW(s->logEdit, EM_SETSEL, 0, cut);
        SendMessageW(s->logEdit, EM_REPLACESEL, FALSE, (LPARAM)L"");
    }

    int end = GetWindowTextLengthW(s->logEdit);
    SendMessageW(s->logEdit, EM_SETSEL, end, end);
    SendMessageW(s->logEdit, EM_REPLACESEL, FALSE, (LPARAM)w.c_str());
    SendMessageW(s->logEdit, EM_SCROLLCARET, 0, 0);
}

std::wstring GetCtrlText(HWND hwnd) {
    int len = GetWindowTextLengthW(hwnd);
    if (len <= 0) return L"";
    std::wstring buf((size_t)len + 1, L'\0');
    int got = GetWindowTextW(hwnd, buf.data(), len + 1);
    if (got <= 0) return L"";
    buf.resize((size_t)got);
    return buf;
}

void SetCtrlText(HWND hwnd, const std::wstring& text) { SetWindowTextW(hwnd, text.c_str()); }

// Read the selected item text of a combo box without any fixed buffer limit
std::wstring ComboSelectionWide(HWND combo) {
    LRESULT sel = SendMessageW(combo, CB_GETCURSEL, 0, 0);
    if (sel < 0) return L"";
    LRESULT len = SendMessageW(combo, CB_GETLBTEXTLEN, (WPARAM)sel, 0);
    if (len < 0 || len == CB_ERR) return L"";
    std::wstring buf((size_t)len + 1, L'\0');
    LRESULT got = SendMessageW(combo, CB_GETLBTEXT, (WPARAM)sel, (LPARAM)buf.data());
    if (got < 0 || got == CB_ERR) return L"";
    buf.resize((size_t)got);   // actual count may differ from CB_GETLBTEXTLEN's estimate
    return buf;
}

// Scale a 96-DPI pixel value to the window's current DPI
inline int S(int dpi, int px) { return MulDiv(px, dpi, 96); }

// Guard-disabled check for display (net / OS only; 'gui' is not a disable).
bool ItemLocked(GuiState* s, const TaskItem& item) { return IsGuardDisabled(item, s->isOnline); }

// ============================================================================
// Source combo: the '* Custom (Modified)' marker handling
// ============================================================================
// targetIndex: index into the rebuilt combo contents, or -1 to auto-select the marker (if any)
void RebuildSourceComboImpl(GuiState* s, int targetIndex) {
    s->suppressSourceEvents = true;
    SendMessageW(s->sourceCombo, CB_RESETCONTENT, 0, 0);
    int markerIndex = -1;
    for (size_t i = 0; i < s->sources.size(); ++i) {
        SendMessageW(s->sourceCombo, CB_ADDSTRING, 0, (LPARAM)s->sources[i].label.c_str());
    }
    if (s->modified) {
        markerIndex = (int)s->sources.size();
        SendMessageW(s->sourceCombo, CB_ADDSTRING, 0, (LPARAM)L"* Custom (Modified)");
    }
    int target = (targetIndex >= 0) ? targetIndex
                                    : (markerIndex >= 0 ? markerIndex : (int)s->sources.size() - 1);
    if (target < 0) target = 0;
    SendMessageW(s->sourceCombo, CB_SETCURSEL, (WPARAM)target, 0);
    s->suppressSourceEvents = false;
}

int SourceComboIndexOf(GuiState* s, const SourceEntry& entry) {
    for (size_t i = 0; i < s->sources.size(); ++i) {
        if (s->sources[i].kind == entry.kind && s->sources[i].detail == entry.detail) return (int)i;
    }
    return -1;
}

void MarkModifiedImpl(GuiState* s) {
    if (s->modified) return;   // already showing the marker; don't rebuild the combo on every keystroke
    s->modified = true;
    RebuildSourceComboImpl(s, -1); // adds marker and selects it
}

} // namespace (helpers need g; keep inline below)

static void MarkModified(GuiState* s) { MarkModifiedImpl(s); }

namespace {

// ============================================================================
// ListView helpers
// ============================================================================
void SetItemCheckedState(GuiState* s, int index, bool checked) {
    ListView_SetCheckState(s->listView, index, checked ? TRUE : FALSE);
    if (index >= 0 && index < (int)s->config.items.size()) s->config.items[index].checked = checked;
}

bool GetItemCheckedState(GuiState* s, int index) {
    return ListView_GetCheckState(s->listView, index) == TRUE;
}

// LVM_SETITEMTEXT with a fully-initialized LVITEM (the ListView_SetItemText macro
// leaves LV_ITEM.mask and friends as uninitialized stack garbage - not safe on all rows).
void SetLvSubItemText(HWND lv, int index, int subItem, const std::wstring& text) {
    LVITEMW lvi{};
    lvi.mask = LVIF_TEXT;
    lvi.iSubItem = subItem;
    std::wstring copy = text; // message copies synchronously
    lvi.pszText = copy.data();
    SendMessageW(lv, LVM_SETITEMTEXT, (WPARAM)(int)index, (LPARAM)&lvi);
}

void CreateListView(GuiState* s, HWND parent) {
    int dpi = s->dpi;
    s->listView = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
        WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SHOWSELALWAYS | WS_TABSTOP | LVS_NOSORTHEADER,
        0, 0, 100, 100, parent, (HMENU)IDC_LISTVIEW, GetModuleHandleW(nullptr), nullptr);

    ListView_SetExtendedListViewStyle(s->listView,
        LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    SendMessageW(s->listView, LVM_ENABLEGROUPVIEW, TRUE, 0);

    LVCOLUMNW col{};
    col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
    col.fmt = LVCFMT_LEFT;
    std::wstring task = L"Task", tags = L"Tags", status = L"Status";
    col.pszText = task.data(); col.cx = S(dpi, 400); ListView_InsertColumn(s->listView, 0, &col);
    col.pszText = tags.data();  col.cx = S(dpi, 210); ListView_InsertColumn(s->listView, 1, &col);
    col.pszText = status.data(); col.cx = S(dpi, 160); ListView_InsertColumn(s->listView, 2, &col);
}

void RebuildListViewAll(GuiState* s) {
    // Destroy and recreate the control for a clean state (items + groups)
    s->configGen++;                // invalidate any WM_APP_LV_CHANGED posted before this rebuild
    s->runGen++;                   // and any WM_APP_ITEM_STATUS / RUN_DONE posted before it
    if (s->listView) {
        DestroyWindow(s->listView);
        s->listView = nullptr;
    }
    CreateListView(s, s->hwnd);
    if (!s->listView) {
        MessageBoxW(s->hwnd, L"Could not create the task list. WinKick will close without executing tasks.",
                    L"WinKick", MB_OK | MB_ICONERROR);
        EnableWindow(s->applyBtn, FALSE);
        PostMessageW(s->hwnd, WM_CLOSE, 0, 0);
        return;
    }
    ApplyFontTo(s, s->listView);
    // The recreated ListView would land at the end of the Tab order; put it back right
    // after deselectBtn, where it was first created.
    if (s->deselectBtn) {
        SetWindowPos(s->listView, s->deselectBtn, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }

    // Suppress LVN_ITEMCHANGING/CHANGED and selection side effects while populating:
    s->suppressListEvents = true;

    // Guard-disabled items show as unchecked (runtime display state only)
    for (auto& item : s->config.items) if (ItemLocked(s, item)) item.checked = false;

    // Groups: category in order of first appearance
    std::vector<std::pair<std::string, int>> catOrder;
    auto categoryGroupId = [&](const std::string& cat) -> int {
        for (auto& p : catOrder) if (p.first == cat) return p.second;
        int id = (int)catOrder.size();
        catOrder.emplace_back(cat, id);
        return id;
    };
    for (auto& item : s->config.items) categoryGroupId(item.category);
    for (auto& kv : catOrder) {
        LVGROUP gr{};
        gr.cbSize = sizeof(gr);
        gr.mask = LVGF_HEADER | LVGF_GROUPID;
        std::wstring header = Widen(kv.first);
        gr.pszHeader = header.data();
        gr.iGroupId = kv.second;
        SendMessageW(s->listView, LVM_INSERTGROUP, (WPARAM)-1, (LPARAM)&gr);
    }

    // Items
    for (auto& item : s->config.items) {
        std::wstring titleW = Widen(item.title);
        LVITEMW lv{};
        lv.mask = LVIF_TEXT | LVIF_GROUPID | LVIF_PARAM;
        lv.iItem = item.index;
        lv.iSubItem = 0;
        lv.pszText = titleW.data();
        lv.iGroupId = categoryGroupId(item.category);
        lv.lParam = (LPARAM)item.index;
        ListView_InsertItem(s->listView, &lv);

        // Tags column
        std::string tags;
        for (size_t i = 0; i < item.tags.size(); ++i) { if (i) tags += ", "; tags += item.tags[i]; }
        if (!item.groupName.empty()) { if (!tags.empty()) tags += ", "; tags += "group:" + item.groupName; }
        SetLvSubItemText(s->listView, item.index, 1, Widen(tags));

        // Status column
        SetLvSubItemText(s->listView, item.index, 2, L"");
    }

    // Apply checkbox state AFTER all inserts (most reliable across comctl versions)
    for (auto& item : s->config.items) {
        ListView_SetCheckState(s->listView, item.index, item.checked ? TRUE : FALSE);
    }

    s->suppressListEvents = false;

    // The recreated listview is born at (0,0,100,100); re-anchor the full layout now
    if (s->hwnd) {
        RECT rc{};
        GetClientRect(s->hwnd, &rc);
        LayoutControls(s, rc.right - rc.left, rc.bottom - rc.top);
    }
}

void RebuildListView(GuiState* s) { RebuildListViewAll(s); }

void RebuildTagComboAll(GuiState* s) {
    s->suppressSourceEvents = true; // reuse guard for combo rebuilds
    SendMessageW(s->tagCombo, CB_RESETCONTENT, 0, 0);
    SendMessageW(s->tagCombo, CB_ADDSTRING, 0, (LPARAM)L"All");
    std::set<std::string> uniq;
    for (auto& item : s->config.items) for (auto& t : item.tags) uniq.insert(t);
    for (auto& t : uniq) SendMessageW(s->tagCombo, CB_ADDSTRING, 0, (LPARAM)Widen(t).c_str());
    SendMessageW(s->tagCombo, CB_SETCURSEL, 0, 0);
    s->suppressSourceEvents = false;
}

void RebuildTagCombo(GuiState* s) { RebuildTagComboAll(s); }

std::wstring StatusTextForState(ItemRunState st, unsigned long exitCode) {
    switch (st) {
        case ItemRunState::NotRun: return L"";
        case ItemRunState::Running: return L"Running...";
        case ItemRunState::Ok: return L"[OK] Done";
        case ItemRunState::Failed: {
            wchar_t buf[64];
            swprintf(buf, _countof(buf), L"[ERR %lu] Failed", exitCode);
            return buf;
        }
        case ItemRunState::Skipped: return L"[SKIP]";
        case ItemRunState::DryRun: return L"[DRY-RUN]";
    }
    return L"";
}

void UpdateItemStatusInList(GuiState* s, int index, ItemRunState state, unsigned long exitCode) {
    if (index < 0 || index >= (int)s->config.items.size()) return;
    s->config.items[index].state = state;
    s->config.items[index].lastExitCode = exitCode;
    std::wstring txt = StatusTextForState(state, exitCode);
    SetLvSubItemText(s->listView, index, 2, txt);
    if (state == ItemRunState::Running) ListView_EnsureVisible(s->listView, index, FALSE);
    // Force repaint of the row
    RECT rc{};
    if (ListView_GetItemRect(s->listView, index, &rc, LVIR_BOUNDS)) {
        InvalidateRect(s->listView, &rc, TRUE);
    }
}

// ============================================================================
// Inspector (description + arg editor pool)
// ============================================================================
void SetEditNumberStyle(HWND edit, bool numeric) {
    LONG style = GetWindowLongW(edit, GWL_STYLE);
    if (numeric) style |= ES_NUMBER; else style &= ~ES_NUMBER;
    SetWindowLongW(edit, GWL_STYLE, style);
}

void UpdateInspector(GuiState* s) {
    s->suppressArgEvents = true;

    for (int k = 0; k < kArgSlots; ++k) {
        ShowWindow(s->argLabels[k], SW_HIDE);
        ShowWindow(s->argEdits[k], SW_HIDE);
        SetCtrlText(s->argLabels[k], L"");
        SetCtrlText(s->argEdits[k], L"");
    }

    int sel = ListView_GetNextItem(s->listView, -1, LVNI_SELECTED);
    s->inspectIndex = -1;
    s->inspectArgCount = 0;

    // Resolve the model index from the row's LVITEM::lParam (consistent with the rest of the
    // ListView code); visual row order coincides with the model only when unsorted.
    int modelIndex = -1;
    if (sel >= 0) {
        LVITEMW lvi{};
        lvi.mask = LVIF_PARAM;
        lvi.iItem = sel;
        if (ListView_GetItem(s->listView, &lvi)) modelIndex = (int)lvi.lParam;
    }
    if (modelIndex >= 0 && modelIndex < (int)s->config.items.size()) {
        TaskItem& item = s->config.items[modelIndex];
        s->inspectIndex = modelIndex;
        s->inspectArgCount = std::min((int)item.args.size(), kArgSlots);

        SetCtrlText(s->descStatic, item.desc.empty() ? L"(no description)" : Widen(item.desc));

        for (int k = 0; k < s->inspectArgCount; ++k) {
            const TaskArg& arg = item.args[k];
            SetCtrlText(s->argLabels[k], Widen(arg.name));
            SetCtrlText(s->argEdits[k], Widen(arg.currentValue));
            SetEditNumberStyle(s->argEdits[k], arg.type == "num");
            ShowWindow(s->argLabels[k], SW_SHOW);
            ShowWindow(s->argEdits[k], SW_SHOW);
        }
    } else {
        SetCtrlText(s->descStatic, L"Select a task above to see its description and arguments.");
    }

    s->suppressArgEvents = false;

    // Inspector height follows the number of visible arg rows
    if (s->hwnd) {
        RECT rc{};
        GetClientRect(s->hwnd, &rc);
        LayoutControls(s, rc.right - rc.left, rc.bottom - rc.top);
    }
}

// ============================================================================
// Group enforcement & selection helpers
// ============================================================================
void EnforceGroupExclusion(GuiState* s, int checkedIndex) {
    if (checkedIndex < 0 || checkedIndex >= (int)s->config.items.size()) return;
    const std::string& gname = s->config.items[checkedIndex].groupName;
    if (gname.empty()) return;
    bool wasEnforcing = s->enforcingGroup;
    s->enforcingGroup = true;
    for (auto& item : s->config.items) {
        if (item.index == checkedIndex) continue;
        if (item.groupName.empty()) continue;
        if (_stricmp(item.groupName.c_str(), gname.c_str()) == 0) {
            if (GetItemCheckedState(s, item.index)) SetItemCheckedState(s, item.index, false);
        }
    }
    s->enforcingGroup = wasEnforcing;
}

// Bulk select/deselect with guard + group first-wins (iterates in file order);
// returns the number of items whose checked state actually changed.
int ApplyBulkToMatching(GuiState* s, const std::wstring& tag) {
    bool isAll = (tag == L"All");
    std::string tagNarrow = Narrow(tag);
    std::set<std::string> pickedGroups;
    s->inBulkOp = true;
    int changed = 0;
    for (auto& item : s->config.items) {
        bool match = isAll;
        if (!match) {
            for (auto& t : item.tags) if (_stricmp(t.c_str(), tagNarrow.c_str()) == 0) { match = true; break; }
        }
        if (!match) continue;
        if (ItemLocked(s, item)) continue;          // guard-disabled: leave unchecked
        if (!item.groupName.empty()) {
            std::string g = env::ToLower(item.groupName);
            if (pickedGroups.count(g)) continue;    // first-wins per group
            pickedGroups.insert(g);
        }
        if (!item.checked) {
            SetItemCheckedState(s, item.index, true);
            EnforceGroupExclusion(s, item.index);   // also clear previously-checked group mates
            ++changed;
        }
    }
    s->inBulkOp = false;
    return changed;
}

int DeselectMatching(GuiState* s, const std::wstring& tag) {
    bool isAll = (tag == L"All");
    std::string tagNarrow = Narrow(tag);
    s->inBulkOp = true;
    int changed = 0;
    for (auto& item : s->config.items) {
        bool match = isAll;
        if (!match) {
            for (auto& t : item.tags) if (_stricmp(t.c_str(), tagNarrow.c_str()) == 0) { match = true; break; }
        }
        if (match && item.checked) { SetItemCheckedState(s, item.index, false); ++changed; }
    }
    s->inBulkOp = false;
    return changed;
}

int ToggleCategory(GuiState* s, const std::string& category, bool select) {
    std::set<std::string> pickedGroups;
    s->inBulkOp = true;
    int changed = 0;
    for (auto& item : s->config.items) {
        if (item.category != category) continue;
        if (!select) {
            if (item.checked) { SetItemCheckedState(s, item.index, false); ++changed; }
            continue;
        }
        if (ItemLocked(s, item)) continue;
        if (!item.groupName.empty()) {
            std::string gr = env::ToLower(item.groupName);
            if (pickedGroups.count(gr)) continue;
            pickedGroups.insert(gr);
        }
        if (!item.checked) {
            SetItemCheckedState(s, item.index, true);
            EnforceGroupExclusion(s, item.index);
            ++changed;
        }
    }
    s->inBulkOp = false;
    return changed;
}

} // namespace


// ============================================================================
// Item execution glue (worker thread -> UI)
// ============================================================================
namespace {

void StartExecution(GuiState* s) {
    if (s->running) return;
    s->running = true;
    const uint64_t runGen = ++s->runGen;          // invalidate messages from any previous run
    s->lastRunDryRun = (SendMessageW(s->dryRunChk, BM_GETCHECK, 0, 0) == BST_CHECKED);
    EnableInteractiveControls(s, false);

    // Reset per-item runtime state so a second Apply starts from a clean slate
    for (auto& item : s->config.items) {
        item.state = ItemRunState::NotRun;
        item.lastExitCode = 0;
        item.lastElapsedMs = 0;
        item.skipReason.clear();
    }
    for (size_t i = 0; i < s->config.items.size(); ++i) {
        SetLvSubItemText(s->listView, (int)i, 2, L"");
    }
    InvalidateRect(s->listView, nullptr, TRUE);

    // Snapshot items for the worker thread (UI thread owns the live vector)
    std::vector<TaskItem> snapshot = s->config.items;

    bool saveLog = (SendMessageW(s->saveLogChk, BM_GETCHECK, 0, 0) == BST_CHECKED);
    std::string logPathUtf8;
    if (saveLog) logPathUtf8 = s->opts.logEnabled ? Narrow(s->opts.logPath) : "log.txt";
    s->lastRunSaveLog = saveLog;
    s->lastRunLogPath = logPathUtf8;

    runner::Options ropt;
    ropt.dryRun = s->lastRunDryRun;
    ropt.silent = false;
    ropt.guiMode = true;
    ropt.noReboot = s->opts.noReboot;
    ropt.noExplorerRestart = s->opts.noExplorerRestart;
    ropt.logPath = logPathUtf8;
    ropt.modeLabel = ropt.dryRun ? "Dry-Run" : "GUI";
    ropt.admin = env::IsRunningAsAdmin();
    ropt.isOnline = s->isOnline;
    ropt.configSourceLabel = Narrow(s->sourceLabel);

    HWND hwnd = s->hwnd;
    std::shared_ptr<LogPump> pump = s->logPump;   // shared: worker posts wakes; UI drains
    std::thread([hwnd, pump, ropt, snapshot, runGen]() mutable {
        runner::LogSink logSink = [pump](const std::string& line) {
            pump->Push(line);
        };
        runner::ItemStateSink stateSink = [hwnd, runGen](int index, ItemRunState state, unsigned long elapsedMs, unsigned long exitCode) {
            auto* msg = new ItemStatusMsg{state, exitCode, elapsedMs, runGen};
            // Pack the generation into wParam alongside the index (msg carries runGen too)
            if (!PostWorkerMessage(hwnd, WM_APP_ITEM_STATUS, (WPARAM)(((uint64_t)runGen << 32) | (uint32_t)index), (LPARAM)msg)) {
                delete msg;  // leave Status column unchanged rather than leak
            }
        };
        runner::Stats stats = runner::RunAll(snapshot, ropt, logSink, stateSink);
        auto* done = new runner::Stats(std::move(stats));
        // Direct post, strictly after every WM_APP_ITEM_STATUS (same thread + FIFO):
        // FinishRun can never observe a run without its final row statuses.
        if (!PostWorkerMessage(hwnd, WM_APP_RUN_DONE, (WPARAM)runGen, (LPARAM)done)) {
            delete done;  // window is gone; nothing sensible to do
        }
    }).detach();
}

} // namespace

static void UpdateApplyShield(GuiState* s) {
    BOOL dry = (SendMessageW(s->dryRunChk, BM_GETCHECK, 0, 0) == BST_CHECKED);
    BOOL shield = (!env::IsRunningAsAdmin() && !dry) ? TRUE : FALSE;
    Button_SetElevationRequiredState(s->applyBtn, shield);
}

// ============================================================================
// UAC hand-off (unelevated -> elevated via ShellExecuteExW runas)
// ============================================================================
namespace {

std::wstring MakeRandomJobPath() {
    wchar_t tempDir[MAX_PATH + 1] = {};
    DWORD dirLen = GetTempPathW(MAX_PATH, tempDir);
    // "WKJ" + 32 hex + ".tmp" = 39 chars; the final path must fit MAX_PATH for CreateFileW to succeed
    if (dirLen == 0 || dirLen + 39 >= MAX_PATH) return L"";
    // 128-bit random filename — not guessable, not sequential; satisfies IsTempJobFilePath
    BYTE rnd[16] = {};
    if (!BCRYPT_SUCCESS(BCryptGenRandom(nullptr, rnd, (ULONG)sizeof(rnd), BCRYPT_USE_SYSTEM_PREFERRED_RNG))) {
        return L"";   // BCryptGenRandom returns NTSTATUS, not BOOL — SUCCESS == 0
    }
    static const wchar_t kHex[] = L"0123456789abcdef";
    wchar_t name[64] = {};
    size_t pos = 0;
    name[pos++] = L'W'; name[pos++] = L'K'; name[pos++] = L'J';
    for (int i = 0; i < 16; ++i) {
        name[pos++] = kHex[rnd[i] >> 4];
        name[pos++] = kHex[rnd[i] & 0xF];
    }
    name[pos++] = L'.'; name[pos++] = L't'; name[pos++] = L'm'; name[pos++] = L'p';
    name[pos] = L'\0';
    return std::wstring(tempDir) + name;
}

// Create the one-time job file with CREATE_NEW (fails if it somehow already exists) and write
// the snapshot directly into it — no reopen-with-CREATE_ALWAYS window.
bool WriteStateSnapshot(GuiState* s, std::wstring& outPath) {
    std::wstring where = MakeRandomJobPath();
    if (where.empty()) return false;
    outPath = std::move(where);
    HANDLE h = CreateFileW(outPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (h == INVALID_HANDLE_VALUE) { outPath.clear(); return false; }
    std::string ini = SerializeIniBlocks(s->config.items);
    const char* p = ini.data();
    size_t left = ini.size();
    BOOL ok = TRUE;
    while (left > 0) {
        DWORD chunk = (DWORD)(left > 65536 ? 65536 : left);
        DWORD wrote = 0;
        if (!WriteFile(h, p, chunk, &wrote, nullptr) || wrote == 0) { ok = FALSE; break; }
        p += wrote; left -= wrote;
    }
    CloseHandle(h);
    if (!ok || left != 0) {
        DeleteFileW(outPath.c_str());
        outPath.clear();
        return false;
    }
    return true;
}

// Resolve a path to an absolute, normalized path for forwarding across the elevation boundary
static std::wstring ResolveFullPath(const std::wstring& path) {
    // Query the required length first so long paths don't fall back to a wrong relative CWD
    DWORD need = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
    if (need == 0) return path;  // best effort
    std::wstring buf((size_t)need + 1, L'\0');
    DWORD n = GetFullPathNameW(path.c_str(), (DWORD)buf.size(), buf.data(), nullptr);
    if (n == 0 || n >= buf.size()) return path;  // best effort
    buf.resize(n);
    return buf;
}

// UAC hand-off (unelevated -> elevated via ShellExecuteExW runas).
// While this runs, the UI is disabled and s->elevating is set, because ShellExecuteExW with "runas"
// can pump messages on the COM-STA thread while the consent UI is pending (UIPI still
// applies to other processes, not to our own reentrant dispatch):
//   - WM_CLOSE must not DestroyWindow(hwnd) underneath us (stale-handle reuse on later DestroyWindow);
//   - IDC_APPLY_BTN must not re-enter and write a second job file / spawn a duplicate UAC prompt.
void HandOffForElevationRelease(GuiState* s) {
    s->elevating = false;
    EnableInteractiveControls(s, true);
    if (s->closeWhenDone) { DestroyWindow(s->hwnd); return; }
    RestoreFocusAfterBusy(s);
}

bool HandOffForElevation(GuiState* s) {
    s->elevating = true;
    EnableInteractiveControls(s, false);

    std::wstring tempPath;
    if (!WriteStateSnapshot(s, tempPath)) {
        MessageBoxW(s->hwnd, L"Could not write the temporary job file in %TEMP%.", L"WinKick",
                    MB_OK | MB_ICONERROR);
        HandOffForElevationRelease(s);   // keep elevating until the modal returns
        return false;
    }

    std::wstring fullBuf(32768, L'\0');
    DWORD n = GetModuleFileNameW(nullptr, fullBuf.data(), (DWORD)fullBuf.size());
    if (n == 0 || n >= fullBuf.size()) {
        DeleteFileW(tempPath.c_str());
        MessageBoxW(s->hwnd, L"Could not determine the full path of winkick.exe for the elevated relaunch.", L"WinKick",
                    MB_OK | MB_ICONERROR);
        HandOffForElevationRelease(s);
        return false;
    }

    bool saveLog = (SendMessageW(s->saveLogChk, BM_GETCHECK, 0, 0) == BST_CHECKED);
    std::wstring params = L"--config \"" + tempPath + L"\" --auto-run --cleanup-config";
    // Forward the user's chosen suppression flags and any custom log path across elevation
    if (s->opts.noReboot) params += L" --no-reboot";
    if (s->opts.noExplorerRestart) params += L" --no-explorer-restart";
    if (s->opts.offline || !s->isOnline) params += L" --offline";
    if (saveLog) {
        std::wstring logTarget = s->opts.logEnabled ? s->opts.logPath : L"log.txt";
        std::wstring lf = ResolveFullPath(logTarget);
        // CommandLineToArgvW rules: an odd run of backslashes before a quote escapes the quote.
        // A trailing '\"' must be doubled so lpParameters parses to the full path.
        size_t run = 0;
        for (size_t p = lf.size(); p > 0 && lf[p - 1] == L'\\'; --p) ++run;
        if (run % 2 == 1) lf += L'\\';
        // Attached --key=value form: the elevated parser takes it verbatim (the unelevated
        // instance already accepted this path). Space-form would re-validate existence and
        // reject a custom --log whose parent directory does not exist.
        params += L" --log=\"" + lf + L"\"";   // always absolute
    }

    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.hwnd = s->hwnd;
    sei.lpVerb = L"runas";
    sei.lpFile = fullBuf.c_str();
    sei.lpParameters = params.c_str();
    // Keep the elevated child in the launcher's CWD so relative paths in tasks.ini keep working
    std::wstring cwdBuf(32768, L'\0');
    DWORD cwdN = GetCurrentDirectoryW((DWORD)cwdBuf.size(), cwdBuf.data());
    sei.lpDirectory = (cwdN > 0 && cwdN < cwdBuf.size()) ? cwdBuf.c_str() : nullptr;
    sei.nShow = SW_SHOWNORMAL;

    if (!ShellExecuteExW(&sei)) {
        DWORD err = GetLastError();
        DeleteFileW(tempPath.c_str());
        if (err == ERROR_CANCELLED) {
            AppendLogUtf8(s, "Elevation cancelled by user; changes not applied.");
        } else {
            std::wstring msg = L"Failed to start elevated process (error " + std::to_wstring(err) + L").";
            MessageBoxW(s->hwnd, msg.c_str(), L"WinKick", MB_OK | MB_ICONERROR);
        }
        HandOffForElevationRelease(s);   // hold busy state until the modal returns
        return false;
    }
    if (sei.hProcess) CloseHandle(sei.hProcess);
    // Unelevated instance exits; the elevated copy takes over with live progress.
    DestroyWindow(s->hwnd);
    return true;
}

} // namespace

// ============================================================================
// Button / control actions
// ============================================================================
namespace {

void HandleApply(GuiState* s) {
    if (s->running || s->elevating || s->fetchInFlight) return;

    bool dryRun = (SendMessageW(s->dryRunChk, BM_GETCHECK, 0, 0) == BST_CHECKED);
    if (!env::IsRunningAsAdmin() && !dryRun) {
        HandOffForElevation(s);
        return;
    }
    StartExecution(s);
}

void DoExportFlow(GuiState* s, const std::wstring& path) {
    std::string ini = SerializeIniBlocks(s->config.items);
    if (!env::WriteFileUtf8(path, ini)) {
        std::wstring msg = L"Could not write export file:\n" + path;
        MessageBoxW(s->hwnd, msg.c_str(), L"WinKick - Export", MB_OK | MB_ICONERROR);
        return;
    }
    AppendLogUtf8(s, "Exported " + std::to_string(s->config.items.size()) +
                     " items to " + Narrow(path) + ".");
}

void HandleExportButton(GuiState* s) {
    std::vector<wchar_t> fileName(32768, L'\0');   // room for long paths (dialog may return >MAX_PATH)
    wcsncpy_s(fileName.data(), fileName.size(), L"tasks-export.ini", _TRUNCATE);
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = s->hwnd;
    ofn.lpstrFilter = L"INI configuration (*.ini)\0*.ini\0All files (*.*)\0*.*\0\0";
    ofn.lpstrFile = fileName.data();
    ofn.nMaxFile = (DWORD)fileName.size();
    ofn.lpstrDefExt = L"ini";
    ofn.lpstrTitle = L"Export WinKick configuration";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_HIDEREADONLY | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetSaveFileNameW(&ofn)) {
        DoExportFlow(s, fileName.data());
    } else if (CommDlgExtendedError() == FNERR_BUFFERTOOSMALL) {
        MessageBoxW(s->hwnd, L"The selected path is longer than the file dialog can return.", L"WinKick - Export", MB_OK | MB_ICONERROR);
    }
}

// Async remote fetch: a stalled URL must not freeze the UI. The worker posts WM_APP_URL_LOADED
// with the result; no GuiState access happens off the UI thread.
void StartUrlFetch(GuiState* s, const SourceEntry& entry, bool isNewSource) {
    if (s->fetchInFlight) {
        MessageBoxW(s->hwnd, L"A remote download is already in progress. Please wait.", L"WinKick",
                    MB_OK | MB_ICONINFORMATION);
        return;
    }
    s->fetchInFlight = true;
    EnableInteractiveControls(s, false);            // prevents Apply/Load/Export while in flight
    AppendLogUtf8(s, "Fetching " + Narrow(entry.detail) + " ...");
    HWND hwnd = s->hwnd;
    std::thread([hwnd, entry, isNewSource]() {
        std::string body, err;
        bool ok = net::FetchUrl(entry.detail, body, err);
        if (ok && !net::LooksLikeIniContent(body)) {
            ok = false;
            err = "Response does not look like a WinKick INI configuration (missing a [Section] header or cmd = entry).";
        }
        auto* res = new UrlFetchResult{entry, isNewSource, ok, std::move(body), std::move(err)};
        if (!PostWorkerMessage(hwnd, WM_APP_URL_LOADED, 0, (LPARAM)res)) delete res;
    }).detach();
}

// Apply the outcome of a remote fetch (on the UI thread, in the message handler)
void FinishUrlFetch(GuiState* s, const UrlFetchResult& res) {
    // fetchInFlight stays true until this function (and any modal boxes inside it) returns:
    // an external WM_CLOSE during the nested modal loop must route through the deferral path.
    const SourceEntry& entry = res.entry;
    if (res.ok) {
        // A successful remote fetch proves connectivity; refresh the net guard
        if (!s->opts.offline) s->isOnline = true;
        s->config = ParseIniBlocks(res.body, Narrow(entry.label));
        s->sourceLabel = entry.label;
        if (res.isNewSource) {
            int existing = SourceComboIndexOf(s, entry);
            if (existing >= 0) s->sources[existing] = entry; else s->sources.push_back(entry);
        }
        Options defaultOpts;
        ApplySelectionFilters(s->config.items, defaultOpts, s->isOnline);
        s->modified = false;
        int idx = SourceComboIndexOf(s, entry);
        s->activeSourceIndex = (idx >= 0) ? idx : (int)s->sources.size() - 1;
        RebuildSourceComboImpl(s, s->activeSourceIndex);
        RebuildListView(s);
        RebuildTagCombo(s);
        UpdateInspector(s);
        AppendLogUtf8(s, "Loaded " + std::to_string(s->config.items.size()) + " items from " + Narrow(entry.label) + ".");
        s->fetchInFlight = false;
        EnableInteractiveControls(s, true);
        return;
    }
    if (res.isNewSource) {
        // The user already asked to close ([X]); don't force an extra modal just to say the fetch failed
        AppendLogUtf8(s, "Fetch failed: " + res.err);
        if (!s->closeWhenDone) {
            std::wstring msg = L"Could not fetch URL:\n" + entry.detail + L"\n\n" + Widen(res.err);
            MessageBoxW(s->hwnd, msg.c_str(), L"WinKick - Load from URL", MB_OK | MB_ICONERROR);
        }
        s->fetchInFlight = false;
        EnableInteractiveControls(s, true);
        return;
    }
    AppendLogUtf8(s, "Fetch failed: " + res.err);
    if (!s->closeWhenDone) {
        MessageBoxW(s->hwnd, (L"Could not load source:\n" + entry.label).c_str(), L"WinKick",
                    MB_OK | MB_ICONERROR);
    }
    // Revert to the active clean source (or the modified marker), whichever applies
    RebuildSourceComboImpl(s, s->modified ? -1 : s->activeSourceIndex);
    s->fetchInFlight = false;
    EnableInteractiveControls(s, true);
}

void ReloadFromSourceEntry(GuiState* s, const SourceEntry& entry) {
    if (entry.kind == 2) { StartUrlFetch(s, entry, /*isNewSource=*/false); return; }
    std::string data;
    bool ok = false;
    if (entry.kind == 0) {
        ok = LoadEmbeddedDefaultTasksIni(data);
    } else {
        ok = env::ReadFileUtf8(entry.detail, data);
    }
    if (!ok) {
        MessageBoxW(s->hwnd, (L"Could not load source:\n" + entry.label).c_str(), L"WinKick",
                    MB_OK | MB_ICONERROR);
        // Revert to the active clean source (or the modified marker), whichever applies
        RebuildSourceComboImpl(s, s->modified ? -1 : s->activeSourceIndex);
        return;
    }
    s->config = ParseIniBlocks(data, Narrow(entry.label));
    s->sourceLabel = entry.label;
    // Manual/clean source switch: file defaults, not CLI filters
    Options defaultOpts;
    ApplySelectionFilters(s->config.items, defaultOpts, s->isOnline);
    s->modified = false;
    int idx = SourceComboIndexOf(s, entry);
    s->activeSourceIndex = (idx >= 0) ? idx : (int)s->sources.size() - 1;
    RebuildSourceComboImpl(s, s->activeSourceIndex);
    RebuildListView(s);
    RebuildTagCombo(s);
    UpdateInspector(s);
    AppendLogUtf8(s, "Loaded " + std::to_string(s->config.items.size()) + " items from " + Narrow(entry.label) + ".");
}

void LoadSourceFromFile(GuiState* s, const std::wstring& path) {
    std::string data;
    if (!env::ReadFileUtf8(path, data)) {
        MessageBoxW(s->hwnd, (L"Could not read file:\n" + path).c_str(), L"WinKick",
                    MB_OK | MB_ICONERROR);
        return;
    }
    SourceEntry entry;
    entry.kind = 1;
    entry.detail = path;
    entry.label = L"File: " + path;
    s->config = ParseIniBlocks(data, Narrow(entry.label));
    s->sourceLabel = entry.label;
    int existing = SourceComboIndexOf(s, entry);
    if (existing >= 0) s->sources[existing] = entry; else s->sources.push_back(entry);
    s->modified = false;
    Options defaultOpts;
    ApplySelectionFilters(s->config.items, defaultOpts, s->isOnline);
    int idx = SourceComboIndexOf(s, entry);
    s->activeSourceIndex = (idx >= 0) ? idx : (int)s->sources.size() - 1;
    RebuildSourceComboImpl(s, s->activeSourceIndex);
    RebuildListView(s);
    RebuildTagCombo(s);
    UpdateInspector(s);
    AppendLogUtf8(s, "Loaded " + std::to_string(s->config.items.size()) + " items from " + Narrow(entry.label) + ".");
}

void LoadSourceFromUrl(GuiState* s, const std::wstring& url) {
    if (url.empty()) return;
    SourceEntry entry;
    entry.kind = 2;
    entry.detail = url;
    entry.label = L"URL: " + url;
    StartUrlFetch(s, entry, /*isNewSource=*/true);
}

void ShowLoadMenu(GuiState* s) {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, MENU_LOAD_FROM_FILE, L"From Local File...");
    AppendMenuW(menu, MF_STRING, MENU_LOAD_FROM_URL, L"From URL...");
    RECT rc{};
    GetWindowRect(s->loadBtn, &rc);
    TrackPopupMenu(menu, TPM_LEFTALIGN | TPM_TOPALIGN | TPM_LEFTBUTTON,
                   rc.left, rc.bottom + 2, 0, s->hwnd, nullptr);
    DestroyMenu(menu);
}

void HandleListViewContextMenu(GuiState* s, int screenX, int screenY) {
    int modelIndex = -1;
    int lvIdx = -1;
    // Keyboard context menu per MSDN is lParam == -1 exactly (i.e. screenX == -1 && screenY == -1).
    // Negative mouse coordinates are valid on multi-monitor setups (left/top secondary monitors).
    const bool fromKeyboard = (screenX == -1 && screenY == -1);
    if (fromKeyboard) {
        // Keyboard context menu (Shift+F10 / Menu key): use the selected row
        lvIdx = ListView_GetNextItem(s->listView, -1, LVNI_SELECTED);
        if (lvIdx < 0) return;
    } else {
        POINT pt{screenX, screenY};
        ScreenToClient(s->listView, &pt);
        LVHITTESTINFO hit{};
        hit.pt = pt;
        lvIdx = ListView_SubItemHitTest(s->listView, &hit);
        if (lvIdx < 0) return;
    }
    {
        LVITEMW lvi{};
        lvi.mask = LVIF_PARAM;
        lvi.iItem = lvIdx;
        if (!ListView_GetItem(s->listView, &lvi)) return;
        modelIndex = (int)lvi.lParam;
        if (modelIndex < 0 || modelIndex >= (int)s->config.items.size()) return;
    }
    std::string category = s->config.items[modelIndex].category;

    s->pendingContextCategory = category;
    HMENU menu = CreatePopupMenu();
    // Escape '&' so category text does not become a mnemonic underline
    std::wstring esc = Widen(category);
    std::wstring labelSel = L"Select All in Category (";
    for (wchar_t ch : esc) { labelSel += ch; if (ch == L'&') labelSel += L'&'; }
    labelSel += L")";
    AppendMenuW(menu, MF_STRING, IDC_CTX_SELECT_CAT, labelSel.c_str());
    AppendMenuW(menu, MF_STRING, IDC_CTX_DESELECT_CAT, L"Deselect All in Category");
    if (fromKeyboard) {
        // Keyboard: place the menu near the selected row (LV row index, not the model index)
        ListView_EnsureVisible(s->listView, lvIdx, FALSE);
        RECT itemRect{};
        POINT pt{0, 0};
        if (ListView_GetItemRect(s->listView, lvIdx, &itemRect, LVIR_LABEL)) {
            pt = POINT{itemRect.left, itemRect.top};
        }
        ClientToScreen(s->listView, &pt);
        TrackPopupMenu(menu, TPM_LEFTALIGN | TPM_TOPALIGN | TPM_LEFTBUTTON,
                       pt.x, pt.y, 0, s->hwnd, nullptr);
    } else {
        TrackPopupMenu(menu, TPM_LEFTALIGN | TPM_TOPALIGN | TPM_LEFTBUTTON,
                       screenX, screenY, 0, s->hwnd, nullptr);
    }
    DestroyMenu(menu);
}

} // namespace

// ============================================================================
// File procs (input dialog) and window creation
// ============================================================================
namespace {

struct InputDialogData {
    std::wstring prompt, text;
    bool ok = false;
    bool done = false;             // set when the dialog is closed (any path)
    UINT dpi = 96;
    HFONT font = nullptr;          // owned; deleted in WM_NCDESTROY (after child teardown)
    HWND owner = nullptr;          // re-enabled before the dialog closes
    int defaultId = IDOK;          // tracked default push button (DM_GETDEFID/DM_SETDEFID)
};

inline int DlgS(const InputDialogData* d, int px) { return MulDiv(px, (int)d->dpi, 96); }

LRESULT CALLBACK InputDialogProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    InputDialogData* d = (InputDialogData*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg) {
        case WM_CREATE: {
            CREATESTRUCTW* cs = (CREATESTRUCTW*)lParam;
            d = (InputDialogData*)cs->lpCreateParams;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)d);
            HFONT font = nullptr;
            LOGFONTW lf{};
            if (env::GetMessageFontForDpi(&lf, d->dpi)) font = CreateFontIndirectW(&lf);
            if (font) { d->font = font; } else { font = (HFONT)GetStockObject(DEFAULT_GUI_FONT); }
            HWND label = CreateWindowExW(0, L"STATIC", d->prompt.c_str(), WS_CHILD | WS_VISIBLE | SS_NOPREFIX,
                DlgS(d, 12), DlgS(d, 12), DlgS(d, 380), DlgS(d, 20), hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
            HWND edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | WS_TABSTOP,
                DlgS(d, 12), DlgS(d, 36), DlgS(d, 380), DlgS(d, 24), hwnd, (HMENU)1001, GetModuleHandleW(nullptr), nullptr);
            SetWindowTextW(edit, d->text.c_str());
            SendMessageW(edit, EM_SETSEL, (WPARAM)d->text.size(), (LPARAM)d->text.size());
            HWND okBtn = CreateWindowExW(0, L"BUTTON", L"OK", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON | WS_TABSTOP,
                DlgS(d, 232), DlgS(d, 70), DlgS(d, 76), DlgS(d, 28), hwnd, (HMENU)IDOK, GetModuleHandleW(nullptr), nullptr);
            HWND cancelBtn = CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
                DlgS(d, 316), DlgS(d, 70), DlgS(d, 76), DlgS(d, 28), hwnd, (HMENU)IDCANCEL, GetModuleHandleW(nullptr), nullptr);
            SendMessageW(label, WM_SETFONT, (WPARAM)font, TRUE);
            SendMessageW(edit, WM_SETFONT, (WPARAM)font, TRUE);
            SendMessageW(okBtn, WM_SETFONT, (WPARAM)font, TRUE);
            SendMessageW(cancelBtn, WM_SETFONT, (WPARAM)font, TRUE);
            // WM_CREATE runs before the dialog is visible/active; setting focus here is a
            // no-op. Real focus is set right after ShowWindow in PromptForText.
            return 0;
        }
        case WM_COMMAND: {
            if (HIWORD(wParam) != BN_CLICKED) break;   // ignore any future non-click notifications (focus/etc.)
            WORD id = LOWORD(wParam);
            if (id == IDOK) {
                HWND edit = GetDlgItem(hwnd, 1001);
                d->text = GetCtrlText(edit);
                d->ok = true;
                EnableWindow(d->owner, TRUE);
                DestroyWindow(hwnd);
                return 0;
            }
            if (id == IDCANCEL) { d->ok = false; EnableWindow(d->owner, TRUE); DestroyWindow(hwnd); return 0; }
            break;
        }
        case WM_CLOSE: d->ok = false; EnableWindow(d->owner, TRUE); DestroyWindow(hwnd); return 0;
        case DM_GETDEFID: {
            // IsDialogMessageW asks the dialog which push button Enter should activate. If the
            // keyboard focus sits on a button, Enter activates that button (standard dialog
            // behavior); otherwise the tracked default (initially OK) is used. The plain
            // DefWindowProcW fallback returns 0 here, which silently broke Enter handling.
            HWND focus = GetFocus();
            if (focus && IsChild(hwnd, focus)) {
                int fid = GetDlgCtrlID(focus);
                wchar_t cls[16] = {};
                GetClassNameW(focus, cls, _countof(cls));
                if (fid != 0 && CompareStringW(LOCALE_INVARIANT, NORM_IGNORECASE, cls, -1, L"BUTTON", -1) == CSTR_EQUAL) {
                    return MAKELONG((WORD)fid, DC_HASDEFID);
                }
            }
            return MAKELONG((WORD)d->defaultId, DC_HASDEFID);
        }
        case DM_SETDEFID: {
            // The dialog manager may re-assign the default button; move the BS_DEFPUSHBUTTON
            // style so the visual border and Enter both follow.
            int newId = (int)wParam;
            if (newId == IDOK || newId == IDCANCEL) {
                if (newId != d->defaultId) {
                    HWND oldBtn = GetDlgItem(hwnd, d->defaultId);
                    HWND newBtn = GetDlgItem(hwnd, newId);
                    if (oldBtn) SendMessageW(oldBtn, BM_SETSTYLE, MAKEWPARAM(BS_PUSHBUTTON, TRUE), TRUE);
                    if (newBtn) SendMessageW(newBtn, BM_SETSTYLE, MAKEWPARAM(BS_DEFPUSHBUTTON, TRUE), TRUE);
                    d->defaultId = newId;
                }
            }
            return TRUE;
        }
        case WM_DESTROY:
            // WM_DESTROY runs before child windows are torn down — children still reference
            // d->font, so do not delete it here. WM_NCDESTROY runs after all children are gone.
            if (d) d->done = true;
            return 0;
        case WM_NCDESTROY:
            if (d) {
                if (d->font && d->font != (HFONT)GetStockObject(DEFAULT_GUI_FONT)) DeleteObject(d->font);
                d->font = nullptr;
            }
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

std::wstring PromptForText(HWND owner, const std::wstring& title, const std::wstring& prompt, const std::wstring& initial) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = InputDialogProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"WinKickInputDialog";
    ATOM atom = RegisterClassExW(&wc);
    if (!atom && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return L"";

    InputDialogData d;
    d.prompt = prompt;
    d.text = initial;
    d.dpi = env::DpiForWindowSafe(owner);
    d.owner = owner;

    // Compute outer bounds from the desired CLIENT size (buttons live at client
    // [12..118]); AdjustWindowRectEx(ForDpi) accounts for caption/frame metrics.
    RECT crect{};
    crect.right = DlgS(&d, 404);
    crect.bottom = DlgS(&d, 118);
    DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    DWORD exStyle = WS_EX_DLGMODALFRAME;
    typedef BOOL(WINAPI* AdjustFn)(LPRECT, DWORD, BOOL, DWORD, UINT);
    static AdjustFn adj = (AdjustFn)GetProcAddress(GetModuleHandleW(L"user32.dll"), "AdjustWindowRectExForDpi");
    if (adj) {
        adj(&crect, style, FALSE, exStyle, d.dpi);
    } else {
        AdjustWindowRectEx(&crect, style, FALSE, exStyle);
    }
    int w = crect.right - crect.left;
    int h = crect.bottom - crect.top;

    RECT rc{};
    GetWindowRect(owner, &rc);
    int x = rc.left + ((rc.right - rc.left) - w) / 2;
    int y = rc.top + ((rc.bottom - rc.top) - h) / 2;
    // Keep the popup fully inside the owner's monitor work area (it could otherwise straddle an edge)
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (HMONITOR mon = MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST); mon != nullptr && GetMonitorInfoW(mon, &mi)) {
        int maxX = (int)mi.rcWork.right - w, maxY = (int)mi.rcWork.bottom - h;
        x = std::max((int)mi.rcWork.left, std::min(x, maxX));
        y = std::max((int)mi.rcWork.top, std::min(y, maxY));
    }

    HWND dlg = CreateWindowExW(exStyle, wc.lpszClassName, title.c_str(),
        style, x, y, w, h, owner, nullptr, GetModuleHandleW(nullptr), &d);
    if (!dlg) return L"";

    EnableWindow(owner, FALSE);
    ShowWindow(dlg, SW_SHOW);
    UpdateWindow(dlg);
    // Activate the dialog and put keyboard focus in the edit box
    SetForegroundWindow(dlg);
    SetFocus(GetDlgItem(dlg, 1001));

    bool sawQuit = false;
    MSG msg{};
    while (!d.done) {
        BOOL got = GetMessageW(&msg, nullptr, 0, 0);
        if (got <= 0) { if (got == 0) sawQuit = true; break; }
        if (!IsDialogMessageW(dlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    if (sawQuit) PostQuitMessage((int)msg.wParam); // preserve the quit request's exit code for the main loop
    // WM_QUIT / GetMessage error path may reach here with dlg still alive; never leave it
    // dangling on the stack-allocated d
    if (IsWindow(dlg)) DestroyWindow(dlg);

    // Enable on owner in case the dialog self-enabled it during destroy, then re-assert
    EnableWindow(owner, TRUE);
    SetForegroundWindow(owner);
    return d.ok ? d.text : L"";
}

} // namespace

// ============================================================================
// Main window layout (96-DPI baseline; everything scales with S(dpi, px))
// ============================================================================
namespace {

constexpr int PAD = 10;
constexpr int ROW_H = 30;
constexpr int INSPECTOR_DESC_H = 34;
constexpr int ARG_ROW_H = 24;
constexpr int INSPECTOR_H_BASE = INSPECTOR_DESC_H + 8;  // desc block + padding
constexpr int FOOTER_H = 40;
constexpr int LOG_H = 120;

int InspectorHeight(GuiState* s) {
    int rows = s->inspectArgCount;   // arg rows currently visible (0 = description only)
    if (rows < 0) rows = 0;
    if (rows > kArgSlots) rows = kArgSlots;
    return S(s->dpi, INSPECTOR_H_BASE + rows * ARG_ROW_H);
}

void LayoutControls(GuiState* s, int clientW, int clientH) {
    int dpi = s->dpi;
    auto S_ = [&](int px) { return S(dpi, px); };
    int pad = S_(PAD);
    int y = pad;

    // Row 1: source management
    HWND srcLabel = s->sourceLabelCtl;
    HWND srcCombo = s->sourceCombo;
    HWND loadBtn = s->loadBtn, exportBtn = s->exportBtn;

    int rowH = S_(ROW_H);
    int labelH = S_(22);
    SetWindowPos(srcLabel, nullptr, pad, y + S_(3), S_(48), labelH, SWP_NOZORDER);
    // comboW chosen so the Export... button's right edge lands exactly at clientW - pad (aligned with listView/log)
    int comboW = std::max(S_(200), clientW - pad * 2 - S_(48) - S_(8) - S_(96) - S_(8) - S_(84));
    SetWindowPos(srcCombo, nullptr, pad + S_(48), y, comboW, S_(400), SWP_NOZORDER);
    int x = pad + S_(48) + comboW + S_(8);
    SetWindowPos(loadBtn, nullptr, x, y, S_(96), rowH, SWP_NOZORDER);
    SetWindowPos(exportBtn, nullptr, x + S_(96) + S_(8), y, S_(84), rowH, SWP_NOZORDER);

    // Row 2: tag filtering
    y += rowH + S_(6);
    SetWindowPos(s->tagLabelCtl, nullptr, pad, y + S_(3), S_(30), labelH, SWP_NOZORDER);
    int tagComboW = S_(240);
    SetWindowPos(s->tagCombo, nullptr, pad + S_(30), y, tagComboW, S_(300), SWP_NOZORDER);
    SetWindowPos(s->selectBtn, nullptr, pad + S_(30) + tagComboW + S_(8), y, S_(76), rowH, SWP_NOZORDER);
    SetWindowPos(s->deselectBtn, nullptr, pad + S_(30) + tagComboW + S_(8) + S_(76) + S_(8), y, S_(84), rowH, SWP_NOZORDER);

    // Bottom block (anchored): log -> footer -> inspector -> listview
    int logH = S_(LOG_H);
    int footerH = S_(FOOTER_H);
    int inspectorH = InspectorHeight(s);
    int descH = S_(INSPECTOR_DESC_H);
    int argRowH = S_(ARG_ROW_H);

    int listY = y + rowH + S_(8);
    int minListH = S_(60);
    // If the ideal layout does not fit vertically, shrink the log first, then the inspector
    // itself (arg rows may clip in pathologically short viewports). The list may shrink to
    // zero but the inspector must never ride up onto the tag row above the list.
    int availForLog = clientH - pad - footerH - inspectorH - S_(6) - minListH - listY;
    if (availForLog < S_(30)) availForLog = S_(30);     // keep at least a small log
    if (logH > availForLog) logH = availForLog;

    int inspectorY = clientH - logH - pad - footerH - inspectorH;
    // Still too tall? Reduce the inspector height to whatever fits after the list's minimum
    // and re-anchor it at listY+S_(6) so it never overlaps the tag row above the list.
    if (inspectorY < listY + S_(6)) {
        inspectorY = listY + S_(6);
        int maxInspectorH = clientH - logH - pad - footerH - inspectorY;
        if (inspectorH > maxInspectorH) inspectorH = std::max(0, maxInspectorH);
    }
    int logY = clientH - logH - pad;
    int footerY = logY - footerH;
    // Never negative (SetWindowPos with a negative height is invalid); 0 means
    // "no room", inspector/footer/log stay non-overlapping as anchored above.
    int listH = inspectorY - S_(6) - listY;
    if (listH < 0) listH = 0;

    SetWindowPos(s->listView, nullptr, pad, listY, clientW - 2 * pad, listH, SWP_NOZORDER);

    // Inspector (desc + only the arg rows currently in use)
    int descSetH = std::min(descH, std::max(0, inspectorH - S_(4)));
    SetWindowPos(s->descStatic, nullptr, pad, inspectorY, clientW - 2 * pad, descSetH, SWP_NOZORDER);
    int labelWArg = S_(110);
    int editH = argRowH - S_(3);
    int editW = S_(280);
    // Hide any arg row whose bottom would paint over the footer row above the log
    int inUse = s->inspectArgCount;
    for (int k = 0; k < kArgSlots; ++k) {
        int ry = inspectorY + descH + k * argRowH;
        SetWindowPos(s->argLabels[k], nullptr, pad, ry + S_(3), labelWArg, argRowH - S_(4), SWP_NOZORDER);
        SetWindowPos(s->argEdits[k], nullptr, pad + labelWArg + S_(4), ry, editW, editH, SWP_NOZORDER);
        if (k < inUse) {
            BOOL show = (ry + editH <= footerY - S_(4)) ? TRUE : FALSE;
            ShowWindow(s->argLabels[k], show);
            ShowWindow(s->argEdits[k], show);
        }
    }

    // Footer
    SetWindowPos(s->dryRunChk, nullptr, pad, footerY + S_(6), S_(92), S_(24), SWP_NOZORDER);
    int applyW = S_(150), applyH = S_(32);
    // Save Log label may embed a long --log basename; fill the space left of Apply
    int saveW = (clientW - pad - applyW - S_(8)) - (pad + S_(100));
    if (saveW < S_(100)) saveW = S_(100);
    SetWindowPos(s->saveLogChk, nullptr, pad + S_(100), footerY + S_(6), saveW, S_(24), SWP_NOZORDER);
    SetWindowPos(s->applyBtn, nullptr, clientW - pad - applyW, footerY + S_(2), applyW, applyH, SWP_NOZORDER);

    // Log drawer
    SetWindowPos(s->logEdit, nullptr, pad, logY, clientW - 2 * pad, logH, SWP_NOZORDER);
}

void EnableInteractiveControls(GuiState* s, bool enable) {
    HWND controls[] = {
        s->sourceCombo, s->tagCombo, s->loadBtn, s->exportBtn, s->selectBtn, s->deselectBtn,
        s->applyBtn, s->dryRunChk, s->saveLogChk, s->listView,
        s->argEdits[0], s->argEdits[1], s->argEdits[2], s->argEdits[3], s->argEdits[4], s->argEdits[5],
    };
    for (HWND h : controls) if (h) EnableWindow(h, enable ? TRUE : FALSE);
}

} // namespace

static bool IsHighContrastOn() {
    // CDDS_ITEMPREPAINT is not hot; query directly so a live Alt+Shift+PrintScreen toggle takes effect
    HIGHCONTRASTW contrast{};
    contrast.cbSize = sizeof(contrast);
    if (!SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0)) return false;
    return (contrast.dwFlags & HCF_HIGHCONTRASTON) != 0;
}

static LRESULT HandleNotify(GuiState* s, LPNMHDR nm) {
    if (nm->idFrom != IDC_LISTVIEW) return 0;

    if (nm->code == NM_CUSTOMDRAW) {
        LPNMLVCUSTOMDRAW cd = (LPNMLVCUSTOMDRAW)nm;
        switch (cd->nmcd.dwDrawStage) {
            case CDDS_PREPAINT:
                return CDRF_NOTIFYITEMDRAW;
            case CDDS_ITEMPREPAINT: {
                // Skip groups/headers: only actual item rows get item colors
                if (cd->dwItemType != LVCDI_ITEM) return CDRF_DODEFAULT;
                // Selected rows: let the theme own the contrast (SYSTEM COLOR_HIGHLIGHTTEXT)
                if ((cd->nmcd.uItemState & CDIS_SELECTED) != 0) return CDRF_DODEFAULT;
                // High Contrast themes: let the theme color everything
                if (IsHighContrastOn()) return CDRF_DODEFAULT;
                int idx = (int)cd->nmcd.lItemlParam;
                bool colored = false;
                COLORREF color = RGB(0, 0, 0);
                if (idx >= 0 && idx < (int)s->config.items.size()) {
                    const TaskItem& it = s->config.items[idx];
                    if (ItemLocked(s, it)) { color = GetSysColor(COLOR_GRAYTEXT); colored = true; }
                    else switch (it.state) {
                        case ItemRunState::Running: color = RGB(0, 102, 204);   colored = true; break;
                        case ItemRunState::Ok:      color = RGB(0, 128, 0);     colored = true; break;
                        case ItemRunState::Failed:  color = RGB(200, 0, 0);     colored = true; break;
                        case ItemRunState::Skipped: color = RGB(110, 110, 110); colored = true; break;
                        case ItemRunState::DryRun:  color = RGB(110, 110, 110); colored = true; break;
                        default: break;
                    }
                }
                if (colored) { cd->clrText = color; return CDRF_NEWFONT; }
                return CDRF_DODEFAULT;
            }
        }
        return CDRF_DODEFAULT;
    }

    if (nm->code == LVN_ITEMCHANGING) {
        NMLISTVIEW* p = (NMLISTVIEW*)nm;
        if (s->suppressListEvents) return FALSE; // during rebuild we set final state deliberately
        if (p->iItem < 0) return FALSE; // bulk notifications carry iItem == -1
        if ((p->uChanged & LVIF_STATE) != 0) {
            UINT oldImg = (p->uOldState & LVIS_STATEIMAGEMASK) >> 12;
            UINT newImg = (p->uNewState & LVIS_STATEIMAGEMASK) >> 12;
            if (oldImg != newImg) {
                int idx = (int)p->lParam;
                if (idx >= 0 && idx < (int)s->config.items.size()) {
                    if (ItemLocked(s, s->config.items[idx])) return TRUE; // guard-disabled: block toggle
                }
            }
        }
        return FALSE;
    }

    if (nm->code == LVN_ITEMCHANGED) {
        NMLISTVIEW* p = (NMLISTVIEW*)nm;
        if (s->suppressListEvents) return 0; // during rebuild; final state already set by us
        // Do NOT touch the listview or other controls from inside the notification:
        // post our own message and return immediately (comctl stays in a clean state).
        UINT oldImg = (p->uOldState & LVIS_STATEIMAGEMASK) >> 12;
        UINT newImg = (p->uNewState & LVIS_STATEIMAGEMASK) >> 12;
        int idx = (int)p->lParam;
        if (p->iItem >= 0 && oldImg != newImg && idx >= 0 && idx < (int)s->config.items.size()) {
            // Mirror synchronously so Apply/Export never read a stale checked flag
            s->config.items[idx].checked = (newImg == 2);
            // A user check: clear group peers in the model immediately (before any Apply/Export).
            // No ListView calls from inside the notification; EnforceGroupExclusion (posted
            // below) syncs the checkbox visuals/model for the peers.
            if (newImg == 2 && !s->enforcingGroup && !s->inBulkOp && !s->config.items[idx].groupName.empty()) {
                const std::string& g = s->config.items[idx].groupName;
                for (auto& it : s->config.items) {
                    if (it.index != idx && !it.groupName.empty() && _stricmp(it.groupName.c_str(), g.c_str()) == 0) {
                        it.checked = false;
                    }
                }
            }
            // Skip posting entirely during bulk/enforcement (those paths manage UI themselves)
            if (!s->enforcingGroup && !s->inBulkOp) {
                WPARAM w = (WPARAM)(((uint64_t)s->configGen << 32) | (uint32_t)idx);
                PostMessageW(s->hwnd, WM_APP_LV_CHANGED, w, (LPARAM)(newImg == 2 ? 1 : 0));
            }
        }
        // Selection may change in bulk (iItem == -1); UpdateInspector re-queries the selection
        if (((p->uNewState ^ p->uOldState) & LVIS_SELECTED) != 0) {
            PostMessageW(s->hwnd, WM_APP_LV_SELECT, 0, 0);
        }
        return 0;
    }
    return 0;
}

// Append one line (\r\n) to a UTF-8 log file if the last run had Save Log enabled AND RunAll
// actually opened the log (otherwise we'd create an orphaned single-line file)
static void AppendLogFileLine(const GuiState* s, const std::string& line, bool logFileOpened) {
    if (!s->lastRunSaveLog || s->lastRunLogPath.empty() || !logFileOpened) return;
    HANDLE h = CreateFileW(Widen(s->lastRunLogPath).c_str(), FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD w = 0;
    std::string l = line + "\r\n";
    WriteFile(h, l.data(), (DWORD)l.size(), &w, nullptr);
    CloseHandle(h);
}

// Restore keyboard focus to a tabstop when a busy operation finished and focus was lost.
// (Disabling the focused control clears keyboard focus; afterwards GetNextDlgTabItem(hwnd,
// GetFocus() /* NULL */, TRUE) returns NULL and Shift+Tab appears dead.)
void RestoreFocusAfterBusy(GuiState* s) {
    if (s->closeWhenDone) return;
    HWND f = GetFocus();
    // GetFocus() == s->hwnd happens after a modal MessageBoxW dismissed (WA_ACTIVE sends focus to the
    // bare frame); that is no more useful than NULL for Tab/Shift+Tab, so refocus a tab stop then too.
    if ((!f || f == s->hwnd) && GetForegroundWindow() == s->hwnd) {
        HWND target = s->applyBtn ? s->applyBtn : s->listView;
        if (target) SetFocus(target);
    }
}

static void FinishRun(GuiState* s, const runner::Stats& stats) {
    // s->running stays true until every modal MessageBoxW below has returned: an external WM_CLOSE
    // posted during the nested modal loop must still route through the deferral path (closeWhenDone),
    // not DestroyWindow(hwnd) the window out from under us mid-handler. Controls stay disabled too.
    char buf[256];
    if (s->lastRunDryRun)
        snprintf(buf, sizeof(buf), "Dry-run finished: %d simulated, %d skipped, %d failed.",
                 stats.dryRun, stats.skipped, stats.failed);
    else
        snprintf(buf, sizeof(buf), "Finished: %d succeeded, %d failed, %d skipped (%llu ms).",
                 stats.succeeded, stats.failed, stats.skipped, stats.totalMs);
    AppendLogUtf8(s, std::string(buf));
    AppendLogFileLine(s, buf, stats.logFileOpened);

    if (!s->closeWhenDone && stats.failed > 0) {
        MessageBoxW(s->hwnd, Widen(buf).c_str(), L"WinKick", MB_OK | MB_ICONWARNING);
    } else if (!s->closeWhenDone && !s->lastRunDryRun) {
        MessageBoxW(s->hwnd, Widen(buf).c_str(), L"WinKick", MB_OK | MB_ICONINFORMATION);
    }

    // End-of-run reboot handling (GUI mode prompts)
    if (!s->closeWhenDone && !s->lastRunDryRun && stats.anyReboot && !s->opts.noReboot) {
        int answer = MessageBoxW(s->hwnd, L"Changes applied. Reboot now?",
                                 L"WinKick", MB_YESNO | MB_ICONQUESTION);
        if (answer == IDYES) {
            std::string msg;
            bool ok = runner::AttemptReboot(msg);
            AppendLogUtf8(s, msg);
            AppendLogFileLine(s, msg, stats.logFileOpened);
            if (!ok) MessageBoxW(s->hwnd, L"Reboot failed. Please restart manually.", L"WinKick", MB_OK | MB_ICONERROR);
        } else {
            AppendLogUtf8(s, "Reboot declined by user.");
            AppendLogFileLine(s, "Reboot declined by user.", stats.logFileOpened);
        }
    } else if (s->closeWhenDone && !s->lastRunDryRun && stats.anyReboot && !s->opts.noReboot) {
        AppendLogUtf8(s, "Reboot required for some applied changes; window closing without a reboot.");
        AppendLogFileLine(s, "Reboot required for some applied changes; window closing without a reboot.", stats.logFileOpened);
    }

    // Only now release the busy state (all nested modal loops are done)
    s->running = false;
    EnableInteractiveControls(s, true);
    UpdateApplyShield(s);
}


// ============================================================================
// Main window procedure
// ============================================================================
static HWND CreateControl(GuiState* s, const wchar_t* cls, const wchar_t* text, DWORD style,
                          DWORD exStyle, HWND parent, int id) {
    HWND h = CreateWindowExW(exStyle, cls, text, WS_CHILD | WS_VISIBLE | style,
                             0, 0, 100, 24, parent, (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
    ApplyFontTo(s, h);
    return h;
}

static void CreateAllControls(GuiState* s) {
    HWND hwnd = s->hwnd;

    s->sourceLabelCtl = CreateControl(s, L"STATIC", L"Source:", 0, 0, hwnd, IDC_SOURCE_LABEL);
    // Tab-focusable controls (Shift+Tab walks backwards); tagged for the manual
    // VK_TAB focus walk in RunGui's message loop (GetNextDlgTabItem).
    const DWORD TABSTOP = WS_TABSTOP;
    s->sourceCombo = CreateControl(s, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | TABSTOP, 0, hwnd, IDC_SOURCE_COMBO);
    s->loadBtn = CreateControl(s, L"BUTTON", L"+ Load...", BS_PUSHBUTTON | TABSTOP, 0, hwnd, IDC_LOAD_BTN);
    s->exportBtn = CreateControl(s, L"BUTTON", L"Export...", BS_PUSHBUTTON | TABSTOP, 0, hwnd, IDC_EXPORT_BTN);

    s->tagLabelCtl = CreateControl(s, L"STATIC", L"Tag:", 0, 0, hwnd, IDC_TAG_LABEL);
    s->tagCombo = CreateControl(s, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | TABSTOP, 0, hwnd, IDC_TAG_COMBO);
    s->selectBtn = CreateControl(s, L"BUTTON", L"Select", BS_PUSHBUTTON | TABSTOP, 0, hwnd, IDC_SELECT_BTN);
    s->deselectBtn = CreateControl(s, L"BUTTON", L"Deselect", BS_PUSHBUTTON | TABSTOP, 0, hwnd, IDC_DESELECT_BTN);

    s->descStatic = CreateControl(s, L"STATIC", L"Select a task above to see its description and arguments.", SS_NOPREFIX, 0, hwnd, IDC_DESC_STATIC);
    for (int k = 0; k < kArgSlots; ++k) {
        s->argLabels[k] = CreateControl(s, L"STATIC", L"", SS_NOPREFIX, 0, hwnd, IDC_ARG_LABEL_0 + k);
        s->argEdits[k] = CreateControl(s, L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE, hwnd, IDC_ARG_EDIT_0 + k);
        ShowWindow(s->argLabels[k], SW_HIDE);
        ShowWindow(s->argEdits[k], SW_HIDE);
    }

    s->dryRunChk = CreateControl(s, L"BUTTON", L"Dry Run", BS_AUTOCHECKBOX | TABSTOP, 0, hwnd, IDC_DRYRUN_CHK);
    std::wstring saveLogLabel = L"Save Log (log.txt)";
    if (s->opts.logEnabled) {
        const std::wstring& p = s->opts.logPath;
        size_t slash = p.find_last_of(L"\\/");
        std::wstring name = (slash == std::wstring::npos) ? p : p.substr(slash + 1);
        if (!name.empty()) {
            // Escape '&' so the button label does not render an accelerator mnemonic
            std::wstring escName;
            for (wchar_t ch : name) { escName += ch; if (ch == L'&') escName += L'&'; }
            saveLogLabel = L"Save Log (" + escName + L")";
        }
    }
    s->saveLogChk = CreateControl(s, L"BUTTON", saveLogLabel.c_str(), BS_AUTOCHECKBOX | TABSTOP, 0, hwnd, IDC_SAVELOG_CHK);
    if (s->opts.logEnabled) SendMessageW(s->saveLogChk, BM_SETCHECK, BST_CHECKED, 0);
    s->applyBtn = CreateControl(s, L"BUTTON", L"Apply Selected", BS_PUSHBUTTON | TABSTOP, 0, hwnd, IDC_APPLY_BTN);

    s->logEdit = CreateControl(s, L"EDIT", L"",
        ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL | WS_BORDER | WS_TABSTOP, 0, hwnd, IDC_LOG_EDIT);
    // EDIT controls default to a 30,000-char limit; allow headroom above the trim cap
    SendMessageW(s->logEdit, EM_SETLIMITTEXT, (WPARAM)(kMaxLogChars + 65536), 0);
    if (s->monoFont) SendMessageW(s->logEdit, WM_SETFONT, (WPARAM)s->monoFont, TRUE);

    // Seed source combo
    RebuildSourceComboImpl(s, -1);
    RebuildTagCombo(s);
    RebuildListView(s);
    UpdateInspector(s);
    UpdateApplyShield(s);
}


static BOOL CALLBACK FontApplyProc(HWND h, LPARAM lp) {
    GuiState* s = (GuiState*)lp;
    if (GetDlgCtrlID(h) == IDC_LOG_EDIT) {
        if (s->monoFont) SendMessageW(h, WM_SETFONT, (WPARAM)s->monoFont, TRUE);
    } else if (s->uiFont) {
        SendMessageW(h, WM_SETFONT, (WPARAM)s->uiFont, TRUE);
    }
    return TRUE;
}

// Create replacement fonts for the given DPI WITHOUT touching the current ones
static void BuildFonts(GuiState* s, HFONT& outUi, HFONT& outMono) {
    outUi = nullptr;
    outMono = nullptr;
    LOGFONTW uiLf{};
    if (env::GetMessageFontForDpi(&uiLf, s->dpi)) {
        outUi = CreateFontIndirectW(&uiLf);
    }
    if (!outUi) outUi = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    LOGFONTW lf{};
    lf.lfHeight = -MulDiv(11, s->dpi, 96);
    lf.lfWeight = FW_NORMAL;
    lf.lfCharSet = DEFAULT_CHARSET;
    wcscpy_s(lf.lfFaceName, L"Consolas");
    outMono = CreateFontIndirectW(&lf);
}

// Build new fonts, apply them to all children, and only then delete the old ones.
// (Children hold HFONTs; deleting under them before WM_SETFONT arrives is unsafe.)
static void ReplaceFonts(GuiState* s) {
    HFONT newUi = nullptr, newMono = nullptr;
    BuildFonts(s, newUi, newMono);
    HFONT oldUi = s->uiFont, oldMono = s->monoFont;
    s->uiFont = newUi;
    s->monoFont = newMono;
    if (s->hwnd) EnumChildWindows(s->hwnd, FontApplyProc, (LPARAM)s);
    if (oldUi && oldUi != (HFONT)GetStockObject(DEFAULT_GUI_FONT)) DeleteObject(oldUi);
    if (oldMono) DeleteObject(oldMono);
}

LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    GuiState* s = (GuiState*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    switch (msg) {
        case WM_CREATE: {
            CREATESTRUCTW* cs = (CREATESTRUCTW*)lParam;
            s = (GuiState*)cs->lpCreateParams;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)s);
            s->hwnd = hwnd;
            s->dpi = env::DpiForWindowSafe(hwnd);
            s->logPump = std::make_shared<LogPump>();
            s->logPump->h = hwnd;
            ReplaceFonts(s);

            CreateAllControls(s);

            // Initial log line
            {
                std::string net1 = std::string("Network: ") + (s->isOnline ? "Online" : "Offline");
                char buf[256];
                snprintf(buf, sizeof(buf), "WinKick " WINKICK_VERSION_STRING " - %zu items loaded | %s",
                         s->config.items.size(), net1.c_str());
                AppendLogUtf8(s, buf);
                AppendLogUtf8(s, "Source: " + Narrow(s->sourceLabel));
            }

            if (s->opts.autoRun) PostMessageW(hwnd, WM_APP_AUTORUN, 0, 0);
            return 0;
        }

        case WM_SIZE: {
            if (!s) break;
            if (wParam == SIZE_MINIMIZED) return 0;  // client rect is 0x0; anchoring controls to it is pointless
            RECT rc{};
            GetClientRect(hwnd, &rc);
            LayoutControls(s, rc.right - rc.left, rc.bottom - rc.top);
            return 0;
        }

        case WM_DPICHANGED: {
            if (!s) break;
            s->dpi = HIWORD(wParam);
            ReplaceFonts(s);   // builds, applies to all children, then deletes old fonts
            if (s->listView) { // rescale columns to the new DPI
                ListView_SetColumnWidth(s->listView, 0, S(s->dpi, 400));
                ListView_SetColumnWidth(s->listView, 1, S(s->dpi, 210));
                ListView_SetColumnWidth(s->listView, 2, S(s->dpi, 160));
            }
            RECT* suggested = (RECT*)lParam;
            SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                         suggested->right - suggested->left,
                         suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            RECT rc{};
            GetClientRect(hwnd, &rc);
            LayoutControls(s, rc.right - rc.left, rc.bottom - rc.top);
            return 0;
        }

        case WM_GETMINMAXINFO: {
            MINMAXINFO* mmi = (MINMAXINFO*)lParam;
            int dpi = s ? s->dpi : (int)env::DpiForSystemSafe();
            mmi->ptMinTrackSize.x = MulDiv(940, dpi, 96);
            mmi->ptMinTrackSize.y = MulDiv(640, dpi, 96);
            // Clamp minimum to the work area of the monitor this window is on
            HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi{};
            mi.cbSize = sizeof(mi);
            if (mon && GetMonitorInfoW(mon, &mi)) {
                int workW = mi.rcWork.right - mi.rcWork.left;
                int workH = mi.rcWork.bottom - mi.rcWork.top;
                if (mmi->ptMinTrackSize.x > workW) mmi->ptMinTrackSize.x = workW;
                if (mmi->ptMinTrackSize.y > workH) mmi->ptMinTrackSize.y = workH;
            }
            return 0;
        }

        case WM_COMMAND: {
            if (!s) break;
            WORD id = LOWORD(wParam);
            WORD code = HIWORD(wParam);
            HWND ctl = (HWND)lParam;
            // Busy guard: controls are disabled (so mouse/keyboard can't reach them), but synthetic
            // WM_COMMAND from automation or a queued message can still arrive — never let it mutate
            // state or open UI mid-operation.
            if (s->running || s->elevating || s->fetchInFlight) {
                switch (id) {
                    case IDC_APPLY_BTN: case IDC_EXPORT_BTN: case IDC_LOAD_BTN:
                    case IDC_SELECT_BTN: case IDC_DESELECT_BTN: case IDC_DRYRUN_CHK: case IDC_SAVELOG_CHK:
                    case IDC_SOURCE_COMBO: case IDC_TAG_COMBO:
                    case MENU_LOAD_FROM_FILE: case MENU_LOAD_FROM_URL:
                    case IDC_CTX_SELECT_CAT: case IDC_CTX_DESELECT_CAT:
                        return 0;
                    default: break;
                }
            }

            // Arg edit boxes: live update (EN_CHANGE)
            if (code == EN_CHANGE && s->argEdits[0]) {
                if (s->running || s->elevating || s->fetchInFlight) return 0;
                int k = -1;
                for (int j = 0; j < kArgSlots; ++j) if (ctl == s->argEdits[j]) { k = j; break; }
                if (k >= 0 && !s->suppressArgEvents && s->inspectIndex >= 0 &&
                    s->inspectIndex < (int)s->config.items.size()) {
                    if (k < (int)s->config.items[s->inspectIndex].args.size()) {
                        s->config.items[s->inspectIndex].args[k].currentValue = Narrow(GetCtrlText(ctl));
                        MarkModified(s);
                    }
                }
                return 0;
            }

            if (code != BN_CLICKED && code != CBN_SELCHANGE && code != 0) break;

            switch (id) {
                case IDC_APPLY_BTN:    HandleApply(s); return 0;
                case IDC_EXPORT_BTN:   HandleExportButton(s); return 0;
                case IDC_LOAD_BTN:     ShowLoadMenu(s); return 0;
                case IDC_SELECT_BTN: {
                    std::wstring tag = ComboSelectionWide(s->tagCombo);
                    if (!tag.empty() && ApplyBulkToMatching(s, tag) > 0) MarkModified(s);
                    return 0;
                }
                case IDC_DESELECT_BTN: {
                    std::wstring tag = ComboSelectionWide(s->tagCombo);
                    if (!tag.empty() && DeselectMatching(s, tag) > 0) MarkModified(s);
                    return 0;
                }
                case IDC_DRYRUN_CHK:   UpdateApplyShield(s); return 0;
                case IDC_SAVELOG_CHK:  return 0;
                case MENU_LOAD_FROM_FILE: {
                    std::vector<wchar_t> fileName(32768, L'\0');
                    OPENFILENAMEW ofn{};
                    ofn.lStructSize = sizeof(ofn);
                    ofn.hwndOwner = hwnd;
                    ofn.lpstrFilter = L"INI configuration (*.ini)\0*.ini\0All files (*.*)\0*.*\0\0";
                    ofn.lpstrFile = fileName.data();
                    ofn.nMaxFile = (DWORD)fileName.size();
                    ofn.lpstrTitle = L"Load WinKick configuration";
                    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_EXPLORER | OFN_NOCHANGEDIR;
                    if (GetOpenFileNameW(&ofn)) {
                        LoadSourceFromFile(s, fileName.data());
                    } else if (CommDlgExtendedError() == FNERR_BUFFERTOOSMALL) {
                        MessageBoxW(hwnd, L"The selected path is longer than the file dialog can return.", L"WinKick - Load", MB_OK | MB_ICONERROR);
                    }
                    return 0;
                }
                case MENU_LOAD_FROM_URL: {
                    std::wstring url = PromptForText(hwnd, L"Load from URL",
                        L"Enter the URL of a WinKick .ini file:", L"https://");
                    // Trim whitespace
                    size_t b = url.find_first_not_of(L" \t\r\n");
                    size_t e = url.find_last_not_of(L" \t\r\n");
                    if (b != std::wstring::npos) LoadSourceFromUrl(s, url.substr(b, e - b + 1));
                    return 0;
                }
                case IDC_CTX_SELECT_CAT:
                    if (!s->pendingContextCategory.empty()) {
                        if (ToggleCategory(s, s->pendingContextCategory, true) > 0) MarkModified(s);
                    }
                    s->pendingContextCategory.clear();
                    return 0;
                case IDC_CTX_DESELECT_CAT:
                    if (!s->pendingContextCategory.empty()) {
                        if (ToggleCategory(s, s->pendingContextCategory, false) > 0) MarkModified(s);
                    }
                    s->pendingContextCategory.clear();
                    return 0;
                case IDC_SOURCE_COMBO: {
                    if (code != CBN_SELCHANGE || s->suppressSourceEvents) return 0;
                    int sel = (int)SendMessageW(s->sourceCombo, CB_GETCURSEL, 0, 0);
                    if (sel < 0) return 0;
                    if (s->modified && sel == (int)s->sources.size()) return 0; // marker re-selected: no-op
                    if (sel < (int)s->sources.size()) ReloadFromSourceEntry(s, s->sources[sel]);
                    return 0;
                }
            }
            return 0;
        }

        case WM_NOTIFY: {
            if (!s) break;
            return HandleNotify(s, (LPNMHDR)lParam);
        }

        case WM_CONTEXTMENU: {
            if (!s) break;
            // Never let a queued/synthetic context menu open during a busy operation
            if (s->running || s->elevating || s->fetchInFlight) return 0;
            if ((HWND)wParam == s->listView) {
                HandleListViewContextMenu(s, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
                return 0;
            }
            break;
        }

        case WM_APP_LOG_WAKE: {
            if (!s || !s->logPump) return 0;
            std::vector<std::string> lines;
            {
                std::lock_guard<std::mutex> lk(s->logPump->m);
                s->logPump->wakeQueued = false;    // reset BEFORE draining so the worker re-arms
                lines.assign(s->logPump->pending.begin(), s->logPump->pending.end());
                s->logPump->pending.clear();
            }
            if (!lines.empty()) AppendLogUtf8Batch(s, lines);
            return 0;
        }

        case WM_APP_LV_CHANGED: {
            if (s) {
                // Drop if a run/fetch/hand-off started since the message was posted
                if (s->running || s->elevating || s->fetchInFlight) return 0;
                uint32_t msgGen = (uint32_t)((uint64_t)wParam >> 32);
                int idx = (int)((uint64_t)wParam & 0xFFFFFFFFULL);
                // Ignore LV_CHANGED messages posted before a config reload
                if ((uint32_t)s->configGen != msgGen) return 0;
                if (idx >= 0 && idx < (int)s->config.items.size()) {
                    // The model is authoritative (LVN_ITEMCHANGED updates it synchronously).
                    // Sync the whole group in the ListView to the model; never resurrect
                    // a peer's checked flag from a stale visual read.
                    const std::string& g = s->config.items[idx].groupName;
                    s->enforcingGroup = true;   // group members' LVN_ITEMCHANGED must not post recursion
                    for (auto& it : s->config.items) {
                        if (!g.empty()) {
                            if (it.groupName.empty() || _stricmp(it.groupName.c_str(), g.c_str()) != 0) continue;
                        } else {
                            if (it.index != idx) continue;
                        }
                        if (GetItemCheckedState(s, it.index) != it.checked) {
                            SetItemCheckedState(s, it.index, it.checked);
                        }
                    }
                    s->enforcingGroup = false;
                    MarkModified(s);   // LV_CHANGED never posts during bulk ops (guarded at the source), so inBulkOp is always false here
                }
            }
            return 0;
        }

        case WM_APP_LV_SELECT: {
            if (s) UpdateInspector(s);
            return 0;
        }

        case WM_APP_URL_LOADED: {
            UrlFetchResult* res = (UrlFetchResult*)lParam;
            if (res) {
                if (s) FinishUrlFetch(s, *res);
                delete res;
            }
            if (!IsWindow(hwnd)) return 0;
            if (s && s->closeWhenDone) { DestroyWindow(hwnd); return 0; }
            if (s) RestoreFocusAfterBusy(s);
            return 0;
        }

        case WM_APP_ITEM_STATUS: {
            int idx = (int)((uint64_t)wParam & 0xFFFFFFFFULL);
            ItemStatusMsg* m = (ItemStatusMsg*)lParam;
            if (m) {
                // Drop status messages posted by an earlier run (or before a reload)
                if (s && m->runGen == s->runGen) {
                    if (idx >= 0 && idx < (int)s->config.items.size()) {
                        s->config.items[idx].state = m->state;
                        s->config.items[idx].lastExitCode = m->exitCode;
                        s->config.items[idx].lastElapsedMs = m->elapsedMs;
                    }
                    UpdateItemStatusInList(s, idx, m->state, m->exitCode);
                }
                delete m;
            }
            return 0;
        }

        case WM_APP_RUN_DONE: {
            // Posted by the worker thread strictly after all WM_APP_ITEM_STATUS posts (FIFO).
            // Guard the generation anyway: a stale RUN_DONE must not finish a newer run.
            runner::Stats* stats = (runner::Stats*)lParam;
            uint64_t msgGen = (uint64_t)wParam;
            if (s && s->logPump) {
                // A wake may have been lost to a failed PostMessageW; drain pending log lines
                // first so the log drawer is complete before the run is marked finished.
                std::vector<std::string> lines;
                {
                    std::lock_guard<std::mutex> lk(s->logPump->m);
                    s->logPump->wakeQueued = false;
                    lines.assign(s->logPump->pending.begin(), s->logPump->pending.end());
                    s->logPump->pending.clear();
                }
                if (!lines.empty()) AppendLogUtf8Batch(s, lines);
            }
            if (stats) {
                if (s && msgGen == s->runGen) FinishRun(s, *stats);
                delete stats;
            }
            if (!IsWindow(hwnd)) return 0;
            if (s && s->closeWhenDone) { DestroyWindow(hwnd); return 0; }
            if (s) RestoreFocusAfterBusy(s);
            return 0;
        }

        case WM_APP_AUTORUN:
            if (s && !s->running && !s->elevating && !s->fetchInFlight) HandleApply(s);
            return 0;

        case WM_CLOSE:
            if (s && (s->running || s->fetchInFlight || s->elevating)) {
                // Never open a modal here: the worker (or the ShellExecuteExW message pump on the
                // calling thread during a UAC hand-off) may post WM_APP_RUN_DONE/WM_APP_URL_LOADED
                // while a message box runs a nested modal loop, stacking completion popups on top of
                // it. Instead, remember the request; the operation's completion path destroys the window.
                if (!s->closeWhenDone) {
                    s->closeWhenDone = true;
                    AppendLogUtf8(s, "Close requested; the window will exit when the current operation finishes.");
                }
                return 0;
            }
            DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            // Children still exist here (DestroyWindow dispatches WM_DESTROY to the parent first);
            // do not delete the fonts they reference until WM_NCDESTROY.
            PostQuitMessage(0);
            return 0;

        case WM_NCDESTROY:
            if (s) {
                if (s->uiFont && s->uiFont != (HFONT)GetStockObject(DEFAULT_GUI_FONT)) DeleteObject(s->uiFont);
                if (s->monoFont) DeleteObject(s->monoFont);
                s->uiFont = nullptr;
                s->monoFont = nullptr;
            }
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ============================================================================
namespace gui {

int RunGui(Config config, bool isOnline, const Options& opts) {
    // Shell dialogs and ShellExecuteEx may host STA COM shell extensions
    HRESULT comHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    struct ComGuard { HRESULT hr; ~ComGuard() { if (SUCCEEDED(hr)) CoUninitialize(); } } comGuard{comHr};

    GuiState state;
    state.config = std::move(config);
    state.isOnline = isOnline;
    state.opts = opts;

    // Apply selection filters (if any) for pre-selection; always enforce guards
    ApplySelectionFilters(state.config.items, opts, isOnline);

    // Seed the source combo: Embedded is always available as a clean revert entry
    state.sources.push_back({ L"Built-in (Embedded)", 0, L"" });
    std::string activeLabel = state.config.sourceLabel;
    if (opts.cleanupConfig) {
        // UAC hand-off job file was deleted by design; do not offer it as a revert target.
        // Embedded stays as the clean revert option. Mark modified so the combo shows
        // "* Custom (Modified)" for the elevated snapshot (and Embedded stays selectable).
        state.sourceLabel = L"WinKick job file (one-time, deleted after loading)";
        state.modified = true;
        state.activeSourceIndex = 0;
    } else if (activeLabel.rfind("File: ", 0) == 0) {
        std::wstring path = Widen(activeLabel.substr(6));
        state.sources.push_back({ L"File: " + path, 1, path });
        state.sourceLabel = L"File: " + path;
        state.activeSourceIndex = (int)state.sources.size() - 1;
    } else if (activeLabel.rfind("URL: ", 0) == 0) {
        std::wstring url = Widen(activeLabel.substr(5));
        state.sources.push_back({ L"URL: " + url, 2, url });
        state.sourceLabel = L"URL: " + url;
        state.activeSourceIndex = (int)state.sources.size() - 1;
    } else {
        state.sourceLabel = L"Built-in (Embedded)";
        state.activeSourceIndex = 0;
    }

    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES;
    InitCommonControlsEx(&icc);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = MainWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"WinKickMainWindow";
    UINT iconDpi = env::DpiForSystemSafe();
    int iconSz = env::GetSystemMetricsForDpiSafe(SM_CXICON, iconDpi);
    int iconSmSz = env::GetSystemMetricsForDpiSafe(SM_CXSMICON, iconDpi);
    wc.hIcon = (HICON)LoadImageW(wc.hInstance, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON, iconSz, iconSz, LR_SHARED);
    wc.hIconSm = (HICON)LoadImageW(wc.hInstance, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON, iconSmSz, iconSmSz, LR_SHARED);
    if (!RegisterClassExW(&wc)) return 3;

    // Center within the work area (clamp the initial size so it never starts off-screen)
    UINT dpi = env::DpiForSystemSafe();
    RECT work{};
    // SPI_GETWORKAREA is not DPI-aware for PerMonitorV2 threads (it reports DPI-virtualized work area);
    // prefer the monitor's physical rcWork.
    HMONITOR primaryMonitor = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO pi{};
    pi.cbSize = sizeof(pi);
    if (primaryMonitor && GetMonitorInfoW(primaryMonitor, &pi)) {
        work = pi.rcWork;
    } else {
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);   // fallback (possibly virtualized)
        if (work.right == 0 && work.bottom == 0) { work.right = 1920; work.bottom = 1080; }
    }
    int workW = work.right - work.left, workH = work.bottom - work.top;
    int winW = std::min((int)MulDiv(1060, dpi, 96), workW);
    int winH = std::min((int)MulDiv(720, dpi, 96), workH);
    int x = work.left + (workW - winW) / 2;
    int y = work.top + (workH - winH) / 2;

    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"WinKick",
        WS_OVERLAPPEDWINDOW, x, y, winW, winH, nullptr, nullptr, wc.hInstance, &state);
    if (!hwnd) return 4;
    // If the actual window DPI differs from the system DPI used for the initial bounds
    // (e.g. display scale raised since logon), re-clamp the outer size to the nearest monitor's work area
    UINT windowDpi = env::DpiForWindowSafe(hwnd);
    if (windowDpi != dpi) {
        RECT mWork = work;   // fallback: the primary work area used for the initial bounds
        HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        if (mon && GetMonitorInfoW(mon, &mi)) mWork = mi.rcWork;
        int mWorkW = mWork.right - mWork.left, mWorkH = mWork.bottom - mWork.top;
        int nw = std::min((int)MulDiv(1060, windowDpi, 96), mWorkW);
        int nh = std::min((int)MulDiv(720, windowDpi, 96), mWorkH);
        int nx = mWork.left + (mWorkW - nw) / 2;
        int ny = mWork.top + (mWorkH - nh) / 2;
        SetWindowPos(hwnd, nullptr, nx, ny, nw, nh, SWP_NOZORDER);
    }

    ShowWindow(hwnd, SW_SHOW);
    // Plain-window startup leaves focus on the window itself, not a tabstop: put focus on the ListView
    if (state.listView) SetFocus(state.listView);
    UpdateWindow(hwnd);

    MSG msg{};
    // Tab/Shift+Tab cycle focus across child controls (the main window is a plain
    // window, not a dialog, so IsDialogMessageW is intentionally avoided: we only want
    // Tab redirection, never upstream consumption of arrows/Enter/Space).
    while (true) {
        BOOL got = GetMessageW(&msg, nullptr, 0, 0);
        if (got == -1) return 1;   // message retrieval failed
        if (got == 0) break;       // WM_QUIT
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_TAB && (GetKeyState(VK_CONTROL) & 0x8000) == 0
            && (GetKeyState(VK_MENU) & 0x8000) == 0) {
            bool back = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            HWND cur = GetFocus();
            // Normalize: focus on the bare window or on a non-child (can happen after busy states)
            // must not make GetNextDlgTabItem(NULL, TRUE) == NULL kill Shift+Tab.
            if (cur == hwnd || (cur != nullptr && !IsChild(hwnd, cur))) cur = nullptr;
            HWND next = GetNextDlgTabItem(hwnd, cur, back ? TRUE : FALSE);
            if (!next && back) {
                // Shift+Tab from nowhere/first tabstop: wrap to the LAST tabstop.
                // (GetNextDlgTabItem(NULL, TRUE) returns NULL, and a naive forward walk wraps
                //  around instead of returning NULL — use the first tabstop as the pivot and
                //  go back once, instead of looping forward forever.)
                HWND first = GetNextDlgTabItem(hwnd, nullptr, FALSE);
                if (first) {
                    HWND last = GetNextDlgTabItem(hwnd, first, TRUE);
                    next = last ? last : first;
                }
            }
            if (next) { SetFocus(next); continue; }
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    return (int)msg.wParam;
}

} // namespace gui

