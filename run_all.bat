@echo off
rem Runs every executable in a build output directory and reports one line each.
rem The Windows counterpart of run_all.sh; see run_all.bat -h for the options.
setlocal enabledelayedexpansion

set "DIR="
set "FILTER="
set "KIND=all"
set "VERBOSE=0"
set "QUIET=0"
set "PASS_ARGS="
rem :usage serves both -h and the error paths; this says which one it was
set "USAGE_EXIT=0"

:parse
if "%~1"=="" goto parsed
rem Guard each value taking option first: without this a trailing -d would set DIR to the
rem empty string and silently fall back to auto detection, ignoring what was asked for.
if /i "%~1"=="-d"        if "%~2"=="" ( set "MISSING=%~1" & goto missing )
if /i "%~1"=="--dir"     if "%~2"=="" ( set "MISSING=%~1" & goto missing )
if /i "%~1"=="-o"        if "%~2"=="" ( set "MISSING=%~1" & goto missing )
if /i "%~1"=="--only"    if "%~2"=="" ( set "MISSING=%~1" & goto missing )
if /i "%~1"=="-d"        ( set "DIR=%~2" & shift & shift & goto parse )
if /i "%~1"=="--dir"     ( set "DIR=%~2" & shift & shift & goto parse )
if /i "%~1"=="-o"        ( set "FILTER=%~2" & shift & shift & goto parse )
if /i "%~1"=="--only"    ( set "FILTER=%~2" & shift & shift & goto parse )
if /i "%~1"=="--tests"   ( set "KIND=tests" & shift & goto parse )
if /i "%~1"=="--bench"   ( set "KIND=bench" & shift & goto parse )
if /i "%~1"=="-v"        ( set "VERBOSE=1" & shift & goto parse )
if /i "%~1"=="--verbose" ( set "VERBOSE=1" & shift & goto parse )
if /i "%~1"=="-q"        ( set "QUIET=1" & shift & goto parse )
if /i "%~1"=="--quiet"   ( set "QUIET=1" & shift & goto parse )
if /i "%~1"=="-h"        ( set "USAGE_EXIT=0" & goto usage )
if /i "%~1"=="--help"    ( set "USAGE_EXIT=0" & goto usage )
if "%~1"=="--"           ( shift & goto collect )
echo unknown option: %~1 1>&2
set "USAGE_EXIT=2"
goto usage

:missing
echo missing value for %MISSING% 1>&2
set "USAGE_EXIT=2"
goto usage

:collect
rem everything past -- goes to every binary, quotes and all
if "%~1"=="" goto parsed
set "PASS_ARGS=!PASS_ARGS! %1"
shift
goto collect

:parsed
cd /d "%~dp0"

if "%DIR%"=="" (
  for %%c in ("zig-out\bin") do (
    if exist "%%~c" if "!DIR!"=="" set "DIR=%%~c"
  )
)
if "%DIR%"=="" (
  echo no build output found - looked for zig-out\bin 1>&2
  echo build first, e.g.: zig build -Dtarget^=x86_64-windows-gnu -Dtests^=true -Dbenchmarks^=true 1>&2
  exit /b 1
)
if not exist "%DIR%" (
  echo directory not found: %DIR% 1>&2
  exit /b 1
)

set "LOG=%TEMP%\grd_run_all_%RANDOM%.log"
set "TOTAL=0"
set "FAILED="
set "FAILCOUNT=0"

rem Guard both loops: a quoted wildcard that matches nothing can reach the loop body as the
rem literal "*.exe", which would be counted as a binary and then "run" as one. if exist takes
rem wildcards, so this settles it before either loop starts.
if not exist "%DIR%\*.exe" (
  echo no executables in %DIR% 1>&2
  exit /b 1
)

rem count first, so the header is honest about what is about to run
for %%f in ("%DIR%\*.exe") do (
  call :selected "%%~nf"
  if "!SEL!"=="1" set /a TOTAL+=1
)
if "%TOTAL%"=="0" (
  echo no matching executables in %DIR% 1>&2
  exit /b 1
)

echo running %TOTAL% binaries from %DIR%
echo.

for %%f in ("%DIR%\*.exe") do (
  call :selected "%%~nf"
  if "!SEL!"=="1" (
    set "NAME=%%~nf                        "
    set "NAME=!NAME:~0,24!"
    <nul set /p "=!NAME! "
    "%%~ff" !PASS_ARGS! > "%LOG%" 2>&1
    set "STATUS=!ERRORLEVEL!"

    rem A gtest verdict makes the rest of the output noise. Without one the binary is a
    rem benchmark or a tool, and what it printed is the whole point of running it.
    set "SUMMARY="
    set "IS_REPORT=0"
    for /f "delims=" %%l in ('findstr /c:"[  PASSED  ]" /c:"[  FAILED  ]" "%LOG%" 2^>nul') do set "SUMMARY=%%l"
    if "!SUMMARY!"=="" (
      set "SUMMARY=exit !STATUS!"
      set "IS_REPORT=1"
    )

    if "!STATUS!"=="0" (
      echo ok   !SUMMARY!
    ) else (
      echo FAIL !SUMMARY!
      set /a FAILCOUNT+=1
      set "FAILED=!FAILED! %%~nf"
    )

    rem type, not for /f: the latter drops blank lines and anything starting with ';'
    set "SHOW=0"
    if "!VERBOSE!"=="1" set "SHOW=1"
    if not "!STATUS!"=="0" set "SHOW=1"
    if "!IS_REPORT!"=="1" if "!QUIET!"=="0" set "SHOW=1"
    rem nothing to show for a binary that printed nothing
    for %%s in ("%LOG%") do if %%~zs==0 set "SHOW=0"
    if "!SHOW!"=="1" (
      type "%LOG%"
      echo.
    )
  )
)

if exist "%LOG%" del "%LOG%" >nul 2>&1

echo.
if "%FAILCOUNT%"=="0" (
  echo all %TOTAL% passed
  exit /b 0
)
echo %FAILCOUNT% of %TOTAL% failed:%FAILED%
exit /b 1

rem ---------------------------------------------------------------------------
rem sets SEL=1 when the binary name passes the --only and --tests/--bench filters
:selected
set "SEL=1"
set "CANDIDATE=%~1"
if not "%FILTER%"=="" (
  echo !CANDIDATE! | findstr /i /c:"%FILTER%" >nul || set "SEL=0"
)
if /i "%KIND%"=="tests" (
  echo !CANDIDATE! | findstr /b /i /c:"bench_" >nul && set "SEL=0"
)
if /i "%KIND%"=="bench" (
  echo !CANDIDATE! | findstr /b /i /c:"bench_" >nul || set "SEL=0"
)
exit /b 0

:usage
echo Runs every executable in a build output directory and reports one line each.
echo.
echo   run_all.bat                              everything in zig-out\bin
echo   run_all.bat -o memory                    only binaries whose name contains "memory"
echo   run_all.bat --tests                      skip the bench_* binaries
echo   run_all.bat -- --gtest_filter=*Arena*    pass arguments through to each binary
echo.
echo Test binaries report a verdict, so only their failures are printed. Binaries without
echo one - the benchmarks - have their output as the result, so it is always shown.
echo Exits non-zero when any binary fails.
echo.
echo Options:
echo   -d, --dir DIR        directory to scan (default: zig-out\bin)
echo   -o, --only PATTERN   run only binaries whose name contains PATTERN
echo       --tests          skip bench_* binaries
echo       --bench          run only bench_* binaries
echo   -v, --verbose        show every binary's output
echo   -q, --quiet          show output only for failures, benchmarks included
echo   -h, --help           this text
echo   --                   everything after this is passed to every binary
echo.
echo Unlike run_all.sh there is no per binary timeout: cmd has no equivalent of
echo timeout(1) that wraps a command rather than sleeping.
exit /b %USAGE_EXIT%
