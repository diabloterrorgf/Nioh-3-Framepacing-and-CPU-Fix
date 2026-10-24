@echo off
rem Builds dinput8.dll and the release zip.
rem Needs Visual Studio 2022 (or its Build Tools) with the "Desktop development with C++" workload,
rem and Clang (LLVM for Windows, or the "C++ Clang tools for Windows" Visual Studio component).
setlocal
cd /d %~dp0

rem ---- MSVC environment
if defined VCINSTALLDIR goto have_msvc
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto no_msvc
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
if not defined VSPATH goto no_msvc
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul
if not defined VCINSTALLDIR goto no_msvc
goto have_msvc
:no_msvc
echo Visual Studio C++ tools (x64) not found.
exit /b 1
:have_msvc

rem ---- Clang: %CLANG%, then LLVM for Windows, then Visual Studio's bundled Clang, then PATH
if not defined CLANG if exist "%ProgramFiles%\LLVM\bin\clang.exe" set "CLANG=%ProgramFiles%\LLVM\bin\clang.exe"
if not defined CLANG if exist "%VCINSTALLDIR%Tools\Llvm\x64\bin\clang.exe" set "CLANG=%VCINSTALLDIR%Tools\Llvm\x64\bin\clang.exe"
if not defined CLANG for /f "delims=" %%i in ('where clang 2^>nul') do if not defined CLANG set "CLANG=%%i"
if not defined CLANG ( echo Clang not found. Install LLVM for Windows or set CLANG=path\to\clang.exe & exit /b 1 )

set MINHOOK=third_party\minhook
if not exist obj\vfr mkdir obj\vfr
del /q obj\*.obj obj\vfr\*.obj 2>nul

cl /nologo /c /O2 /MT /W3 /Foobj\ /I%MINHOOK%\include %MINHOOK%\src\buffer.c %MINHOOK%\src\hook.c %MINHOOK%\src\trampoline.c %MINHOOK%\src\hde\hde64.c || exit /b 1
cl /nologo /c /O2 /MT /W4 /EHsc /std:c++17 /Foobj\ /I%MINHOOK%\include src\dllmain.cpp src\pacer.cpp || exit /b 1
ml64 /nologo /c /Foobj\qpcstub.obj src\qpcstub.asm || exit /b 1
rem variable-timestep core ported from nioh3fps (MIT): GNU C/asm, compiled by Clang for the MSVC ABI
for %%f in (src\vfr\*.c src\vfr\*.S) do "%CLANG%" --target=x86_64-pc-windows-msvc -O2 -std=gnu11 -fno-strict-aliasing -fms-runtime-lib=static -Wall -Wextra -Wno-unused-parameter -Wno-missing-braces -D_CRT_SECURE_NO_WARNINGS -I%MINHOOK%\include -c %%f -o obj\vfr\%%~nf.obj || exit /b 1
if not exist bin mkdir bin
link /nologo /DLL /OPT:REF /OPT:ICF /OUT:bin\dinput8.dll /DEF:src\dinput8.def obj\*.obj obj\vfr\*.obj kernel32.lib user32.lib gdi32.lib dwmapi.lib || exit /b 1
del /q bin\dinput8.lib bin\dinput8.exp 2>nul

rem ---- release zip: the files a player copies into the Nioh 3 folder
copy /y nioh3fix.ini bin\ >nul
copy /y Nioh3Fix_README.txt bin\ >nul
copy /y LICENSE bin\Nioh3Fix_LICENSE.txt >nul
copy /y THIRD_PARTY_NOTICES.md bin\Nioh3Fix_THIRD_PARTY_NOTICES.txt >nul
powershell -NoProfile -Command "Compress-Archive -Force -Path bin\dinput8.dll,bin\nioh3fix.ini,bin\Nioh3Fix_README.txt,bin\Nioh3Fix_LICENSE.txt,bin\Nioh3Fix_THIRD_PARTY_NOTICES.txt -DestinationPath bin\Nioh3Fix.zip" || exit /b 1
echo BUILD OK: bin\dinput8.dll, bin\Nioh3Fix.zip
