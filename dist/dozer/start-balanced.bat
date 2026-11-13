@echo off
setlocal DisableDelayedExpansion
rem Dozer - BALANCED: the balanced settings. Foliage is rendered at a performance cost.
rem Dozer launcher for dozer-cli.exe (Dozer's program without menus).
rem Put this file next to dozer-cli.exe.
rem Start: put your game (.nsp .xci .nsz .xcz .nup .nx) in the Games folder next to this file and double-click
rem this file; or drag a game file onto it; or write the game's path in GAME below.
rem First start: when the user folder has no config\sdl2-config.ini yet, it is made as a copy of
rem config\qt-config.ini (the controller and settings of the menu program, dozer.exe). An existing
rem sdl2-config.ini is never changed. The .ini edits are made by dozer-config.ps1 next to this file
rem (a small, readable PowerShell script). Quit the game with Alt+F4.
rem Bananza-specific lines are marked; for other games use start-safe.bat.
rem ---- your settings -------------------------------------------------------------------
rem GAME: the game to start (a drag-and-drop file wins over this line, this line over Games).
set "GAME="
rem CONFIG: a separate sdl2-config .ini to use; empty = automatic (above, README).
set "CONFIG="
rem FULLSCREEN: -f starts in full screen; empty = window.
set "FULLSCREEN=-f"
rem DK_DYN: 1 = Bananza dynamic frame rate patch (in memory, README), 0 = off.
set "DK_DYN=1"
rem MKW_DYN: 1 = Mario Kart World dynamic frame rate patch (in memory, README), 0 = off.
set "MKW_DYN=1"
rem OSD: 1 = FPS / speed overlay through RivaTuner Statistics Server (if running).
set "OSD=1"
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
rem Opt-in switches this launcher does not use: cleared.
set EDEN_NX2_PRESENT_INTERVAL=
set EDEN_NX2_MODE_GOV=
set EDEN_NX2_DK_DYN_MIN_FPS=
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
rem Read traps stay on when this is unset. start-performance.bat sets it to 0.
set EDEN_NX2_READ_TRAPS=
set EDEN_NX2_RX_ONE_SUBMIT=
set EDEN_NX2_RX_COALESCE=
set EDEN_NX2_RX_PITCH_FORCED=
set EDEN_NX2_RX_MASKED_TARGETS=
set EDEN_NX2_REACTIVE_STATS=
set EDEN_NX2_RX_NO_COPYBACK=
set EDEN_NX2_RX_NO_RT_LATCH=
set EDEN_NX2_RX_COALESCE_LOOSE=
rem The main switches (README_DOZER.md, "Launchers").
rem Split GPU thread: the back as stages (cut a), acquire peek, leaner front macro path.
set EDEN_NX2_BACK_STAGES=1
set EDEN_NX2_BACK_CUTS=a
set EDEN_NX2_ACQUIRE_PEEK=1
set EDEN_NX2_JIT_SAME=1
set EDEN_NX2_FRONT_MACRO_LEAN=1
rem Donkey Kong Bananza: follow the game's own 60/30/20 fps choice at the right speed.
set EDEN_NX2_PRESENT_INTERVAL_WORD=12:16:4
set EDEN_NX2_MODE_PACE=2
set EDEN_NX2_PACE_SPIN_US=1500
rem Donkey Kong Bananza dynamic frame rate (DK_DYN above; 0 = the game's own 60/30 rule).
set EDEN_NX2_DK_DYN=%DK_DYN%
rem Mario Kart World dynamic frame rate (MKW_DYN above).
set EDEN_NX2_MKW_DYN=%MKW_DYN%
rem FSR3 frame generation: generated frames blended for the time they are shown (only
rem does something when frame generation is on in the config, README).
set EDEN_NX2_FG_TIMED=1
rem Game FPS, display FPS and speed in RivaTuner Statistics Server's overlay (OSD above).
set EDEN_NX2_OSD=%OSD%
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
if exist "%HERE%dozer-config.ps1" goto prepstart
echo  [Dozer] dozer-config.ps1 is not next to this .bat: the settings step is skipped.
exit /b 0
:prepstart
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
if not defined FRESH goto checkrx
set "NX2_INI=%SDL%"
set "NX2_SET=use_reactive_flushing\default=false|use_reactive_flushing=false"
call :psset
if errorlevel 1 goto checkrx
echo  [Dozer] Reactive flushing off in the new copy (the measured setup).
exit /b 0
:checkrx
set "NX2_INI=%SDL%"
set "NX2_MODE=check"
"%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "%HERE%dozer-config.ps1"
rem exit code 42 = reactive flushing on; anything else (0, or PowerShell not found) = no note
if errorlevel 43 exit /b 0
if not errorlevel 42 exit /b 0
echo  [Dozer] Note: reactive flushing is on in your sdl2-config.ini; this launcher was measured with
echo            it off (README_DOZER.md, "Reactive flushing"). The file was not changed.
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
