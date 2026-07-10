@echo off
REM ============================================================
REM  run_game_agent.bat — Agent-friendly PS2Recomp test runner
REM
REM  TEMPLATE — Adapt placeholders before first use:
REM    {{RUNNER_PATH}}     = full path to the runner exe
REM    {{RUNNER_EXE_NAME}} = runner exe BASENAME for taskkill (e.g. dc2_runner.exe,
REM                          ps2EntryRunner.exe) — varies per project, do NOT assume
REM    {{ELF_PATH}}        = full path to game ELF (SLES_xxx.xx)
REM    {{PROJECT_ROOT}}    = full path to PS2Recomp project root
REM
REM  Features:
REM   - Auto-kills after TIMEOUT seconds (default 15)
REM   - Captures ALL stdout+stderr to a log file
REM   - Prints log path at exit for easy view_file
REM   - Exit code: 0=ran+killed, 1=error, game's own code otherwise
REM
REM  Usage:
REM   run_game_agent.bat           (15 sec timeout, default log)
REM   run_game_agent.bat 30        (30 sec timeout)
REM   run_game_agent.bat 10 mylog  (10 sec, custom log name)
REM ============================================================

set "RUNNER={{RUNNER_PATH}}"
set "RUNNER_EXE={{RUNNER_EXE_NAME}}"
set "ELF={{ELF_PATH}}"
set "LOGDIR={{PROJECT_ROOT}}\logs"
set "TIMEOUT_SEC=%~1"
set "LOGNAME=%~2"

REM Default timeout 15 seconds
if "%TIMEOUT_SEC%"=="" set "TIMEOUT_SEC=15"

REM Default log name: agent_run_latest
if "%LOGNAME%"=="" set "LOGNAME=agent_run_latest"

set "LOGFILE=%LOGDIR%\%LOGNAME%.log"

REM Create logs dir if needed
if not exist "%LOGDIR%" mkdir "%LOGDIR%"

REM Pre-flight checks
if not exist "%RUNNER%" (
    echo ERROR: Runner not found at %RUNNER%
    echo        Did you build? Run: cmake --build build64
    exit /b 1
)
if not exist "%ELF%" (
    echo ERROR: ELF not found at %ELF%
    exit /b 1
)

REM Clear previous log
if exist "%LOGFILE%" del "%LOGFILE%"

echo [agent_runner] Launching with %TIMEOUT_SEC%s timeout...
echo [agent_runner] Log: %LOGFILE%
echo [agent_runner] Runner: %RUNNER%
echo.

REM Start the game in background, redirect stdout+stderr to log.
REM NOTE: the cmd /c wrapper is REQUIRED — a bare `start /B prog > log` attaches
REM the redirection to `start` itself, not the child, and the log stays empty.
start "" /B cmd /c ""%RUNNER%" "%ELF%" > "%LOGFILE%" 2>&1"

REM Get the PID of the runner (most recent process named %RUNNER_EXE%)
REM Give it a moment to start
timeout /t 1 /nobreak > nul 2>&1

REM Wait for TIMEOUT_SEC seconds
echo [agent_runner] Waiting %TIMEOUT_SEC% seconds...
timeout /t %TIMEOUT_SEC% /nobreak > nul 2>&1

REM Kill the runner
echo [agent_runner] Timeout reached, killing runner...
taskkill /F /IM "%RUNNER_EXE%" > nul 2>&1

REM Small delay for file flush
timeout /t 1 /nobreak > nul 2>&1

REM Report results
echo.
echo ============================================================
echo [agent_runner] DONE
echo [agent_runner] Log file: %LOGFILE%
echo [agent_runner] Log size:
for %%A in ("%LOGFILE%") do echo   %%~zA bytes
echo ============================================================

REM Count lines for quick summary  
for /f %%C in ('find /c /v "" ^< "%LOGFILE%"') do echo [agent_runner] Log lines: %%C

exit /b 0
