# Changelog

## [1.1.0](https://github.com/waki-chin/winkick/releases/tag/v1.1.0)

- Added the optional `successCode` INI key. Tasks can accept one or more
  exit codes, such as `0, 3010` for an installer or `0, 1, 2, 3, 4, 5, 6, 7`
  for Robocopy. Existing tasks still accept only `0` unless configured otherwise.
- Custom success codes survive export and elevation. Run logs still show the
  original exit code.

## [1.0.0](https://github.com/waki-chin/winkick/releases/tag/v1.0.0) — 2026-11-18

- Native x64 GUI/CLI task selection, arguments, guards, UAC handoff, and logging.
- Opt-in application/package-manager catalog and Windows preferences.
- Robust WinGet resolution, explicit installer wait/no-restart flags, and
  two-stage WSL setup.
- Embedded-by-default catalog; remote executable task definitions require opt-in.
- Windows x64 downloads with SHA-256 checksums and build provenance.
