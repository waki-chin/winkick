# Preparation validation — 2026-11-17

Validated locally on Windows 11 x64 with Visual Studio Build Tools 18.
These results are not a claim that a hosted GitHub release has run.

| Check | Result |
| --- | --- |
| Native x64 application build, `/W4 /WX`, static CRT | Passed |
| CLI/execution-engine fixture suite | 111 passed, 0 failed |
| Catalog structure/defaults/native list and dry-run | 65 opt-in tasks verified |
| Embedded PowerShell command-body syntax | 47 bodies parsed |
| Mocked WinGet/WSL/installer-result fragments | 19 passed |
| Disposable Git ignore/attributes/release-policy tests | 29 passed |
| Interactive GUI fixture suite | 14 passed, 0 failed |
| PE architecture/imports/ASLR/DEP/CFG/manifest/version/resources | Passed |
| Same-checkout/toolchain repeat-build EXE SHA-256 | Matched |
| All GitHub YAML and workflow PowerShell blocks | Parsed |
| Workflow lint, actionlint 1.7.12 (download hash checked) | Passed |
| Clean Git checkout with declared line endings | Built and passed safe gates |
| Tagged packaging from that disposable checkout | Passed; checkout remained clean |
| Release asset set, SHA-256 entries, ZIP contents and source metadata | Passed |
| Publisher's read-only asset-validation block | Accepted exact assets; rejected tampering |
| Existing/stale local package output | Rejected |

At the time of these preparation checks, only disposable fixtures were
initialized, committed, or tagged for Git tests. Existing machine
reports/screenshots and the previous executable
were preserved locally, with a full pre-cleanup recovery backup outside the
project.

## Release simplification — 2026-11-18

Linted the single tag-triggered workflow and parsed its PowerShell steps.
Reran the full local safe suite. In a disposable tagged checkout without the
test suite, the build and packaging steps passed, and the publisher's read-only
asset check accepted the package and rejected a modified catalog.

Read-only catalog checks also confirmed identical listings for UTF-8 LF and
UTF-8 CRLF with a BOM, and that startup ignores an unrequested working-directory
`tasks.ini`. The runtime source order was not changed.

## Not validated here

- Hosted `windows-2022` compiler/SDK/image behavior.
- GitHub token permissions, OIDC/Sigstore issuance and online attestation lookup.
- Environment reviewers, rulesets, protected/immutable tags and releases.
- Actual release creation/uploads/publication and failure recovery on GitHub.
- Fresh installation of all catalog applications, every OS/edition/DPI scenario,
  actual reboot/Explorer restart, or Windows Authenticode publisher trust.

No production catalog command, software installation, reboot, or Explorer restart
was performed for this validation. GUI tests used the harmless test catalog.
Follow the setup and release checklists after connecting GitHub; use disposable
Windows VMs for real installer/lifecycle checks.
