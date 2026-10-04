# Install the local texture importer's pinned tools on Windows; never download game data.
# The same as setup_wwhd_tools.sh: a Python environment in build\wwhd-tools with the
# versions in scripts\wwhd\requirements.txt, and the small Yaz0 decoder when Visual Studio's
# C compiler is installed (without it the importer uses its slower Python decoder).
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$venv = Join-Path $root 'build\wwhd-tools'
$python = if ($env:PYTHON) { $env:PYTHON } else { 'python' }
& $python -m venv $venv
if ($LASTEXITCODE) { throw "python -m venv failed (Python 3.11 or newer is needed)" }
$venvPython = Join-Path $venv 'Scripts\python.exe'
& $venvPython -m pip install -r (Join-Path $root 'scripts\wwhd\requirements.txt')
if ($LASTEXITCODE) { throw "pip install failed" }

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = if (Test-Path $vswhere) {
    & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
}
$vcvars = if ($vs) { Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat' }
if ($vcvars -and (Test-Path $vcvars)) {
    $work = Join-Path $venv 'yaz0-build'
    New-Item -ItemType Directory -Force $work | Out-Null
    $source = Join-Path $root 'scripts\wwhd\yaz0_native.c'
    $dll = Join-Path $venv 'wwhd_yaz0.dll'
    $bat = Join-Path $work 'build.bat'
    Set-Content -Encoding ascii $bat @(
        '@echo off',
        # vcvars sets these itself, and can fail on an old value with "(x86)" in it.
        'set INCLUDE=',
        'set LIB=',
        'set LIBPATH=',
        "set `"PATH=%SystemRoot%\system32;%SystemRoot%;%SystemRoot%\System32\WindowsPowerShell\v1.0;$(Split-Path -Parent $vswhere)`"",
        "call `"$vcvars`" >nul || exit /b 1",
        "cl /nologo /O2 /LD `"$source`" /Fo`"$work\\`" /Fe`"$dll`" /link /EXPORT:wwhd_yaz0_decode /IMPLIB:`"$work\wwhd_yaz0.lib`" >nul")
    & cmd /c $bat
    if ($LASTEXITCODE) { Write-Host 'The Yaz0 decoder did not build; the importer will use its slower Python one.' }
} else {
    Write-Host "No Visual Studio C compiler found; the importer will use its slower Python Yaz0 decoder."
}
Write-Host "Ready: $venvPython $(Join-Path $root 'scripts\import_wwhd_textures.py') --help"
