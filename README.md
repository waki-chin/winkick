# WinKick

A small native Windows setup utility: review an INI checklist, choose tasks,
then apply them through a Win32 GUI or command line.

- Windows 10/11 **x64**, including supported IoT Enterprise/LTSC installations.
- C++17 and Windows SDK APIs; statically linked C/C++ runtime.
- One executable, with the reviewed task catalog embedded at build time.
- All shipped tasks are **unchecked by default**.
- Dry-run, OS/network guards, argument editors, logging, and explicit elevation.

> **This tool runs shell commands, potentially as Administrator.**
> Read the selected commands and descriptions before applying them. `--all` means
> the entire catalog, not a recommended preset. Compilation, checksums, and
> provenance establish origin/integrity, not that every task is safe for your PC.

## Download and verify

Use the [Releases page](https://github.com/waki-chin/winkick/releases) for
official downloads when available. Release assets are built from the version
tag by GitHub Actions, not uploaded from a maintainer's PC:

- `winkick.exe` — standalone x64 executable.
- `winkick-vVERSION-windows-x64.zip` — executable, catalog, documentation/license,
  and build metadata.
- `tasks.ini`, `build-info.json`, `SHA256SUMS`, `provenance.sigstore.json`.

Use a current GitHub CLI to verify the exact download against the publishing
workflow and tag. Replace `v1.0.0` with the tag of your download:

```powershell
Get-FileHash .\winkick.exe -Algorithm SHA256
# Compare with the winkick.exe entry in SHA256SUMS.
gh attestation verify .\winkick.exe --repo waki-chin/winkick `
  --signer-workflow waki-chin/winkick/.github/workflows/release.yml `
  --source-ref refs/tags/v1.0.0 --deny-self-hosted-runners
```

A hash from the same download location alone is not proof of authenticity.
Provenance is **not Authenticode signing**; Windows may still show SmartScreen or
an unknown publisher. No signing certificate/private key is stored in this repo.

## Use

Double-click the executable for the checklist. Browsing and dry-runs do not need
elevation; applying real tasks requests UAC if necessary.

```powershell
# Read-only inspection
.\winkick.exe --list
.\winkick.exe --dry-run --pick "Show file extensions" --no-reboot --no-explorer-restart

# Use your own trusted catalog in the GUI
.\winkick.exe --config .\my-tasks.ini

# From an elevated terminal: apply only the explicitly selected task
.\winkick.exe --silent --pick "Show file extensions" --log=setup.log `
  --no-reboot --no-explorer-restart

.\winkick.exe --help
```

The default catalog is embedded: WinKick doesn't automatically look for a
nearby `tasks.ini` or fetch task commands on startup. Use `--config` for a local
file, or `--url` or the GUI's **Load from URL** action for a trusted HTTPS
catalog. Unless `--offline` is supplied, a short HTTPS connectivity probe may
still run to determine whether `net` tasks are available.

For automation, wait for this Windows-GUI-subsystem process to exit. In PowerShell,
use `Start-Process -Wait -PassThru` for exit codes or a
`System.Diagnostics.Process` with redirected streams for captured output.

Exit codes: **0** completed without task failures, **1** task/privilege/reboot
failure, **2** command-line/configuration/output error. Guard-skipped items are
not task failures. General shell success remains `exit == 0`; selected installer
tasks explicitly normalize completed-install code 3010 and show a restart notice.

## Catalog

The catalog offers package-manager setup, the selected application installs,
and a smaller set of Windows preferences. See [task documentation](docs/tasks.md)
and inspect the actual [catalog](res/tasks.ini).

- WSL is two-stage: enable prerequisites, restart manually if required, then
  install WSL. No Linux distribution is installed automatically.
- Defender sample submission is a separate advanced privacy trade-off:
  `NeverSend` **disables Block at First Sight**. It is not security hardening.
- The classic context-menu option is a legacy workaround, not guaranteed on
  newer Windows builds.
- App installers and package repositories are external dependencies; their
  current contents are not covered by WinKick's executable attestation.
- Per-user settings target the account running WinKick. Task arguments are raw
  command-template substitutions, not an input-sandbox boundary.

## Build from source

Install Visual Studio 2022 or later **C++ Build Tools**, the x64 compiler and a
Windows SDK. Build scripts discover installations through `vswhere`; an explicit
`VCVARS` path can override detection. No WinGet/Scoop/Chocolatey bootstrap is run
by the build.

```powershell
.\build.bat
.\scripts\test-build.ps1
.\scripts\check-reproducible.ps1
.\build\winkick.exe

# Optional local package, after tests; dist/ must not already exist.
.\scripts\package.ps1
```

`VERSION` is the only product-version source. Build-generated headers, resources,
objects, analysis outputs, and the EXE belong in `build/`; test evidence is in
`tests/out/`. The EXE embeds `res/tasks.ini`; rebuilding is necessary after catalog
changes unless loading that file explicitly with `--config`.

Run the full safe test suite locally before tagging a release; the repeat
build is optional. GitHub builds on version tags only and checks the EXE,
including its `--version` output and exact embedded catalog, before packaging.
Neither the local automated tests nor GitHub install the catalog's apps or
perform real reboot/Explorer restart actions.

## Repository layout

```text
src/                 Native application code
res/                 Catalog, icon, manifest, resource IDs
tests/               Harmless fixtures and test sources (no committed binaries)
scripts/             Build verification, versioning, packaging, release checks
.github/             Release workflow, dependency updates, issue templates
docs/                Task format, GitHub setup, release process, design input
VERSION              Product version
build.bat            Local x64 /MT build -> build/winkick.exe
```

Build outputs, release bundles, logs, local exports, credentials/certificates,
and machine-specific historical reports do **not** belong in Git. Existing
historical materials on the development machine are preserved under ignored
`.local/`, not shipped or included in source history.

See [contributing](CONTRIBUTING.md), [security](SECURITY.md),
[GitHub setup](docs/github-setup.md), and [release procedure](docs/releasing.md).
Local preparation results and untested integrations are in
[validation.md](docs/validation.md).

## License

[MIT](LICENSE). Third-party applications, fonts, downloaded package-manager
installers, Windows components, and Microsoft build tools retain their own
licenses and terms.
