# Security policy

Dofram sees everything on your screen, so it is built to keep that to itself. Problems that could leak screen contents, or that let other programs misuse Dofram, are treated as security issues.

## What Dofram does to stay safe

- Screenshots never leave your computer unless you paste or save them somewhere yourself. There is no upload, no cloud service, no telemetry and no crash reporting.
- Dofram does not go online on its own. The only network access is the manual *Check for Updates on GitHub* command: one HTTPS request to `api.github.com` for the latest release number. Nothing is downloaded or run, and no information about you or your screenshots is sent.
- The **Box** tool replaces pixels with a solid colour, so redacted information is gone from the saved image. Blur and mosaic only hide it visually and, at small sizes, can sometimes be partly reversed; use Box for passwords, card numbers and IDs.
- Saved images contain only pixels: no metadata about you, your computer or the window you captured.
- Dofram runs with normal user rights. Only installing to or uninstalling from `Program Files` asks for administrator approval, and only when you choose to.

## Reporting a vulnerability

Please do not open a public issue. Use GitHub's private vulnerability reporting on this repository (*Security > Report a vulnerability*) and describe how to reproduce the problem. Please do not include real screenshots with private information. You can expect a first answer within a week.
