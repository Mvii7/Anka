# Dozer changelog

Changes since **PokeDinner-Eglantine 2.0.0** (commit `70ecb39ef0`). The full history is in git
(`git log 70ecb39ef0..`); this file groups it by topic. Most changes are behind `EDEN_NX2_*`
switches (`nx2_switches_DOZER.txt`; in the source `dist/nx2_switches.txt`): the opt-in ones are
off unless a launcher or the menu program's Tuning tab sets them, the per-draw shortcuts are on
and `=0` brings back the 2.0.0 code path.

## Unreleased (Dozer)

- **Dozer** is a fork of Anka (EddizEge). The program, its window titles, dialogs, the About box and
  the build are renamed (`dozer.exe`, `dozer-cli.exe`, the launchers and the documents in
  `dist/dozer/`), with a new logo drawn by `dist/dozer/dozer_logo.py`: a D whose flat edge is a
  bulldozer blade. Anka's copyright and credit are kept in the About box and the readmes; the
  changelog below up to Anka 1.0.1 is Anka's history, which Dozer builds on.
- The settings tab called *Anka* (Emulation > Configure...) is now called *Tuning*, and its
  dialogs no longer carry the name. The settings, presets and the `[Anka]` group of
  `qt-config.ini` are unchanged, so an existing configuration keeps working.

## Anka 1.0.1 (`anka-v1.0.1`)

- Reads **`.nup`** files (NUP, the "Unencrypted (Archival) Package" of the Eglantine nx2 project):
  File > Load File, the game list (name, icon, version) and the launchers' `Games` folder;
  `anka-cli.exe` opens a `.nup` path too. Read only: Anka does not make, edit or extract NUP
  files. Only use NUP files made from your own dumps.
- The NUP code comes from upstream Eglantine nx2 and keeps its authors in git: a55uka (the
  container, the loader, the version in the game list), Sssnowyyy (large zstd parts decompressed
  once, table bounds and directory cycles), Mcm190 (the platform from the update's ExeFS),
  SavageCore (loader review), Dr. Sergio and Frizti (the decompression cache,
  `EDEN_NUP_CACHE_MIB`, LayeredFS mods for NUP titles), 999sian (the program id of a `.nup`
  started directly is known before boot; per-game settings are no longer replaced by the global
  ones when the settings file is read again during the controller setup). Not taken from that
  work: the NUP packer and editor, the extraction, and 999sian's pipeline cache changes.
- A Donkey Kong Bananza `.nup` gets Anka's settings for that game like any other format (the Anka
  tab, the dynamic frame rate, per-game settings).

## Anka 1.0.0 (`anka-v1.0.0`)

Base: PokeDinner-Eglantine 2.0.0. Programs: `anka.exe` (the Qt menu program) and `anka-cli.exe`
(the SDL program without menus), both built with LTO and profile-guided optimization. Measured
in Donkey Kong Bananza on one PC. About 400 commits, made between 31 October and
6 November 2026 (modified by the Anka maintainer; see `NOTICE_ANKA.txt` in the release zips).

### The name

- The emulator is now called **Anka** ("Anka by EddizEge"): a Nintendo Switch 2 emulator for
  Windows, focused on performance. Anka is the Zümrüdüanka of Turkish myth, the phoenix that rises
  from its ashes: an emulator born from the ashes of yuzu.
- Built on Eden, Eglantine (the PokeDinner fork by Franseven), yuzu and Sudachi; their copyrights
  and credits are kept. It is not an official release of those projects and is not affiliated
  with Nintendo. Upstream names, copyrights and licenses are unchanged.
- Repository: https://github.com/EddizEge/Anka (branch `anka`, tag `anka-v1.0.0`).
- A logo of its own: a phoenix rising, with flame wings and a flame crest, gold, orange and red
  on a dark warm tile. `dist/dozer.ico` is the program and window icon (16-256 px),
  `dist/qt_themes/default/icons/256x256/dozer.png` the About image and `dist/dozer.bmp` anka-cli's
  window icon; all three are made by `dist/dozer/dozer_logo.py`.
- Window titles: "Anka by EddizEge" (in a game: "Anka by EddizEge | <game> | <version> |
  <GPU>"; anka-cli: "Anka by EddizEge | <branch>-<desc>"). The About dialog says "Anka is an
  open-source Nintendo Switch 2 emulator for Windows, focused on performance.", links Source
  Code, Report a problem and License to github.com/EddizEge/Anka, has a copyright, no-warranty
  and GPLv3 line and the trademark line, and at the bottom "Built on Eden and Eglantine
  (Franseven). Not an official release of those projects; please report Anka problems on its
  GitHub page, not to them." The log line is "Anka settings for <title id> ...".

### The menu program

- `anka.exe`: 2.0.0's Qt front end built from this tree against Qt 6.8.3 (the version 2.0.0
  ships).
- A standalone install: `Anka-1.0.0-win64.zip` is unzipped into a new, empty folder and runs on
  its own. It holds `anka.exe`, `anka-cli.exe`, the Qt 6.8.3 DLLs and plugins they need, the
  Visual C++ runtime DLLs, the launchers and an empty `user\` folder, so Anka starts in portable
  mode and keeps its settings, keys, firmware, saves, caches and log in that folder (not in
  `%APPDATA%\eden`, not in another install). Coming from PokeDinner-Eglantine 2.0.0: copy your
  user folder next to `anka.exe`. Do not unzip Anka into a 2.0.0 folder: its Qt DLLs would
  replace 2.0.0's.
- An **Anka** tab in Emulation > Configure...: the switches of `start-recommended.bat` as eight
  settings in two groups. All games: split GPU thread, acquire peek, the same-value filter in
  the macro JIT, the lean front macro path, timed frame generation, the FPS overlay in
  RivaTuner. Donkey Kong Bananza only (off in other games): the dynamic frame rate and the
  frame pacing. They are kept in a new `[Anka]` group of `qt-config.ini`.
- A **Preset** box at the top of the tab, with a line under it about the choice:
  - Recommended: all eight on, Reactive Flushing (Graphics > Advanced) off: the measured setup
    (with GPU Mode Fast, the default),
    the same as `start-recommended.bat`. On Anka's first start (no `[Anka]` group yet) it is
    chosen and Reactive Flushing is turned off in `qt-config.ini`. Choosing it later turns
    Reactive Flushing off too.
  - Recommended + Reactive Flushing: all eight on, Reactive Flushing and barrier feedback loops
    on (Graphics > Advanced; Reactive Flushing needs it) and `EDEN_NX2_RX_RT_FIT=3`.
    Experimental: on our PC about 7% slower at the game's opening and about 15% faster in open
    areas. The same as `start-reactive-experimental.bat`.
  - Safe: all eight off, Frame generation off, and the GPU shortcuts Anka keeps on by default
    off too (`EDEN_NX2_LIGHT_DRAW=0` and the rest of `start-safe.bat`'s list of 61), in every
    game. Other graphics settings such as Reactive Flushing stay as they are; Anka's fixes and
    its changes without a switch stay in.
  - Custom: chosen by itself when a box is changed by hand.

  Recommended was measured in Donkey Kong Bananza only. If another game shows image or speed
  problems, try Safe first and report it with the game's name.
- A **Frame generation** box in the tab: Off (the default), 2x, Dynamic. Off turns frame
  generation off. 2x is FSR3 with one generated frame per real frame (fixed); on a 60 Hz screen
  it holds the game at 30 fps. Dynamic is FSR3 with a 120 fps target, capped at the screen's
  refresh rate when it is known; it steps between 1x and 4x only while the screen can show the
  extra frames and the game's own frame rate holds (a step that lowers the game's frame rate by
  more than 8% is undone). Another setup made in the Graphics tab (LSFG, another multiplier or
  target) shows as "Other (Graphics tab)" and is left as it is. Generated frames need a screen
  faster than the game's frame rate; on a 60 Hz screen they only help below 30 fps. The presets
  do not change it, except Safe, which sets it to Off. It writes the Graphics tab's frame
  generation settings (Enable Frame Generation, type FSR3, multiplier / target frame rate).
- Reactive Flushing (changed through a preset) and Frame generation are graphics settings that
  cannot change in a running game: changed while a game runs, Anka keeps them and applies them
  when you press Restart Now, or when the game stops if you chose Later (the tab says so).
- The program sets the switches in its environment before the first game of a session starts;
  the Bananza ones only for Bananza (title id `040074A01BF12000`, the one the dynamic frame rate
  patch checks). A switch set outside the program (the environment, a `.bat` of your own that
  starts `anka.exe`, or `nx2_switches.txt` next to the program or in `%APPDATA%\eden`) wins and
  is never changed. A box whose switches are all set outside is greyed out; one with some of
  them set outside sets the rest; the tooltip names them with their values.
- Automatic restart: the switches are read once per session, and settings take effect at the
  first game started. Changing them with OK or Apply after a game was started asks "Restart
  Anka now to apply?" (Restart Now / Later). Restart Now closes Anka and opens it again with
  the running game (unsaved game progress is lost), or with the same command line when no game
  is running. Before the first game nothing is asked. Starting a game that needs other
  switches than the session's first game (Bananza after another game, or the other way round)
  asks Restart (opens again with that game) / Start Anyway / Cancel.
- Known limits: a game started from the Home Menu applet runs with the session's settings (no
  question); Restart starts the game normally (its per-game settings, its first program). The
  menu program was not measured: every number is from the program without menus.
- The RTSS on-screen display (`OSD`) works in the menu program too (the writer moved from the
  SDL program to `frontend_common`).
- GPU Mode (GPU accuracy) starts at **Fast** (Low), as in newer Eden versions (2.0.0 started at
  Accurate, High, on desktop); our measurements used Fast. If a game shows image errors, set
  Graphics > Advanced > GPU Mode to Accurate for that game (its Properties) or globally.
- Fix: the Graphics tab saved *Enable Frame Generation* as off when the checkbox of one of
  frame generation's sub-settings (Queue Target, for example) was unchecked; now only *Enable
  Frame Generation* decides, in the global and the per-game settings.
- Fix: after Restart Now the new Anka waits for the old one to close, so the new session writes
  its log (`log\eden_log.txt`) again.

### Donkey Kong Bananza terrain and the GPU command stream

- NVN's indexed indirect draw records are drawn on the host GPU (`vkCmdDrawIndexedIndirect`)
  instead of being read back on the CPU each frame (`HLE_INDIRECT`, on).
- Consecutive terrain indirect draws can merge into one multi-draw on the split back
  (`INDIRECT_BATCH=1`, off: merged, Bananza's terrain drew garbage polygons and lost chunks);
  the indirect draw reset's uploads are batched, sorted and written in
  runs (`RESET_BATCH`, `RESET_SORT`, `RESET_RUNS`, `RESET_LAST`, on).
- GPU-written macro parameters are flushed with the 256 KiB window around them, and no second
  drain follows a flush check (`PBFLUSH_WINDOW_KIB`).
- Macro parameters are executed without a second FIFO copy (snowstormxd, picked from upstream).
- The `max(x, -x)` fold for NVIDIA also covers predicate masks merged by predicated moves
  (ZendNevs): NVIDIA's compiler miscompiles that form into a device loss, which Bananza's shaders
  reached a few minutes into a run. Draw-parameter constant buffer reads outside vertex shaders are
  left alone.
- The software 2D blit untiles and tiles with the Switch 2 GOB order (`BLIT_NATIVE_TILING`, Orchid
  `6a1bd310`, from MFS-code/Eglantine-steamdeck): small mip-chain copies came out as a weave of
  misplaced 4x4 blocks, the cause of the stripes on far, minified terrain.
- A present image is cropped to the rectangle the game sets in the present request (words 6 to
  8), as in the Selevet and ZendNevs forks: Bananza handheld draws into part of a larger image,
  and the whole image showed stale tiles around the frame. Mario Kart World's fixed crop is
  kept for presents without one.

### Split GPU thread

- The back can run as a pipeline of stages (`BACK_STAGES=1`, `BACK_CUTS=a`, off by default): the
  engines on stage A, memory and recording on BC, with register replicas, rings and a join
  protocol. A three-stage cut (`BACK_CUTS=c`) is in as an experiment, not for play.
- Macro writes travel as one ring entry (`WRITE_BATCH`), batch entries run in the engine's own
  loop (`BATCH_DIRECT`), constant buffer data words are written in runs (`CB_RUNS`); the GPU tick
  skips its lock when nothing is pending (`TICK_SKIP`). All on.
- Front: a semaphore acquire that memory already meets no longer drains the back
  (`ACQUIRE_PEEK=1`, opt-in); the macro JIT drops state writes that change nothing and the front
  macro calls are leaner (`JIT_SAME=1`, `FRONT_MACRO_LEAN=1`, opt-in).
- Optional physical cores of their own for the back and the front (`BACK_CPU`, off).

### Per-draw work on the GPU thread (on by default; `=0` turns each off)

- Light draws: draws that change only buffer addresses, uniforms or storage buffers set up only
  what changed (`LIGHT_DRAW`, `LIGHT_UNIFORM`, `LIGHT_UNIFORM_VALUES`, `LIGHT_STORAGE`,
  `UNIFORM_SYNC_SKIP`, `STORAGE_KEEP`, `LIGHT_SYNC_SKIP`).
- Buffers: uniform buffers reused by content and by stage slot (`UBO_REUSE`, `UBO_DIRECT`,
  `UBO_STAGE`, `UBO_BIND_SKIP`), remembered storage buffer bindings (`SSBO_MEMO`, `SSBO_FAST`),
  synchronizations that would find nothing skipped and remembered (`SYNC_SKIP`, `SYNC_EXACT`,
  `SYNC_MEMO`, `SYNC_SETS`), cheaper vertex and index buffer updates (`VERTEX_SKIP`,
  `VERTEX_FAST`, `VERTEX_CLEAN`, `INDEX_SKIP`, `LAYOUT_MEMO`), GPU-written pages marked a word at
  a time and answered from a summary (`MARK_WORDS`, `GPU_WRITTEN_FILTER`).
- Memory and caches: constant buffer writes of bytes memory already holds skipped
  (`SAME_SKIP`), small guest writes through remembered page pointers (`SMALL_WRITE`), cheaper
  invalidations, range sets, slot vectors, LRU touches, usage trackers, query and shader lookups
  (`INVAL_HULL`, `INVAL_SKIP`, `RANGE_SKIP`, `SLOT_SKIP`, `TOUCH_SKIP`, `TEX_TOUCH_SKIP`,
  `ALIAS_LAZY`, `USAGE_SKIP`, `USAGE_RESET_SKIP`, `QUERY_BOUNDS`, `SHADER_BOUNDS`,
  `DIRTY_THUNK_SKIP`, `DESC_XLAT`).
- Descriptors and pipelines: image descriptors reused when a stage draws with the same views
  (`DESC_REUSE`, `DESC_HULL`, `DESC_SECONDARY`), pipeline keys compared hashes first
  (`KEY_CMP`), the Vulkan worker woken less often (`NOTIFY_LAZY`).
- The kernel's priority queue finds the first set priority with one instruction
  (`FAST_PRIORITY_SCAN`). Per-draw switches are read once into atomics.

### Frame pacing (Donkey Kong Bananza; all opt-in)

- vi2 present diagnostics, a present interval taken from the game's request
  (`PRESENT_INTERVAL_WORD=12:16:4`: the game's own 60/30/20 fps choice at the right speed),
  a fixed interval, a 60 Hz vblank grid, presents paced by the request's refresh rate
  (`PRESENT_INTERVAL`, `VBLANK_GRID`, `PRESENT_RATE`).
- A controller that steers the game's 60/30/20 fps mode by holding presents (`MODE_PACE=1`, and
  v2.1 with `MODE_PACE=2`), with tuning knobs, a spin before each wake (`PACE_SPIN_US`) and
  sleeps in the guest kernel (`PACE_GUEST_SLEEP`). An earlier governor through the GPU timer
  (`MODE_GOV`), a busy-time GPU clock and a GPU timer divisor (`GPU_BUSY_CLOCK`,
  `GPU_TICK_DIV`).
- vi2 experiments for the VRR path: reply tables, kind events, notifications, VRR vsync events
  (`VI2_*`, `VRR_VSYNC`).

### Donkey Kong Bananza dynamic frame rate (DK-DYN, opt-in)

- `DK_DYN=1`: a port of the ZendNevs fork's `dk_dynamic_fps` patch (v2.0.5). While the game's
  main module loads, its frame rate controller is patched in memory to run anywhere from 40 to
  60 fps with the clock stepped by the real frame time. vi2 then reports VRR support and paces
  presents to every vblank (`PACE_ONE`). The check words are read inside the text segment only.
- `DK_DYN_MIN_FPS=<1-60>` moves the controller's 40 fps limit.

### Frame generation

- The built-in optical flow frame is interpolated between two real frames; its motion image
  stays in `GENERAL` layout and its center term is blended forward.
- `FG_TIMED=1`: FSR3 generated frames are blended for the time they are shown (the display
  refresh asked from Windows, or `FG_REFRESH_HZ`); `FG_DIAG` logs the timing.
- float16 stays on for Ampere and newer when frame generation starts with LSFG.
- The frame counters count the images sent to the screen, generated ones included.

### Reactive flushing (all off by default)

For `use_reactive_flushing=true`; none of these was measured with frame generation (README,
"Reactive flushing").

- `RX_RT_FIT=1`: a render target lookup takes the narrower image with the target's memory when
  the guest's surface clip is exactly that image (Bananza's opening renders into a 2112x1179
  target and samples the same memory as a 2095x1179 texture: one image instead of a whole copy
  per draw); a color feedback loop ends the render pass first (`barrier_feedback_loops=true`).
  `=2` (diagnostic) leaves the barrier out.
- `RX_RT_FIT=3` (experimental; the reactive launcher's setting): as `=1`, but the feedback barrier
  is recorded inside the render pass (color attachment writes before fragment shader reads, by
  region), so the render pass and the light draw path go on; a depth feedback loop still ends
  it, and render targets are looked up again only when the surface clip changes. Measured on 5
  November: +16% at Bananza's opening against `=1`, the same in the open area. Its image has not
  been checked by eye yet.
- Experiments without a measured gain: `FLUSH_FIRST_READ` (a buffer page the GPU wrote is
  flushed at the guest's first CPU read), `READ_LATCH_AGE` (what CPU reads latched is dropped
  after that many frames unread), `RX_ONE_SUBMIT` (no empty second submission after a fence's
  own), `RX_COALESCE` (buffer copy backs ride along with fences made anyway), `RX_PITCH_FORCED`
  (pitch linear images stay downloaded at every bind).
- `RX_MASKED_TARGETS`: a draw marks only the targets its masks let it write. Known defect (a
  texture blit ignores the mask): do not turn it on.
- Diagnostics: `REACTIVE_STATS` (counters and a log line about every 10 s; with `RX_RT_FIT=3`
  also draws and in-pass barriers), `RX_NO_COPYBACK`, `RX_NO_RT_LATCH`, `RX_COALESCE_LOOSE`
  (with `RX_COALESCE`, copy backs skip fences made only for queued work; it weakens reactive
  flushing's ordering and only measures what the extra submissions cost).

### On-screen display

- `OSD=1` (anka-cli and the menu program, on Windows): game FPS, display FPS with frame
  generation's multiplier, and the game's speed in RivaTuner Statistics Server's on-screen
  display every 0.5 s; the slot is emptied on every exit.

### Shaders and accuracy

- Ampere: a BRA whose offset field has flag value 1 is lowered as a plain branch (`BRA_FLAG`):
  Bananza's fragment effect shaders had one, which got the program rejected and its draws
  skipped; HSETP2 with a register operand is lowered (`HSETP2_REG`); TXD with any channel mask;
  FRND with a constant buffer source (Takoroka); Bananza's shifted QMD layout accepted.
- HMMA: emulated by default instead of rejecting the program, f32 accumulation rounded every
  eight products, FP16 texture results packed with c0 in the low half (Franseven, picked from
  2.5.0); programs that use R240-R245 stay rejected, and a stand-in keeps a program whose
  pipeline fails to build; pipeline cache versions bumped for the new lowering.
- A stand-in for Bananza's tensor-core upscalers, found by size (`UPSCALE_*`).
- Register write history is opt-in in release builds (snowstormxd; `3D_HISTORY`).
- The core index of rasterizer downloads is clamped; invalid sampler and image view indices are
  logged at trace level; a failed GPU wait is logged before it ends the process.
- Video decoder: end of stream is marked only on a picture of the last input; post-stream
  release fences stay pending (Javier; `VDEC_EOS_RELEASE_NOW` releases them at once).

### Picked from upstream and other forks (authors kept)

| Commit | Author | Change |
|---|---|---|
| `4f9a1ab394` | gabrielskaminski | shader_recompiler: include all components in IsGeneric upper bound |
| `9e3d502eb3` | Dee1 | audio_core: unregister audio in/out managers before they are destroyed |
| `e2ca5fa32a` | Dee1 | audio_core: keep audio in/out managers alive while their sessions are open |
| `f298aa60e0` | Franseven | shader: pack FP16 texture results with c0 in the low half |
| `1ebe930a83` | Franseven | ampere: emulate HMMA by default instead of rejecting the program |
| `85baca0639` | Franseven | shader: accumulate HMMA.1688/.16816 in f32, round every eight products |
| `ee1299f1f9` | snowstormxd | gpu: make register-write history opt-in in release builds |
| `c1d93d0599` | snowstormxd | macro: execute native parameters without a second FIFO copy |
| `97f91db69d` | Takoroka | nx2: lower FRND with a constant buffer source |
| `d662cb3eae` | Franz | Prevent Memory Block ID Overflow and Validate Page Contiguity |
| `b0b61d1ef2` | Duck912 | video_core: read the framebuffer before recording the upscaled-output blit |
| `ac206cde3c` | rdenno | vk_texture_cache: give framebuffers the fewest layers of their attachments |
| `bfad43bba1` | rdenno | texture_cache: look up X8Z24 with UNORM, UINT, UNORM, UNORM components |
| `0285c681f6` | ZnOw01 | video_core: bound-check macro position writes |
| `02dc13a51c` | Javier (Xenon-Hacks) | nx2: video decoder: keep post-stream release fences pending |
| `3be4fca0e0` | Zoe L. | fix: avoid variable shadowing |

Ideas ported from other forks, credited in the code: Selevet (`SLOT_SKIP`, `USAGE_RESET_SKIP`,
`ASYNC_SKIP`), elBart0 (`UBO_BIND_SKIP`, `KEY_CMP`), Duck912 / moceure's eden-nx2-android
(`INLINE_FIX` and others), ZendNevs (`DK_DYN`, `PACE_ONE`).

### Diagnostics and tools (logging only)

- `EDEN_NX2_GPU_PROFILE` zones and counters for the split thread, its front and the back
  stages; `MODE_DIAG`, `PRESENT_DIAG`, `VI2_DIAG`, `HID_DIAG`, `INDIRECT_DIAG`, `FG_DIAG`,
  `VDEC_TRACE`, `EDEN_NX2_IPC_TRACE` (service request backtraces), root table diagnostics.
- anka-cli: a timed key script on a virtual gamepad and an automatic exit for unattended
  measuring (`AUTO_INPUT_FILE`, `AUTO_EXIT`).
- Unit tests for the new logic, the settings tab's presets included (`src/tests/`).

### Build

- `/QIntel-jcc-erratum` is a build option (`YUZU_MSVC_JCC_ERRATUM`).
- Release programs built with link-time code generation and profile-guided optimization, the
  menu program with anka-cli's profile (`BUILDING_DOZER.md`); the menu program against Qt 6.8.3,
  whose DLLs and plugins the program zip carries.

### Not in this release

- Measurements of the menu program (`anka.exe`): every number is from the program without
  menus (then called eden-cli, now `anka-cli.exe`).
- A reactive flushing launcher that is checked: `start-reactive-experimental.bat`
  (`RX_RT_FIT=3`) is the second choice next to the recommended one, experimental until it is
  measured with frame generation and its image is checked.
- The "M" stage of the split GPU thread work.
- Zelda support and per-game presets: planned for the next release (no date). From now on every
  update goes into one emulator, Anka.

### Documentation

- `NX2_NOTES.md` and `NX2_GPU_THREAD_SPLIT.md`: what was tried, measured and why.
- `nx2_switches_DOZER.txt` (in the source `dist/nx2_switches.txt`): every switch with its default.
- `README_DOZER.md`, this file, `BUILDING_DOZER.md`, the launchers in `dist/dozer/`. The release
  texts are in English only.
- Launchers (`start-recommended.bat`, `start-reactive-experimental.bat`, `start-safe.bat`;
  their messages start with `[Anka]`): they find the game in the `Games` folder next to them
  (asking which one when there are several), and on the first start make
  `user\config\sdl2-config.ini` as a copy of `anka.exe`'s `qt-config.ini` (the recommended one
  turns reactive flushing off in that copy; the reactive one makes its own
  `sdl2-config-reactive.ini`). An existing `sdl2-config.ini` is never changed. The edits are made
  by `anka-config.ps1`, a small, readable PowerShell script next to them.
