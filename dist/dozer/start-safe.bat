@echo off
setlocal DisableDelayedExpansion
rem Dozer - SAFE: the shortcuts Dozer keeps on by default set to 0, its opt-in switches cleared.
rem Dozer launcher for dozer-cli.exe (Dozer's program without menus).
rem Put this file next to dozer-cli.exe.
rem Start: put your game (.nsp .xci .nsz .xcz .nup .nx) in the Games folder next to this file and double-click
rem this file; or drag a game file onto it; or write the game's path in GAME below.
rem First start: when the user folder has no config\sdl2-config.ini yet, it is made as a copy of
rem config\qt-config.ini (the controller and settings of the menu program, dozer.exe). An existing
rem sdl2-config.ini is never changed. The .ini edits are made by dozer-config.ps1 next to this file
rem (a small, readable PowerShell script). Quit the game with Alt+F4.
rem Use it to tell whether a problem comes from the new switches. Changes that have no
rem switch stay in (CHANGELOG_DOZER.md).
rem ---- your settings -------------------------------------------------------------------
rem GAME: the game to start (a drag-and-drop file wins over this line, this line over Games).
set "GAME="
rem CONFIG: a separate sdl2-config .ini to use; empty = automatic (above, README).
set "CONFIG="
rem FULLSCREEN: -f starts in full screen; empty = window.
set "FULLSCREEN=-f"
rem ---------------------------------------------------------------------------------------
set "HERE=%~dp0"
if not exist "%HERE%dozer-cli.exe" goto noexe
if not "%~1"=="" set "GAME=%~f1"
cd /d "%HERE%"
if defined GAME goto havegame
call :findgames
if "%NGAMES%"=="0" goto nogame
if "%NGAMES%"=="1" goto havegame
goto choose
:havegame
if not exist "%GAME%" goto gamemissing
if defined CONFIG goto userconfig
call :prepconfig
if errorlevel 1 exit /b 1
goto configdone
:userconfig
if not exist "%CONFIG%" goto noconfig
:configdone
rem Opt-in switches: cleared (off).
set EDEN_NX2_BACK_STAGES=
set EDEN_NX2_BACK_CUTS=
set EDEN_NX2_ACQUIRE_PEEK=
set EDEN_NX2_JIT_SAME=
set EDEN_NX2_FRONT_MACRO_LEAN=
set EDEN_NX2_PRESENT_INTERVAL=
set EDEN_NX2_PRESENT_INTERVAL_WORD=
set EDEN_NX2_MODE_PACE=
set EDEN_NX2_PACE_SPIN_US=
set EDEN_NX2_MODE_GOV=
set EDEN_NX2_DK_DYN=
set EDEN_NX2_DK_DYN_MIN_FPS=
set EDEN_NX2_MKW_DYN=
set EDEN_NX2_FG_TIMED=
set EDEN_NX2_OSD=
set EDEN_NX2_BACK_CPU=
set EDEN_NX2_SEMEXEC_DEFER=
set EDEN_NX2_SKIP_EMPTY_DRAWS=
set EDEN_NX2_ASYNC_SKIP=
set EDEN_NX2_VBLANK_GRID=
set EDEN_NX2_VRR_VSYNC=
set EDEN_NX2_GPU_BUSY_CLOCK=
set EDEN_NX2_GPU_TICK_DIV=
set EDEN_NX2_PACE_GUEST_SLEEP=
set EDEN_NX2_PRESENT_RATE=
set EDEN_NX2_PRESENT_DIAG=
set EDEN_NX2_MODE_DIAG=
set EDEN_NX2_MODE_DIAG_MAX=
set EDEN_NX2_PACE_ONE=
set EDEN_NX2_MEMBAR=
set EDEN_NX2_BACK_CORE=
set EDEN_NX2_FRONT_CORE=
set EDEN_NX2_FG_DIAG=
set EDEN_NX2_RX_RT_FIT=
set EDEN_NX2_FLUSH_FIRST_READ=
set EDEN_NX2_READ_LATCH_AGE=
set EDEN_NX2_RX_ONE_SUBMIT=
set EDEN_NX2_RX_COALESCE=
set EDEN_NX2_RX_PITCH_FORCED=
set EDEN_NX2_RX_MASKED_TARGETS=
set EDEN_NX2_REACTIVE_STATS=
set EDEN_NX2_RX_NO_COPYBACK=
set EDEN_NX2_RX_NO_RT_LATCH=
set EDEN_NX2_RX_COALESCE_LOOSE=
rem Read traps are on unless this is 0. Safe turns them off.
set EDEN_NX2_READ_TRAPS=0
rem Shortcuts that are on by default: off (=0, the code path of 2.0.0).
set EDEN_NX2_LIGHT_DRAW=0
set EDEN_NX2_LIGHT_UNIFORM=0
set EDEN_NX2_LIGHT_UNIFORM_VALUES=0
set EDEN_NX2_LIGHT_STORAGE=0
set EDEN_NX2_UNIFORM_SYNC_SKIP=0
set EDEN_NX2_STORAGE_KEEP=0
set EDEN_NX2_UBO_REUSE=0
set EDEN_NX2_UBO_DIRECT=0
set EDEN_NX2_UBO_STAGE=0
set EDEN_NX2_UBO_BIND_SKIP=0
set EDEN_NX2_SSBO_FAST=0
set EDEN_NX2_SSBO_MEMO=0
set EDEN_NX2_SYNC_SETS=0
set EDEN_NX2_SYNC_SKIP=0
set EDEN_NX2_SYNC_EXACT=0
set EDEN_NX2_SYNC_MEMO=0
set EDEN_NX2_VERTEX_SKIP=0
set EDEN_NX2_VERTEX_FAST=0
set EDEN_NX2_VERTEX_CLEAN=0
set EDEN_NX2_LIGHT_SYNC_SKIP=0
set EDEN_NX2_INDEX_SKIP=0
set EDEN_NX2_INLINE_FIX=0
set EDEN_NX2_TOUCH_SKIP=0
set EDEN_NX2_TEX_TOUCH_SKIP=0
set EDEN_NX2_ALIAS_LAZY=0
set EDEN_NX2_USAGE_SKIP=0
set EDEN_NX2_USAGE_RESET_SKIP=0
set EDEN_NX2_GPU_WRITTEN_FILTER=0
set EDEN_NX2_MARK_WORDS=0
set EDEN_NX2_INVAL_HULL=0
set EDEN_NX2_INVAL_SKIP=0
set EDEN_NX2_SMALL_WRITE=0
set EDEN_NX2_RANGE_SKIP=0
set EDEN_NX2_SLOT_SKIP=0
set EDEN_NX2_QUERY_BOUNDS=0
set EDEN_NX2_SHADER_BOUNDS=0
set EDEN_NX2_DESC_REUSE=0
set EDEN_NX2_DESC_HULL=0
set EDEN_NX2_DESC_SECONDARY=0
set EDEN_NX2_DESC_XLAT=0
set EDEN_NX2_KEY_CMP=0
set EDEN_NX2_NOTIFY_LAZY=0
set EDEN_NX2_RESET_BATCH=0
set EDEN_NX2_RESET_LAST=0
set EDEN_NX2_RESET_RUNS=0
set EDEN_NX2_RESET_SORT=0
set EDEN_NX2_WRITE_BATCH=0
set EDEN_NX2_BATCH_DIRECT=0
set EDEN_NX2_CB_RUNS=0
set EDEN_NX2_TICK_SKIP=0
set EDEN_NX2_HLE_INDIRECT=0
set EDEN_NX2_INDIRECT_BATCH=0
set EDEN_NX2_LAYOUT_MEMO=0
set EDEN_NX2_SAME_SKIP=0
set EDEN_NX2_HSETP2_REG=0
set EDEN_NX2_DIRTY_THUNK_SKIP=0
set EDEN_NX2_FAST_PRIORITY_SCAN=0
set EDEN_NX2_PBFLUSH_WINDOW_KIB=0
set EDEN_NX2_UPSCALE_SCAN=0
set EDEN_NX2_UPSCALE_LEARN=0
set EDEN_NX2_UPSCALE_HEADER=0
if defined CONFIG goto withconfig
"%HERE%dozer-cli.exe" %FULLSCREEN% -g "%GAME%"
goto ran
:withconfig
"%HERE%dozer-cli.exe" -c "%CONFIG%" %FULLSCREEN% -g "%GAME%"
:ran
rem Keep the window open when the emulator stopped with an error (any non-zero code, a crash's
rem is negative), so it can be read.
if not "%errorlevel%"=="0" pause
goto end
:nogame
echo.
echo  No game found. Put your game file (.nsp .xci .nsz .xcz .nup .nx) in the Games folder next to
echo  this file, or drag it onto this .bat.
echo.
pause
exit /b 1
:gamemissing
echo.
echo  The game file was not found:
echo  "%GAME%"
echo.
pause
exit /b 1
:noconfig
echo.
echo  The CONFIG file in this .bat was not found:
echo  "%CONFIG%"
echo  dozer-cli would make a new one with default settings. Fix the CONFIG line
echo  (right click, Edit) or empty it.
echo.
pause
exit /b 1
:noexe
echo.
echo  dozer-cli.exe is not next to this .bat. Put both in the Dozer folder (README_DOZER.md).
echo.
pause
exit /b 1

rem ---- the game list: the Games folder next to this file, subfolders too ----------------
:findgames
set "NGAMES=0"
if not exist "%HERE%Games\" exit /b 0
for /r "%HERE%Games" %%F in (*.nsp *.xci *.nsz *.xcz *.nup *.nx) do (
  set "F=%%~fF"
  call :addgame
)
exit /b 0
:addgame
set /a NGAMES+=1
if "%NGAMES%"=="1" set "GAME=%F%"
exit /b 0
:choose
echo.
echo  Games in the Games folder:
set "I=0"
for /r "%HERE%Games" %%F in (*.nsp *.xci *.nsz *.xcz *.nup *.nx) do (
  set "F=%%~nxF"
  call :showgame
)
echo.
:ask
set "SEL="
set /p "SEL= Number (1-%NGAMES%; Enter = quit): "
if not defined SEL goto end
set "SEL=%SEL:"=%"
set "PICK="
for /l %%I in (1,1,%NGAMES%) do if "%SEL%"=="%%I" set "PICK=%%I"
if not defined PICK goto ask
set "I=0"
for /r "%HERE%Games" %%F in (*.nsp *.xci *.nsz *.xcz *.nup *.nx) do (
  set "F=%%~fF"
  call :pickgame
)
goto havegame
:showgame
set /a I+=1
setlocal EnableDelayedExpansion
echo   !I!. !F!
endlocal
exit /b 0
:pickgame
set /a I+=1
if "%I%"=="%PICK%" set "GAME=%F%"
exit /b 0

rem ---- the config: the user folder as dozer-cli finds it (user\ next to the program, else
rem %APPDATA%\eden); an existing sdl2-config.ini is only read, never written ---------------
:prepconfig
set "UDIR=%APPDATA%\eden"
if exist "%HERE%user\" set "UDIR=%HERE%user"
set "CDIR=%UDIR%\config"
set "SDL=%CDIR%\sdl2-config.ini"
set "FRESH="
if exist "%SDL%" goto sdlready
if not exist "%CDIR%\qt-config.ini" goto noini
copy /y "%CDIR%\qt-config.ini" "%SDL%" >nul
if errorlevel 1 goto noini
set "FRESH=1"
echo  [Dozer] config\sdl2-config.ini was made from qt-config.ini: the controller and settings of
echo            the menu program (dozer.exe) are used.
:sdlready
exit /b 0
:noini
echo.
echo  [Dozer] No settings yet: neither config\sdl2-config.ini nor config\qt-config.ini in
echo            "%CDIR%".
echo            Start dozer.exe once and set up your controller there, then start this again.
echo.
pause
exit /b 1
:psset
set "NX2_MODE=set"
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "%HERE%dozer-config.ps1"
if errorlevel 1 goto psfail
exit /b 0
:psfail
echo  [Dozer] Could not edit "%NX2_INI%".
exit /b 1
:end
endlocal
