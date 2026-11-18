# One-time GitHub setup

The canonical repository is [waki-chin/winkick](https://github.com/waki-chin/winkick).
Use this checklist when setting up a separate repository or reviewing its settings.

## Before the first push

1. Confirm the MIT license and attribution match your ownership/intent.
2. Create an empty repository with default branch **main** (public recommended for
   freely available GitHub provenance attestations).
3. Review the public source and docs. `.local/` contains historical private-machine
   evidence and is ignored; do not force-add it. Binaries are release assets only.
4. For a separate repository, initialize and push locally when ready:

   ```powershell
   git init -b main
   git add .
   git status --short
   git diff --cached --stat
   # Inspect staged content before the first commit.
   git commit -m "Prepare WinKick source and verified release builds"
   git remote add origin https://github.com/OWNER/REPO.git
   git push -u origin main
   ```

   If importing an existing repository instead, use `git add --renormalize .`
   after reviewing `.gitattributes`. Never force-add ignored EXEs/keys/logs.

## Repository settings (cannot be set by files alone)

- Enable Actions, restrict allowed actions as appropriate, and require full-SHA
  pins. Keep default `GITHUB_TOKEN` permissions read-only.
- Protect **main** from force pushes and deletion. If you have collaborators,
  consider requiring PRs and review. There is no PR CI check to require.
- Protect **v*** tags: only release maintainers may create them; block updating
  or deleting published tags. A pushed tag is executable release automation.
- Optionally add `.github/CODEOWNERS` if other people will review changes.
- Create the **release** environment and limit deployments to version tags.
  Configure required approval if your plan supports it. If you are the only
  maintainer, don't enable prevention of self-review unless another reviewer
  can approve. Naming the environment in YAML does **not** configure approval.
- Enable immutable releases if available. The workflow attaches all assets while
  the release is a draft, then publishes. Never move/reuse a published tag.
- Enable private vulnerability reporting, Dependabot alerts, secret scanning and
  push protection where available. Confirm the repository/plan supports artifact
  attestations; private-repository support depends on plan.
- Do not add a PAT or signing key for the normal release path: the workflow uses
  scoped `GITHUB_TOKEN` and short-lived OIDC.
- Run the local test suite on the exact commit you intend to tag. Keep the
  release workflow on main before pushing the first tag.

## Verification after connecting

Test a prerelease tag such as `v1.0.1-rc.1` after setting `VERSION` accordingly
and committing it to main. Confirm the build job only has read access and only
the publishing job requests write and OIDC permissions.

Confirm the release stays a draft on an upload failure, existing release assets
cannot be overwritten by a rerun, provenance verification succeeds for the real
repository/workflow/tag, and the release EXE's embedded catalog matches its
downloaded `tasks.ini`. Hosted Windows GUI/UAC behavior still needs a manual pass.
