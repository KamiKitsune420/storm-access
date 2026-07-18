# Storm Access — NARUTO STORM 4 screen-reader accessibility mod

Make NARUTO SHIPPUDEN: Ultimate Ninja Storm 4 (PC) playable for blind players by
speaking menu/UI text through a screen reader. This file is the onboarding doc —
read it first.

## User context

- The player is **blind**, uses a **controller**, and runs **NVDA**.
- Output to the user is via screen reader: keep explanations concise; when
  writing prose for them, use lists, not tables.
- The user directs; Claude codes, builds, installs, and explains. The user is the
  only one who can launch the game and report what they hear — there is no way to
  observe in-game behavior from the tools, so every change is verified by the user.

## How it works (architecture)

- We **fork the community modding framework** `ns4moddingapi` (by Zealot Tormunds,
  cloned into `storm access/ns4moddingapi/`). It injects via an
  **`xinput9_1_0.dll` proxy** (the project folder is named `d3dcompiler_47_og`
  for legacy reasons but it builds the xinput proxy; `xinput9_1_0.cpp` DllMain
  loads the real xinput as `xinput9_1_0_o.dll`, then runs `NS4Framework::Main`).
- **The one chokepoint that matters:** the game has a function `MessageToString`
  (v1.09 offset `0xAB8720`, otherwise pattern-scanned) that converts every UI
  message id (e.g. `gamemodeselect_018`) into display text (e.g. "Story"). The
  framework already hooks it in `ccGeneralGameFunctions::Hook_MsgToString`
  (+ `_Alt`). We read every conversion there and speak the relevant ones.
- **Speech**: Tolk (auto-detects NVDA/JAWS/SAPI). We load `Tolk.dll` dynamically
  (no import lib) in our own module.

### Our code

All accessibility code lives in
`ns4moddingapi/d3dcompiler_47_og/Accessibility.cpp` / `.h`
(class `moddingApi::Accessibility`). It is wired in at three points:
- `NS4Framework::Main()` → `Accessibility::Initialize()` (loads Tolk, opens log).
- `NS4Framework::Update()` → `Accessibility::Update()` (polls controller).
- `ccGeneralGameFunctions::Hook_MsgToString` and `_Alt` →
  `Accessibility::OnMessageDecoded(id, decodedText)` just before they return.

### The announcement model (the core logic, in `OnMessageDecoded`)

The game re-translates the focused item when you move, but each highlight
translates a whole **bundle** of strings at once, and background/idle text also
gets translated. To pick out "the focused item" from that noise:

1. **Nav gate** — only announce text decoded within `NAV_WINDOW_MS` (500 ms) of a
   controller input. `Accessibility::Update()` sets `g_lastNavTick` on **any**
   button or left-stick movement (any button, so dialogs opened by face/Back
   buttons also speak).
2. **First-useful-per-navigation** — the focused item translates first in the
   bundle, so announce only the first useful string per nav tick
   (`g_consumedNavTick`); ignore the rest of the bundle.
3. **Question priority** — a string containing `?` (a confirmation question) may
   override the first announcement, because dialogs translate the focused option
   before the question but the question is what matters (`g_questionNavTick`).
4. **Skip junk** — empty strings and `"???"` placeholders are skipped.
5. **Suppress metadata** — `SUPPRESS_IDS` drops strings whose number/value the
   game draws separately (e.g. "Estimated Play Time:  minutes", "Completion: %").
6. **Mode-name map** — `MODE_NAMES` maps main-menu mode ids to short spoken names
   (the big mode titles are graphics, not text; only descriptions are text), and
   queues the description after the name.
7. **Coverage list** — `ANNOUNCE_PREFIXES` is the allowlist of safe menu families.
   Expand it from the debug log as new screens are mapped.
8. **Risky list** — `RISKY_PREFIXES` (battle/transition families) are skipped for
   **speech only** to avoid battle spam. NOTE: this is no longer about crashes
   (see below); it just keeps battle UI quiet until we design battle text on
   purpose. The allowlist OVERRIDES risky: an id in `ANNOUNCE_PREFIXES` speaks
   even if a risky prefix also matches it (e.g. `battlestartmenu_*`, the practice
   pause menu, shares the `battle` prefix but is a deliberate menu we want).
9. **Chrome skip** — `CHROME_IDS` (exact ids: `MSG_Confirm/Cancel/Back/Close/
   Save/Comp/Random`, `option_169`, `PC_option_000`) are footer button-legend
   labels. They decode in every bundle, sometimes FIRST, so if announced they
   steal the focus slot. Never announced. (`MSG_Yes`/`MSG_No` are real options,
   NOT chrome.)
10. **First-useful does NOT consume on a repeat** — if the first decoded item
   equals the last announcement (a re-decoded header/footer), it is skipped
   WITHOUT consuming the nav slot, so a later DIFFERENT string in the same bundle
   still wins. This is what makes lists like jutsu-customization speak instead of
   going silent. (See `OnMessageDecoded`.)
11. **Popups** — `POPUP_PREFIXES` speak IGNORING the nav gate, because popups
   appear with no controller input (e.g. `MSG_AutoSaveWarn`, login bonus, item
   acquired). Deduped on consecutive identical text. Find their ids in the debug
   log's `(popup?)` lines (popup capture logs any text decoded OUTSIDE the nav
   window, once per id per session, risky families excluded).
12. **Character-select fighter NAMING** (rewritten) — the fighter's NAME is a
   portrait graphic (never decoded). On each hover the info panel decodes the
   fighter's signature (`c_jyu_*`/`c_com_*`), ultimate (`c_ult_*`), and form tag
   (`c_cha_*`/`c_costume_*`). We map those ids to a character NAME via `CHAR_NAMES[]`
   in `Accessibility.cpp` and speak e.g. "Sasuke Uchiha, Rinne Sharingan".
   - **Key on the ULTIMATE** (unique per fighter). Signatures are a fallback and
     are only mapped when unique — several are SHARED and are deliberately NOT in
     the table (Rasengan = Naruto/Minato, Fire Ball = Itachi/Sasukes, Water Dragon
     = Zabuza/Suigetsu, Chidori = Sasuke/Kakashi). An unmapped fighter speaks its
     signature jutsu rather than a wrong name.
   - One hover's ids are **buffered** (`g_pc*` under `g_charMtx`) and spoken once
     via `FlushChar()` — on the form tag, the next hover's signature, or a 160 ms
     timeout in `Update()`. Multiple signatures of one hover are merged
     (`CHAR_SAME_HOVER_MS`) so e.g. Chakra-Mode Naruto doesn't blurt "Rasengan".
   - **Fighter vs jutsu-list:** a real fighter hover decodes an ULTIMATE (or a form
     tag); the jutsu-CUSTOMIZATION screen decodes bare signatures with none. So
     "ultimate OR form tag present" ⇒ speak the name; otherwise ⇒ speak the jutsu
     (this preserves the customization screen). Chakra-Mode Naruto decodes NO
     ultimate, so the form tag is what marks it a fighter.
   - **Context** (`g_charSelectUntil`, 6 s) now self-refreshes on ANY char-select
     info id (not just sparse markers) — that fixed the old bug where it expired
     mid-navigation and went silent. Refresh runs before the announce block, so a
     decode after an expiry re-opens the context AND is still announced.
   - **Battle-leak gate:** `league_*` ("Battle") decodes at fight start → sets
     `g_inBattle` (suppresses ALL char-select speech, zeroes the context);
     `characterselect_*`/`MSG_Random` clear it on return to the grid. This stops
     in-fight jutsu-use decodes (lone `c_jyu`) from being spoken.
   - Name style is full/clan ("Naruto Uzumaki", "Kaguya Otsutsuki"); title-only
     characters stay as titles ("Third Raikage"). The id→character map, the full
     108 roster, shared-signature gotchas, and still-unknown ids live in
     **`CHARACTER_JUTSU_MAP.md`** — extend the table from debug logs (decode the
     jutsu TEXT, prefer the ultimate).

## Build

Toolchain: MSBuild 2022. The project ships targeting v141 + Windows SDK
10.0.17763 which are usually NOT installed — override to whatever IS installed
(here v143 + SDK 10.0.26100). One source fix is required and already applied:
`#include <string>` added to `FileParser.h` (newer toolset dropped a transitive
include).

```bash
"/c/Program Files/Microsoft Visual Studio/2022/Community/MSBuild/Current/Bin/amd64/MSBuild.exe" \
  d3dcompiler_47_og.vcxproj \
  //p:Configuration=Release //p:Platform=x64 \
  //p:PlatformToolset=v143 //p:WindowsTargetPlatformVersion=10.0.26100.0 \
  //v:minimal //nologo //clp:ErrorsOnly //m
```
(Run from `ns4moddingapi/d3dcompiler_47_og/`. Adjust toolset/SDK to what
`Get-ChildItem` shows under `VC/Tools/MSVC` and `Windows Kits/10/Include`.)

Output: `x64/Release/xinput9_1_0.dll`.

## Install (into the GAME ROOT, one level up from `storm access`)

All additive; do NOT copy while `NSUNS4.exe` is running.
- built `xinput9_1_0.dll` → game root `xinput9_1_0.dll`
- real xinput (`C:\Windows\System32\xinput9_1_0.dll`) → game root
  `xinput9_1_0_o.dll`  (the proxy LoadLibrary's this — MUST exist)
- `storm access/lib/Tolk.dll`, `nvdaControllerClient64.dll`, `SAAPI64.dll`
  → game root
- `moddingapi/config.ini` with `[General]` + `EnableConsole=0`, and an EMPTY
  `moddingapi/mods/` folder (the mods dir must exist when config is present, or
  the framework's directory_iterator throws).

To disable the mod for an A/B test: rename game-root `xinput9_1_0.dll` →
`xinput9_1_0.dll.disabled` (game falls back to system xinput, no mod).

## Debug logging workflow (how we discover message ids)

- Toggle with **Back + Y** ("debug logging on/off"). Default off.
- Writes `storm_access.log` to the game root.
- It logs every id decoded within the nav window, **once per navigation** — so it
  shows what each individual move re-translates (and whether a move translates
  nothing = a "decode-once" menu). It logs battle-family ids too (logging runs
  before the risky-skip; only *speech* skips risky).
- It ALSO logs `(popup?) id = "text"` for any text decoded OUTSIDE the nav window
  (once per id per session, risky families excluded) — this is how we discover
  popup/auto-notification ids (login bonus, item acquired, autosave notice). To
  capture a boot-time popup, enable logging while it is still on screen, then
  press the d-pad so it re-decodes.
- Workflow: user turns logging on, navigates the target screen slowly, turns it
  off, sends the log; we map the ids into `ANNOUNCE_PREFIXES` / `MODE_NAMES` /
  `SUPPRESS_IDS` / `POPUP_PREFIXES` / `CHARSELECT_INFO_PREFIXES`.

## Controller shortcuts

- **Back + X** — repeat last announcement.
- **Back + Y** — toggle debug logging.

## Hard-won learnings (don't relearn these)

- **The battle-entry crash was the framework's CONTENT hooks, not our message
  reading.** Disabling `CpkLoader`, `SpecialConditionManager`, and
  `PartnerManager` hooks (via `#if 0` in `HookFunctions::InitializeHooks`) fixed
  it. We don't need any of them (they're for content mods). Keep them off.
  A `std::recursive_mutex` was also added around the message hooks for thread
  safety; harmless to keep.
- **Two kinds of menus.** (a) Re-decode-on-move menus (main menu, story/chapter
  lists, free battle, options) — the focused item re-translates when you move, so
  we can follow it. Works great. (b) Decode-once menus (yes/no dialogs; the Boruto
  hub re-decodes only the *description* not the label) — moving a cursor doesn't
  re-translate, so we cannot follow the selection via the message hook. The fix
  for (b) is the big next task: read the selected INDEX from game memory (or hook
  the menu's selection function).
- Main-menu mode titles are **graphics**; only descriptions are text → mapped.
- Some strings have **numbers drawn separately** (play time, completion %, "Move
  to ?") — unrecoverable from the message hook; suppressed.

## Current status

Working: main menu, story chapter/episode list, free battle, options, online,
collection, Boruto/Adventure hub (reads descriptions), confirmation dialog
*questions*, practice/pause menu (`battlestartmenu_*`), **character select — speaks
the hovered fighter's real NAME + form** (id→character table, ~75 of 108 mapped;
see item 12 and `CHARACTER_JUTSU_MAP.md`), jutsu/Ninjutsu customization list, stage
select (`c_sta_*`/`map_icon_*`), autosave-notice popup (`MSG_AutoSaveWarn`, speaks
unprompted). Battles stay quiet (the `league_*` battle-leak gate is confirmed).

Known gaps:
- Character NAMING is ~70% done; remaining fighters just need to appear in a debug
  log so their ultimate id can be mapped. A few unknowns are listed in
  `CHARACTER_JUTSU_MAP.md` ("Perfume Spray" kunoichi, child Obito vs child Sasuke).
- yes/no selection movement (decode-once) — still open.
- Login-bonus / item-acquired popups: mechanism is built (`POPUP_PREFIXES`) but
  ids not yet captured (need a `(popup?)` log line while one is on screen).
- Battle result screen (`battleresult_*`, "Check Acquired Items") — risky-blocked.
- map/teleport UI (`map_telop_*`, `map_jump_*`) and overworld roaming navigation.
- Possible unconfirmed "speaks twice on mode load" report — could not reproduce
  from the log; the chrome-skip change may have already fixed it. Needs the exact
  repeated words from the user to trace.

## Roadmap / next steps

1. Decode-once solution: read focused index from memory → fixes yes/no dialogs
   and gives real labels everywhere (incl. real character NAMES). Biggest win.
2. Capture login-bonus / item popup ids → add to `POPUP_PREFIXES`.
3. Battle text / battle result screen (deliberately, off-thread/buffered — never
   file I/O in the hook during a battle transition).
4. Roaming/overworld navigation guidance.

## Migration to a different / genuine copy of the game

This was developed against one install. Moving to another copy mainly means
repeating the **Install** steps in that copy's root. Notes:
- The framework **pattern-scans** for the message function on non-1.09 versions,
  so it adapts to different builds; the hardcoded `0xAB8720` path is only for
  v1.09. If a different version misbehaves, check `gameVersion` detection and the
  scan results (enable the console: `moddingapi/config.ini` `EnableConsole=1`).
- Steam vs other distributions: the `.vcxproj` Release|x64 has a stale
  `AdditionalLibraryDirectories` pointing at a Steam path; it built fine without
  it here, but if linking fails on another machine, fix or remove that path.
- Everything in `storm access/` (the SDK fork + `lib/` + this file) is portable;
  copy the whole folder and rebuild.

## Key files

- `ns4moddingapi/d3dcompiler_47_og/Accessibility.cpp` / `.h` — all our logic.
- `ns4moddingapi/d3dcompiler_47_og/ccGeneralGameFunctions.cpp` — message hooks
  (our `OnMessageDecoded` call + recursive_mutex).
- `ns4moddingapi/d3dcompiler_47_og/HookFunctions.cpp` — `InitializeHooks`
  (content hooks disabled here).
- `ns4moddingapi/d3dcompiler_47_og/NS4Framework.cpp` — Init/Update wiring.
- `storm access/CHARACTER_JUTSU_MAP.md` — id→character map, full roster, and the
  still-unknown ids for character-select naming (see item 12).
- `storm access/lib/` — Tolk.dll, nvdaControllerClient64.dll, SAAPI64.dll.
- `storm_access.log` (game root, runtime) — debug log.

## GitHub

Public repo: https://github.com/KamiKitsune420/storm-access (branch `main`). We
develop in the Steam game-folder working copy; the git repo is a separate copy at
`C:\Users\adels\Documents\GitHub\storm access`. To push: copy changed files from
the working copy into the repo copy, then commit + push there. `.gitignore`
excludes the Ghidra project (`gidra proj/`), build outputs, and logs.
