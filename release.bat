@echo off
set "GAME_EXE=OutlawGolf2"
cd /d "%~dp0"
rem started from Explorer (double-clicked): keep the window open at the end to show how it went
set "PAUSE_AT_END="
echo %cmdcmdline% | find /i "%~nx0" >nul && set "PAUSE_AT_END=1"
rem 7-Zip, from the PATH or where its installer puts it; found before anything is deleted
set "SEVENZIP="
for %%z in (7z.exe) do if not "%%~$PATH:z"=="" set "SEVENZIP=%%~$PATH:z"
if not defined SEVENZIP if exist "%ProgramFiles%\7-Zip\7z.exe" set "SEVENZIP=%ProgramFiles%\7-Zip\7z.exe"
if not defined SEVENZIP if exist "%ProgramFiles(x86)%\7-Zip\7z.exe" set "SEVENZIP=%ProgramFiles(x86)%\7-Zip\7z.exe"
if not defined SEVENZIP (echo 7-Zip not found: install it from https://www.7-zip.org & goto :end)
set "OUT=out\build\release"
rem the Visual C++ runtime beside the executable, so a fresh Windows install needs nothing more
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -property installationPath`) do set "VSDIR=%%i"
for /d %%v in ("%VSDIR%\VC\Redist\MSVC\1*") do for /d %%c in ("%%v\x64\Microsoft.VC*.CRT") do set "VCRUNTIME=%%c"
if not exist "%VCRUNTIME%\vcruntime140.dll" (echo Visual C++ runtime not found: install Visual Studio with the C++ workload & goto :end)
if not exist "%OUT%\%GAME_EXE%.exe" call build.bat || goto :end
rem the configure step writes where the SDK is
if not exist "%OUT%\sdk_dir.txt" cmake --preset release >nul || goto :end
if not exist "%OUT%\sdk_dir.txt" (echo %OUT%\sdk_dir.txt was not written, update the SDK. & goto :end)
set /p SDK=<out\build\release\sdk_dir.txt
del /q "%GAME_EXE%.zip" 2>nul
rem an empty assets folder beside the exe, where the game (files or .iso) goes
rd /s /q "%OUT%\release" 2>nul
mkdir "%OUT%\release\assets"
rem only the runtime DLLs the executables import; xbe_recomp.exe needs its own copy, Windows only looks beside the
rem exe it starts
del /q "%OUT%\compiler\*.dll" 2>nul
for %%f in (msvcp140 vcruntime140) do copy /y "%VCRUNTIME%\%%f.dll" "%OUT%\compiler\" >nul
rem the compiler folder without its work folder: no game code goes in the zip, the first start builds it
"%SEVENZIP%" a "%GAME_EXE%.zip" ".\%OUT%\%GAME_EXE%.exe" ".\%OUT%\release\assets" ".\%OUT%\compiler" .\README.md .\LICENSE "%SDK%\THIRD_PARTY.txt" "%VCRUNTIME%\msvcp140.dll" "%VCRUNTIME%\vcruntime140.dll" "%VCRUNTIME%\vcruntime140_1.dll" -xr!work
if exist "%GAME_EXE%.zip" (echo. & echo Release written: %GAME_EXE%.zip) else (echo. & echo The zip was not written.)
:end
if defined PAUSE_AT_END pause
