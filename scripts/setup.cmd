@echo off
rem
rem One command for a fresh clone: submodules and the sample assets.
rem
rem   scripts\setup.cmd
rem
rem The Windows half of scripts/setup.sh. The commit hook is not installed here: it is a
rem POSIX shell script that Git for Windows runs through its own bash, and installing it
rem from cmd would write a file this platform cannot check.
rem
setlocal
pushd "%~dp0.." || exit /b 1

echo ==^> submodules
git submodule update --init --recursive || goto :fail

echo ==^> dependencies
for %%T in (cmake.exe ninja.exe glslangValidator.exe git.exe python3.exe) do (
    where /q %%T || echo   missing: %%T
)

echo ==^> assets
call "%~dp0substrate.cmd" fetch-assets || goto :fail

echo.
echo next:
echo   scripts\build.cmd                 engine and unit suite -- no runnable binary
echo   scripts\build_game.cmd demo       the demo, and a program to run
echo   scripts\run.cmd                   what the build directory holds
echo   scripts\test.cmd                  the unit suite
echo   scripts\new_game.cmd mygame       start your own
popd
exit /b 0

:fail
set "CODE=%ERRORLEVEL%"
popd
exit /b %CODE%
