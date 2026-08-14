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
setlocal
set "REPO=%~dp0.."
pushd "%REPO%"
"%REPO%\build\windows\bin\holocron.exe" %*
set "EXITCODE=%ERRORLEVEL%"
popd
exit /b %EXITCODE%
