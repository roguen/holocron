@echo off
REM SPDX-License-Identifier: GPL-3.0-or-later
REM Copyright (c) 2026 Roguen Keller
REM
REM scripts\holocron.cmd
REM
REM A `holocron` you can type from anywhere, that still behaves like it was
REM launched from the repo root.
REM
REM WHY THIS EXISTS
REM
REM Putting build\windows\bin directly on PATH makes the bare word `holocron`
REM resolve, but gatekeeper.toml is looked up relative to the CALLER'S working
REM directory -- not the exe's -- so typing `holocron` from anywhere but the
REM repo root reproduces issue 308 exactly: a temporary identity, no token,
REM "NOT A CAST TARGET", loud and correct and useless if what you wanted was
REM your actual Theater PC. `resolve_data_path` only ever resolves against the
REM platform data directory, which is empty on every desktop build by design
REM (platform_paths.hpp) -- so there is no code-side fix for this that would
REM not also change how `holocron.exe` behaves when run deliberately from a
REM different directory, which is a real use nothing here should take away.
REM
REM So the fix is at the launch site instead: pin the working directory to the
REM repo root before the real exe ever sees an argument, exactly what typing
REM `.\build\windows\bin\holocron.exe` from the repo root already did.
REM
REM Add scripts\ to PATH, not build\windows\bin -- this file is what should
REM resolve for the bare word `holocron`, not the exe underneath it.
REM
REM WHICH BUILD IT RUNS: AN INSTALLED RELEASE IF THERE IS ONE, ELSE THE DEV
REM BUILD.
REM
REM `..\holocron-dist\<version>\` is where a published Windows zip is unpacked
REM on the rack -- a sibling of the repo, like holocron-agent, so it is outside
REM the tree and never appears in a diff. The newest directory there wins.
REM
REM Preferring it matters and is not tidiness. `scripts\build.cmd` configures
REM DEBUG, so the dev build carries the debug CRT and is several times the size
REM and materially slower -- and it is what `holocron` ran until this was added,
REM which meant the theater was running a debug binary while a Release artifact
REM sat published and unused. The fallback keeps a fresh clone working with no
REM install step, which is the case that made this file useful in the first
REM place.
setlocal
set "REPO=%~dp0.."
set "EXE=%REPO%\build\windows\bin\holocron.exe"

REM Newest installed release, if any. `dir /b /o-n` sorts by name descending,
REM which is right for v1.0.9 vs v1.0.10 only up to nine -- fine for now, and
REM the version is printed below so a wrong pick is visible rather than silent.
if exist "%REPO%\..\holocron-dist" (
    for /f "delims=" %%d in ('dir /b /a:d /o-n "%REPO%\..\holocron-dist" 2^>nul') do (
        if exist "%REPO%\..\holocron-dist\%%d\holocron.exe" (
            set "EXE=%REPO%\..\holocron-dist\%%d\holocron.exe"
            goto :found
        )
    )
)
:found

pushd "%REPO%"
"%EXE%" %*
set "EXITCODE=%ERRORLEVEL%"
popd
exit /b %EXITCODE%
