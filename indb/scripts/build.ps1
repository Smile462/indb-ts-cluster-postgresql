#  Copyright (c) 2026 The in-database time-series clustering authors.
#  This source code is licensed under both the GPLv2 (found in the
#  COPYING file in the root directory) and Apache 2.0 License
#  (found in the LICENSE.Apache file in the root directory).
#
# Builds the PostgreSQL extension DLL with MSVC and installs it into the
# PostgreSQL library directory.
#
#   powershell -ExecutionPolicy Bypass -File scripts/build.ps1
#
param(
  [string]$PgRoot = 'E:\pgsql',
  [string]$BuildTools = 'E:\BuildTools',
  [string]$OutDir = 'E:\build\indb-pg',
  [switch]$Install
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $PSScriptRoot
$vcvars = Join-Path $BuildTools 'VC\Auxiliary\Build\vcvars64.bat'
if (-not (Test-Path $vcvars)) { throw "vcvars64.bat not found: $vcvars" }
if (-not (Test-Path (Join-Path $PgRoot 'lib\postgres.lib'))) { throw "postgres.lib not found under $PgRoot" }

New-Item -ItemType Directory -Path $OutDir -Force | Out-Null

$coreInclude = Join-Path $here 'vendor\core\include'
$coreSources = @(
  (Join-Path $here 'vendor\core\src\math.cpp'),
  (Join-Path $here 'vendor\core\src\meta.cpp'),
  (Join-Path $here 'vendor\core\src\query.cpp')
)
$bridgeSource = Join-Path $here 'extension\bridge.cpp'
$extSource = Join-Path $here 'extension\indb_tsc.c'

$pgInclude = Join-Path $PgRoot 'include'
$pgServerInclude = Join-Path $pgInclude 'server'
$pgPortInclude = Join-Path $pgServerInclude 'port\win32'
$pgMsvcInclude = Join-Path $pgServerInclude 'port\win32_msvc'

$objects = @()
$commands = @()

foreach ($source in ($coreSources + $bridgeSource)) {
  $obj = Join-Path $OutDir ((Split-Path $source -Leaf) -replace '\.cpp$', '.obj')
  $objects += $obj
  $commands += "cl /c /nologo /O2 /MD /std:c++20 /EHsc /D_CRT_SECURE_NO_WARNINGS /DNOMINMAX " +
               "/I`"$coreInclude`" /I`"$here`" `"$source`" /Fo`"$obj`""
}
$extObj = Join-Path $OutDir 'indb_tsc.obj'
$objects += $extObj
$commands += "cl /c /nologo /O2 /MD /DWIN32 /D_CRT_SECURE_NO_WARNINGS " +
             "/I`"$pgServerInclude`" /I`"$pgInclude`" /I`"$pgMsvcInclude`" /I`"$pgPortInclude`" /I`"$here\extension`" " +
             "`"$extSource`" /Fo`"$extObj`""

$dll = Join-Path $OutDir 'indb_tsc.dll'
$linkLine = "link /DLL /NOLOGO /OUT:`"$dll`" " + (($objects | ForEach-Object { "`"$_`"" }) -join ' ') +
            " /LIBPATH:`"$(Join-Path $PgRoot 'lib')`" postgres.lib"
$commands += $linkLine

$script = ($commands -join "`r`n")
$batch = Join-Path $OutDir 'build.bat'
Set-Content -Path $batch -Value ("@echo off`r`ncall `"$vcvars`" >nul 2>&1`r`n" + $script + "`r`n") -Encoding ASCII

Write-Host "Building in $OutDir"
& cmd /c "`"$batch`""
if ($LASTEXITCODE -ne 0) { throw "build failed with exit code $LASTEXITCODE" }
if (-not (Test-Path $dll)) { throw "DLL was not produced" }
Write-Host "Built $dll"

if ($Install) {
  $target = Join-Path $PgRoot 'lib\indb_tsc.dll'
  Copy-Item $dll $target -Force
  Write-Host "Installed $target"
}
