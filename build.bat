@echo off
setlocal
set ROOT=%~dp0
set OUT=%ROOT%out
if not exist "%OUT%" mkdir "%OUT%"

call "C:\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 goto fail

echo === resources (icon) ===
cd /d "%ROOT%src"
rc /nologo /fo "%OUT%\app.res" app.rc
if errorlevel 1 goto fail
cd /d "%ROOT%"

echo === host (x64) ===
cl /nologo /O2 /MT /EHsc /W3 /utf-8 /DWIN32_LEAN_AND_MEAN /DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0A00 /I"%ROOT%third_party\minhook" /Fe:"%OUT%\ClassicMinMax.exe" /Fo:"%OUT%\host.obj" "%ROOT%src\host.cpp" "%OUT%\app.res" /link /SUBSYSTEM:WINDOWS user32.lib gdi32.lib shell32.lib advapi32.lib
if errorlevel 1 goto fail

echo === hook dll (x64) ===
cl /nologo /O2 /MT /EHsc /W3 /utf-8 /DWIN32_LEAN_AND_MEAN /DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0A00 /I"%ROOT%third_party\minhook" /LD /Fe:"%OUT%\MinMaxHook64.dll" /Fo:"%OUT%\dll64.obj" "%ROOT%src\minmax_dll.cpp" /link /LIBPATH:"%ROOT%third_party\minhook" user32.lib gdi32.lib msimg32.lib advapi32.lib libMinHook-x64-v141-mt.lib
if errorlevel 1 goto fail

call "C:\BuildTools\VC\Auxiliary\Build\vcvars32.bat" >nul
if errorlevel 1 goto fail

echo === hook dll (x86) ===
cl /nologo /O2 /MT /EHsc /W3 /utf-8 /DWIN32_LEAN_AND_MEAN /DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0A00 /I"%ROOT%third_party\minhook" /LD /Fe:"%OUT%\MinMaxHook32.dll" /Fo:"%OUT%\dll32.obj" "%ROOT%src\minmax_dll.cpp" /link /LIBPATH:"%ROOT%third_party\minhook" user32.lib gdi32.lib msimg32.lib advapi32.lib libMinHook-x86-v141-mt.lib
if errorlevel 1 goto fail

echo BUILD_OK
exit /b 0

:fail
echo BUILD_FAILED
exit /b 1

