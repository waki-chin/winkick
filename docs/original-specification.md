# Engineering Specification: Modular Windows Setup & Task Utility (`WinSetup`)

> Historical design input, not the current user guide. In particular, current
> releases default to the embedded catalog (remote loading is explicit), build
> into `build/`, and use the release process in [releasing.md](releasing.md).

## 1. Architecture, Build Target & Constraints
* **Language:** Modern C++ (C-style Win32 API usage + STL containers `std::string`, `std::wstring`, `std::vector`, `std::set`, `std::map`, `std::thread`, RAII wrappers).
* **Frameworks:** **Zero third-party libraries.** Uses strictly built-in Windows SDK headers & libraries:
  * `windows.h`, `commctrl.h` (`comctl32.lib`), `winhttp.h` (`winhttp.lib`), `wininet.h` (`wininet.lib`), `shellapi.h` (`shell32.lib`), `commdlg.h` (`comdlg32.lib`), `shlwapi.h` (`shlwapi.lib`).
* **Target Architecture:** **64-bit (`x64`)** exclusively (avoids WOW64 registry redirection bugs when writing to `HKLM\SOFTWARE`).
* **Runtime Linking:** Static C/C++ Runtime (**`/MT`** in MSVC) so the `.exe` runs on barebones Windows installations (e.g., fresh Windows 11 IoT Enterprise LTSC) with zero Visual C++ Redistributable dependencies.
* **Subsystem & Console Handling:**
  * Link with **`/SUBSYSTEM:WINDOWS`** (`wWinMain` entry point) so double-clicking launches only the GUI with no flashing terminal window.
  * When CLI output flags (`--help`, `--version`, `--list`, `--dry-run`, `--silent`, `--dump-config`, `--export`) are detected, immediately call `AttachConsole(ATTACH_PARENT_PROCESS)` and reopen `stdout`/`stderr` (`freopen_s(..., "CONOUT$", "w", ...)`), allowing seamless terminal output when invoked from CMD or PowerShell.
* **Application Manifest (Embedded Resource):**
  * `requestedExecutionLevel level="asInvoker"` *(never `requireAdministrator`—elevation is deferred cleanly).*
  * `Microsoft.Windows.Common-Controls` `6.0.0.0` *(enables ListView groups, checkboxes, custom draw, and modern visual styles).*
  * `<dpiAwareness>PerMonitorV2</dpiAwareness>`.
* **Embedded Default Config:** Pack the default `tasks.ini` into the binary as `RCDATA` (`IDR_DEFAULT_TASKS` in `.rc`) and extract via `FindResourceW` / `LoadResource` / `LockResource` / `SizeofResource`.
* **Character Encoding:** All `.ini` configs, GitHub responses, and `log.txt` files are **UTF-8** (`std::string`). All Win32 API calls use **UTF-16** (`std::wstring`) converted exclusively via `MultiByteToWideChar` / `WideCharToMultiByte` (`CP_UTF8`).

---

## 2. Configuration File Schema (`tasks.ini` INI-Block Format)

### 2.1 Syntax Example
```ini
# Comments start with # or ; (outside key values)

[Privacy | Disable Telemetry]
param = check:1
tags  = common, gaming, dev, reboot
desc  = Disables Windows diagnostic telemetry policy (effective on IoT/Enterprise).
cmd   = reg add "HKLM\SOFTWARE\Policies\Microsoft\Windows\DataCollection" /v AllowTelemetry /t REG_DWORD /d 0 /f

[UI | Taskbar Alignment: Left]
param = check:1
tags  = common, win11, restart-explorer, group:taskbar
desc  = Aligns Start button and taskbar icons to the left.
cmd   = reg add "HKCU\Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced" /v TaskbarAl /t REG_DWORD /d 0 /f

[Tools | Mirror Dev Folder]
param       = check:0
tags        = dev
desc        = Mirrors Source folder to Destination using multiple threads.
arg.Source  = txt:C:\Dev
arg.Dest    = txt:D:\Backup
arg.Threads = num:4
cmd         = robocopy "{Source}" "{Dest}" /MT:{Threads}

[Apps | Mozilla Firefox]
param = check:1
tags  = common, net
desc  = Installs latest Firefox silently via WinGet.
cmd   = winget install --id Mozilla.Firefox -e --silent --accept-package-agreements --accept-source-agreements
```

### 2.2 Block & Field Rules
1. **Section Header (`[Category | Title]`):**
   * Begins a new `TaskItem`.
   * Split on the **first `|`** and trim whitespace on both sides → `category`, `title`. (If `|` is missing, `category = "General"`, `title = trimmed_header`.)
2. **Key-Value Lines (`key = value`):**
   * Split on the **first `=`** only; trim whitespace around `key` and leading whitespace before `value`.
   * **Comments:** Lines whose first non-whitespace character is `#` or `;` are ignored.
   * **`param`:** Comma-separated tokens, order-independent.
     * `check:1` → `checked = true`.
     * `check:0` (or `param` omitted entirely) → `checked = false`.
   * **`tags`:** Comma-separated list of trimmed lowercase strings.
     * Any tag starting with `group:` defines the item's mutual-exclusion group (`groupName = tag.substr(6)`).
   * **`desc`:** Optional single-line human-readable description.
   * **`arg.<Name>`:** Ordered named input parameter (`<Name>` is extracted after `arg.`).
     * Value is split on the **first `:` only** (preserving Windows drive colons like `C:\Dev`).
     * `txt:<default>` (or `text:<default>`) → string argument.
     * `num:<default>` → integer argument (stored as string/int).
     * Arguments are stored in `std::vector<TaskArg>` in exact appearance order.
   * **`cmd`:** Verbatim shell command template (preserving all `|`, `&`, `;`, `"`, `\`, `#`, `=` characters). Contains `{<Name>}` placeholders corresponding to `arg.<Name>`.
3. **Execution Order = Source File Order:**
   * Items are stored in `std::vector<TaskItem> items` in exact parsed file order (`index = 0 .. N-1`).
   * Execution **always** iterates `0 .. N-1`, completely independent of how the GUI visually groups items by category.

---

## 3. Native Tags, Guards & Group Resolution

### 3.1 Native (Engine-Level) Tags
All tags appear in CLI filters and logs. The following tags carry special built-in behavior:

| Tag | Category | Behavior |
| :--- | :--- | :--- |
| **`net`** | Guard | Requires active internet connectivity. If offline: **disabled/grayed out** & unchecked in GUI; **skipped** (`SKIPPED: Offline`) during execution. |
| **`win11`**, **`win10`**, **`win7`** | Guard | OS compatibility checked via `RtlGetVersion` (`win7`: `6.1`, `win10`: `10.0` build `< 22000`, `win11`: `10.0` build `>= 22000`). **Uses OR logic:** If an item has **no** OS tags, it runs anywhere. If it has 1+ OS tags, it is compatible if **at least one** matches the current OS; otherwise disabled/grayed out in GUI and skipped (`SKIPPED: OS mismatch`). |
| **`gui`** | Guard | Marks commands that open a visible window or require interactive input. In **GUI mode**, launches visibly (without `CREATE_NO_WINDOW` / `SW_HIDE`). In **`--silent` mode**, automatically **skipped** (`SKIPPED: Interactive GUI required`). |
| **`restart-explorer`** | Lifecycle | If at least one executed item with this tag finishes (and `--no-explorer-restart` is not set), kills and restarts `explorer.exe` **once** at the end of the entire run. |
| **`reboot`** | Lifecycle | If at least one executed item with this tag finishes (and `--no-reboot` is not set), triggers end-of-run reboot handling. |
| **`group:<name>`** | Mutual Exclusion | Links items into an exclusive radio-style group. |

*(All other tags like `common`, `dev`, `gaming`, `vanguard-warn` are pure user-defined metadata tags.)*

### 3.2 `group:<name>` First-Wins Precedence
* **On initial file parse:** Iterate `0 .. N-1`. Maintain `std::set<std::string> seenGroupDefault`. If an item belongs to `group:X` and has `check:1`:
  * If `"X"` is not in `seenGroupDefault`, keep `checked = true` and insert `"X"`.
  * If `"X"` is already in `seenGroupDefault`, force `checked = false`.
  * If no item in `"X"` has `check:1`, all items in `"X"` remain `false`.
* **On GUI checkbox toggle:** If user checks an item in `group:X`, automatically uncheck all other items in `group:X`.
* **On bulk tag selection (`Select` in GUI, `--tags`, `--all`):** Iterate `0 .. N-1`. Maintain `std::set<std::string> pickedGroups`. Only check the **first** eligible (not guard-disabled) matching item per `group:X`.

---

## 4. Source Resolution & Networking

### 4.1 Startup Source Priority
1. **`--config <path>` (`-c`):** Load local UTF-8 `.ini` file. If missing/unreadable, report an explicit error (do not silently mask a typo).
2. **`--offline` (`-o`):** Immediately skip network operations and load embedded `RCDATA`.
3. **`--url <url>` (`-u`):** Fetch via WinHTTP HTTPS (`WINHTTP_FLAG_SECURE`). Report error if unreachable.
4. **Default Built-in GitHub Raw URL:** Fetch via WinHTTP. If offline, DNS fails, HTTP status `!= 200`, or response invalid, silently fall back to embedded `RCDATA`.

### 4.2 Caching & Connectivity
* **CDN Cache Bypass:** Append `?c=<GetTickCount64()>` (or `&c=...` if `?` exists) to GitHub raw URLs so Fastly CDN serves the newest commit immediately.
* **Connectivity Check:** Determine `bool isOnline` on startup via `InternetGetConnectedState` combined with a quick WinHTTP/TCP check (or successful GitHub fetch). Exposed to the `net` guard and logged in `log.txt`.

---

## 5. CLI Specification

### 5.1 Complete Flag Reference
All flags support `--flag=val` and `--flag val`. Optional path arguments (`[path]`) treat the next token as a path **only** if it exists and does not begin with `-`; otherwise they use their default filename.

* **Source Flags:**
  * `-c, --config <path>` : Load config from local `.ini` file.
  * `-u, --url <url>` : Fetch config from custom raw URL.
  * `-o, --offline` : Skip network fetch; force embedded default `tasks.ini`.
  * `-d, --dump-config [path]` : Write embedded default `tasks.ini` to disk (default: `tasks.ini`) and exit.
  * `-e, --export [path]` : Resolve selection/filters, write full resulting `.ini` with updated `check:1`/`check:0` (default: `tasks-export.ini`), and exit without executing.
* **Selection & Filter Flags** *(work in both `--silent` and GUI pre-selection)*:
  * `-t, --tags <t1,t2,...>` : Select items matching any listed tag (case-insensitive).
  * `-x, --exclude <t1,t2,...>` : Exclude items matching any listed tag.
  * `-p, --pick <s1,s2,...>` : Select items whose `Category | Title` contains any substring (case-insensitive).
  * `-k, --skip <s1,s2,...>` : Exclude items whose `Category | Title` contains any substring (case-insensitive).
  * `-a, --all` : Select all compatible items (subject to `-x`/`-k`/`group:`/guards).
* **Execution & Output Flags:**
  * `-s, --silent` : Run headless without opening GUI.
  * `-n, --dry-run` : Simulate run; print/log exact commands (after `{Arg}` substitution) without running them.
  * `-l, --log [path]` : Enable log output to file (default: `log.txt`).
  * `-q, --quiet` : Suppress stdout messages during `--silent` (still writes `-l` if active).
  * `-r, --reboot` : In `--silent` mode, automatically reboot if any executed item has the `reboot` tag.
  * `--no-reboot` : Suppress both auto-reboot and GUI reboot prompt.
  * `--no-explorer-restart` : Suppress `explorer.exe` restart at end.
  * `--list` : Print formatted table of items (`Index`, `Category`, `Title`, `Default`, `Tags`, `Guard Status`) to stdout and exit.
  * `-h, --help` : Print CLI help & exit.
  * `-v, --version` : Print version & exit.
* *(Internal UAC Handoff Flags: `--auto-run`, `--cleanup-config`)*

### 5.2 Selection Evaluation Precedence
Filters are order-independent on the command line and always evaluate in this exact pipeline:
1. **Base State:**
   * If `-a` (`--all`): select all items.
   * Else if `-t` (`--tags`) or `-p` (`--pick`) provided: start with all items **deselected**, then select matches from `-t` and `-p`.
   * Else: start with the config's default `check:1` items.
2. **Exclusions (Always Win):** Deselect any item matching `-x` (`--exclude`) or `-k` (`--skip`).
3. **Guards & Groups:** Apply `net` and OS guards, then enforce `group:<name>` first-wins across remaining selected items.
4. *(Note: During `--export`, do **not** strip `gui`-tagged items unless excluded via `-x`/`-k`; during `--silent` execution, skip `gui` items at runtime and log `SKIPPED: Requires GUI`.)*

---

## 6. GUI Specification (Pure Win32 API)

### 6.1 Visual Layout & Controls
1. **Font & Resizing:** Query system `lfMessageFont` (`Segoe UI`) via `SystemParametersInfoW(SPI_GETNONCLIENTMETRICS)` and apply to all controls; apply `Consolas` to the log box. Handle `WM_SIZE` dynamically so Top Bar and Bottom Inspector/Log stay anchored while the ListView stretches smoothly.
2. **Header Row 1 (Source Management):**
   * `Source:` `ComboBox` (`CBS_DROPDOWNLIST`): Always contains `Built-in (Embedded)`, plus active `URL: ...` / `File: ...` entries.
   * Whenever the user toggles any checkbox, edits any `arg.<Name>`, or runs bulk `Select`/`Deselect`, automatically add/select a `* Custom (Modified)` entry in the dropdown while keeping the clean original source entries selectable to allow instant revert.
   * **`[ + Load... ]`** button: Opens popup menu with **`From Local File...`** (`GetOpenFileNameW`) and **`From URL...`** (modal input dialog). On load success, adds entry to dropdown and selects it.
   * **`[ Export... ]`** button: Opens `GetSaveFileNameW` (default `tasks-export.ini`) and writes full `.ini` snapshot with live check states and argument values.
3. **Header Row 2 (Tag Filtering):**
   * `Tag:` `ComboBox` (`CBS_DROPDOWNLIST`): Item 0 is `"All"`, followed by sorted unique non-`group:` tags from the loaded config.
   * **`[ Select ]`** button: Checks compatible items matching the chosen tag (or all compatible items for `"All"`), enforcing `group:` first-wins and leaving guard-disabled items unchecked.
   * **`[ Deselect ]`** button: Unchecks items matching the chosen tag (or all items for `"All"`).
4. **Main Checklist (`WC_LISTVIEW`):**
   * Styles: `LVS_REPORT | LVS_SHOWSELALWAYS` + `LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER` + `LVM_ENABLEGROUPVIEW`.
   * Columns: **`Task`** *(Checkbox + Title)* | **`Tags`** | **`Status`**.
   * Grouped visually by `Category` (in order of first appearance). Each item's `LVITEM.lParam` stores its index in `std::vector<TaskItem> items`.
   * **Per-Category Toggle:** Support ListView Group Task link (`LVGF_TASK` → `"Toggle All"` handled via `LVN_LINKCLICK`) and/or right-click row context menu (`Select All in Category` / `Deselect All in Category`).
   * **Row Guard Enforcement & `NM_CUSTOMDRAW`:**
     * Intercept `LVN_ITEMCHANGING` to block toggling items disabled by `net` (offline) or OS mismatch, and to enforce `group:<name>` mutual exclusion.
     * Color text via `NM_CUSTOMDRAW`: Disabled (`Gray`), `Running...` (`Blue`), `[OK] Done` (`Green`), `[ERR <code>] Failed` (`Red`), `[SKIP]` / `[DRY-RUN]` (`Muted Gray`).
5. **Bottom Inspector Panel (Description + Dynamic `arg.<Name>` Editor):**
   * Read-only `Static` label displaying `desc` of the highlighted row.
   * Reusable fixed pool of e.g. **6 hidden `(Static Label + Edit Box)` pairs**. On `LVN_ITEMCHANGED`, update labels/values for `selectedItem.args.size()` slots, toggle `ES_NUMBER` for `num:` args vs standard text for `txt:` args, and `ShowWindow(SW_SHOW)` / `SW_HIDE` accordingly. `EN_CHANGE` updates `item.args[k].currentValue` live and marks Source as `* Custom (Modified)`.
6. **Footer & Live Log Drawer:**
   * Checkboxes: `[ ] Dry Run` and `[ ] Save Log (log.txt)`.
   * **`[ Apply Selected ]`** button (displays native UAC shield via `Button_SetElevationRequiredState(..., TRUE)` when `!IsUserAnAdmin()` and `!dryRun`).
   * Read-only scrollable multiline `Edit` box (`Consolas`) streaming execution output in real time.

---

## 7. Privilege Elevation (`asInvoker` Handoff)
* **Unelevated Operations (Zero UAC Prompts):** `--help`, `--version`, `--list`, `--dump-config`, `--export`, `--dry-run`, and opening/browsing/editing in the GUI never prompt for Admin.
* **GUI Elevation on Apply:**
  1. When `[ Apply Selected ]` is clicked while `!IsUserAnAdmin()` and `!dryRun`:
  2. Export current UI state (all checks & edited `arg.` values) to `%TEMP%\winsetup_job_<pid>.ini`.
  3. Relaunch self via `ShellExecuteExW` (`lpVerb = L"runas"`) passing `--config "%TEMP%\winsetup_job_<pid>.ini" --auto-run --cleanup-config` (plus `--log` if `Save Log` is checked).
  4. On launch, the elevated instance loads the temp config, immediately deletes the temp file (`--cleanup-config`), opens the elevated GUI, and begins executing automatically (`--auto-run`) so the user watches live progress. Exit the unelevated instance.
* **CLI `--silent` Elevation Check:** If `--silent` (and not `--dry-run`) is launched without Admin rights, log/print `"Error: Administrator privileges required for --silent mode."` and exit with code `1` (preventing broken detached consoles in headless scripts).

---

## 8. Execution Engine, Threading & Post-Run Lifecycle
1. **Worker Thread:** In GUI mode, disable interactive controls and run the loop on a dedicated `std::thread`, posting `PostMessageW` updates (`WM_APP_ITEM_STATUS`, `WM_APP_LOG_LINE`, `WM_APP_RUN_DONE`) back to the UI thread so the window never freezes.
2. **Command Substitution & Dispatch:**
   * Loop `i = 0 .. items.size()-1`. Skip unchecked items and guard-blocked items (`net`, OS, or `gui` if in `--silent`).
   * Replace every `{Name}` occurrence in `item.cmd` with the corresponding `arg.<Name>` value.
   * If `dryRun`: mark `[DRY-RUN]` and log command.
   * Otherwise spawn `cmd.exe /c "<substituted_cmd>"`:
     * Standard items: `CREATE_NO_WINDOW` + `CreatePipe` redirecting `stdout` & `stderr` (`bInheritHandle = TRUE`; **immediately close write handle in parent** before reading so `ReadFile` never deadlocks).
     * `gui`-tagged items (GUI mode): launch visibly (`SW_SHOWNORMAL`, no `CREATE_NO_WINDOW`).
   * Read `GetExitCodeProcess`. Record success (`exit == 0`), failure (`exit != 0`), and elapsed ms.
3. **Post-Run Lifecycle (Executed once at end if `!dryRun`):**
   * **Explorer Restart:** If any executed item carried `restart-explorer` (and `!noExplorerRestart`), execute `taskkill /f /im explorer.exe` followed by launching `explorer.exe`.
   * **Reboot:** If any executed item carried `reboot` (and `!noReboot`):
     * GUI mode: Prompt `MessageBoxW(MB_YESNO | MB_ICONQUESTION)` *"Changes applied. Reboot now?"*; if Yes, acquire `SeShutdownPrivilege` and call `ExitWindowsEx(EWX_REBOOT, 0)` (or `shutdown /r /t 0`).
     * `--silent` mode: If `--reboot` (`-r`) was specified, initiate reboot; otherwise print/log `"Reboot required for some applied changes."`.

---

## 9. Logging Specification (`log.txt`)
When `-l` / `--log` or `[x] Save Log` is enabled, write a UTF-8 log structured as follows:
1. **Run Header:** Timestamp, tool version, mode (`GUI` / `Silent` / `Dry-Run`), `Admin: Yes/No`, `Network: Online/Offline`, `OS: Windows <10/11> Build <N>`, `Config Source`.
2. **Per-Item Entry:**
   ```text
   [14:22:11] [1/12] [Privacy] Disable Telemetry
     CMD: cmd.exe /c "reg add ..."
     OUT: The operation completed successfully.
     RES: OK (exit=0, 14ms)
   ```
   *(Progress Output Normalization: Before appending captured stdout to `log.txt` or GUI log, strip/collapse carriage-return `\r` spinner frames—such as `winget` download bars—keeping only the final clean line per `\r` overwrite.)*
3. **Footer Summary:**
   * Counts (`Succeeded | Failed | Skipped | Total Duration`) and a dedicated **`FAILED ITEMS`** section repeating every failed item and its exit code so errors are visible without scrolling through installer logs.

---

## 10. Suggested File Structure (Single Binary Build)
```text
WinSetup/
├── src/
│   ├── main.cpp           # wWinMain, AttachConsole, CLI dispatch vs GUI launch
│   ├── cli.hpp / .cpp     # Flag parser, precedence resolver, --list / --help / --export
│   ├── config.hpp / .cpp  # INI-block parser, group: first-wins, serializer (export)
│   ├── net.hpp / .cpp     # WinHTTP fetcher (+cache buster), InternetGetConnectedState
│   ├── env.hpp / .cpp     # RtlGetVersion OS check, IsUserAnAdmin, UTF-8/UTF-16 helpers
│   ├── runner.hpp / .cpp  # CreateProcessW + CreatePipe, {Arg} replace, \r cleaner, log.txt
│   └── gui.hpp / .cpp     # Win32 window, ListView groups/customdraw, dynamic arg pool, UAC handoff
├── res/
│   ├── app.rc             # Manifest + RCDATA "tasks.ini" + app icon
│   ├── app.manifest       # asInvoker + ComCtl v6 + PerMonitorV2 DPI
│   └── tasks.ini          # Default embedded INI-block configuration
└── build.bat              # cl /O2 /EHsc /MT /DUNICODE /D_UNICODE ...
```
