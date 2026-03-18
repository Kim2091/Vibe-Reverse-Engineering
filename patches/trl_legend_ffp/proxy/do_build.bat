@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" x86 >/dev/null 2>&1
cd /d "C:\Users\skurtyy\Documents\GitHub\TombRaiderLegendRTX\Vibe-Reverse-Engineering\patches\trl_legend_ffp\proxy"
echo Compiling d3d9_main.c...
cl.exe /nologo /O1 /GS- /W3 /Zl /c /D "WIN32" /D "NDEBUG" d3d9_main.c
if errorlevel 1 exit /b 1
echo Compiling d3d9_wrapper.c...
cl.exe /nologo /O1 /GS- /W3 /Zl /c /D "WIN32" /D "NDEBUG" d3d9_wrapper.c
if errorlevel 1 exit /b 1
echo Compiling d3d9_device.c...
cl.exe /nologo /O1 /Oi /GS- /W3 /Zl /c /D "WIN32" /D "NDEBUG" d3d9_device.c
if errorlevel 1 exit /b 1
echo Linking d3d9.dll...
link.exe /nologo /DLL /NODEFAULTLIB /ENTRY:_DllMainCRTStartup@12 /DEF:d3d9.def /OUT:d3d9.dll d3d9_main.obj d3d9_wrapper.obj d3d9_device.obj kernel32.lib
if errorlevel 1 exit /b 1
echo === Build successful ===
del *.obj *.lib *.exp 2>/dev/null
