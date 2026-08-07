<#
.SYNOPSIS
  Builds the native encoder for Windows x64 and drops it into the plug-in.

.DESCRIPTION
  Requires CMake and the MSVC build tools. The C runtime is linked statically
  (/MT) so the plug-in has no redistributable prerequisites on the
  photographer's machine.

.EXAMPLE
  pwsh scripts/build_windows.ps1
  pwsh scripts/build_windows.ps1 -Configuration Debug -SkipTests
#>
param(
	[ValidateSet('Release', 'Debug')]
	[string]$Configuration = 'Release',
	[switch]$SkipTests
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$buildDir = Join-Path $repoRoot 'encoder/build'

cmake -S (Join-Path $repoRoot 'encoder') -B $buildDir -A x64
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed" }

cmake --build $buildDir --config $Configuration --parallel
if ($LASTEXITCODE -ne 0) { throw "cmake build failed" }

if (-not $SkipTests) {
	ctest --test-dir $buildDir -C $Configuration --output-on-failure
	if ($LASTEXITCODE -ne 0) { throw "tests failed" }
}

$source = Join-Path $buildDir "$Configuration/iso21496_encoder.exe"
if (-not (Test-Path $source)) {
	$source = Join-Path $buildDir 'iso21496_encoder.exe'
}
$destDir = Join-Path $repoRoot 'plugin/iso21496.lrdevplugin/bin/windows'
New-Item -ItemType Directory -Force -Path $destDir | Out-Null
Copy-Item $source (Join-Path $destDir 'iso21496_encoder.exe') -Force

Write-Host "built $destDir/iso21496_encoder.exe"
