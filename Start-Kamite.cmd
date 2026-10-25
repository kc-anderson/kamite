@echo off
rem Starts the game (PC recomp) in a window. Put your own disc image (USA, SLUS-21503) in game\ (or set KAMITE_ISO).
rem The minimized "Kamite log" window only collects the log; it closes by itself with the game.
rem Log of the last session: build\port\bin\logs\play_last.log (the one before is kept as logs\play_<time>.log)
cd /d "%~dp0"
set PS2X_AUDIO_STATS=1
set PS2X_PACING_STATS=1
python -I build.py keep-play-log
start "Kamite log" /min cmd /c "python -I build.py run > build\port\bin\logs\play_last.log 2>&1"
