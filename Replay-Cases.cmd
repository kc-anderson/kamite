@echo off
rem Replays the test cases in cases\ (claude/test-mode.md): the game's logic at 30 FPS and at 60 FPS, headless, then
rem the comparison report. Double-click it. A case is replayed again whenever kamite.exe changed since its last
rem replay (a new build = every case again: the regression check), otherwise skipped. All reports end up in
rem cases\summary.txt. One case at a time: the game cannot run twice at once. The game runs in this window (no
rem window of its own to steal the focus); a replay that ends with an error is tried once more and, if it fails again,
rem the case is not stamped, so the next double-click replays it.
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
        set "FAILED="
        for %%r in (0 1) do (
            if %%r==0 (set "RATE=30") else (set "RATE=60")
            echo Replaying %%~nxf at !RATE! FPS...
            kamite.exe --headless --replay-case "%%f" --fps60 %%r
            if not "!errorlevel!"=="0" (
                echo   the !RATE! FPS replay stopped early ^(exit code !errorlevel!^); trying it again...
                kamite.exe --headless --replay-case "%%f" --fps60 %%r
                if not "!errorlevel!"=="0" set "FAILED=1"
            )
        )
        echo Comparing %%~nxf...
        python tools\fps60\case_report.py "%%f" > "%%f.report.txt" 2>&1
        if defined FAILED (
            echo   a replay failed twice: this case runs again next time
        ) else (
            >"%%f.build" echo !BUILD!
        )
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
