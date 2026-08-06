@echo off
rem
rem Bootstrap `substrate` and hand it the command line.
rem
rem   scripts\substrate.cmd <command> [args]
rem
rem The Windows half of scripts/substrate.sh, and it exists for the same reason: something
rem has to run cmake before any C++ tool exists. Everything else on this platform is a
rem three-line .cmd that comes straight back here.
rem
rem Needs cmake, ninja and a MinGW-w64 g++ on PATH -- MSYS2's mingw64 shell or w64devkit.
rem There is no MSVC path and no `if(MSVC)` branch in the build; see guides/building.md.
rem
setlocal
set "REPO_ROOT=%~dp0.."
pushd "%REPO_ROOT%" || exit /b 1

set "CLI_DIR=build\.cli"

rem rapidjson is the CLI's one dependency and it is a submodule, so the check that would
rem otherwise be a compiler error three minutes in happens here instead.
if not exist "external\rapidjson\include\rapidjson\document.h" (
    echo ==^> initialising submodules
    git submodule update --init --recursive || goto :fail
)

rem Configure once, build every time. The build is a ninja no-op in the ordinary case;
rem skipping it is how a stale tool outlives the edit that was supposed to change it.
rem Output is shown on failure rather than discarded: a build that fails here fails with no
rem other explanation, and running a binary that was never produced says only "not found".
if not exist "%CLI_DIR%\build.ninja" (
    cmake -B "%CLI_DIR%" -S tools -G Ninja -DCMAKE_BUILD_TYPE=Release >nul 2>&1 || (
        cmake -B "%CLI_DIR%" -S tools -G Ninja -DCMAKE_BUILD_TYPE=Release
        goto :fail
    )
)
cmake --build "%CLI_DIR%" --target substrate-cli >nul 2>&1 || (
    cmake --build "%CLI_DIR%" --target substrate-cli
    echo error: could not build the substrate CLI ^(see above^)
    goto :fail
)

"%CLI_DIR%\substrate.exe" %*
set "CODE=%ERRORLEVEL%"
popd
exit /b %CODE%

:fail
set "CODE=%ERRORLEVEL%"
popd
exit /b %CODE%
