@echo off
rem Dofram build script for Windows (MSVC).
rem Usage: build.bat [release ^| debug ^| test ^| icon ^| cover ^| shots ^| clean]
rem   release  build\Dofram.exe (default; hardened with /GS and Control Flow Guard)
rem   debug    build\Dofram.exe with debug information
rem   test     release build, then the off-screen tests in build\test\
rem   icon     redraw assets\dofram.ico with tools\makeicon.c, then build
rem   cover    draw docs\images\cover.png with tools\mkcover.c
rem   shots    draw the README screenshots in docs\images\ with tools\mkshots.c
rem   clean    delete build\
rem Requires Visual Studio 2019+ or the free "Build Tools for Visual Studio"
rem with the "Desktop development with C++" workload. No other dependencies.
rem Output: build\Dofram.exe (static C runtime, nothing else to install)

setlocal EnableExtensions
cd /d "%~dp0"

set "MODE=%~1"
if "%MODE%"=="" set "MODE=release"

if /i "%MODE%"=="clean" (
    if exist build rmdir /s /q build
    echo Cleaned.
    exit /b 0
)

rem --- Locate the compiler -------------------------------------------------
where cl >nul 2>nul
if not errorlevel 1 goto have_cl

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto no_vs
set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR goto no_vs
call "%VSDIR%\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul
if errorlevel 1 goto no_vs
where cl >nul 2>nul
if errorlevel 1 goto no_vs

:have_cl
if not exist build\obj mkdir build\obj

set "COMMON=/nologo /std:c11 /utf-8 /W4 /wd4100 /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /D_WIN32_WINNT=0x0A00 /DWINVER=0x0A00 /D_CRT_SECURE_NO_WARNINGS"
set "LIBS=user32.lib gdi32.lib gdiplus.lib comctl32.lib comdlg32.lib shell32.lib ole32.lib uuid.lib advapi32.lib winhttp.lib"
rem Sources shared by Dofram.exe, the tests and tools\mkshots.c (the tests include editor.c and settingsdlg.c).
set "SHARED=src\image.c src\ops.c src\settings.c src\util.c src\install.c src\update.c"

rem --- Cover image (tools\mkcover.c) -----------------------------------------
if /i not "%MODE%"=="cover" goto not_cover
if not exist tools\mkcover.c (
    echo [ERROR] tools\mkcover.c does not exist yet.
    exit /b 1
)
if not exist docs\images mkdir docs\images
echo [cover] Building tools\mkcover.c...
cl %COMMON% /O2 /MT tools\mkcover.c /Fobuild\obj\ /Febuild\mkcover.exe /link gdi32.lib gdiplus.lib ole32.lib || goto failed
build\mkcover.exe docs\images\cover.png || goto failed
echo.
echo Done: %CD%\docs\images\cover.png
exit /b 0
:not_cover

rem --- The target must not be in use -----------------------------------------
rem Only build\Dofram.exe matters; a copy running from another folder is fine.
if exist build\Dofram.exe (
    2>nul (>>"build\Dofram.exe" (call )) || (
        echo [ERROR] build\Dofram.exe is running. Right-click its tray icon, choose Exit, then build again.
        exit /b 1
    )
)

set "CFLAGS=%COMMON% /O2 /GL /MT /GS /guard:cf /DNDEBUG"
set "LFLAGS=/LTCG /OPT:REF /OPT:ICF /GUARD:CF"
if /i "%MODE%"=="debug" (
    set "CFLAGS=%COMMON% /Od /Zi /MTd"
    set "LFLAGS=/DEBUG"
)

rem --- Icon (drawn by tools\makeicon.c) -------------------------------------
if /i "%MODE%"=="icon" if exist assets\dofram.ico del /q assets\dofram.ico
if exist assets\dofram.ico (
    echo [1/3] Icon present: assets\dofram.ico
    goto resources
)
echo [1/3] Drawing the icon...
if not exist assets mkdir assets
cl %COMMON% /O2 /MT tools\makeicon.c /Fobuild\obj\ /Febuild\makeicon.exe /link gdiplus.lib ole32.lib || goto failed
build\makeicon.exe assets\dofram.ico assets\dofram-256.png || goto failed

:resources
echo [2/3] Resources...
rc /nologo /fo build\obj\dofram.res src\dofram.rc || goto failed

rem --- Screenshots for the README (tools\mkshots.c) ---------------------------
if /i not "%MODE%"=="shots" goto not_shots
if not exist tools\mkshots.c (
    echo [ERROR] tools\mkshots.c does not exist yet.
    exit /b 1
)
if not exist docs\images mkdir docs\images
if not exist build\shots mkdir build\shots
rem An empty dofram.ini next to the tool keeps its settings out of %APPDATA%.
type nul > build\shots\dofram.ini
echo [shots] Building tools\mkshots.c...
cl %COMMON% /O2 /MT /Isrc tools\mkshots.c %SHARED% ^
   build\obj\dofram.res /Fobuild\obj\ /Fdbuild\obj\ /Febuild\shots\mkshots.exe ^
   /link /SUBSYSTEM:CONSOLE /MANIFEST:NO %LIBS% || goto failed
build\shots\mkshots.exe build\shots || goto failed
copy /y build\shots\screenshot.png docs\images\ >nul || goto failed
copy /y build\shots\editor-window.png docs\images\ >nul || goto failed
echo.
echo Done: %CD%\docs\images
exit /b 0
:not_shots

echo [3/3] Dofram.exe...
cl %CFLAGS% src\app.c src\editor.c src\settingsdlg.c %SHARED% ^
   build\obj\dofram.res /Fobuild\obj\ /Fdbuild\obj\ /Febuild\Dofram.exe ^
   /link %LFLAGS% /SUBSYSTEM:WINDOWS /MANIFEST:NO %LIBS% || goto failed

if /i not "%MODE%"=="test" goto done
echo [test] Off-screen editor and logic tests...
if not exist build\test mkdir build\test
rem An empty dofram.ini next to the test program keeps its settings out of %APPDATA%.
type nul > build\test\dofram.ini
cl %COMMON% /Od /Zi /MTd /Isrc tests\test_editor.c %SHARED% ^
   build\obj\dofram.res /Fobuild\obj\ /Fdbuild\obj\ /Febuild\test\test_editor.exe ^
   /link /SUBSYSTEM:CONSOLE /MANIFEST:NO %LIBS% || goto failed
build\test\test_editor.exe || goto failed

:done
echo.
echo Done: %CD%\build\Dofram.exe
exit /b 0

:no_vs
echo [ERROR] MSVC was not found. Install Visual Studio or "Build Tools for Visual Studio"
echo         with the "Desktop development with C++" workload, or run this script
echo         from a "Developer Command Prompt".
exit /b 1

:failed
echo.
echo [ERROR] Build failed.
exit /b 1
