# Task catalog

The authoritative catalog is [`res/tasks.ini`](../res/tasks.ini). There is no
implicit preset: all shipped `check` values are zero.

```ini
[Explorer | Show file extensions]
param = check:0
tags = settings, explorer, win10, win11, restart-explorer
desc = Shows file-name extensions for the current user.
cmd = reg add "HKCU\Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced" /v HideFileExt /t REG_DWORD /d 0 /f
```

Each section is one task, executed in file order regardless of visual grouping.
`arg.Name = txt:default` or `num:default` adds an editable `{Name}` placeholder.
Commands are single-line shell templates; substitution does not escape arguments.
Only full-line `#`/`;` comments are comments. Values can contain meaningful
whitespace. Export writes current selections and edited argument values.

## Tags

| Tag | Effect |
| --- | --- |
| `net` | Disabled/skipped when connectivity is unavailable or forced offline |
| `win10`, `win11`, `win7` | OS guards; multiple OS tags are OR, not AND |
| `gui` | Interactive task; skipped in silent mode |
| `restart-explorer` | Successful real execution may trigger one end-of-run shell restart |
| `reboot` | Marks a possible restart requirement; GUI may prompt, silent auto-reboot requires `--reboot` |
| `group:name` | Mutually exclusive selections; first eligible item wins in bulk selection |
| Other tags | Filter metadata, not dependency or privilege enforcement |

Use `--no-reboot --no-explorer-restart` to suppress lifecycle handling. Static
`reboot` tags are conservative: a successful install can prompt even if it did
not return 3010. The runner does not dynamically parse installer output.

## Source selection and trust

Order: explicit `--config` > `--offline` > explicit `--url` > embedded catalog.
WinKick does not automatically look for a `tasks.ini` in the working directory
or next to the EXE. Local files should be saved as UTF-8; LF and CRLF both work.
An explicit source error is an error, not a silent substitution. Without
`--offline`, local/embedded loads may probe HTTPS reachability to evaluate `net`.
The embedded configuration is verified against the source file during release.

Installing tools is not a transaction. Later tasks can fail if an earlier
bootstrap fails; changes already made are not automatically rolled back. The
current runner has no general task timeout/cancellation for arbitrary commands.

## Particular choices

- Application installs are independent, not radio groups.
- Nerd Fonts currently selects JetBrainsMono, not the entire collection.
- Build Tools targets 2022 with the C++ workload for Windows 10 compatibility.
- VC++ x86 and x64 runtimes are independent; a 64-bit OS can need both.
- WSL prerequisites and installation are separate tasks. Restart manually when
  feature servicing requires it; the WSL install refuses pending prerequisites.
- PUA blocking is hardening. Defender `NeverSend` is a privacy trade-off that
  disables Block at First Sight and is explicitly labelled as reducing protection.
- Some registry policies/workarounds depend on edition/build or can be overridden
  by organization management. A successful registry write is not proof that a
  future Windows release honors it.

Do not use `--silent --all` as a casual example: it requests the entire catalog,
including advanced options and every listed application.
