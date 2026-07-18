# Naruto Storm 4 Accessibility Mod Handoff

This file is a handoff for continuing the blind-accessibility mod work in `NARUTO SHIPPUDEN Ultimate Ninja Storm 4`.

## Goal

Make menus and other useful UI text speak through a screen reader for a blind player.

Current speech backend:
- `Tolk.dll`
- verified working with NVDA

Current injection/load approach:
- root-level `XINPUT9_1_0.dll` proxy/framework load
- active root DLL is produced from `ns4moddingapi`

## Current Status

What works:
- The game loads our DLL successfully.
- NVDA speech works from inside `NSUNS4.exe`.
- Main menu speech works.
- Story chapter/event board speech partially works.
- Story submenus now have explicit mappings for:
  - `Display Recap`
  - `Tutorial`
  - `Item List`
  - `Previous Chapter`
  - `Next Chapter`
  - `Play Episode`
  - `Play Event`
- Added controller shortcuts:
  - `Back + X` repeats the last spoken item
  - `Back + Y` speaks a short accessibility help line

What does not currently work safely:
- Battle requirements speech
- General battle/cutscene/playback text speech

Important constraint:
- Attempts to read battle requirements from the current `Hook_MsgToString` path caused instability or crashes before battle/start transitions.

## Active Files

Primary source file:
- `mod\tools\ns4moddingapi\d3dcompiler_47_og\ccGeneralGameFunctions.cpp`

Built DLL output:
- `mod\tools\ns4moddingapi\d3dcompiler_47_og\x64\Release\xinput9_1_0.dll`

Installed active DLL:
- `XINPUT9_1_0.dll`

Other runtime files in game root:
- `xinput9_1_0_o.dll`
- `Tolk.dll`
- `SAAPI64.dll`
- `nvdaControllerClient64.dll`

Logs:
- `storm4_api_messages.log`
- `storm4_api_menu_trace.log`
- `storm4_accessibility.log`

## Build Command

Use MSBuild:

```powershell
& 'C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\amd64\MSBuild.exe' `
  'C:\Users\adels\Documents\games\NARUTO SHIPPUDEN Ultimate Ninja Storm 4\mod\tools\ns4moddingapi\d3dcompiler_47_og\d3dcompiler_47_og.vcxproj' `
  /p:Configuration=Release /p:Platform=x64
```

Then copy:

```powershell
Copy-Item -LiteralPath 'C:\Users\adels\Documents\games\NARUTO SHIPPUDEN Ultimate Ninja Storm 4\mod\tools\ns4moddingapi\d3dcompiler_47_og\x64\Release\xinput9_1_0.dll' `
  -Destination 'C:\Users\adels\Documents\games\NARUTO SHIPPUDEN Ultimate Ninja Storm 4\XINPUT9_1_0.dll' -Force
```

Do not copy while `NSUNS4.exe` is running.

## Reverse-Engineering Findings

The game exposes a lot of UI text through message conversion hooks in `ns4moddingapi`.

Known useful message families:
- `gamemodeselect_*`
- `presence_*`
- `main_outline_*`
- `advMainMenu_*`
- `advStartMenu_*`
- `item_list_*`
- `networkmode_*`
- `onlineMenu_*`
- `PC_network_sys_*`
- `freebattle_*`
- `freemenu_*`
- `battle_preset_*`
- `practice_*`
- `tournament_*`
- `survival_*`
- `collection_*`
- `collect_*`
- `collectTop_*`
- `FinalSpSkillCutIn_*`
- `tutorial_check_*`

Known risky families for this hook path:
- `battle_*`
- `Survival_sys_*`
- `t_battlecondition_*`
- `Battlemission_*`
- `eventcheck_*`
- `battlestartmenu_*`
- `episode*`

These risky families were associated with crashes or instability around pre-battle/cutscene transitions when speech handling touched them.

## Current Speech Logic

Current design in `ccGeneralGameFunctions.cpp`:
- Uses `Hook_MsgToString` and `Hook_MsgToString_Alt`
- Filters for accessibility-relevant message families
- Uses recent controller navigation timing from XInput to gate submenu announcements
- Uses cooldowns and primary/secondary suppression to reduce spam
- Uses Tolk directly in-process

Primary focus currently includes:
- main menu mode-select items
- story chapter/event board items
- explicit story submenu items listed above

Accessibility hotkeys:
- `Back + X`: repeat last announcement
- `Back + Y`: help announcement

## What Caused Problems

These approaches were tried and should be treated carefully:

1. Inline/breakpoint hooks into guessed internal UI functions
- caused crashes
- abandoned in favor of framework/message hook approach

2. Broad “speak any short string”
- caused wrong speech and spam
- especially in online/collection menus

3. Battle requirements speech in current message hook
- strings were present in logs
- but using them in this hook path caused crashes before battle or during transitions

## Good Next Steps

Recommended safe next steps:

1. Test the currently installed stable build.
- Verify launch stability.
- Verify story chapter submenus speak better.
- Verify `Back + X` and `Back + Y`.

2. Keep expanding stable menu coverage only from safe families.
- collection
- free battle
- online
- options
- chapter submenus

3. For battle requirements, do not reuse the current direct approach.
- Use a separate later-stage hook or state-specific path.
- The data exists, but this specific hook location is too fragile during transition.

4. Use logs to drive mappings.
- `storm4_api_messages.log`
- `storm4_api_menu_trace.log`

## Known Message IDs Already Mapped

Examples already handled:
- `gamemodeselect_018` -> Story
- `gamemodeselect_019` -> Adventure
- `gamemodeselect_020` -> Collection
- `gamemodeselect_011` -> Free Battle
- `gamemodeselect_012_pc` -> Online Battle
- `gamemodeselect_026` -> Boruto Story
- `advStartMenu_015` -> Tutorial
- `item_list_000` -> Item List
- `advMainMenu_006` -> Display Recap
- `advMainMenu_012` -> Previous Chapter
- `advMainMenu_013` -> Next Chapter
- `advMainMenu_020` -> Play Episode
- `advMainMenu_021` -> Play Event
- `networkmode_536` -> Customize Character
- `networkmode_541` -> Bingo Book
- `networkmode_002` -> Leaderboards
- `PC_network_sys_000` -> Store
- `battle_preset_000` -> Preset

## User Context

The player is blind and uses a controller.

Practical accessibility priorities:
- reliable menu focus speech
- repeat-last-item shortcut
- stable behavior over aggressive coverage
- avoid speech spam
- avoid anything that destabilizes gameplay transitions

## Current Installed Build

Installed `XINPUT9_1_0.dll` timestamp when this handoff was written:
- April 4, 2026, about 1:36 PM local time

This is intended to be the safer build with:
- battle-requirements speech removed
- story submenu speech improved
- repeat/help shortcuts added
