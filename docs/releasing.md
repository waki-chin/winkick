# Releasing WinKick

## Release contract

Official releases are made by pushing a new **vVERSION** tag, never by uploading
a locally compiled EXE in the GitHub UI. The tag must match `VERSION`, resolve to
the checked-out commit, and already be merged into `origin/main`.

The release workflow:

1. Checks out that event commit on a clean Windows 2022 hosted runner.
2. Builds x64 `/MT` from `src/` and embeds that commit's catalog/manifest/version.
3. Checks the PE, manifest, CLI version and exact embedded catalog, then
   packages the fresh EXE, ZIP, catalog, build metadata and SHA256SUMS.
4. Downloads that run's **immutable artifact ID** in a separate publishing job.
5. Rechecks filenames, hashes, source SHA/tag and dirty-source metadata without
   executing the downloaded EXE or any build-job scripts.
6. Uses GitHub OIDC to attest the exact release assets and exports the Sigstore
   bundle. No long-lived signing secret is needed.
7. Creates a draft, uploads assets, then publishes. Existing releases are not
   replaced. Prerelease version suffixes produce GitHub prereleases.

Checksum entries cover the EXE, ZIP, catalog and build metadata. SHA256SUMS and
the provenance bundle are not self-hashed; the signed attestation includes
SHA256SUMS and the payloads, and the bundle carries its own verifiable signature.

GitHub account settings/rulesets are necessary to protect this process; see
[github-setup.md](github-setup.md). A repository administrator can bypass any
file-based policy. Do not manually publish another release outside this workflow.

## Maintainer checklist

- Review code, application package IDs, external bootstrap origins and all
  privileged catalog commands.
- Confirm every shipped task remains opt-in.
- Change only `VERSION` for product versioning; generated headers/resources derive
  from it. Update CHANGELOG and documentation.
- On the clean commit you intend to tag, run `.\build.bat` and
  `.\scripts\test-build.ps1` locally. GitHub does not run the full suite.
- Test real GUI/UAC, keyboard navigation, relevant OS editions/DPI, and actual
  selected installers on disposable Windows VMs. The hosted build does not test those.
- Review third-party licenses, Windows/VS support requirements and security
  trade-offs. General nonzero tool-specific success codes need explicit handling.
- Configure the release environment/rulesets/immutable releases first.
- Merge the reviewed release commit into main, then tag it:

  ```powershell
  git switch main
  git pull --ff-only
  git tag -a v1.0.0 -m "WinKick 1.0.0"
  git push origin v1.0.0
  ```

- If approval is configured, approve publication after checking the source
  commit, local test results, and successful hosted build.
- Download the published EXE, compare its hash and verify its provenance with
  the real repo/workflow/tag as described in README.

## Failures and recovery

Do not tag unmerged code, reuse a version, move a published tag, or use
`gh release upload --clobber`. If a workflow fails before creating a release,
fix the underlying problem and use a new reviewed version/tag when source must
change. If a partial **draft** exists, inspect it; a maintainer may remove that
unpublished draft before rerunning the unchanged tag's workflow. The workflow
deliberately refuses to overwrite it automatically.

Do not publish the draft manually to bypass failed gates. Treat any unexpected
asset/hash/attestation mismatch as an incident.

## Limits of reproducibility and signing

`/Brepro` removes ordinary PE timestamp nondeterminism. To check repeatability,
run `.\scripts\check-reproducible.ps1` locally. This is **not** a claim that
different MSVC/Windows SDK/runner versions produce the same bytes. Versions are
recorded in build-info.json; ZIP metadata/provenance timestamps also vary.

No Authenticode certificate is configured. If code signing is added later, keep
the certificate in a protected signing service/environment, sign before the
final checksums/attestations, and compare unsigned builds before signing.
