@echo off
rem Replays every new test case in cases\ (claude/test-mode.md): the game's logic at 30 FPS and at 60 FPS, headless,
rem then the comparison report. Double-click, or run from a prompt. Skips what is already done; delete a case's
rem .csv / .report.txt files to redo it. One case at a time: the game cannot run twice at once.
setlocal
cd /d "%~dp0"
if not exist cases\*.case (
    echo No cases in %~dp0cases
    goto :end
)
for %%f in (cases\*.case) do (
    if not exist "%%f.30.csv" (
        echo Replaying %%~nxf at 30 FPS...
        start "" /wait kamite.exe --headless --replay-case "%%f" --fps60 0
    )
    if not exist "%%f.60.csv" (
        echo Replaying %%~nxf at 60 FPS...
        start "" /wait kamite.exe --headless --replay-case "%%f" --fps60 1
    )
    if exist tools\fps60\case_report.py (
        if not exist "%%f.report.txt" (
            echo Comparing %%~nxf...
            python tools\fps60\case_report.py "%%f" > "%%f.report.txt" 2>&1
        )
    )
)
echo Done.
:end
if "%~1"=="" pause
