# Contributing

1. Keep changes focused and explain user-visible behavior.
2. Build on Windows x64 with the C++ workload and Windows SDK:
   `.\build.bat`.
3. Run `.\scripts\test-build.ps1` locally. For build changes, consider also
   running `.\scripts\check-reproducible.ps1`.
4. For GUI changes, additionally run `tests/gui_test.ps1` on an interactive
   desktop. Keyboard/DPI checks and the deliberately gated UAC/elevated tests are
   manual checks, not unattended PR requirements.
5. Never run a production catalog as part of a test, or test actual shutdown,
   service changes, Explorer termination, or software installation on a
   contributor's normal machine.

Task-catalog PRs must describe registry/policy scope, supported Windows versions,
expected exit codes, restart behavior, and trade-offs. Keep shipped defaults
unchecked and use exact package IDs/trusted HTTPS origins. Add/update safe
mocked tests when changing resolution, staging, or installer result handling.
Do not weaken Defender/UAC/firewall protection silently or hide errors as success.

Do not commit EXEs, downloaded installers, build/test outputs, machine logs,
credentials or signing keys. Preserve the `.gitattributes` rules, especially
CRLF for batch files and meaningful whitespace in INI argument values.

Use the pull-request checklist. Workflow/build changes and catalog changes merit
especially careful maintainer review. VERSION changes must keep release tags
consistent; generated version headers are never edited or committed.

By submitting contributions, you agree to license your contributions under this
repository's MIT license. Third-party assets must have compatible provenance and
attribution; do not assume a downloaded program inherits WinKick's license.
