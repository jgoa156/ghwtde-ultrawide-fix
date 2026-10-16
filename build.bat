@echo off
rem Builds build\ghwtde_ultrawide.asi (x86, static CRT; loaded by an ASI loader). Needs Visual Studio 2022+ with the C++ x86 tools and a
rem Windows SDK.
setlocal
set ROOT=%~dp0
set VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe
set PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set VS=%%i
if not defined VS (echo No Visual Studio with C++ tools found & exit /b 1)
call "%VS%\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul || exit /b 1
if not exist "%ROOT%build" mkdir "%ROOT%build"
cl /nologo /std:c++17 /O2 /MT /EHsc /W4 /wd4100 /LD /DNOMINMAX ^
   /I "%ROOT%deps\reshade\include" /I "%ROOT%deps\imgui" ^
   "%ROOT%src\ghwtde_ultrawide.cpp" /Fo"%ROOT%build\\" /Fe"%ROOT%build\ghwtde_ultrawide.asi" /link /NOLOGO user32.lib
exit /b %errorlevel%
