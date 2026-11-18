# Repository preparation decisions

- **Source-only repository:** source/resources, safe fixtures, build/release
  scripts and public documentation are committed. EXEs/downloaded installers,
  build outputs, generated analysis, logs, exports and local diagnostics are not.
- **Private history preserved locally:** old reports/screenshots were moved to
  ignored `.local/history/`, and the pre-preparation EXE to `.local/archive/`.
  A complete pre-cleanup backup was also made outside the project. They are not
  treated as current public product documentation.
- **Predictable directories:** build outputs -> `build/`, packages -> `dist/`,
  test evidence -> `tests/out/`. Root stays source/documentation only.
- **One version:** root VERSION generates an ignored version header used by the
  GUI, CLI, logs, HTTP user agent, and Windows executable resources.
- **Public repository:** the canonical repository is `waki-chin/winkick`.
  Rulesets, environment reviewers and private reporting are repository settings.
- **MIT:** the license and Windows executable resource credit `waki-chin`,
  as approved by the owner.
- **Task integrity:** default tasks now come from the embedded catalog, not a
  mutable placeholder GitHub URL. Explicit trusted remote loads remain possible.
  Network reachability checks still run unless forced offline.
- **Fresh tagged releases:** run full tests locally. One GitHub workflow builds
  only on version tags, checks tag/version/main ancestry and the compiled EXE,
  and packages it. No precompiled binary can be a release input.
- **Least privilege:** the build job is read-only; only a separate, protected
  publisher gets release/OIDC permissions. No source checkout/EXE execution there.
- **Integrity evidence:** full-SHA action pins, immutable artifact IDs, SHA-256,
  build metadata, Sigstore/GitHub provenance, no asset clobbering, draft-first
  upload, optional repository-level immutable releases.
- **No overstated guarantee:** provenance is not Authenticode or a malware audit.
  External installers/scripts are not frozen by the EXE attestation. Hosted
  toolchains remain trust dependencies; repeatability can be checked locally.
