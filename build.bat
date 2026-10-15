@echo off
rem Builds dinput8.dll (32-bit) for BFME2 with the installed Visual Studio 2022.
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul || exit /b 1
cd /d "%~dp0"
cl /nologo /O2 /MT /W4 /LD zoomfix.c /link /DEF:dinput8.def /OUT:dinput8.dll || exit /b 1
del /q zoomfix.obj zoomfix.exp zoomfix.lib 2>nul
echo Built %~dp0dinput8.dll
