@echo off
rem Builds test_inflate.exe with Visual Studio 2010's PC compiler and runs it
rem against the data make_test_zip.py made: the folder given, or .\data.
rem
rem   python make_test_zip.py C:\somewhere\data
rem   build_and_run.bat C:\somewhere\data
setlocal
cd /d "%~dp0"
call "%ProgramFiles(x86)%\Microsoft Visual Studio 10.0\VC\vcvarsall.bat" x86 >nul
if not exist build mkdir build
cl /nologo /O2 /EHsc /W3 /D_CRT_SECURE_NO_WARNINGS /Fobuild\ /Febuild\test_inflate.exe ^
    test_inflate.cpp ..\..\InflateSource.cpp ^
    ..\..\zlib\adler32.c ..\..\zlib\crc32.c ..\..\zlib\inffast.c ..\..\zlib\inflate.c ^
    ..\..\zlib\inftrees.c ..\..\zlib\zutil.c
if errorlevel 1 exit /b 1
if "%~1"=="" (build\test_inflate.exe data) else (build\test_inflate.exe "%~1")
