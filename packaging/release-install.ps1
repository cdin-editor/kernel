[CmdletBinding()]
param(
    [string]$Prefix = (Join-Path $env:LOCALAPPDATA 'cdin-kernel'),
    [switch]$AddBinToUserPath
)

$ErrorActionPreference = 'Stop'
$PackageRoot = $PSScriptRoot

$RequiredFiles = @(
    (Join-Path $PackageRoot 'include\cdin\kernel.h'),
    (Join-Path $PackageRoot 'lib\libcdin_kernel.a'),
    (Join-Path $PackageRoot 'lib\libcdin_kernel.dll.a'),
    (Join-Path $PackageRoot 'bin\cdin_kernel.dll')
)
foreach ($File in $RequiredFiles) {
    if (-not (Test-Path -LiteralPath $File -PathType Leaf)) {
        throw "Release archive is incomplete; missing: $File"
    }
}

$Prefix = [System.IO.Path]::GetFullPath($Prefix)
foreach ($Directory in @('include', 'lib', 'bin', 'share')) {
    $Source = Join-Path $PackageRoot $Directory
    if (-not (Test-Path -LiteralPath $Source -PathType Container)) { continue }
    $Destination = Join-Path $Prefix $Directory
    New-Item -ItemType Directory -Path $Destination -Force | Out-Null
    Get-ChildItem -LiteralPath $Source -Force | ForEach-Object {
        Copy-Item -LiteralPath $_.FullName -Destination $Destination -Recurse -Force
    }
}

if ($AddBinToUserPath) {
    $CurrentPath = [Environment]::GetEnvironmentVariable('Path', 'User')
    $PathEntries = @($CurrentPath -split ';' | Where-Object { $_ })
    $BinPath = Join-Path $Prefix 'bin'
    if (-not ($PathEntries | Where-Object { $_.TrimEnd('\\') -ieq $BinPath.TrimEnd('\\') })) {
        $NewPath = (@($PathEntries) + $BinPath) -join ';'
        [Environment]::SetEnvironmentVariable('Path', $NewPath, 'User')
    }
}

Write-Host "Installed cdin-kernel to $Prefix"
Write-Host "Header: $(Join-Path $Prefix 'include\cdin\kernel.h')"
Write-Host "Static library: $(Join-Path $Prefix 'lib\libcdin_kernel.a')"
Write-Host "DLL: $(Join-Path $Prefix 'bin\cdin_kernel.dll')"
Write-Host 'Link consumers against lib\libcdin_kernel.a (static) or lib\libcdin_kernel.dll.a (shared).'
if (-not $AddBinToUserPath) {
    Write-Host 'Keep bin\ available on PATH or place cdin_kernel.dll next to the consuming executable.'
}
