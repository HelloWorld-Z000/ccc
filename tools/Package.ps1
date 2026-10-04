<#
.SYNOPSIS
Build, test and package matching binary/source releases with license notices.
#>
[CmdletBinding()]
param([switch]$SkipBuild, [string]$CMake = 'cmake', [string]$VcpkgRoot = $env:VCPKG_ROOT)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if (-not $VcpkgRoot) { throw 'Set VCPKG_ROOT or pass -VcpkgRoot.' }
$env:VCPKG_ROOT = $VcpkgRoot
$versionMatch = Select-String -LiteralPath "$root/CMakeLists.txt" -Pattern 'VERSION\s+(\d+\.\d+\.\d+)' | Select-Object -First 1
if (-not $versionMatch) { throw 'No project version found.' }
$version = $versionMatch.Matches[0].Groups[1].Value
$name = "Cinematic Conversation Camera $version"
$cmakeExe = (Get-Command $CMake -ErrorAction Stop).Source
$ctest = Join-Path (Split-Path $cmakeExe) 'ctest.exe'
if (-not $SkipBuild) {
    & $cmakeExe --build "$root/build/skyrim" --config Release --parallel 8
    if ($LASTEXITCODE) { throw 'Release build failed.' }
}
& $ctest --test-dir "$root/build/skyrim" -C Release --output-on-failure --no-tests=error
if ($LASTEXITCODE) { throw 'Release tests failed.' }
$dll = Get-Item -LiteralPath "$root/build/skyrim/Release/SceneDirector.dll"
if ($dll.VersionInfo.FileVersion -ne "$version.0") { throw 'DLL/project version mismatch.' }
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss-fff'
$stage = Join-Path $root "build/release-$version-$stamp"
$binary = Join-Path $stage 'binary'
$source = Join-Path $stage 'source'
New-Item -ItemType Directory -Force -Path "$binary/SKSE/Plugins", $source | Out-Null
Copy-Item -LiteralPath $dll.FullName, "$root/config/SD.ini" -Destination "$binary/SKSE/Plugins"
foreach ($doc in @('README.md','USAGE.md','LICENSE','EXCEPTIONS.md','LICENSING.md','THIRD-PARTY-NOTICES.md','BUILDING.md','CHANGELOG.md')) {
    Copy-Item -LiteralPath "$root/$doc" -Destination $binary
    Copy-Item -LiteralPath "$root/$doc" -Destination $source
}
Copy-Item -LiteralPath "$root/licenses" -Destination $binary -Recurse
Copy-Item -LiteralPath "$root/licenses" -Destination $source -Recurse
foreach ($dir in @('src','include','cmake','tests')) { Copy-Item -LiteralPath "$root/$dir" -Destination $source -Recurse }
New-Item -ItemType Directory -Path "$source/config", "$source/tools" | Out-Null
Copy-Item -LiteralPath "$root/config/SD.ini" -Destination "$source/config"
Copy-Item -LiteralPath $PSCommandPath -Destination "$source/tools"
foreach ($file in @('CMakeLists.txt','CMakePresets.json','vcpkg.json','vcpkg-configuration.json','.gitmodules','NEXUS_DESCRIPTION.bbcode')) {
    Copy-Item -LiteralPath "$root/$file" -Destination $source
}
function Copy-GitSnapshot([string]$repo, [string]$destination) {
    $changes = & git -C $repo status --porcelain
    if ($LASTEXITCODE -or $changes) { throw "Dependency snapshot must be clean: $repo" }
    $files = & git -C $repo ls-files
    if ($LASTEXITCODE) { throw "Cannot enumerate $repo" }
    foreach ($relative in $files) {
        $from = Join-Path $repo $relative
        if (Test-Path -LiteralPath $from -PathType Leaf) {
            $to = Join-Path $destination $relative
            New-Item -ItemType Directory -Force -Path (Split-Path $to) | Out-Null
            Copy-Item -LiteralPath $from -Destination $to
        }
    }
}
Copy-GitSnapshot "$root/extern/CommonLibSSE-NG" "$source/extern/CommonLibSSE-NG"
Copy-GitSnapshot "$root/extern/CommonLibSSE-NG/extern/openvr" "$source/extern/CommonLibSSE-NG/extern/openvr"
Copy-GitSnapshot "$root/build/skyrim/_deps/hde64-src" "$source/third-party-sources/minhook"
$installed = "$root/build/skyrim/vcpkg_installed"
New-Item -ItemType Directory -Path "$source/third-party-build-info" | Out-Null
Copy-Item -LiteralPath "$installed/vcpkg/status" -Destination "$source/third-party-build-info/vcpkg-status.txt"
foreach ($dep in @('fmt','spdlog','nlohmann-json','rapidcsv','directxmath','directxtk')) {
    $trees = @(Get-ChildItem -LiteralPath "$VcpkgRoot/buildtrees/$dep/src" -Directory)
    if (-not $trees.Count) { throw "Missing corresponding dependency sources: $dep" }
    $depDestination = "$source/third-party-sources/$dep"
    New-Item -ItemType Directory -Force -Path $depDestination | Out-Null
    foreach ($tree in $trees) { Copy-Item -LiteralPath $tree.FullName -Destination $depDestination -Recurse }
    Copy-Item -LiteralPath "$installed/x64-windows-static-md/share/$dep" -Destination "$source/third-party-build-info/$dep" -Recurse
}
$commonlibCommit = & git -C "$root/extern/CommonLibSSE-NG" rev-parse HEAD
$openvrCommit = & git -C "$root/extern/CommonLibSSE-NG/extern/openvr" rev-parse HEAD
# Relative paths by prefix: [IO.Path]::GetRelativePath needs PowerShell 7.
$sourcePrefix = [IO.Path]::GetFullPath($source).TrimEnd('\') + '\'
$sourceFiles = @(Get-ChildItem -LiteralPath $source -File -Recurse | Sort-Object FullName | ForEach-Object {
    [ordered]@{ path = $_.FullName.Substring($sourcePrefix.Length).Replace('\','/'); sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash }
})
[ordered]@{ version=$version; commonlibCommit=$commonlibCommit; openvrCommit=$openvrCommit; dllSha256=(Get-FileHash -LiteralPath $dll.FullName).Hash; files=$sourceFiles } |
    ConvertTo-Json -Depth 5 | Set-Content -LiteralPath "$source/SOURCE-MANIFEST.json" -Encoding utf8
Copy-Item -LiteralPath "$source/SOURCE-MANIFEST.json" -Destination $binary
$out = Join-Path $root 'package'
New-Item -ItemType Directory -Force -Path $out | Out-Null
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
# Entry names use '/' as the ZIP spec requires. Under Windows PowerShell,
# ZipFile.CreateFromDirectory writes '\', which some mod managers misread.
function New-Zip([string]$directory, [string]$zipPath) {
    $prefix = [IO.Path]::GetFullPath($directory).TrimEnd('\') + '\'
    $archive = [IO.Compression.ZipFile]::Open($zipPath, [IO.Compression.ZipArchiveMode]::Create)
    try {
        foreach ($file in Get-ChildItem -LiteralPath $directory -File -Recurse | Sort-Object FullName) {
            $entryName = $file.FullName.Substring($prefix.Length).Replace('\', '/')
            [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $file.FullName, $entryName,
                [IO.Compression.CompressionLevel]::Optimal)
        }
    } finally {
        $archive.Dispose()
    }
}
foreach ($entry in @(@{dir=$binary; suffix=''}, @{dir=$source; suffix=' Source'})) {
    $zip = Join-Path $out "$name$($entry.suffix).zip"
    foreach ($existing in @($zip, "$zip.sha256")) {
        if (Test-Path -LiteralPath $existing) {
            $backup = "$out/backups/$stamp"
            New-Item -ItemType Directory -Force -Path $backup | Out-Null
            Copy-Item -LiteralPath $existing -Destination $backup
            Remove-Item -LiteralPath $existing
        }
    }
    New-Zip $entry.dir $zip
    $hash = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash
    "$hash  $([IO.Path]::GetFileName($zip))" | Set-Content -LiteralPath "$zip.sha256" -Encoding ascii
    Write-Output "Prepared: $zip"
}
Write-Output "Staging and source manifest: $stage"
