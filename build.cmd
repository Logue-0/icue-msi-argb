@echo off
rem Builds bin\MysticLight_SDK_x64.dll with Visual Studio 2022 (Desktop C++ workload).
setlocal
set VS=%ProgramFiles%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat
if not exist "%VS%" set VS=%ProgramFiles%\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat
if not exist "%VS%" set VS=%ProgramFiles%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat
call "%VS%" >nul || (echo Visual Studio 2022 not found & exit /b 1)
cd /d "%~dp0"
if not exist bin mkdir bin
if not exist obj mkdir obj
cl /nologo /LD /MT /O2 /W3 src\shim.c /Fo:obj\ /Fe:bin\MysticLight_SDK_x64.dll /link /IMPLIB:obj\shim.lib oleaut32.lib hid.lib setupapi.lib || exit /b 1
echo Built bin\MysticLight_SDK_x64.dll
