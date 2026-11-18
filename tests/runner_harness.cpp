// WinKick - TEST-ONLY integration harness for runner::RunAll.
// NOT part of the shipped winkick.exe binary. Exercises the real execution
// engine (process spawn, pipe capture, exit codes, guards, logging, \r cleanup)
// with harmless commands only.
#include "../src/config.hpp"
#include "../src/runner.hpp"
#include "../src/env.hpp"
#include "../src/cli.hpp"

#include <windows.h>
#include <iostream>
#include <fstream>
#include <sstream>

using namespace std;

static string ReadAll(const char* path) {
    ifstream f(path, ios::binary);
    stringstream ss; ss << f.rdbuf();
    return ss.str();
}

static bool Contains(const string& haystack, const string& needle) {
    return haystack.find(needle) != string::npos;
}

int main() {
    SetAppInstance(nullptr); // embedded resource not needed for this harness

    string iniPath = "tests\\test_tasks.ini";
    string ini = ReadAll(iniPath.c_str());
    if (ini.empty()) { cerr << "cannot read " << iniPath << endl; return 3; }

    int failures = 0;
    auto check = [&](const char* name, bool cond) {
        cout << (cond ? "[PASS] " : "[FAIL] ") << name << endl;
        if (!cond) ++failures;
    };

    // ---------- Scenario 1: capture + failure accounting ----------
    {
        Config cfg = ParseIniBlocks(ini, "Test");
        for (auto& item : cfg.items) {
            item.checked = (item.title == "Echo Greeting" ||
                            item.title == "Intentional Failure" ||
                            item.title == "Spinner Simulation");
        }
        runner::Options opts{};
        opts.dryRun = false;
        opts.silent = true;           // headless semantics
        opts.guiMode = false;
        opts.noReboot = true;         // never reboot in tests
        opts.noExplorerRestart = true;
        opts.logPath = "tests\\out\\harness_log.txt";
        opts.modeLabel = "Silent";
        opts.admin = false;
        opts.isOnline = false;        // ping item skipped via pipeline-independent guard anyway
        opts.configSourceLabel = "Test";

        string capturedLog;
        runner::LogSink sink = [&](const string& line) { capturedLog += line; capturedLog += '\n'; };

        runner::Stats stats = runner::RunAll(cfg.items, opts, sink, nullptr);

        check("s1: two successes recorded (echo + spinner)", stats.succeeded == 2);
        check("s1: one failure recorded", stats.failed == 1);
        check("s1: failed item is 'Intentional Failure'",
              stats.failedItems.size() == 1 && stats.failedItems[0].title == "Intentional Failure");
        check("s1: failing exit code is 7",
              stats.failedItems.size() == 1 && stats.failedItems[0].exitCode == 7);
        check("s1: spinner output normalized (100% done present)",
              Contains(capturedLog, "progress: 100% done"));
        check("s1: spinner intermediate frames collapsed",
              !Contains(capturedLog, "10%progress"));
        check("s1: log contains run header",
              Contains(capturedLog, "Mode: Silent") && Contains(capturedLog, "Config Source: Test"));
        check("s1: log contains FAILED ITEMS section",
              Contains(capturedLog, "FAILED ITEMS:") && Contains(capturedLog, "exit=7"));
        check("s1: log file written", Contains(ReadAll("tests\\out\\harness_log.txt"), "FAILED ITEMS:"));

        // item states reflect reality
        for (auto& item : cfg.items) {
            if (item.title == "Echo Greeting")
                check("s1: echo item state == Ok", item.state == ItemRunState::Ok);
            if (item.title == "Intentional Failure")
                check("s1: failure item state == Failed", item.state == ItemRunState::Failed);
        }
    }

    // ---------- Scenario 2: arg substitution renders real command ----------
    {
        Config cfg = ParseIniBlocks(ini, "Test");
        for (auto& item : cfg.items) item.checked = (item.title == "Robocopy List Only");
        runner::Options opts{};
        opts.dryRun = true;           // substitution is logged, nothing executed
        opts.noReboot = true;
        opts.noExplorerRestart = true;
        opts.modeLabel = "Dry-Run";
        opts.configSourceLabel = "Test";
        string capturedLog;
        runner::Stats stats = runner::RunAll(cfg.items, opts,
            [&](const string& line) { capturedLog += line; capturedLog += '\n'; }, nullptr);
        check("s2: placeholder substituted (/MT:4)", Contains(capturedLog, "/MT:4"));
        check("s2: no placeholder left ({Threads})", !Contains(capturedLog, "{Threads}"));
        check("s2: txt arg substituted (C:\\Windows)", Contains(capturedLog, "C:\\Windows"));
        check("s2: dry-run did not execute (stats.dryRun == 1)", stats.dryRun == 1 && stats.failed == 0);
    }

    // ---------- Scenario 3: guards block at runtime even if checked ----------
    {
        Config cfg = ParseIniBlocks(ini, "Test");
        for (auto& item : cfg.items)
            item.checked = (item.title == "Net Guard Demo" || item.title == "Windows 7 Only" ||
                            item.title == "Open Notepad");
        runner::Options opts{};
        opts.dryRun = true;
        opts.silent = true;           // headless -> gui-tagged items skipped
        opts.noReboot = true;
        opts.noExplorerRestart = true;
        opts.modeLabel = "Dry-Run";
        opts.isOnline = false;        // net guard active
        opts.configSourceLabel = "Test";
        string capturedLog;
        runner::Stats stats = runner::RunAll(cfg.items, opts,
            [&](const string& line) { capturedLog += line; capturedLog += '\n'; }, nullptr);
        check("s3: net item skipped (Offline)", Contains(capturedLog, "SKIPPED (Offline)"));
        check("s3: win7 item skipped (OS mismatch)", Contains(capturedLog, "SKIPPED (OS mismatch)"));
        check("s3: gui item skipped in headless (Interactive GUI required)",
              Contains(capturedLog, "SKIPPED (Interactive GUI required)"));
        check("s3: nothing simulated", stats.dryRun == 0 && stats.succeeded == 0);
    }

    // ---------- Scenario 4: lifecycle tags must not fire in dry-run ----------
    {
        Config cfg = ParseIniBlocks(ini, "Test");
        for (auto& item : cfg.items)
            item.checked = (item.title == "Explorer Restart Demo" || item.title == "Reboot Demo");
        runner::Options opts{};
        opts.dryRun = true;
        opts.noReboot = false;        // explicitly allow, but dry-run must suppress anyway
        opts.noExplorerRestart = false;
        opts.modeLabel = "Dry-Run";
        opts.configSourceLabel = "Test";
        runner::Stats stats = runner::RunAll(cfg.items, opts, [](const string&) {}, nullptr);
        check("s4: dry-run does not flag explorer restart", !stats.anyRestartExplorer);
        check("s4: dry-run does not flag reboot", !stats.anyReboot);
    }

    // ---------- Scenario 5: NormalizeCarriageReturns unit checks ----------
    {
        using runner::NormalizeCarriageReturns;
        check("s5: CRLF preserved", NormalizeCarriageReturns("a\r\nb\r\n") == "a\nb\n");
        check("s5: bare LF preserved", NormalizeCarriageReturns("a\nb\n") == "a\nb\n");
        check("s5: spinner overwrite keeps last frame",
              NormalizeCarriageReturns("10%\r50%\r100% done\r\n") == "100% done\n");
        check("s5: trailing CR without newline",
              NormalizeCarriageReturns("x\ry\rz") == "z");
        check("s5: empty input", NormalizeCarriageReturns("").empty());
    }

    // ---------- Scenario 6: arg parser edge cases ----------
    {
        Config cfg = ParseIniBlocks(
            "[T | Drive Colon Test]\n"
            "param = check:1\n"
            "arg.Path = txt:C:\\Dev\\Sub\n"
            "arg.Ratio = num:8\n"
            "cmd = echo {Path} {Ratio}\n"
            , "Test");
        check("s6: drive colon survived parsing", cfg.items.size() == 1 && cfg.items[0].args[0].currentValue == "C:\\Dev\\Sub");
        check("s6: num arg parsed as num", cfg.items.size() == 1 && cfg.items[0].args[1].type == "num");
        check("s6: substitution", cfg.items.size() == 1 &&
              runner::SubstituteArgs(cfg.items[0].cmd, cfg.items[0].args) == "echo C:\\Dev\\Sub 8");
    }

    // ---------- Scenario 7: parser/serializer and substitution regressions ----------
    {
        Config cfg = ParseIniBlocks(
            "\xEF\xBB\xBF[T | Whitespace]\n\r \t\n"
            "arg.Path = txt: leading and trailing \t\n"
            "cmd = echo {Path}  \n", "Test");
        check("s7: malformed whitespace line is ignored", cfg.items.size() == 1);
        if (cfg.items.size() == 1) {
            check("s7: argument whitespace preserved", cfg.items[0].args[0].currentValue == " leading and trailing \t");
            check("s7: command trailing whitespace preserved", cfg.items[0].cmd == "echo {Path}  ");
            Config roundtrip = ParseIniBlocks(SerializeIniBlocks(cfg.items), "Roundtrip");
            check("s7: argument roundtrip", roundtrip.items.size() == 1 &&
                  roundtrip.items[0].args[0].currentValue == cfg.items[0].args[0].currentValue);
            check("s7: command roundtrip", roundtrip.items.size() == 1 && roundtrip.items[0].cmd == cfg.items[0].cmd);
        }
        vector<TaskArg> args{{"A", "txt", "", "{B}"}, {"B", "txt", "", "done"}};
        check("s7: substitution never cascades", runner::SubstituteArgs("{A} {B}", args) == "{B} done");
        check("s7: nested literal braces preserved", runner::SubstituteArgs("if (1) { {B} }", args) == "if (1) { done }");
        check("s7: unknown placeholder preserved", runner::SubstituteArgs("{Unknown}", args) == "{Unknown}");
        check("s7: trailing CR keeps final frame", runner::NormalizeCarriageReturns("one\rtwo\r") == "two");
        for (int c = 0; c < 256; ++c) {
            string line(1, (char)c);
            ParseIniBlocks(line + " \t\n[T]\ncmd=echo ok\n", "Byte fuzz");
        }
        check("s7: parser tolerates all single-byte prefixes", true);
    }

    // ---------- Scenario 8: CLI validation and selection precedence ----------
    {
        auto rejected = [](const wchar_t* cmd) {
            ::Options opts;
            string err;
            return !ParseCommandLine(cmd, opts, err) && !err.empty();
        };
        check("s8: required value cannot eat next flag", rejected(L"winkick --tags --silent"));
        check("s8: empty CSV rejected", rejected(L"winkick --tags=,"));
        check("s8: boolean attached value rejected", rejected(L"winkick --silent=false"));
        check("s8: empty optional value rejected", rejected(L"winkick --log=\"\""));
        Config cfg = ParseIniBlocks(
            "[T|A]\ntags=common,group:g\ncmd=echo a\n"
            "[T|B]\ntags=common,group:g\ncmd=echo b\n"
            "[T|Net]\ntags=common,net\ncmd=echo net\n"
            "[T|GUI]\ntags=gui\ncmd=echo gui\n", "Test");
        ::Options opts;
        opts.selectAll = true;
        opts.skips.push_back("a"); // A is excluded before group first-wins; B must win.
        ApplySelectionFilters(cfg.items, opts, false);
        check("s8: exclusion precedes group resolution", !cfg.items[0].checked && cfg.items[1].checked);
        check("s8: offline guard clears selection", !cfg.items[2].checked);
        check("s8: GUI selection retained for export", cfg.items[3].checked);
    }

    // ---------- Scenario 9: quoted executable paths and real output limits ----------
    {
        const wstring path = L"tests\\out\\quoted tool.cmd";
        bool wrote = env::WriteFileUtf8(path, "@echo off\r\necho quoted-tool:%~1\r\nexit /b 0\r\n");
        check("s9: quoted command fixture written", wrote);
        Config cfg = ParseIniBlocks(
            "[T|Quoted]\nparam=check:1\ncmd=\"tests\\out\\quoted tool.cmd\" \"two words\"\n"
            "[T|Empty]\nparam=check:1\ncmd= \n"
            "[T|Flood]\nparam=check:1\ncmd=for /L %i in (1,1,20000) do @echo 0123456789012345678901234567890123456789012345678901234567890123456789\n",
            "Test");
        runner::Options opts;
        opts.silent = true;
        opts.noReboot = true;
        opts.noExplorerRestart = true;
        string log;
        runner::Stats stats = runner::RunAll(cfg.items, opts,
            [&](const string& line) { log += line + '\n'; }, nullptr);
        check("s9: quoted path and spaced argument execute", cfg.items[0].state == ItemRunState::Ok &&
              Contains(log, "quoted-tool:two words"));
        check("s9: isolated deterministic cmd flags logged", Contains(log, "cmd.exe /d /s /c"));
        check("s9: empty real command fails with 127", cfg.items[1].state == ItemRunState::Failed && cfg.items[1].lastExitCode == 127);
        check("s9: flood output drains without deadlock", cfg.items[2].state == ItemRunState::Ok);
        check("s9: output truncation is explicit", Contains(log, "output truncated at 1 MB"));
        check("s9: failure accounting includes empty command", stats.succeeded == 2 && stats.failed == 1);
        DeleteFileW(path.c_str());
    }

    // ---------- Scenario 10: real-time output and stdin EOF ----------
    {
        Config cfg = ParseIniBlocks(
            "[T|Streaming]\nparam=check:1\ncmd=echo stream-before & ping -n 3 127.0.0.1 >nul & echo stream-after\n"
            "[T|No stdin]\nparam=check:1\ncmd=set /p ignored=prompt & echo stdin-eof\n", "Test");
        runner::Options opts;
        opts.silent = true;
        opts.noReboot = true;
        opts.noExplorerRestart = true;
        ULONGLONG start = GetTickCount64(), firstOutput = 0;
        string log;
        runner::Stats stats = runner::RunAll(cfg.items, opts, [&](const string& line) {
            log += line + '\n';
            if (Contains(line, "OUT: stream-before")) firstOutput = GetTickCount64() - start;
        }, nullptr);
        check("s10: complete output lines stream before process exit",
              Contains(log, "OUT: stream-before") && stats.totalMs > firstOutput + 1000);
        check("s10: streamed lines are not duplicated",
              log.find("OUT: stream-before") == log.rfind("OUT: stream-before") &&
              Contains(log, "stream-after"));
        check("s10: child stdin reads EOF instead of hanging", stats.succeeded == 2 && Contains(log, "stdin-eof"));
    }

    cout << (failures == 0 ? "\nALL HARNESS SCENARIOS PASSED" : "\nHARNESS FAILURES: ") << (failures == 0 ? "" : to_string(failures)) << endl;
    return failures == 0 ? 0 : 1;
}
