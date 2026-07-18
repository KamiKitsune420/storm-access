# Storm 4 Accessibility Mod — project home

Self-contained dev folder for the NSUNS4 blind-accessibility mod. Everything needed
to continue lives here; the surrounding game folder is just an install target.

## What's here
- `ns4moddingapi/` — the forked SDK we build. Our code is `Accessibility.cpp/.h`, wired into
  `ccGeneralGameFunctions.cpp` (Hook_MsgToString) and `NS4Framework.cpp` (Main/Update).
- `lib/` — speech libs shipped to the game: Tolk.dll, nvdaControllerClient64.dll, SAAPI64.dll.
- `build-and-install.ps1` — builds the mod and installs runtime files into the game.
- `MESSAGE_ID_REFERENCE.md` — message-ID families from the old Codex build (reference only).
- `logs/` — captured `storm_access.log` runs (`.genuine` = Steam exe, `.dev` = cracked exe).
- `_template/` — HappyStarfish Unity a11y template studied for principles (not code).
- `CLAUDE.md` — working notes.

## Install target = GENUINE Steam copy
`C:\Program Files (x86)\Steam\steamapps\common\NARUTO SHIPPUDEN Ultimate Ninja STORM 4`
(Confirmed working 2026-06-21.) The D: folder is the older CRACKED dev copy. The two
`NSUNS4.exe` binaries differ; the framework pattern-scans the MessageToString hook, so it
works on both — but only the genuine exe is what the user actually plays.

## Build + install (one command)
```powershell
pwsh "./build-and-install.ps1"          # build, then install to the Steam copy
pwsh "./build-and-install.ps1" -SkipBuild   # reinstall current build only
```
Requires VS 2022 + v143 toolset + SDK 10.0.26100. Close the game first (DLL locks while running).

## How the mod works (one paragraph)
xinput9_1_0.dll proxy injects the framework, which hooks the game's `MessageToString`. Every
menu string the game decodes flows through our `OnMessageDecoded`, which speaks it via Tolk,
gated to the active navigation (XInput d-pad/button window) and filtered by message-ID prefix,
with dedupe. Only the message-read hooks are enabled; the framework's content-mod hooks are
disabled (they crashed battle entry). See the project memory for the full design history.

## Known next work
- Decode-once menus (yes/no dialogs, some adventure hubs) are invisible to the passive hook —
  need to read the selection index from game memory / hook the highlight fn (deferred RE task).
- Character select & map/teleport UI not yet covered (would spam without dedicated handling).
- Keyboard fallback shortcuts (controller is XInput, confirmed working, but keyboard users unserved).
