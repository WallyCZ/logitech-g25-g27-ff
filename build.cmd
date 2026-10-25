@echo off
rem Builds bin\<arch>\G25FF.dll and bin\<arch>\G25FFAgent.exe for x64 and x86 with the newest installed
rem Visual Studio (C++ workload)
setlocal
set PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%
set VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set VSDIR=%%i
if not defined VSDIR (
    echo Visual Studio with C++ tools not found
    exit /b 1
)
call :build x64 || exit /b 1
call :build x86 || exit /b 1
echo Build OK
exit /b 0

:build
setlocal
call "%VSDIR%\VC\Auxiliary\Build\vcvarsall.bat" %1 >nul || exit /b 1
set OUT=%~dp0bin\%1
set OBJ=%~dp0obj\%1
if not exist "%OUT%" mkdir "%OUT%"
if not exist "%OBJ%" mkdir "%OBJ%"
cl /nologo /utf-8 /c /O2 /MT /W4 /EHsc /std:c++17 /DUNICODE /D_UNICODE /DNOMINMAX /Fo"%OBJ%\\" "%~dp0src\*.cpp" || exit /b 1
rem Version resource from G25FF_VERSION (set by package.ps1, e.g. 1.0.2); the MSI needs versioned files
rem to replace a DLL left by an earlier installation
set VERSION=%G25FF_VERSION%
if not defined VERSION set VERSION=0.0.0.0
for /f "tokens=1-4 delims=." %%a in ("%VERSION%.0.0.0") do (
    > "%OBJ%\version.h" echo #define G25FF_VERSION %%a,%%b,%%c,%%d
    >> "%OBJ%\version.h" echo #define G25FF_VERSION_STRING "%%a.%%b.%%c.%%d"
)
rc /nologo /i "%OBJ%" /fo"%OBJ%\G25FF.res" "%~dp0src\G25FF.rc" || exit /b 1
link /nologo /DLL /DEF:"%~dp0src\G25FF.def" /OUT:"%OUT%\G25FF.dll" "%OBJ%\*.obj" "%OBJ%\G25FF.res" hid.lib setupapi.lib dinput8.lib dxguid.lib cfgmgr32.lib newdev.lib msi.lib ole32.lib advapi32.lib user32.lib || exit /b 1
rem The agent program, G25FFAgent.exe: the agent code from the DLL build with its own entry point
if not exist "%OBJ%\agentexe" mkdir "%OBJ%\agentexe"
cl /nologo /utf-8 /c /O2 /MT /W4 /EHsc /std:c++17 /DUNICODE /D_UNICODE /DNOMINMAX /Fo"%OBJ%\agentexe\\" "%~dp0src\agentexe\main.cpp" || exit /b 1
rc /nologo /i "%OBJ%" /fo"%OBJ%\agentexe\G25FFAgent.res" "%~dp0src\agentexe\G25FFAgent.rc" || exit /b 1
link /nologo /SUBSYSTEM:WINDOWS /OUT:"%OUT%\G25FFAgent.exe" "%OBJ%\agentexe\main.obj" "%OBJ%\agent.obj" "%OBJ%\settings.obj" "%OBJ%\wheel.obj" "%OBJ%\log.obj" "%OBJ%\agentexe\G25FFAgent.res" hid.lib setupapi.lib advapi32.lib user32.lib || exit /b 1
exit /b 0
