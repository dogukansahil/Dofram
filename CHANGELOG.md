# Changelog

All notable changes to Dofram are documented in this file. The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## Versioning

Dofram uses [Semantic Versioning](https://semver.org/) in the form `MAJOR.MINOR.PATCH`:

- **PATCH** (`1.0.0` to `1.0.1`): bug fixes only, no new behaviour.
- **MINOR** (`1.0.1` to `1.1.0`): new features that keep existing settings, shortcuts and command line options working.
- **MAJOR** (`1.4.2` to `2.0.0`): changes that break something users rely on.

Every change that reaches `main` is listed under *Unreleased* first. A release moves those entries under a new version heading, bumps the number in `src/version.h` (and the manifest and the version in `docs/images/badge-release.svg`, the release badge of the README), and is tagged `vMAJOR.MINOR.PATCH`; the CI then publishes `Dofram.exe` and its SHA-256 checksum for that tag. Version `1.0.0` is the first public release.

## [Unreleased]

## [1.0.0] - 2026-11-26

First public version.

### Added

- Tray application with a global hotkey (`Ctrl+Shift+X` by default), single instance and command line options (`--capture`, `--tray`, `--settings`, `--portable`, `--install`, `--uninstall`).
- Frozen-screen area selection over all monitors, per-monitor DPI aware, with an 8x magnifier, pixel coordinates and resize handles.
- In-place editing with movable and resizable objects: blur, mosaic, redaction box, frame, arrow, pen, highlighter and text; colour palette, size levels, undo and redo.
- Separate editor window with zoom, scrolling and Save as (PNG, JPG, BMP).
- Copy to the clipboard as bitmap and PNG; save with a file name pattern; notifications that open the folder.
- Settings dialog, portable mode (`dofram.ini` next to the executable), start with Windows, desktop and Start menu shortcuts.
- Self-installer: install to Program Files with a Start menu shortcut and an *Installed apps* entry, uninstall that can also remove the settings, and an optional offer on first start that can be turned off.
- About box and a manual *Check for Updates on GitHub* command (one HTTPS request, never automatic, nothing downloaded).
- Plain C on Win32 and GDI+, static C runtime, no external dependencies; release builds with Control Flow Guard.
- Off-screen test suite, and repository images (cover and screenshots) drawn in code from made-up content.

[Unreleased]: https://github.com/dogukansahil/Dofram/compare/v1.0.0...HEAD
[1.0.0]: https://github.com/dogukansahil/Dofram/releases/tag/v1.0.0
