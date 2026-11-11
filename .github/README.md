<p align="center"><img src="../dist/qt_themes/default/icons/256x256/dozer.png" width="128" alt="Dozer"></p>

<p align="center"><b>Dozer — a Nintendo Switch 2 emulator for Windows, tuned for Donkey Kong Bananza (more games may follow).</b></p>

<p align="center">Not affiliated with Nintendo or the Eglantine project; please report problems in
this repository's <a href="https://github.com/Mvii7/Dozer/issues">Issues</a>, not to them.</p>

<p align="center"><b>Support is very limited.</b> Dozer is currently tuned for one game, Donkey Kong Bananza.
Other games are not supported yet. More may be added later, and any game Dozer is tuned for will be
listed here.</p>

## Downloads and setup

Download the latest release for your system from the
[Releases page](https://github.com/Mvii7/Dozer/releases).

Dozer runs on its own. You need Windows 10 or 11 (64-bit) and a GPU with a current Vulkan
driver, and the Microsoft Visual C++ runtime installed; the release carries the Qt 6.8.3 DLLs and
plugins.

1. Unzip the release into a new folder. If it has no `user` folder, make an empty one next to
   `dozer.exe`: that makes Dozer portable (settings, keys, firmware and saves stay in it). Dozer
   also reads `.nup` game files (see the README).
2. Run **`dozer.exe`**.
3. Install your own keys and firmware as with other Eden-based emulators (Dozer provides none;
   only use what you legally own).

To uninstall, delete the Dozer folder; move your games and its `user` folder (keys, firmware,
saves) out first if you want to keep them.

### Without menus: dozer-cli and the launchers

`dozer-cli.exe` is the program without menus. The launchers are in `dist/dozer/` in the source:
copy them next to `dozer-cli.exe`, put your game in a `Games` folder next to them (or drag it onto a
launcher) and double-click a launcher:

| Launcher | When to use it |
|---|---|
| `start-recommended.bat` | **Recommended.** The fastest settings we measured (Donkey Kong Bananza), read traps on. |
| `start-fast.bat` | **Fast.** The recommended settings with read traps off (`EDEN_NX2_READ_TRAPS=0`). |
| `start-reactive-experimental.bat` | **Experimental.** Read traps off and Reactive Flushing on: on our PC about 7% slower at the game's opening and about 15% faster in open areas. If you see image errors, go back to the recommended one. |
| `start-safe.bat` | **For troubleshooting and for games other than Bananza.** The GPU shortcuts Dozer keeps on by default turned off (`EDEN_NX2_LIGHT_DRAW=0` and the rest of its list), read traps off, and the optional settings that change play cleared. Dozer's fixes and its changes without a switch stay in. |

On the first start a launcher makes dozer-cli's settings file (`user\config\sdl2-config.ini`) as
a copy of `dozer.exe`'s `qt-config.ini`, so your controller setup comes along; it never changes
a settings file that already exists (`dozer-config.ps1`, a small, readable PowerShell script,
does this). The game starts in full screen: **quit with Alt+F4**.

## Settings

In `dozer.exe`: **Emulation > Configure... > Tuning**. The **Preset** box at the top has five
choices:

- **Recommended** (the first start's choice): all nine settings on, including read traps,
  Reactive Flushing off (Graphics > Advanced). The setup we measured (with GPU Mode Fast, the
  default); the same as `start-recommended.bat`. On Dozer's first start it also turns Reactive
  Flushing off in `qt-config.ini` by itself; choosing Recommended later turns it off as well.
- **Fast**: the recommended settings with read traps off (`EDEN_NX2_READ_TRAPS=0`) and Reactive
  Flushing off. The same as `start-fast.bat`.
- **Experimental**: the recommended settings with read traps off, Reactive Flushing on, barrier
  feedback loops on (Graphics > Advanced; Reactive Flushing needs it) and `EDEN_NX2_RX_RT_FIT=3`.
  The same as `start-reactive-experimental.bat`.
- **Safe**: all nine off, Frame generation off, and the GPU shortcuts Dozer keeps on by default
  off too (`EDEN_NX2_LIGHT_DRAW=0` and the rest of `start-safe.bat`'s list), in every game.
  Other graphics settings such as Reactive Flushing stay as they are; Dozer's fixes and its
  changes without a switch stay in.
- **Custom**: chosen by itself when you change a box by hand. Unticking read traps while
  Recommended is selected does this.

Recommended was measured in Donkey Kong Bananza only. If another game shows image or speed
problems, try Safe first and report it with the game's name.

GPU Mode (GPU accuracy) starts at **Fast** (Low), as in newer Eden versions (2.0.0 started at
Accurate, High, on desktop); our measurements used Fast. If a game shows image errors, set
Graphics > Advanced > GPU Mode to Accurate for that game (its Properties) or globally.

Below the presets, seven settings apply to **all games** (Read traps is the first) and two to **Donkey Kong Bananza only**
(Dynamic frame rate and Frame pacing; off in other games). The settings are read once per
session and take effect at the first game you start. If you change them after a game was
already started, or start a game that needs other settings (Bananza after another game or the
other way round), Dozer asks whether to restart and does it for you. A switch you set outside
the program (your environment, your own `.bat` or `nx2_switches.txt`) always wins; its box is
greyed out.

The tab also has a **Frame generation** box:

- **Off** (the default): frame generation off.
- **2x**: FSR3, one generated frame per real frame (fixed). On a 60 Hz screen it holds the game
  at 30 fps.
- **Dynamic**: FSR3 with a 120 fps target, capped at the screen's refresh rate when it is known.
  It steps between 1x and 4x only while the screen can show the extra frames and the game's own
  frame rate holds (a step that lowers the game's frame rate by more than 8% is undone).
- *Other (Graphics tab)* is shown, not chosen, when the Graphics tab holds another frame
  generation setup; the Tuning tab leaves it as it is.

Generated frames need a screen faster than the game's frame rate; on a 60 Hz screen they only
help below 30 fps. The presets do not change it, except Safe, which sets it to Off. The box
writes the Graphics tab's frame generation settings (Enable Frame Generation, type FSR3,
multiplier / target frame rate).

Reactive Flushing (through a preset) and Frame generation are graphics settings, which cannot
change in a running game: changed while a game runs, Dozer keeps them and applies them when you
press **Restart Now**, or when the game stops if you chose **Later** (the tab says so).
Details: "The Tuning tab" in [`README_DOZER.md`](../README_DOZER.md).

## Saves and settings

With a `user` folder next to `dozer.exe` (make an empty one), Dozer runs in portable mode: its
settings (`user\config\qt-config.ini`), keys, firmware, saves (`user\nand`) and log stay in that
folder, apart from any other emulator; it does not use `%APPDATA%\eden` (except that `dozer.exe`
also reads a `nx2_switches.txt` there, if you made one).

## Building

See [`BUILDING_DOZER.md`](../BUILDING_DOZER.md).

## License

Dozer is licensed under the **GNU General Public License v3.0 or later**
([`LICENSE.txt`](../LICENSE.txt)), like Anka, Eden and Eglantine. The source of every build is this
repository at the build's commit. `dozer.exe` links Qt 6.8.3 dynamically (LGPL-3.0; Qt Charts
GPL-3.0); the Qt DLLs in a build are the official, unmodified binaries, and you may replace them
with your own build of Qt 6.8.3. Third-party license texts are in [`LICENSES`](../LICENSES).

## Credits

**Built on.** Dozer is a fork of Anka (EddizEge) and builds on Eden, Eglantine (the PokeDinner fork
by Franseven), yuzu and Sudachi; their copyrights and credits are kept. It is not an official release of those
projects. Please report Dozer's problems here, not to them.

- EddizEge's [Anka](https://github.com/EddizEge/Anka).
- Franseven's [Eglantine](https://git.eden-emu.dev/Franseven/PokeDinner-a-Eglantine-Fork)
  (PokeDinner-a-Eglantine-Fork).
- The [Eden](https://git.eden-emu.dev/eden-emu/eden), yuzu and Sudachi teams and all their
  contributors.
- Forks whose ideas were ported, credited in the code and the changelog: Selevet (the handheld
  picture crop), ZendNevs (read traps, the shader fix for NVIDIA, the `dk_dynamic_fps` patch),
  Orchid and Eglantine-steamdeck (the far terrain stripes fix), elBart0, and Duck912 / moceure's
  eden-nx2-android. The upstream commits picked into Dozer are listed with their authors in the changelog.

Much of Dozer's code, its commit messages and its documents were written with AI coding
assistants (Claude, by Anthropic) under the direction of the human maintainer, who decided what to
try, played and tested the builds, and is responsible for them. Commits made with an assistant
say so in their trailers, as the Eden project's AI policy asks.

## Issues

Report problems at https://github.com/Mvii7/Dozer/issues (please not to Franseven, the Eden
project or the other upstream projects). Say which program (`dozer.exe` or `dozer-cli.exe`) and
which preset or launcher you used, the game's version, and attach the log file
(`user\log\eden_log.txt` in the Dozer folder).

Nintendo Switch and Donkey Kong Bananza are trademarks of Nintendo. Use Dozer only with games
you own.
