@echo off
rem Shared build for the Lister plugins in this family.
rem   call core\build.cmd "%~dp0" <name>
rem produces out\stage\<name>.wlx64 and out\<name>.zip in the plugin's root.
rem
rem Expects, in the plugin root: src\*.cpp (the lister_config), src\<name>.rc,
rem src\pluginst.inf, res\ (the viewer page), vendor\wv2 (WebView2 SDK) and any
rem other vendor\<lib> folders, all of which are copied into the plugin's web\.
rem Set VCVARS to override the Visual Studio detection.
setlocal
set "ROOT=%~1"
set "NAME=%~2"
set "CORE=%~dp0"
if not defined ROOT goto :usage
if not defined NAME goto :usage
if not "%ROOT:~-1%"=="\" set "ROOT=%ROOT%\"

rem Not inside an if-block: %ProgramFiles(x86)% expands to a path containing a
rem closing paren, which would end the block early.
if defined VCVARS goto :gotvcvars
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" set "VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2^>nul`) do set "VCVARS=%%i\VC\Auxiliary\Build\vcvars64.bat"
:gotvcvars
if not exist "%VCVARS%" (
  echo Could not find vcvars64.bat. Set VCVARS to its full path and retry.
  exit /b 1
)
call "%VCVARS%" >nul || exit /b 1

if not exist "%ROOT%vendor\wv2\build\native\include\WebView2.h" (
  echo Missing dependencies - run fetch-deps.cmd first.
  exit /b 1
)

rem Total Commander keeps a Lister plugin loaded for the life of the process, so
rem it must not be pointed at this build tree - install from the zip into TC's own
rem plugins folder instead.
if exist "%ROOT%out\stage\%NAME%.wlx64" (
  2>nul ( type nul >>"%ROOT%out\stage\%NAME%.wlx64" ) || (
    echo out\stage\%NAME%.wlx64 is locked - close Total Commander and retry.
    exit /b 1
  )
)

if exist "%ROOT%out\stage" rmdir /S /Q "%ROOT%out\stage"
mkdir "%ROOT%out\stage" || exit /b 1

rc /nologo /fo "%ROOT%out\%NAME%.res" "%ROOT%src\%NAME%.rc" || exit /b 1

cl /nologo /std:c++17 /EHsc /O2 /W4 /sdl /guard:cf /LD /MT ^
   /I "%ROOT%vendor\wv2\build\native\include" /I "%CORE%src" ^
   "%ROOT%src\*.cpp" "%CORE%src\lister_webview.cpp" ^
   /Fo:"%ROOT%out\\" /Fe:"%ROOT%out\stage\%NAME%.wlx64" ^
   /link /DEF:"%CORE%src\lister.def" "%ROOT%out\%NAME%.res" ^
   /guard:cf /DYNAMICBASE /NXCOMPAT /HIGHENTROPYVA ^
   "%ROOT%vendor\wv2\build\native\x64\WebView2LoaderStatic.lib" ^
   user32.lib ole32.lib oleaut32.lib advapi32.lib shell32.lib version.lib gdi32.lib || exit /b 1

cl /nologo /std:c++17 /EHsc /O2 /W3 "%CORE%test\host.cpp" ^
   /Fo:"%ROOT%out\\" /Fe:"%ROOT%out\host.exe" /link user32.lib || exit /b 1

rem The viewer page and every vendored library land in the plugin's web\ folder.
robocopy "%ROOT%res" "%ROOT%out\stage\web" /E /NFL /NDL /NJH /NJS /NP >nul
for /d %%d in ("%ROOT%vendor\*") do (
  if /i not "%%~nxd"=="wv2" robocopy "%%d" "%ROOT%out\stage\web" /E /NFL /NDL /NJH /NJS /NP >nul
)
copy /Y "%ROOT%src\pluginst.inf" "%ROOT%out\stage\" >nul
if exist "%ROOT%LICENSE.md" copy /Y "%ROOT%LICENSE.md" "%ROOT%out\stage\" >nul
if exist "%ROOT%THIRD-PARTY.md" copy /Y "%ROOT%THIRD-PARTY.md" "%ROOT%out\stage\" >nul
del /Q "%ROOT%out\*.obj" "%ROOT%out\stage\*.exp" "%ROOT%out\stage\*.lib" "%ROOT%out\%NAME%.zip" 2>nul

powershell -NoProfile -Command ^
  "Compress-Archive -Path '%ROOT%out\stage\*' -DestinationPath '%ROOT%out\%NAME%.zip' -Force" || exit /b 1

echo Built %ROOT%out\stage\%NAME%.wlx64 and %ROOT%out\%NAME%.zip
exit /b 0

:usage
echo usage: core\build.cmd ^<plugin root^> ^<name^>
exit /b 2
