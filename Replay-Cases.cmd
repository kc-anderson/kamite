@echo off
rem Replays the test cases in cases\ (claude/test-mode.md): the game's logic at 30 FPS and at 60 FPS, headless, then
rem the comparison report. Double-click it. A case is replayed again whenever kamite.exe changed since its last
rem replay (a new build = every case again: the regression check), otherwise skipped. All reports end up in
rem cases\summary.txt. One case at a time: the game cannot run twice at once.
setlocal enabledelayedexpansion
cd /d "%~dp0"
if not exist cases\*.case (
    echo No cases in %~dp0cases
    goto :end
)
for %%e in (kamite.exe) do set "BUILD=%%~te %%~ze"
for %%f in (cases\*.case) do (
    set "OLD="
    if exist "%%f.build" set /p OLD=<"%%f.build"
    if not "!OLD!"=="!BUILD!" (
        del /q "%%f.30.csv" "%%f.60.csv" "%%f.30.shapes.csv" "%%f.60.shapes.csv" "%%f.report.txt" 2>nul
        echo Replaying %%~nxf at 30 FPS...
        start "" /wait kamite.exe --headless --replay-case "%%f" --fps60 0
        echo Replaying %%~nxf at 60 FPS...
        start "" /wait kamite.exe --headless --replay-case "%%f" --fps60 1
        echo Comparing %%~nxf...
        python tools\fps60\case_report.py "%%f" > "%%f.report.txt" 2>&1
        >"%%f.build" echo !BUILD!
    ) else (
        echo %%~nxf: already replayed with this build
    )
)
(for %%f in (cases\*.case) do (
    type "%%f.report.txt"
    echo.
    echo ------------------------------------------------------------------------------------------------------------
)) > cases\summary.txt 2>nul
echo Done. Reports: cases\summary.txt
:end
if "%~1"=="" pause
