# Architecture

Dofram is one Win32 program written in C11. It uses only what ships with Windows: the Win32 API, GDI and GDI+ (through its flat C API, declared by hand in `src/gdip.h`), the common controls and dialogs, the shell, COM for shortcuts and WinHTTP for the manual update check. The C runtime is linked statically.

## Flow of a capture

1. `app.c` owns a hidden message window, the tray icon and the global hotkey. A named mutex keeps one copy running; a second start finds the first window and asks it to capture (or to open settings).
2. On capture, `image.c` copies the whole virtual screen (every monitor) with `BitBlt` and `CAPTUREBLT` into one 32-bit image. The process is per-monitor DPI aware (v2), so these are real pixels.
3. `editor.c` shows that frozen image in a borderless overlay over all monitors. The user selects an area, then edits on the same overlay; or, depending on the settings, the area goes straight to the clipboard, a file, or the separate editor window.
4. When the user copies or saves, the objects are drawn onto a copy of the image and the result is cropped to the area. `image.c` writes PNG, JPEG or BMP with the GDI+ encoders and puts a DIB and a PNG on the clipboard.

## The editor

One implementation, two hosts:

- **Overlay** (zoom 1): the frozen screen, the selection with its handles, the dimmed outside, the magnifier and the toolbar next to the selection.
- **Window**: a normal resizable window for an already cropped image, with zoom, scrolling and Save as.

Edits are a list of `Op` objects (`ops.c`) in **image coordinates**: blur, mosaic, box, frame, arrow, pen, marker and text. The area is just another rectangle, so moving or resizing it never moves the objects, and the output is cropped last. Undo and redo store copies of the object list and the area (up to 200 steps).

While the user drags, only the changed region (plus the objects that touch it) is drawn again; the result always matches a full redraw, which the tests check. Lines, arrows and text use GDI+ anti-aliasing; blur (three box passes), mosaic, the box and the highlighter work directly on the pixels.

The toolbar is drawn by the editor itself (no child controls), so it scales with the monitor's DPI and looks the same in both hosts.

## Settings

`settings.c` reads and writes a small UTF-8 ini file: `%APPDATA%\Dofram\dofram.ini`, or `dofram.ini` next to the executable in portable mode. `settingsdlg.c` is the settings dialog (layout in `dofram.rc`) with a custom key-combination box for the hotkey.

## Installer and updates

`install.c` parses the command line and implements the self-installer. Machine-wide changes (copying to `%ProgramFiles%\Dofram`, the Start menu shortcut, the `Uninstall` and `App Paths` keys under HKLM) are made by an elevated copy of the executable started with `--install` or `--uninstall`. Per-user changes (the start-up entry, the settings file) stay in the non-elevated process, so they land in the right profile. A running installed copy is asked to exit with a private window message; no process is ever killed.

`update.c` runs the manual update check on a worker thread: one HTTPS `GET` to `api.github.com/repos/dogukansahil/Dofram/releases/latest` without cookies or credentials. It reads `tag_name` and `html_url`, compares versions and posts the result back to the tray window. Only release pages of this repository are ever opened.

## Version

`src/version.h` is the single source for the version, name, URL and copyright. The resource script includes it for the version info; `src/dofram.manifest` holds the same number and is updated by hand on release.

## Tests

`tests/test_editor.c` includes `editor.c` and `settingsdlg.c` directly so it can reach their internals. It creates the editor on hidden windows, drives it with synthetic messages (no real input), checks the objects, the area and the output pixels, and writes every rendered frame to `build\test\frames\`. Pure logic (hotkey parsing, settings, effects, command line, install decision, version comparison and the release JSON parsing) is tested without touching the system.

## Images

`tools/makeicon.c` draws the icon, `tools/mkcover.c` the repository cover and `tools/mkshots.c` the README screenshots. `mkshots.c` builds a made-up desktop in code and runs the real editor drawing code over it, so the screenshots show the actual interface without anything from a real screen.

## Rules for new code

- No third-party code, libraries or assets.
- Keep the build warning-free at `/W4`.
- New behaviour comes with a test in `tests/test_editor.c`.
- Anything that changes the system (registry, files outside the build folder) must stay out of the tests.
