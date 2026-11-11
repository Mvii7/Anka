# Dozer

**Dozer — a Nintendo Switch 2 emulator for Windows, tuned for Donkey Kong Bananza (more games may follow).** A fork of
[Anka](https://github.com/EddizEge/Anka) by EddizEge.

Not affiliated with Nintendo or the Eglantine project; please report problems in this
repository's Issues, not to them. https://github.com/Mvii7/Dozer/issues

**Support is very limited.** Dozer is currently tuned for one game, Donkey Kong Bananza. Other
games are not supported yet. More may be added later, and any game Dozer is tuned for will be listed
here.

## At a glance

Dozer runs on its own: unzip it into a new, empty folder (see "Install"). It ships two programs:

- **`dozer.exe`**, the menu program: a Qt front end (game list, menus, settings) with Dozer's
  settings built in and a **Tuning** tab in its settings. This is the easy way.
- **`dozer-cli.exe`**, the program without menus (SDL), with launchers (`start-*.bat`) that start
  it with the recommended settings.

- This is a preview build with no version number yet: the commit is in the window title and the
  log.
- Source: https://github.com/Mvii7/Dozer (GPLv3, see "License").
- Changes: `CHANGELOG_DOZER.md`. Every switch: `dist/nx2_switches.txt`.

## What you need

- Windows 10 or 11, 64-bit.
- Your own **keys, firmware and game dumps**. Dozer provides none of these and never will; only
  use what you legally own.
- Nothing else to install: the zip carries the Qt 6.8.3 DLLs and plugins `dozer.exe` needs and
  the Microsoft Visual C++ runtime DLLs. (Should Windows still report a missing
  `VCRUNTIME140.dll` or `MSVCP140.dll`, install the Microsoft Visual C++ 2015-2022 x64 runtime.)
- A GPU with a current **Vulkan** driver. Only an NVIDIA RTX 4060 Ti (Anka's author) and an RTX 5070 Ti (Dozer's maintainer) were tried.
- An x86-64 CPU. The build asks for no AVX2 (the only AVX2 code is in the Opus audio codec,
  which checks the CPU at run time), but only a Ryzen 7 5700X was tried. Many changes speed up
  the GPU thread on a fast desktop CPU; on a slow CPU expect less.
- Multicore CPU emulation on (the default). The frame pacing switches need it.
- Optional: RivaTuner Statistics Server (RTSS) for the on-screen FPS / speed display.

## Download and check

Download the latest release for your system from the Releases page
(https://github.com/Mvii7/Dozer/releases). The program is not signed: Windows SmartScreen may warn
the first time.

## Install

This is an experimental build with no warranty (see "License").

1. Unzip the release into a new, empty folder. If it has no `user` folder, make an empty one next
   to `dozer.exe` (portable mode: settings, keys, firmware and saves stay in it).
2. Run **`dozer.exe`** (its window title is "Dozer").
3. Install your own keys and firmware as with other Eden-based emulators (Dozer provides none;
   only use what you legally own).

Then add your game folder to the game list ("Add New Game Directory") and start a game.

Game files: `.nsp`, `.xci`, `.nsz`, `.xcz` and **`.nup`** (see "NUP files").

### NUP files

Dozer reads `.nup` files: NUP, the "Unencrypted (Archival) Package" of the Eglantine nx2
project, one file that holds a game's program, its update and its metadata (name, icon,
version). Open one with **File > Load File**, or keep it in a game folder of the game list (it
shows its name, icon and version), or put it in the `Games` folder for the launchers;
`dozer-cli.exe` opens a `.nup` path too. A Donkey Kong Bananza `.nup` gets Dozer's settings for
that game like any other format. Dozer only reads NUP: it does not make, edit or extract one.
Only use NUP files made from your own dumps.

Compressed parts are decompressed on use; a cache of 512 MiB keeps them
(`EDEN_NUP_CACHE_MIB=<n>`, 64 or more, sets another size), and a part larger than half of it is
decompressed once into `user\cache\nup` (removed when the game closes: it needs that much free
disk space).

The zip holds `dozer.exe`, `dozer-cli.exe`, the Qt DLLs and plugins `dozer.exe` needs, the Visual
C++ runtime DLLs, the launchers and an empty `user\` folder. Because of that `user\` folder Dozer
starts in portable mode: it keeps its settings (`user\config\qt-config.ini`), keys, firmware,
saves (`user\nand`), shader caches and log (`user\log\eden_log.txt`) in it, and does not use
`%APPDATA%\eden` or another emulator's folder (the one exception: `dozer.exe` also reads a
`nx2_switches.txt` in `%APPDATA%\eden`, if you made one; see "Switches set outside win").

Settings: **Emulation > Configure... > Tuning**. On the first start the preset is
**Recommended**, and Dozer also turns *Enable Reactive Flushing* off in
its `qt-config.ini` (Graphics > Advanced).

GPU Mode (GPU accuracy) starts at **Fast** (Low), as in newer Eden versions (2.0.0 started at
Accurate, High, on desktop); our measurements used Fast. If a game shows image errors, set
Graphics > Advanced > GPU Mode to Accurate for that game (its Properties) or globally.

### The Tuning tab

At the top of the tab is a **Preset** box; a one-line description of the chosen preset is shown
under it.

| Preset | What it sets | Same as |
|---|---|---|
| **Recommended** (the first start's choice) | all eight settings below on, *Enable Reactive Flushing* off (Graphics > Advanced): the recommended setup (with GPU Mode Fast, the default). Dozer's first start also turns Reactive Flushing off in `qt-config.ini` by itself; choosing Recommended later turns it off too | `start-recommended.bat` |
| **Recommended + Reactive Flushing** | all eight on, Reactive Flushing on, barrier feedback loops on (Graphics > Advanced; Reactive Flushing needs it) and `EDEN_NX2_RX_RT_FIT=3`. Experimental: slower at the game's opening and faster in open areas in Anka's author's tests. ("Reactive flushing") | `start-reactive-experimental.bat` |
| **Safe** | all eight off, Frame generation off, and the GPU shortcuts Dozer keeps on by default off too (`EDEN_NX2_LIGHT_DRAW=0` and the rest of `start-safe.bat`'s list), in every game. Other graphics settings such as Reactive Flushing stay as they are; Dozer's fixes and its changes without a switch stay in | `start-safe.bat` |
| **Custom** | chosen by itself when you change a box by hand | - |

Recommended is made for Donkey Kong Bananza only. If another game shows image or speed
problems, try Safe first and report it with the game's name.

**Frame generation.** The tab also has a **Frame generation** box with three choices:

| Choice | What it does |
|---|---|
| **Off** (the default) | frame generation off |
| **2x** | FSR3, one generated frame per real frame (fixed). On a 60 Hz screen it holds the game at 30 fps |
| **Dynamic** | FSR3 with a 120 fps target, capped at the screen's refresh rate when it is known. It steps between 1x and 4x only while the screen can show the extra frames and the game's own frame rate holds: a step that lowers the game's frame rate by more than 8% is undone |
| *Other (Graphics tab)* | not a choice: shown when the Graphics tab holds another frame generation setup (LSFG, or another multiplier or target frame rate), which the Tuning tab leaves as it is |

Generated frames need a screen faster than the game's frame rate; on a 60 Hz screen they only
help below 30 fps. The presets do not change this box, except Safe, which sets it to Off. The
box writes the Graphics tab's frame generation settings (*Enable Frame Generation*, the type
FSR3, and the multiplier or the target frame rate), so you see the same choice there.

Below the preset are the eight settings, in two groups:

| Setting | Switches it sets (`EDEN_NX2_...`) | Group |
|---|---|---|
| Split GPU thread | `BACK_STAGES=1`, `BACK_CUTS=a` | All games |
| Acquire peek | `ACQUIRE_PEEK=1` | All games |
| Same-value filter in the macro JIT | `JIT_SAME=1` | All games |
| Lean front macro path | `FRONT_MACRO_LEAN=1` | All games |
| Timed frame generation | `FG_TIMED=1` (only does something with frame generation on) | All games |
| FPS overlay in RivaTuner | `OSD=1` (RTSS, if it runs) | All games |
| Dynamic frame rate | `DK_DYN=1` | Donkey Kong Bananza only (off in other games) |
| Frame pacing | `PRESENT_INTERVAL_WORD=12:16:4`, `MODE_PACE=2`, `PACE_SPIN_US=1500` | Donkey Kong Bananza only (off in other games) |

These are the switches `start-recommended.bat` sets ("Launchers", "Main switches"). The program
sets them in its own environment just before a game starts. The two Bananza settings are set
only when the game's title id is 040074A01BF12000 (the one the dynamic frame rate patch checks);
for any other game they stay off. The log says which were set ("Dozer settings for <title id>
...").

**When a change takes effect.** The switches are read once per session. The settings take effect
at the first game you start, so before the first game there is no question: they simply apply to
it. If you change them with **OK** or **Apply** after a game was already started in this
session, Dozer asks "Restart Dozer now to apply?": **Restart Now** closes Dozer and opens it again,
with the running game if one is running (unsaved game progress is lost), otherwise with the same
command line; **Later** keeps the session's switches. When you start a game that needs other
switches than the session's first game (Bananza after another game, or the other way round),
Dozer asks: **Restart** (opens again with that game), **Start Anyway** (keeps the session's
switches) or **Cancel**.

**Graphics settings while a game runs.** Reactive Flushing (changed through a preset) and Frame
generation are graphics settings, and those cannot change in a running game. If you change them
while a game runs, Dozer keeps the change and applies it when you press **Restart Now**, or when
the game stops if you chose **Later**; the tab says so.

**Switches set outside win.** A switch set outside the program (in your environment, in a `.bat`
of your own that starts `dozer.exe`, or in a `nx2_switches.txt` next to `dozer.exe` or in
`%APPDATA%\eden`) is never changed by Dozer. A box whose switches are all set outside is greyed
out; when only some are (one of the three frame pacing ones, say), the box sets the rest. The
tooltip names the variables set outside, with their values.

The tab's settings are kept in the `[Dozer]` group of `user\config\qt-config.ini`.

**To uninstall,** delete the Dozer folder. **Move out what you want to keep first:** your game
files in `Games`, and the `user` folder (your keys, firmware and saves).

### The second way: dozer-cli and the launchers

1. Put your game file (`.nsp`, `.xci`, `.nsz`, `.xcz` or `.nup`) in the `Games` folder (subfolders
   are fine), or drag it onto the launcher instead.
2. Double-click `start-recommended.bat`.

**On the first start** the launcher prepares the settings. dozer-cli uses the same `user\` folder
as `dozer.exe` (the one next to the programs): the same keys, firmware and saves. It keeps its
settings in `user\config\sdl2-config.ini`, apart from the menu program's `qt-config.ini`. When
`sdl2-config.ini` does not exist yet, the launcher makes it as a copy of `qt-config.ini`, so the
controller and settings you set up in `dozer.exe` come along, and turns reactive flushing off in
the copy (the recommended setup, see "Reactive flushing"). It never changes an existing
`sdl2-config.ini`, nor `qt-config.ini`. The editing is done by `dozer-config.ps1` next to the
launchers, a small, readable PowerShell script; without it the launchers skip this step and
start the game. If neither file exists, the launcher stops with a message: start `dozer.exe`
once and set up your controller there, then start the launcher again. With more than one game
in `Games`, the launcher lists them and asks for a number. The launchers' messages start with
`[Dozer]`. (Without a `user\` folder next to them, the launchers and dozer-cli would use
`%APPDATA%\eden` instead; keep the folder.)

The game starts in full screen; dozer-cli has no menu. **Quit with Alt+F4.**

## Launchers

| Launcher | When to use it |
|---|---|
| `start-recommended.bat` | **Recommended.** The recommended settings (Donkey Kong Bananza). |
| `start-reactive-experimental.bat` | **Experimental.** Reactive flushing on: slower at the game's opening and faster in open areas in Anka's author's tests. If you see image errors, go back to the recommended one. |
| `start-safe.bat` | The GPU shortcuts Dozer keeps on by default off, the opt-in switches that change play cleared; Dozer's fixes and its changes without a switch stay in: for troubleshooting and for games other than Bananza. |

A launcher starts, in this order: a game file dragged onto it, the `GAME=` line, or the game in
the `Games` folder next to it (with several, it asks which one). To change a line, open the
`.bat` in a text editor (right click, Edit). The top of each launcher has the lines meant to be
changed:

| Line | Meaning |
|---|---|
| `GAME=` | the game to start (a dragged file wins; this line wins over `Games`) |
| `CONFIG=` | another `sdl2-config` .ini for this launcher; empty = automatic (above; the reactive launcher uses `sdl2-config-reactive.ini`) |
| `FULLSCREEN=-f` | full screen; empty = window |
| `DK_DYN=1` | Bananza dynamic frame rate patch (below); `0` = off |
| `OSD=1` | FPS / speed display through RTSS; `0` = off |

(`DK_DYN=` and `OSD=` are in the recommended and reactive launchers only.) If the path of your
game contains `&`, `^`, `,`, `;` or `=` but no space, Windows passes a dragged file badly: put
the game in `Games` or write it in the `GAME=` line instead.
A launcher stops with a message when its `CONFIG=` file does not exist (dozer-cli would quietly
make a new one with default settings), and keeps its window open when the emulator exits with an
error. Take the launchers from the release zip: they need Windows line endings (CRLF).

- **`start-recommended.bat`** - the settings we play with (Donkey Kong Bananza):
  the GPU thread's back as stages (`BACK_STAGES=1`, `BACK_CUTS=a`), `ACQUIRE_PEEK=1`,
  `JIT_SAME=1`, `FRONT_MACRO_LEAN=1`; Bananza's own 60/30/20 fps choice followed at the right
  speed (`PRESENT_INTERVAL_WORD=12:16:4`, `MODE_PACE=2`, `PACE_SPIN_US=1500`); the dynamic
  frame rate patch (`DK_DYN=1`; while it is applied, the two pacing lines above are set aside:
  they act with `DK_DYN=0` or when the patch does not apply); timed FSR3 generated frames
  (`FG_TIMED=1`, only does something with frame generation on); the RTSS display (`OSD=1`). The
  Bananza-specific lines are marked; for other games use the safe launcher, or remove them. It
  is meant for reactive flushing **off**: it turns it off in an `sdl2-config.ini` it makes,
  and only prints a note when an existing one has it on ("Reactive flushing").
- **`start-safe.bat`** - for troubleshooting and for games other than Bananza: Dozer's shortcut
  switches that are on by default set to `0`, the opt-in ones that change play cleared (a few
  diagnostic ones, such as `AUTO_INPUT_FILE` or `VI2_REPLY`, are not cleared: do not set them in
  your system environment). Use it to see whether a problem comes from these switches. Dozer's
  fixes and its changes without a switch (the accuracy fixes and upstream picks in the
  changelog) stay in.
- **`start-reactive-experimental.bat`** - EXPERIMENTAL, the second choice next to the
  recommended one: the recommended settings with reactive flushing on, plus `RX_RT_FIT=3` (one
  image for Bananza's opening render target instead of a copy per draw). In Anka's author's
  tests it trades the opening for open areas: slower at the game's opening, faster in open areas,
  against the same build with reactive flushing off. Not checked yet: together with frame generation, and the
  image with `RX_RT_FIT=3`. Reactive flushing is a config setting, not a switch: with `CONFIG=`
  empty this launcher uses `config\sdl2-config-reactive.ini`, which it makes once as a copy of
  `sdl2-config.ini` with reactive flushing and `barrier_feedback_loops` on ("Reactive flushing"
  below). With a `CONFIG=` file that has reactive flushing off, `RX_RT_FIT=3` still acts, a
  combination that was not tested.

dozer-cli reads the switches only from its environment (the launchers); it does not read the
Tuning tab. `nx2_switches_DOZER.txt` lists every switch with its default and what it does. Under
that name no program reads it: `dozer.exe` (not dozer-cli) reads a `nx2_switches.txt` next to it
and one in `%APPDATA%\eden`, so copy the lines you want into a `nx2_switches.txt` of your own.

## Main switches

All are environment variables named `EDEN_NX2_<NAME>`. The full list (about 230 new names, most
of them diagnostics or tuning knobs) is in `nx2_switches_DOZER.txt`. "Default" is dozer-cli
without a launcher; the menu program sets the recommended ones from its Tuning tab.

| Switch | Default | What it does |
|---|---|---|
| `BACK_STAGES=1`, `BACK_CUTS=a` | off | the split GPU thread's back runs as a pipeline of stages |
| `ACQUIRE_PEEK=1` | off | a GPU semaphore wait already met goes on without draining the back |
| `JIT_SAME=1`, `FRONT_MACRO_LEAN=1` | off | the front's macro JIT drops writes that change nothing |
| `PRESENT_INTERVAL_WORD=12:16:4` | off | presents follow the game's own frame interval (Bananza) |
| `MODE_PACE=2`, `PACE_SPIN_US=1500` | off | steers Bananza's 60/30/20 fps mode by frame timing |
| `DK_DYN=1` | off | Bananza dynamic frame rate patch, in memory (below) |
| `DK_DYN_MIN_FPS=<n>` | 40 | the patch's lower limit |
| `FG_TIMED=1` | off | FSR3 generated frames blended for the time they are shown |
| `OSD=1` | off | game FPS, display FPS (with frame generation's multiplier) and speed in RTSS |
| `LIGHT_DRAW`, `LIGHT_UNIFORM`, `UBO_*`, `SYNC_*`, `VERTEX_*`, `RESET_*`, ... (about 60) | **on** | per-draw shortcuts on the GPU thread; `=0` is the 2.0.0 code path, many accept `=2` to check themselves and log |
| `HLE_INDIRECT` | off | host GPU reads Bananza's indirect records; that buffer still holds the debris reset's zero count, so the floor piece is never drawn. `=1` opts in |
| `INDIRECT_BATCH`, `PBFLUSH_WINDOW_KIB` | **on** | consecutive indirect draws merge; GPU-written macro parameters flush in 256 KiB windows |
| `RX_RT_FIT=3` | off | meant for reactive flushing (it acts without it too, not measured that way): Bananza's opening renders into one image instead of copying it per draw, ordered by a barrier inside the render pass (experimental; `=1` ends the render pass instead, slower; both need `barrier_feedback_loops=true`) |
| `FLUSH_FIRST_READ`, `READ_LATCH_AGE`, `RX_ONE_SUBMIT`, `RX_COALESCE`, `RX_PITCH_FORCED` | off | reactive flushing experiments; no gain measured, not recommended |
| `RX_MASKED_TARGETS` | off | **known defect, do not turn on** |
| `RX_COALESCE_LOOSE` | off | diagnostic only (it weakens the ordering reactive flushing gives): not for play |

## Donkey Kong Bananza dynamic frame rate (DK-DYN)

`EDEN_NX2_DK_DYN=1` applies a game-specific patch: when Bananza's main module is loaded, a few
instructions of the game's frame rate controller are rewritten **in memory** so that the game
may run at any rate from 40 to 60 fps instead of dropping to 30, with its clock stepped by the
real frame time (the game's speed stays right down to 24 fps). It is a port of the
`dk_dynamic_fps` patch of the ZendNevs fork (v2.0.5). Nothing is written to disk, no game file is
changed, and with the switch unset no module is touched. It only applies to Bananza (title
040074A01BF12000) and only if the expected instructions are found. `DK_DYN_MIN_FPS=<1-60>`
moves the 40 fps limit.

It is off by default in dozer-cli; `start-recommended.bat` turns it on (`DK_DYN=1` at its top),
and so does the menu program's Tuning tab (Dynamic frame rate, on in the Recommended presets,
Bananza only). With it the game's frame time varies by design (any rate between 40 and 60, in
steps of 2); below 40 fps the game still drops to its 30 fps rule. While it is applied,
`PRESENT_INTERVAL_WORD` and `MODE_PACE` are set aside and presents are paced to every vblank.

## Frame generation (optional)

PokeDinner-Eglantine 2.0.0 already has frame generation; Dozer adds `FG_TIMED`, shows the
multiplier on the OSD and has a **Frame generation** box in its Tuning tab ("The Tuning tab").
Two kinds exist:

- **FSR3** (built in, the default kind). This is the recommended kind.
- **LSFG** (Lossless Scaling frame generation). It needs `Lossless.dll` from the paid
  *Lossless Scaling* app. **Dozer does not include it**; put your own copy in the user folder's
  `lossless\` folder if you want this kind.

In the menu program, choose 2x or Dynamic in the Tuning tab's Frame generation box (it writes the
Graphics tab's frame generation settings, which you can also set there). To turn FSR3
frame generation on for dozer-cli, close the program and add these lines under `[Renderer]` in
`config\sdl2-config.ini` (the `\default=false` lines make dozer-cli read the value):

```ini
frame_gen\default=false
frame_gen=true
frame_gen_target_rate\default=false
frame_gen_target_rate=120
```

`frame_gen_target_rate` is the frame rate frame generation aims for (we use 120 on a 120 Hz+
display; `0` uses `frame_gen_multiplier`, 2-4). `frame_gen_type=0` would pick LSFG.

## Reactive flushing

`use_reactive_flushing` (`[Renderer]`) copies data the GPU wrote back to game memory when the
game reads it. **The program's default on desktop is on.** Dozer's recommended setup has it **off**: in Anka's
author's tests, turning it on helped in open areas and hurt at the game's opening. Look at your `sdl2-config.ini`:

```ini
use_reactive_flushing\default=false
use_reactive_flushing=false
```

matches our recommended setup; `=true` (or no such lines) is reactive flushing on. In the menu
program it is *Enable Reactive Flushing* under Graphics > Advanced: the Tuning tab's
**Recommended** preset turns it off (also on Dozer's first start), **Recommended + Reactive
Flushing** turns it on with barrier feedback loops (and `RX_RT_FIT=3`), **Safe** leaves it as it
is. The launchers take care of it for dozer-cli. `start-recommended.bat` writes
these two lines into the `sdl2-config.ini` it makes on the first start; it never changes an
existing one, it only prints a note when reactive flushing is on there (change the two lines by
hand, with the program closed, for the recommended setup). `start-reactive-experimental.bat` uses
its own copy, `config\sdl2-config-reactive.ini`, made once from `sdl2-config.ini` with
`use_reactive_flushing=true` and `barrier_feedback_loops=true`. A setting changed later in one
of the two files does not reach the other: delete `sdl2-config-reactive.ini` and the reactive
launcher makes it again.

This release adds switches for reactive flushing (all off by default; `nx2_switches_DOZER.txt`,
"reactive flushing"). `RX_RT_FIT` targets the cost found at the game's opening: there Bananza
renders into a 2112x1179 image while it samples the same memory as a 2095x1179 one, and with
reactive flushing every draw copied one into the other. With the switch the two are one image,
and a draw that reads it has to come after the draws that wrote it: `=1` ends the render pass
before such a draw; `=3` records a barrier inside the render pass instead (color attachment
writes before fragment shader reads, by region), so the render pass goes on. Both need
`barrier_feedback_loops=true` under `[Renderer]` (the switch does nothing without it).

In Anka's author's tests `RX_RT_FIT=3` was clearly better than `=1` at the opening and the same in the
open area. Against reactive flushing off it is a trade that depends on the scene. That is why there
are two setups for play, as launchers and as presets of the Tuning tab: recommended (reactive
flushing off) and reactive experimental (on, with `RX_RT_FIT=3`). **Experimental:** reactive flushing was **never measured together with
frame generation**, and the image with `RX_RT_FIT=3` has not been checked by eye yet (its barrier
inside the render pass covers a fragment reading the pixel it renders, which is what the Vulkan
spec defines for feedback loops). If you see wrong images with it, set `RX_RT_FIT=1` in the
launcher or use the recommended one. Do not turn on `RX_MASKED_TARGETS` (known defect: a texture
blit ignores the mask) or `RX_COALESCE_LOOSE` (diagnostic only).

## Known issues

- **A game started from the Home Menu applet** in `dozer.exe` runs with the session's settings;
  Dozer does not ask about a restart there.
- **Restart starts the game normally**: with its per-game settings and its first program.
- The launchers copy `qt-config.ini` once, on the first start; settings changed in the menu
  programs later do not reach dozer-cli (`sdl2-config.ini`). To copy them again, rename
  `sdl2-config.ini` (and `sdl2-config-reactive.ini`) and start a launcher.
- **Black blocks with a bright edge** appear in some places. They were seen in a session with
  frame generation and, before that, without it; the likely cause is an older rendering issue of
  this game (also reported for upstream), not frame generation, but it is not settled. If you
  see them, note whether they stay in place in the world, try without frame generation and with
  `start-safe.bat` (or the Safe preset), and report where.
- **Reactive flushing** (the program's default; the recommended launcher turns it off only in a
  `sdl2-config.ini` it makes, the Recommended preset, also on Dozer's first start, in
  `qt-config.ini`) can cost FPS at the game's opening; its new switches were not tried
  with frame generation.
- **AMD and Intel GPUs were not tried**; neither were other games. The Bananza switches
  (`PRESENT_INTERVAL_WORD`, `MODE_PACE`, `DK_DYN`) are meant for Bananza only.
- The OSD writes to RTSS's shared memory; if the program is killed (Task Manager), the text can
  stay on screen until RTSS is restarted.
- A program built with profile-guided optimization is fastest on the paths it was trained on.
  This release reuses the profile of an earlier training run (commit `c8bbd32fe9`, 3 Nov:
  Bananza with the switches of that day, before `DK_DYN`, `FG_TIMED` and `RX_RT_FIT`, reactive
  flushing off). Newer and other paths work, maybe a little slower. Both programs use it; the
  menu program's own Qt code gets no profile.

## AI disclosure

Much of this fork's code, its commit messages and these documents were written with AI coding
assistants (Claude, by Anthropic) working under the direction of the human maintainer, who decided
what to try, played and tested the builds, and is responsible for them. Commits made with an
assistant say so in their trailers, as the Eden project's AI policy asks (`docs/policies/AI.md`).
This fork is published on its own and is not submitted upstream. The measurements above are Anka's
author's. The upstream commits picked into this fork are listed with their authors in the changelog.

## Credits

**Built on.** Dozer is a fork of Anka (EddizEge) and builds on Eden, Eglantine (the PokeDinner fork by Franseven), yuzu and
Sudachi; their copyrights and credits are kept. It is not an official release of those
projects. Anka's base is PokeDinner-a-Eglantine-Fork 2.0.0, commit `70ecb39ef0`
(https://git.eden-emu.dev/Franseven/PokeDinner-a-Eglantine-Fork). The DK-DYN patch is a port of
`dk_dynamic_fps` from the ZendNevs fork, and the upstream commits picked into Dozer are listed
with their authors in the changelog.

## License

Like Anka, Eden and PokeDinner-Eglantine, Dozer is licensed under the **GNU General Public License
v3.0 or later** (`LICENSE.txt`). It is a modified version of Anka (EddizEge), which is a modified
version of PokeDinner-Eglantine 2.0.0; what was changed is in `CHANGELOG_DOZER.md` and in git. The
source of every build is this repository at the build's commit: https://github.com/Mvii7/Dozer. The
Dozer logo (`dist/dozer.ico`, the program icon, and
`dist/qt_themes/default/icons/256x256/dozer.png`, used for the window and the About box: a D whose
flat edge is a bulldozer blade) is drawn by `dist/dozer/dozer_logo.py` and is licensed like the
code under GPL-3.0-or-later. Third-party components keep their own licenses; their texts are in
`LICENSES\`. The menu program links Qt 6.8.3 dynamically (Qt Base and Qt SVG under LGPL-3.0, Qt
Charts under GPL-3.0): the Qt DLLs and plugins in a build are the official, unmodified Qt 6.8.3
binaries, and you may replace them with your own build of Qt 6.8.3. QuaZip (LGPL-2.1 with a static
linking exception) is compiled into it. FFmpeg, OpenSSL, SDL3 and sirit are linked from prebuilt
packages; FFmpeg (LGPL-2.1-or-later, patched) is linked statically: to use a modified FFmpeg, build
it as described in `BUILDING_DOZER.md` and link the program against it. The OpenSSL package also
compiles in Mozilla's CA certificate list (MPL-2.0), and sirit's repository carries GPL-3.0 and
AGPL-3.0 license files next to its BSD-3-Clause file headers.

Nintendo Switch and Donkey Kong Bananza are trademarks of Nintendo. This project has no
affiliation with Nintendo. Use it only with games you own.
