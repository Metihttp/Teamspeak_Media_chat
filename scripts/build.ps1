# Builds the plugin for the 64-bit and 32-bit TeamSpeak 3 client and packages both as
# dist\TSMedia-<version>.ts3_plugin (same layout as plugins from myteamspeak.com: package.ini + plugins\*.dll).
# Users install it by double-clicking the .ts3_plugin file.
#
#   powershell -ExecutionPolicy Bypass -File scripts\build.ps1 [-Qt64 <dir>] [-Qt32 <dir>] [-No32] [-Install]
#
# -No32     skip the 32-bit build (package then lists win64 only)
# -Install  also copies the 64-bit DLL into %APPDATA%\TS3Client\plugins (close TeamSpeak first)

param(
    [string]$Qt64 = "C:\dev\Qt\5.15.2\msvc2019_64",
    [string]$Qt32 = "C:\dev\Qt\5.15.2\msvc2019",
    [string]$Config = "Release",
    [switch]$No32,
    [switch]$Install
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$dist = Join-Path $root "dist"

# Locate CMake (standalone install or the one bundled with Visual Studio / Build Tools).
$cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source
if (-not $cmake) {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    $cmake = Join-Path $vs "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
}
if (-not (Test-Path $cmake)) { throw "CMake not found. Install Visual Studio Build Tools with the C++ workload." }

function Build-Plugin([string]$arch, [string]$qtDir, [string]$buildDir, [string]$dllName, [bool]$withTests) {
    if (-not (Test-Path "$qtDir\lib\cmake\Qt5")) { throw "Qt 5.15.2 for $arch not found at $qtDir" }
    $platform = if ($arch -eq "win64") { "x64" } else { "Win32" }
    $extra = if ($withTests) { @() } else { @("-DTSMEDIA_BUILD_TESTS=OFF", "-DTSMEDIA_BUILD_TOOLS=OFF") }
    # Test hooks are forced off: the CMake cache of this folder may still have them from a test build.
    & $cmake -S $root -B $buildDir -G "Visual Studio 17 2022" -A $platform "-DCMAKE_PREFIX_PATH=$qtDir" -DTSMEDIA_TESTHOOKS=OFF @extra | Out-Host
    if ($LASTEXITCODE) { throw "CMake configure failed ($arch)" }
    & $cmake --build $buildDir --config $Config -- /m /nologo /verbosity:minimal | Out-Host
    if ($LASTEXITCODE) { throw "Build failed ($arch)" }
    $dll = Join-Path $buildDir "$Config\$dllName"
    if (-not (Test-Path $dll)) { throw "Missing $dll" }
    return $dll
}

$dlls = @{}
$dlls["win64"] = Build-Plugin "win64" $Qt64 (Join-Path $root "build") "tsmedia_win64.dll" $true
if (-not $No32) {
    $dlls["win32"] = Build-Plugin "win32" $Qt32 (Join-Path $root "build-x86") "tsmedia_win32.dll" $false
}

$version = (Select-String -Path (Join-Path $root "CMakeLists.txt") -Pattern 'project\(tsmedia VERSION ([0-9.]+)').Matches[0].Groups[1].Value
# The same text as TeamSpeak's Addons list shows (CMakeLists.txt is the only place to change it).
$description = (Select-String -Path (Join-Path $root "CMakeLists.txt") -Pattern 'set\(PLUGIN_DESCRIPTION\s+"([^"]+)"\)').Matches[0].Groups[1].Value
if (-not $description) { throw "PLUGIN_DESCRIPTION not found in CMakeLists.txt" }
$platforms = (@("win32", "win64") | Where-Object { $dlls.ContainsKey($_) }) -join ", "

# Package: a .ts3_plugin is a zip with package.ini + plugins\<name>_<platform>.dll
$stage = Join-Path $root "build\package"
if (Test-Path $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
New-Item -ItemType Directory -Force "$stage\plugins" | Out-Null
foreach ($dll in $dlls.Values) { Copy-Item $dll "$stage\plugins\" }
@"
Name = TS Media chat
Type = Plugin
Author = MehdiHttp
Version = $version
Platforms = $platforms
Description = "$description"
"@ | Set-Content -Encoding UTF8 "$stage\package.ini"

New-Item -ItemType Directory -Force $dist | Out-Null
$plugin = Join-Path $dist "TSMedia-$version.ts3_plugin"
if (Test-Path $plugin) { Remove-Item -LiteralPath $plugin }

# Same entry layout as packages from myteamspeak.com (e.g. AutoFollow): package.ini first, then an explicit
# "plugins/" folder entry, then the DLLs; '/' separators, deflate.
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
$stream  = [System.IO.File]::Open($plugin, [System.IO.FileMode]::CreateNew)
$archive = New-Object System.IO.Compression.ZipArchive($stream, [System.IO.Compression.ZipArchiveMode]::Create)
try {
    # TeamSpeak's package_inst decides "folder or file" from the MS-DOS attributes, not from the trailing '/':
    # without FILE_ATTRIBUTE_DIRECTORY (0x10) on "plugins/" it fails with "Failed to extract: plugins".
    $fileAttr = 0x20 # FILE_ATTRIBUTE_ARCHIVE, as in packages from myteamspeak.com
    $dirAttr  = 0x10 # FILE_ATTRIBUTE_DIRECTORY
    $level = [System.IO.Compression.CompressionLevel]::Optimal
    $entry = [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, "$stage\package.ini", "package.ini", $level)
    $entry.ExternalAttributes = $fileAttr
    $entry = $archive.CreateEntry("plugins/")
    $entry.ExternalAttributes = $dirAttr
    foreach ($dll in (Get-ChildItem "$stage\plugins" -Filter *.dll | Sort-Object Name)) {
        $entry = [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $dll.FullName, "plugins/" + $dll.Name, $level)
        $entry.ExternalAttributes = $fileAttr
    }
} finally {
    $archive.Dispose()
    $stream.Dispose()
}

# Verify: every DLL in the package must be byte-identical to the build output.
$check = [System.IO.Compression.ZipFile]::OpenRead($plugin)
try {
    foreach ($dll in $dlls.Values) {
        $entry = $check.GetEntry("plugins/" + (Split-Path $dll -Leaf))
        if (-not $entry -or $entry.Length -ne (Get-Item $dll).Length) { throw "Package check failed for $dll" }
    }
} finally {
    $check.Dispose()
}
Write-Host "Package: $plugin ($platforms)"

if ($Install) {
    $target = Join-Path $env:APPDATA "TS3Client\plugins"
    New-Item -ItemType Directory -Force $target | Out-Null
    Copy-Item $dlls["win64"] $target -Force
    Write-Host "Installed: $target\tsmedia_win64.dll"
}
