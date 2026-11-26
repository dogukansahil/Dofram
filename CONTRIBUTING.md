# Contributing to Dofram

Thanks for your interest in Dofram! Bug reports, ideas and pull requests are all welcome.

## Before you start

- For anything larger than a small fix, please open an issue first so we can agree on the approach.
- Read [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md). The most important rules are:
  - **No third-party code or libraries.** Only what Windows provides (Win32, GDI+, the system DLLs) and the C standard library. Do not copy code from other projects, even small snippets, and do not add assets (icons, fonts, images, sounds) you did not create yourself.
  - **One small executable.** The C runtime is linked statically and nothing is installed next to `Dofram.exe`.

## Building and testing

Run `build.bat test` from the repository root. It builds `build\Dofram.exe` and then the off-screen test program, which drives the editor on hidden windows and writes every rendered frame to `build\test\frames\`. Look at the frames when you change anything visual. Every change to the editor, the edit objects or the settings should come with a test case in `tests/test_editor.c`.

`build.bat` stops with a message if `build\Dofram.exe` is in use. Exit Dofram from its tray menu first, or test with a copy that runs from another folder.

## Code style

- C11, compiled with MSVC at `/W4`. The build must stay warning-free.
- 4 spaces, no tabs, braces on the same line except for function bodies.
- Keep functions small and names descriptive; comment *why*, not *what*.
- New source files start with the same license header as the existing ones (`SPDX-License-Identifier: GPL-3.0-only`).
- User interface text is English, short and plain.

## Pull requests

- One topic per pull request, with a clear description of what changes for the user.
- Include screenshots for visual changes. Use made-up content only: no private data, no other products' names or logos.
- Update `CHANGELOG.md` under *Unreleased*.

## Versions and releases

Dofram follows Semantic Versioning; the rules are at the top of [CHANGELOG.md](CHANGELOG.md). In short: fixes bump the patch number, new features the minor number, breaking changes the major number. To release:

1. Move the *Unreleased* entries in `CHANGELOG.md` under the new version and date.
2. Update the numbers in `src/version.h` and the `version` attribute in `src/dofram.manifest`.
3. Commit, then tag `vX.Y.Z` and push the tag. The CI builds `Dofram.exe` and attaches it, with its SHA-256 checksum, to the GitHub release.

The repository images are drawn in code: `build.bat cover` redraws `docs/images/cover.png` and `build.bat shots` redraws the screenshots. On GitHub, upload the cover under *Settings > General > Social preview*.

## License of contributions

By submitting a contribution you agree that it is your own original work and that it is licensed under the [GNU GPL version 3](LICENSE) (`GPL-3.0-only`), the same license as the rest of the project.
