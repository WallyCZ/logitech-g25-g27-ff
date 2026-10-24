@echo off
setlocal
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set VSDIR=%%i
call "%VSDIR%\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul || exit /b 1
cd /d "%~dp0"
cl /nologo /O2 /MT /W4 /EHsc hidraw.cpp || exit /b 1
