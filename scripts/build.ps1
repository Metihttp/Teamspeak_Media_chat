# Builds the plugin for the 64-bit and 32-bit TeamSpeak 3 client and packages both as
# dist\TSMedia-<version>.ts3_plugin (same layout as plugins from myteamspeak.com: package.ini + plugins\*.dll).
# Users install it by double-clicking the .ts3_plugin file.
#
#   powershell -ExecutionPolicy Bypass -File scripts\build.ps1 [-Qt64 <dir>] [-Qt32 <dir>] [-No32] [-Install]
#                                                             [-Unsigned] [-SigningDir <dir>] [-KeyId <n>]
#                                                             [-CaptureReference <TeamSpeak folder>] [-VerifyPublished]
#
# -No32       skip the 32-bit build (package then lists win64 only)
# -Install    also copies the 64-bit DLL into %APPDATA%\TS3Client\plugins (close TeamSpeak first)
#
# Updates (docs/UPDATES.md): when the release signing key is available (-SigningDir, default
# $env:TSMEDIA_SIGNING_DIR or C:\dev\tsmedia-devtools\signing), the build turns the update check on
# (TSMEDIA_UPDATER=ON), writes the raw update assets (TSMedia-<ver>-win64.update, ...) and the signed
# dist\tsmedia-update.json, checks the signature against the public key in src\update\updatekeys.h,
# and prints the draft-first release commands. Without the key (anyone else's build) it builds with the
# update check off, so a self-built copy never replaces itself, and says so.
# -Unsigned   build without the update check and without signing, even if the key is there
# -KeyId <n>  the signing key (1 = daily; 2 = recovery, which must come with -RevokeKeys 1)
# -RevokeKeys <ids>  key ids the release revokes for good (docs/UPDATES.md, "Keys")
# -CaptureReference <dir>  save the exports of TeamSpeak's Qt DLLs in <dir> as reference\ts-exports\<arch>.txt
#             (once per TeamSpeak version); later builds check every Qt symbol the plugin imports against it
# -VerifyPublished  download the published tsmedia-update.json of the latest release and check its
#             signature and version (network: github.com), then stop

param(
    [string]$Qt64 = "C:\dev\Qt\5.15.2\msvc2019_64",
    [string]$Qt32 = "C:\dev\Qt\5.15.2\msvc2019",
    [string]$Config = "Release",
    [switch]$No32,
    [switch]$Install,
    [switch]$Unsigned,
    [string]$SigningDir = "",
    [int]$KeyId = 1,
    [int[]]$RevokeKeys = @(),
    [string]$CaptureReference = "",
    [switch]$VerifyPublished
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$dist = Join-Path $root "dist"
$repoSlug = "Metihttp/Teamspeak_Media_chat"
if (-not $SigningDir) { $SigningDir = if ($env:TSMEDIA_SIGNING_DIR) { $env:TSMEDIA_SIGNING_DIR } else { "C:\dev\tsmedia-devtools\signing" } }

# ---- helpers for the update manifest --------------------------------------------------------------

# The public keys built into the plugin, read from src\update\updatekeys.h: id -> 64 bytes X||Y.
function Get-TrustedKeys {
    $text = Get-Content -Raw (Join-Path $root "src\update\updatekeys.h")
    $keys = @{}
    foreach ($m in [regex]::Matches($text, '\{\s*(\d+)\s*,\s*(?:true|false)\s*,\s*\{([^}]*)\}\}')) {
        $bytes = [byte[]]@($m.Groups[2].Value -split ',' | ForEach-Object { $_.Trim() } | Where-Object { $_ } | ForEach-Object { [Convert]::ToByte($_.Substring(2), 16) })
        if ($bytes.Length -eq 64) { $keys[[int]$m.Groups[1].Value] = $bytes }
    }
    return $keys
}

function Test-ManifestSignature([string]$json, [hashtable]$keys) {
    $outer = $json | ConvertFrom-Json
    if ($outer.format -ne 1) { throw "Manifest format is not 1." }
    $xy = $keys[[int]$outer.key]
    if (-not $xy) { throw "Manifest is signed with key $($outer.key), which the plugin doesn't have." }
    $payload = [Convert]::FromBase64String($outer.payload)
    $sig = [Convert]::FromBase64String($outer.sig)
    $params = New-Object System.Security.Cryptography.ECParameters
    $params.Curve = [System.Security.Cryptography.ECCurve+NamedCurves]::nistP256
    $point = New-Object System.Security.Cryptography.ECPoint
    $point.X = [byte[]]$xy[0..31]; $point.Y = [byte[]]$xy[32..63]
    $params.Q = $point
    $ecdsa = [System.Security.Cryptography.ECDsa]::Create($params)
    if (-not $ecdsa.VerifyData($payload, $sig, [System.Security.Cryptography.HashAlgorithmName]::SHA256)) { throw "Manifest signature check failed." }
    return ([System.Text.Encoding]::UTF8.GetString($payload) | ConvertFrom-Json)
}

function Get-Dumpbin {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    $tools = Get-ChildItem (Join-Path $vs "VC\Tools\MSVC") -Directory | Sort-Object Name -Descending | Select-Object -First 1
    $exe = Join-Path $tools.FullName "bin\Hostx64\x64\dumpbin.exe"
    if (-not (Test-Path $exe)) { throw "dumpbin.exe not found (Visual Studio C++ tools)." }
    return $exe
}

# module -> list of imported names (normal imports only; delay-loaded Media Foundation DLLs may be missing by design)
function Get-Imports([string]$dumpbin, [string]$file) {
    $result = @{}
    $module = $null
    foreach ($line in (& $dumpbin /nologo /imports $file)) {
        if ($line -match '^\s+Section contains the following delay load imports') { break }
        if ($line -match '^\s{4}(\S+\.(dll|DLL))\s*$') { $module = $Matches[1]; $result[$module] = New-Object System.Collections.Generic.List[string]; continue }
        if ($module -and $line -match '^\s+[0-9A-F]+\s+(\S+)\s*$') { $result[$module].Add($Matches[1]) }
    }
    return $result
}

function Get-Exports([string]$dumpbin, [string]$file) {
    $names = New-Object System.Collections.Generic.HashSet[string]
    foreach ($line in (& $dumpbin /nologo /exports $file)) {
        if ($line -match '^\s+\d+\s+[0-9A-F]+\s+[0-9A-F]{8}\s+(\S+)') { [void]$names.Add($Matches[1]) }
        elseif ($line -match '^\s+\d+\s+[0-9A-F]+\s+(\S+)\s+\(forwarded') { [void]$names.Add($Matches[1]) }
    }
    return $names
}

# The DLLs the plugin may import. A new one fails the build: on a friend's PC it may be missing, and
# a plugin that doesn't load can never update itself again.
$allowedModules = @("Qt5Core.dll", "Qt5Gui.dll", "Qt5Widgets.dll", "KERNEL32.dll", "USER32.dll", "GDI32.dll", "SHELL32.dll", "ADVAPI32.dll",
                    "ole32.dll", "OLEAUT32.dll", "bcrypt.dll", "d3d11.dll", "dxgi.dll", "dwmapi.dll", "propsys.dll",
                    "MSVCP140.dll", "MSVCP140_1.dll", "VCRUNTIME140.dll", "VCRUNTIME140_1.dll")

function Test-Imports([string]$dumpbin, [string]$dll, [string]$arch) {
    $imports = Get-Imports $dumpbin $dll
    foreach ($m in $imports.Keys) {
        $ok = $m -like "api-ms-win-*" -or ($allowedModules | Where-Object { $_ -ieq $m })
        if (-not $ok) { throw "$dll imports $m, which is not on the allowlist (scripts\build.ps1). A missing DLL on a user's PC would stop the plugin from loading." }
        if ($m -ieq "winhttp.dll") { throw "$dll imports winhttp.dll: it must be loaded on demand only." }
    }
    $reference = Join-Path $root "reference\ts-exports\$arch.txt"
    if (-not (Test-Path $reference)) {
        Write-Warning "No ${reference}: Qt symbols not checked against a real TeamSpeak $arch install (run once with -CaptureReference)."
        return
    }
    $known = @{}
    foreach ($line in Get-Content -Encoding UTF8 $reference) { $p = $line.Split("`t"); if ($p.Length -eq 2) { $known["$($p[0].Trim([char]0xFEFF).ToLower())`t$($p[1])"] = $true } }
    foreach ($m in $imports.Keys | Where-Object { $_ -like "Qt5*" }) {
        foreach ($name in $imports[$m]) {
            if (-not $known.ContainsKey("$($m.ToLower())`t$name")) { throw "$dll imports $m!$name, which TeamSpeak's $m doesn't export ($reference)." }
        }
    }
    Write-Host "Import check passed for $arch (Qt symbols checked against $reference)."
}

# ---- one-off modes ----------------------------------------------------------------------------------

if ($VerifyPublished) {
    $url = "https://github.com/$repoSlug/releases/latest/download/tsmedia-update.json"
    Write-Host "Fetching $url"
    $json = (Invoke-WebRequest -UseBasicParsing -Uri $url).Content
    if ($json -is [byte[]]) { $json = [System.Text.Encoding]::UTF8.GetString($json) }
    $payload = Test-ManifestSignature $json (Get-TrustedKeys)
    Write-Host "Published manifest OK: version $($payload.version), signed, files: $(@($payload.files.PSObject.Properties.Name) -join ', ')"
    exit 0
}

if ($CaptureReference) {
    $dumpbin = Get-Dumpbin
    $arch = if (Test-Path (Join-Path $CaptureReference "ts3client_win64.exe")) { "win64" } else { "win32" }
    $out = Join-Path $root "reference\ts-exports\$arch.txt"
    New-Item -ItemType Directory -Force (Split-Path $out) | Out-Null
    $lines = foreach ($qt in @("Qt5Core.dll", "Qt5Gui.dll", "Qt5Widgets.dll")) {
        foreach ($name in (Get-Exports $dumpbin (Join-Path $CaptureReference $qt))) { "$qt`t$name" }
    }
    Set-Content -Path $out -Value $lines -Encoding UTF8
    Write-Host "Saved $($lines.Count) exports of TeamSpeak's Qt DLLs ($arch) to $out"
    exit 0
}

# ---- build --------------------------------------------------------------------------------------------

# Locate CMake (standalone install or the one bundled with Visual Studio / Build Tools).
$cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source
if (-not $cmake) {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    $cmake = Join-Path $vs "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
}
if (-not (Test-Path $cmake)) { throw "CMake not found. Install Visual Studio Build Tools with the C++ workload." }

# Signing (and with it the update check) only with the release key at hand.
$signTool = Join-Path $SigningDir "tsmedia-keys.ps1"
$haveKey = (Test-Path $signTool) -and ((Test-Path (Join-Path $SigningDir "release-key-$KeyId.dpapi")) -or (Test-Path (Join-Path $SigningDir "release-key-$KeyId.pkey")))
$sign = $haveKey -and -not $Unsigned
if ($sign) {
    Write-Host "Release signing key $KeyId found in $SigningDir. The update check is built in and the release is signed."
} elseif ($Unsigned) {
    Write-Host "-Unsigned: building without the update check and without tsmedia-update.json."
} else {
    Write-Host "No release signing key in ${SigningDir}: building without the update check (a copy you build yourself never replaces itself) and without tsmedia-update.json."
}
$updater = if ($sign) { "ON" } else { "OFF" }

function Build-Plugin([string]$arch, [string]$qtDir, [string]$buildDir, [string]$dllName, [bool]$withTests) {
    if (-not (Test-Path "$qtDir\lib\cmake\Qt5")) { throw "Qt 5.15.2 for $arch not found at $qtDir" }
    $platform = if ($arch -eq "win64") { "x64" } else { "Win32" }
    $extra = if ($withTests) { @() } else { @("-DTSMEDIA_BUILD_TESTS=OFF", "-DTSMEDIA_BUILD_TOOLS=OFF") }
    # Test hooks are forced off: the CMake cache of this folder may still have them from a test build.
    & $cmake -S $root -B $buildDir -G "Visual Studio 17 2022" -A $platform "-DCMAKE_PREFIX_PATH=$qtDir" -DTSMEDIA_TESTHOOKS=OFF "-DTSMEDIA_UPDATER=$updater" @extra | Out-Host
    if ($LASTEXITCODE) { throw "CMake configure failed ($arch)" }
    & $cmake --build $buildDir --config $Config -- /m /nologo /verbosity:minimal | Out-Host
    if ($LASTEXITCODE) { throw "Build failed ($arch)" }
    $dll = Join-Path $buildDir "$Config\$dllName"
    if (-not (Test-Path $dll)) { throw "Missing $dll" }
    return $dll
}

$dlls = @{}
$buildDirs = @{ "win64" = (Join-Path $root "build"); "win32" = (Join-Path $root "build-x86") }
$dlls["win64"] = Build-Plugin "win64" $Qt64 $buildDirs["win64"] "tsmedia_win64.dll" $true
if (-not $No32) {
    $dlls["win32"] = Build-Plugin "win32" $Qt32 $buildDirs["win32"] "tsmedia_win32.dll" $false
}

$version = (Select-String -Path (Join-Path $root "CMakeLists.txt") -Pattern 'project\(tsmedia VERSION ([0-9.]+)').Matches[0].Groups[1].Value
# The same text as TeamSpeak's Addons list shows (CMakeLists.txt is the only place to change it).
$description = (Select-String -Path (Join-Path $root "CMakeLists.txt") -Pattern 'set\(PLUGIN_DESCRIPTION\s+"([^"]+)"\)').Matches[0].Groups[1].Value
if (-not $description) { throw "PLUGIN_DESCRIPTION not found in CMakeLists.txt" }
$platforms = (@("win32", "win64") | Where-Object { $dlls.ContainsKey($_) }) -join ", "

# Every DLL must load on users' PCs: only allowlisted DLLs, and only Qt symbols TeamSpeak's Qt exports.
$dumpbin = Get-Dumpbin
foreach ($arch in $dlls.Keys) { Test-Imports $dumpbin $dlls[$arch] $arch }

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

# ---- update assets and the signed manifest -------------------------------------------------------------

if ($sign) {
    function Get-Sha256([string]$file) { (Get-FileHash -Algorithm SHA256 -LiteralPath $file).Hash.ToLower() }

    # Up to 5 "- " bullets under "### Highlights" in docs\release-notes\v<version>.md, markdown removed.
    $notesFile = Join-Path $root "docs\release-notes\v$version.md"
    if (-not (Test-Path $notesFile)) { throw "Missing ${notesFile}: the update dialog shows its Highlights." }
    $notes = New-Object System.Collections.Generic.List[string]
    $inHighlights = $false
    foreach ($line in Get-Content -Encoding UTF8 $notesFile) {
        if ($line -match '^###\s+Highlights') { $inHighlights = $true; continue }
        if ($inHighlights -and $line -match '^#{1,6}\s') { break }
        if ($inHighlights -and $line -match '^\s*-\s+(.+)$' -and $notes.Count -lt 5) {
            $text = $Matches[1] -replace '\[([^\]]+)\]\([^)]+\)', '$1' -replace '[*_`]', ''
            $notes.Add($text.Trim())
        }
    }
    if ($notes.Count -eq 0) { throw "No '- ' bullets under '### Highlights' in $notesFile." }

    $files = [ordered]@{}
    $assets = New-Object System.Collections.Generic.List[string]
    foreach ($arch in @("win64", "win32")) {
        if (-not $dlls.ContainsKey($arch)) { continue }
        $machine = if ($arch -eq "win64") { "x64" } else { "x86" }
        $dllAsset = Join-Path $dist "TSMedia-$version-$arch.update"
        Copy-Item -LiteralPath $dlls[$arch] -Destination $dllAsset -Force
        $files["plugins/tsmedia_$arch.dll"] = [ordered]@{ size = (Get-Item $dllAsset).Length; sha256 = (Get-Sha256 $dllAsset); machine = $machine }
        $assets.Add($dllAsset)
        $helper = Join-Path $buildDirs[$arch] "$Config\tsmedia_update_helper.exe"
        if (-not (Test-Path $helper)) { throw "Missing $helper" }
        $helperAsset = Join-Path $dist "TSMedia-$version-helper-$arch.update"
        Copy-Item -LiteralPath $helper -Destination $helperAsset -Force
        $files["helper/tsmedia_update_helper_$arch.exe"] = [ordered]@{ size = (Get-Item $helperAsset).Length; sha256 = (Get-Sha256 $helperAsset); machine = $machine }
        $assets.Add($helperAsset)
    }
    $payloadObject = [ordered]@{
        format    = 1
        product   = "tsmedia"
        version   = $version
        tag       = "v$version"
        published = (Get-Date).ToUniversalTime().ToString("yyyy-MM-dd")
        package   = [ordered]@{ name = "TSMedia-$version.ts3_plugin"; size = (Get-Item $plugin).Length; sha256 = (Get-Sha256 $plugin) }
        files     = $files
        notes     = @($notes)
    }
    if ($RevokeKeys.Count -gt 0) { $payloadObject["revokeKeys"] = @($RevokeKeys) }
    $payloadBytes = (New-Object System.Text.UTF8Encoding($false)).GetBytes(($payloadObject | ConvertTo-Json -Compress -Depth 6))
    if ($payloadBytes.Length -gt 48KB) { throw "The manifest payload is larger than 48 KiB." }
    $payloadFile = Join-Path $dist "tsmedia-update.payload.tmp"
    $sigFile = Join-Path $dist "tsmedia-update.sig.tmp"
    [System.IO.File]::WriteAllBytes($payloadFile, $payloadBytes)
    try {
        & powershell -NoProfile -ExecutionPolicy Bypass -File $signTool Sign -KeyId $KeyId -InFile $payloadFile -OutFile $sigFile -Dir $SigningDir -Quiet
        if ($LASTEXITCODE) { throw "Signing failed." }
        $sig = [System.IO.File]::ReadAllBytes($sigFile)
    } finally {
        Remove-Item -LiteralPath $payloadFile, $sigFile -ErrorAction SilentlyContinue
    }
    $manifest = '{"format":1,"key":' + $KeyId + ',"payload":"' + [Convert]::ToBase64String($payloadBytes) + '","sig":"' + [Convert]::ToBase64String($sig) + '"}'
    # Self-check against the public key the plugin really contains.
    $checked = Test-ManifestSignature $manifest (Get-TrustedKeys)
    if ($checked.version -ne $version) { throw "Self-check read version $($checked.version)." }
    $manifestFile = Join-Path $dist "tsmedia-update.json"
    [System.IO.File]::WriteAllText($manifestFile, $manifest, (New-Object System.Text.UTF8Encoding($false)))
    Write-Host "Signed manifest: $manifestFile (key $KeyId, $($notes.Count) highlights, self-check passed)"

    $assetArgs = (@($plugin, $manifestFile) + $assets | ForEach-Object { "`"$_`"" }) -join " "
    Write-Host ""
    Write-Host "Release (draft first, so 'latest' never lacks tsmedia-update.json):"
    Write-Host "  gh release create v$version --draft --title `"TS Media chat $version`" --notes-file docs/release-notes/v$version.md $assetArgs"
    Write-Host "  gh release edit v$version --draft=false --latest      (a backport: --latest=false)"
    Write-Host "  powershell -ExecutionPolicy Bypass -File scripts\build.ps1 -VerifyPublished"
}

if ($Install) {
    $target = Join-Path $env:APPDATA "TS3Client\plugins"
    New-Item -ItemType Directory -Force $target | Out-Null
    Copy-Item $dlls["win64"] $target -Force
    Write-Host "Installed: $target\tsmedia_win64.dll"
}
