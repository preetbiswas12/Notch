# Builds DynamicIsland.exe: the Dynamic-Island-for-Windows Windhawk mod as a
# standalone executable using Windhawk's own bundled Clang 20 toolchain.
#
# Key details of this recipe (learned the hard way):
#  - -include island_standalone_api.h replaces Windhawk's windhawk_api.h with an
#    INI-backed settings/storage shim; the mod source itself is untouched.
#  - kernel32 must be linked explicitly: llvm-mingw only resolves some APIs
#    (GetPrivateProfileIntW) through libkernel32.a, and this driver doesn't add
#    kernel32 to the default library set.
#  - libuuid.a provides the CLSID_/GUID_ symbols used by WIC code.
#  - libc++/libc++abi/libunwind are pinned to their STATIC archives (.a) with
#    "-x none"; the driver's implicit -lc++ would pick libc++.dll.a, which
#    imports Windhawk's private libc++.whl / libunwind.whl runtime DLLs - those
#    only exist inside Windhawk-hosted processes and make the standalone exe
#    exit with STATUS_DLL_NOT_FOUND (0xC0000135).
#  - The toolchain ships no static mingw libc either; msvcrt.dll based libc via
#    -lmsvcrt (driver default) is fine on any Windows 10/11 system.
$ErrorActionPreference = "Stop"

$clang = "C:\Program Files\Windhawk\Compiler\bin\clang++.exe"
$lib   = "C:\Program Files\Windhawk\Compiler\x86_64-w64-mingw32\lib"
$mod   = Join-Path $PSScriptRoot "..\mod\dynamic-island-for-windows.wh.cpp"
$outDir = Join-Path $PSScriptRoot "out"
$out    = Join-Path $outDir "DynamicIsland.exe"

New-Item -ItemType Directory -Force -Path $outDir | Out-Null

& $clang `
  "-x","c++","-std=c++23","-target","x86_64-w64-mingw32","-municode","-mwindows",`
  "-DUNICODE","-D_UNICODE","-DWINVER=0x0A00","-D_WIN32_WINNT=0x0A00","-D_WIN32_IE=0x0A00","-DNTDDI_VERSION=0x0A000008","-D__USE_MINGW_ANSI_STDIO=0",`
  "-DISLAND_STANDALONE",`
  "-include",(Join-Path $PSScriptRoot "island_standalone_api.h"),`
  "-O2","-Wall","-Wextra",`
  "-Wno-unused-parameter","-Wno-missing-field-initializers","-Wno-cast-function-type-mismatch",`
  "-o",$out,`
  (Join-Path $PSScriptRoot "island_main.cpp"),$mod,`
  "-lkernel32","-luser32","-lgdi32","-lshell32","-lole32","-loleaut32","-lwinmm","-lshcore","-ld2d1","-ldwrite","-ldwmapi",`
  "-lsetupapi","-lwinhttp","-lpdh","-ladvapi32","-limm32","-lshlwapi","-lcomdlg32","-luuid","-lwindowsapp","-lws2_32",`
  "-x","none","$lib\libc++.a","$lib\libc++abi.a","$lib\libunwind.a" 2>&1

Write-Output ("exit: {0}" -f $LASTEXITCODE)
if (Test-Path $out) {
  Write-Output ("exe:  {0}  ({1:N0} bytes)" -f $out, (Get-Item $out).Length)
}
