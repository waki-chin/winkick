# Security policy

## Reporting

Use [Report a vulnerability](https://github.com/waki-chin/winkick/security/advisories/new)
for private disclosure.
Do not put credentials, private logs, exploitable payloads, or sensitive machine
details in a public issue. If private reporting is unavailable, request a private
contact channel without disclosing the vulnerability publicly.

Only the latest published release is intended to receive fixes. No response-time
or security-support SLA is currently offered.

## Trust boundaries

- Catalogs are executable shell instructions. Treat a downloaded/local INI as you
  would a script; review it before selecting tasks or granting UAC.
- Default tasks come from the exact embedded release catalog. Remote task loads
  are explicit and HTTPS-only. The connectivity probe does not load commands.
- The GUI's UAC handoff elevates selected operations. It is not an arbitrary-code
  sandbox or a security boundary against a malicious same-user process.
- Raw argument substitution deliberately permits shell syntax. It is not safe
  to populate task arguments from untrusted external input.
- Local logs/exports can contain private data. They are ignored by Git, but
  maintainers must review files before posting diagnostics.
- Installer/repository changes after a release are outside that release's
  attestation. WinGet performs its own installer hash checks; selected bootstrap
  commands download and execute vendor scripts at run time.
- Turning off Defender sample submission reduces protection; PUA blocking and
  sample-submission privacy are separate choices.

## Release provenance

The tagged-source build and binary verification job has read-only repository
permission. Full tests are run locally before tagging. Only the
separate publishing job receives release/OIDC/attestation permissions; it never
executes the compiled EXE or checks out source. Actions are pinned by full commit
SHA. Publication uses a fresh immutable artifact ID, checks checksums/metadata,
attests the exact assets, uploads to a draft, then publishes. Existing releases
are not overwritten.

This proves workflow origin/integrity when verified with the expected repository,
workflow and source tag. It does not prove the code is harmless, defeat a
compromised maintainer/GitHub/runner/toolchain, or replace code review. Hosted
runner images and compiler/SDK updates are external trust dependencies.

No Authenticode certificate is configured. GitHub OIDC attestations do not make
Windows display a verified Authenticode publisher.
