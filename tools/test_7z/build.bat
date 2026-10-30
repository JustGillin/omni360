@echo off
rem Builds test_7z.exe with Visual Studio 2010's PC compiler: SevenZip.cpp and
rem the 7-Zip decoder in lzma\, with shim\xtl.h standing in for the XDK's -
rem and test_7z_stream.exe, the same with a 64KB block limit, so the streaming
rem of big blocks runs on small archives. run_test.py calls this, then runs them.
setlocal
cd /d "%~dp0"
call "%ProgramFiles(x86)%\Microsoft Visual Studio 10.0\VC\vcvarsall.bat" x86 >nul
if not exist build\lzma mkdir build\lzma
cl /nologo /c /O2 /W3 /D_CRT_SECURE_NO_WARNINGS /Fobuild\lzma\ ^
    ..\..\lzma\7zArcIn.c ..\..\lzma\7zBuf.c ..\..\lzma\7zCrc.c ..\..\lzma\7zCrcOpt.c ^
    ..\..\lzma\7zDec.c ..\..\lzma\7zStream.c ..\..\lzma\Bcj2.c ..\..\lzma\Bra.c ..\..\lzma\Bra86.c ^
    ..\..\lzma\BraIA64.c ..\..\lzma\Delta.c ..\..\lzma\Lzma2Dec.c ..\..\lzma\LzmaDec.c ^
    ..\..\lzma\Ppmd7.c ..\..\lzma\Ppmd7Dec.c
if errorlevel 1 exit /b 1
cl /nologo /O2 /EHsc /W3 /D_CRT_SECURE_NO_WARNINGS /Ishim /I..\.. /Fobuild\ /Febuild\test_7z.exe ^
    test_7z.cpp ..\..\src\SevenZip.cpp build\lzma\*.obj
if errorlevel 1 exit /b 1
if not exist build\stream mkdir build\stream
cl /nologo /O2 /EHsc /W3 /D_CRT_SECURE_NO_WARNINGS /DSEVEN_ZIP_MAX_BLOCK=65536ULL /Ishim /I..\.. ^
    /Fobuild\stream\ /Febuild\test_7z_stream.exe test_7z.cpp ..\..\src\SevenZip.cpp build\lzma\*.obj
