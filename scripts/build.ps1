# Builds the plugin for the 64-bit and 32-bit TeamSpeak 3 client and packages both as
# dist\TSMedia-<version>.ts3_plugin (same layout as plugins from myteamspeak.com: package.ini + plugins\*.dll).
# Users install it by double-clicking the .ts3_plugin file.
#
#   powershell -ExecutionPolicy Bypass -File scripts\build.ps1 [-Release] [-Sign] [-Qt64 <dir>] [-Qt32 <dir>] [-No32] [-Install]
#                                                             [-SigningDir <dir>] [-KeyId <n>] [-RevokeKeys <ids>] [-BridgeManifest <file>]
#                                                             [-CaptureReference <TeamSpeak folder>] [-VerifyPublished [-VerifyFrom <url>]]
#
# -No32       skip the 32-bit build (package then lists win64 only); refused when tsmedia-update.json is
#             signed (2.2.1, -Sign): 2.2.0 users on 32-bit TeamSpeak would never be updated
# -Install    also copies the 64-bit DLL into %APPDATA%\TS3Client\plugins (close TeamSpeak first)
#
# Updates (docs/UPDATES.md). Without -Release the build has no update check, so a copy you build
# yourself never replaces itself. -Release builds the update check in (TSMEDIA_UPDATER=ON) and writes
# dist\upload-<version>\ with exactly the files of the GitHub release:
#   TSMedia-<ver>.ts3_plugin, the .update files (raw DLLs and restart helpers),
#   tsmedia-update-v2.json  unsigned: what 2.2.1 and later read; every file is checked against its SHA-256
#   tsmedia-update.json     signed: the only file 2.2.0 reads. For 2.2.1 (the bridge) it is signed with
#                           key 1 automatically; later releases reuse the published signed file of
#                           v2.2.1, so 2.2.0 users still update to 2.2.1 first and from there on. No key needed.
# -Release also needs final notes (docs\release-notes\v<version>.md without its DRAFT comment) and the
# release date in CHANGELOG.md ("## [<version>] - YYYY-MM-DD").
# -Sign       also sign tsmedia-update.json for this version (2.2.0 users then update to it directly).
#             Key 1 from the signing folder (-SigningDir, default $env:TSMEDIA_SIGNING_DIR or
#             C:\dev\tsmedia-devtools\signing), DPAPI, no prompt.
# -KeyId <n>, -RevokeKeys <ids>  the signing key, and key ids a signed release revokes for good
# -BridgeManifest <file>  the signed tsmedia-update.json of v2.2.1 to reuse, instead of downloading it
# -CaptureReference <dir>  save the exports of TeamSpeak's Qt DLLs in <dir> as reference\ts-exports\<arch>.txt
#             (once per TeamSpeak version); later builds check every Qt symbol the plugin imports against it
# -VerifyPublished  download both manifests of the latest release and every file they list, and check
#             sizes, SHA-256 and the signature of tsmedia-update.json, that the signed file still serves
#             2.2.0 users on both architectures, and that the manifest is the latest release's own
#             (network: github.com), then stop.
#             -VerifyFrom <url> checks another server with GitHub's paths instead (tests).

param(
    [string]$Qt64 = "C:\dev\Qt\5.15.2\msvc2019_64",
    [string]$Qt32 = "C:\dev\Qt\5.15.2\msvc2019",
    [string]$Config = "Release",
    [switch]$No32,
    [switch]$Install,
    [switch]$Release,
    [switch]$Sign,
    [string]$SigningDir = "",
    [int]$KeyId = 1,
    [int[]]$RevokeKeys = @(),
    [string]$BridgeManifest = "",
    [string]$CaptureReference = "",
    [switch]$VerifyPublished,
    [string]$VerifyFrom = ""
)

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue" # Invoke-WebRequest is many times slower with its progress bar
if ([Net.ServicePointManager]::SecurityProtocol -ne [Net.SecurityProtocolType]::SystemDefault) {
    [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
}
$root = Split-Path -Parent $PSScriptRoot
$dist = Join-Path $root "dist"
$repoSlug = "Metihttp/Teamspeak_Media_chat"
if (-not $SigningDir) { $SigningDir = if ($env:TSMEDIA_SIGNING_DIR) { $env:TSMEDIA_SIGNING_DIR } else { "C:\dev\tsmedia-devtools\signing" } }
# The first version that reads tsmedia-update-v2.json. 2.2.0 reads only a signed tsmedia-update.json,
# so this release is signed, and later releases publish its signed file again (the bridge).
$bridgeVersion = [version]"2.2.1"
$v2Name = "tsmedia-update-v2.json"
$legacyName = "tsmedia-update.json"

# ---- helpers for the update manifests -------------------------------------------------------------

# The public keys built into the plugin, read from src\update\updatekeys.h: id -> 64 bytes X||Y.
# The test key 99 only for tests against a local server (release builds don't trust it).
function Get-TrustedKeys([switch]$WithTestKey) {
    $text = Get-Content -Raw (Join-Path $root "src\update\updatekeys.h")
    $keys = @{}
    foreach ($m in [regex]::Matches($text, '\{\s*(\d+)\s*,\s*(?:true|false)\s*,\s*\{([^}]*)\}\}')) {
        $id = [int]$m.Groups[1].Value
        if ($id -eq 99 -and -not $WithTestKey) { continue }
        $bytes = [byte[]]@($m.Groups[2].Value -split ',' | ForEach-Object { $_.Trim() } | Where-Object { $_ } | ForEach-Object { [Convert]::ToByte($_.Substring(2), 16) })
        if ($bytes.Length -eq 64) { $keys[$id] = $bytes }
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

# Either manifest format, checked like the plugin does (the plugin's own parser is stricter still):
# format 2 = plain and unsigned (no signature fields allowed), format 1 = signed (signature must verify).
# Returns @{ Payload; Signed; KeyId; Version; Tag }.
function Read-Manifest([string]$json, [hashtable]$keys) {
    $outer = $json | ConvertFrom-Json
    if ($outer.format -eq 2) {
        foreach ($field in @("key", "sig", "payload")) {
            if ($outer.PSObject.Properties.Name -contains $field) { throw "An unsigned (format 2) manifest has a '$field' field." }
        }
        $payload = $outer; $signed = $false; $key = 0
    } elseif ($outer.format -eq 1) {
        $payload = Test-ManifestSignature $json $keys; $signed = $true; $key = [int]$outer.key
    } else {
        throw "Unknown manifest format '$($outer.format)'."
    }
    if ($payload.product -ne "tsmedia") { throw "Manifest product is '$($payload.product)'." }
    if ($payload.version -notmatch '^(0|[1-9][0-9]{0,2})\.(0|[1-9][0-9]{0,2})\.(0|[1-9][0-9]{0,2})$') { throw "Manifest version '$($payload.version)' is not valid." }
    if ($payload.tag -ne "v$($payload.version)") { throw "Manifest tag '$($payload.tag)' doesn't match its version." }
    return @{ Payload = $payload; Signed = $signed; KeyId = $key; Version = [version]$payload.version; Tag = $payload.tag }
}

# manifest "files" key -> release asset name (the plugin builds the same names from the version).
function Get-AssetName([string]$fileKey, [string]$version) {
    switch ($fileKey) {
        "plugins/tsmedia_win64.dll" { return "TSMedia-$version-win64.update" }
        "plugins/tsmedia_win32.dll" { return "TSMedia-$version-win32.update" }
        "helper/tsmedia_update_helper_win64.exe" { return "TSMedia-$version-helper-win64.update" }
        "helper/tsmedia_update_helper_win32.exe" { return "TSMedia-$version-helper-win32.update" }
    }
    return $null
}

function Get-Sha256([string]$file) { (Get-FileHash -Algorithm SHA256 -LiteralPath $file).Hash.ToLower() }

# Downloads $url to $file; $false on 404, throws on anything else (after 3 tries for network errors).
# A time limit per try: without one, a connection that stalls keeps the build waiting for good.
function Save-Url([string]$url, [string]$file) {
    for ($attempt = 1; ; $attempt++) {
        try {
            Invoke-WebRequest -UseBasicParsing -Uri $url -OutFile $file -TimeoutSec 120
            return $true
        } catch [System.Net.WebException] {
            $response = $_.Exception.Response
            if ($response) {
                $status = [int]$response.StatusCode
                $response.Close() # or the next request may get a dead keep-alive connection
                if ($status -eq 404) { return $false }
            }
            if ($attempt -ge 3) { throw }
        } catch [System.IO.IOException] {
            if ($attempt -ge 3) { throw }
        }
        Start-Sleep -Seconds 2
    }
}

function Read-Utf8([string]$file) { [System.IO.File]::ReadAllText($file, (New-Object System.Text.UTF8Encoding($false))) }

# The tag GitHub's "latest release" points to (/releases/latest redirects to /releases/tag/<tag>), or
# $null if the server doesn't say.
function Get-LatestTag([string]$base) {
    try {
        $request = [System.Net.HttpWebRequest]::Create("$base/releases/latest")
        $request.AllowAutoRedirect = $false
        $request.Timeout = 30000
        $request.Method = "HEAD"
        $request.UserAgent = "tsmedia-build"
        $response = $request.GetResponse()
        try { $location = $response.Headers["Location"] } finally { $response.Close() }
    } catch {
        if ($_.Exception.InnerException -and $_.Exception.InnerException.Response) { $_.Exception.InnerException.Response.Close() }
        return $null
    }
    if ($location -match '/releases/tag/([^/?#]+)$') { return [uri]::UnescapeDataString($Matches[1]) }
    return $null
}

# The architectures a manifest has a plugin DLL for ("win64", "win32").
function Get-PluginArchs($payload) {
    return @(@("win64", "win32") | Where-Object { $payload.files.PSObject.Properties.Name -contains "plugins/tsmedia_$_.dll" })
}

# ---- one-off modes ----------------------------------------------------------------------------------

if ($VerifyPublished) {
    $base = if ($VerifyFrom) { $VerifyFrom.TrimEnd('/') + "/$repoSlug" } else { "https://github.com/$repoSlug" }
    $keys = if ($VerifyFrom) { Get-TrustedKeys -WithTestKey } else { Get-TrustedKeys }
    $temp = Join-Path ([System.IO.Path]::GetTempPath()) ("tsmedia-verify-" + [guid]::NewGuid().ToString("N"))
    New-Item -ItemType Directory -Force $temp | Out-Null
    $failed = 0
    $found = @{}
    try {
        foreach ($name in @($v2Name, $legacyName)) {
            $url = "$base/releases/latest/download/$name"
            Write-Host "Fetching $url"
            $file = Join-Path $temp $name
            if (-not (Save-Url $url $file)) { Write-Host "  not in the latest release"; continue }
            try {
                $m = Read-Manifest (Read-Utf8 $file) $keys
            } catch {
                Write-Host "  FAILED: $($_.Exception.Message)"; $failed++; continue
            }
            $found[$name] = $m
            $checked = 0
            foreach ($p in $m.Payload.files.PSObject.Properties) {
                $asset = Get-AssetName $p.Name $m.Payload.version
                if (-not $asset) { continue } # unknown to this version of the plugin too
                $assetFile = Join-Path $temp ("$($m.Tag)-$asset")
                if (-not (Save-Url "$base/releases/download/$($m.Tag)/$asset" $assetFile)) { Write-Host "  FAILED: $asset is missing in $($m.Tag)"; $failed++; continue }
                $size = (Get-Item -LiteralPath $assetFile).Length
                $sha = Get-Sha256 $assetFile
                if ($size -ne [int64]$p.Value.size -or $sha -ne $p.Value.sha256) { Write-Host "  FAILED: $asset doesn't match the manifest (size $size, SHA-256 $sha)"; $failed++; continue }
                $checked++
            }
            if ($m.Payload.package) {
                $pkgFile = Join-Path $temp ("$($m.Tag)-$($m.Payload.package.name)")
                if (-not (Save-Url "$base/releases/download/$($m.Tag)/$($m.Payload.package.name)" $pkgFile)) { Write-Host "  FAILED: $($m.Payload.package.name) is missing"; $failed++ }
                elseif ((Get-Item -LiteralPath $pkgFile).Length -ne [int64]$m.Payload.package.size -or (Get-Sha256 $pkgFile) -ne $m.Payload.package.sha256) { Write-Host "  FAILED: $($m.Payload.package.name) doesn't match the manifest"; $failed++ }
                else { $checked++ }
            }
            $how = if ($m.Signed) { "signed with key $($m.KeyId)" } else { "unsigned" }
            Write-Host "  version $($m.Version), $how, $checked files match their size and SHA-256"
        }
        $v2 = $found[$v2Name]; $legacy = $found[$legacyName]
        if (-not $legacy) {
            Write-Host "FAILED: no valid signed ${legacyName}: users of 2.2.0 get no update."; $failed++
        } elseif (-not $legacy.Signed) {
            Write-Host "FAILED: ${legacyName} is unsigned: 2.2.0 can't read it."; $failed++
        } else {
            Write-Host "2.2.0 users are offered $($legacy.Version)."
            # 2.2.0 users of an architecture the signed file leaves out are never offered anything.
            foreach ($arch in @("win64", "win32") | Where-Object { (Get-PluginArchs $legacy.Payload) -notcontains $_ }) {
                Write-Host "FAILED: $legacyName has no $arch DLL: users of 2.2.0 on $arch TeamSpeak get no update."; $failed++
            }
        }
        if ($v2) {
            Write-Host "2.2.1 and later are offered $($v2.Version)."
            if ($legacy -and $legacy.Version -gt $v2.Version) { Write-Host "FAILED: $legacyName offers a newer version than $v2Name."; $failed++ }
            # Since 2.2.1 tsmedia-update.json is the signed bridge (2.2.1 or later); an older one strands 2.2.0 users.
            if ($legacy -and $legacy.Signed -and $legacy.Version -lt $bridgeVersion) {
                Write-Host "FAILED: $legacyName is for $($legacy.Version): it must be the signed file of $bridgeVersion or later, or users of 2.2.0 are never updated."; $failed++
            }
            foreach ($arch in @("win64", "win32") | Where-Object { (Get-PluginArchs $v2.Payload) -notcontains $_ }) { Write-Host "  note: $v2Name has no $arch DLL: $arch users of 2.2.1 and later are not offered $($v2.Version)." }
        } elseif ($legacy -and $legacy.Version -ge $bridgeVersion) {
            Write-Host "FAILED: $v2Name is missing (every release since $bridgeVersion has it)."; $failed++
        } elseif ($legacy) {
            Write-Host "2.2.1 and later read $legacyName (a release made before $bridgeVersion)."
        }
        # The manifest a release carries must describe that release, not an older one uploaded by mistake.
        $latestTag = Get-LatestTag $base
        $own = if ($v2) { $v2 } else { $legacy }
        if (-not $latestTag) {
            Write-Host "  note: couldn't read which release is the latest; its tag was not compared."
        } elseif ($own -and $own.Tag -ne $latestTag) {
            $ownName = if ($v2) { $v2Name } else { $legacyName }
            Write-Host "FAILED: the latest release is $latestTag, but its $ownName is for $($own.Tag)."; $failed++
        } elseif ($own) {
            Write-Host "The latest release is $latestTag."
        }
    } finally {
        Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
    }
    if ($failed) { Write-Host "Published release check FAILED ($failed problems)."; exit 1 }
    Write-Host "Published release OK."
    exit 0
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

# ---- what this build is ------------------------------------------------------------------------------

$version = (Select-String -Path (Join-Path $root "CMakeLists.txt") -Pattern 'project\(tsmedia VERSION ([0-9.]+)').Matches[0].Groups[1].Value
# The same text as TeamSpeak's Addons list shows (CMakeLists.txt is the only place to change it).
$description = (Select-String -Path (Join-Path $root "CMakeLists.txt") -Pattern 'set\(PLUGIN_DESCRIPTION\s+"([^"]+)"\)').Matches[0].Groups[1].Value
if (-not $description) { throw "PLUGIN_DESCRIPTION not found in CMakeLists.txt" }

if ($RevokeKeys.Count -gt 0) { $Sign = $true } # only a signed manifest can revoke keys
if ($Sign) { $Release = $true }
$notesFile = Join-Path $root "docs\release-notes\v$version.md"
$signNow = $false
$bridgeBytes = $null
if ($Release) {
    # Checked before the long build: the notes, and how tsmedia-update.json comes about.
    if (-not (Test-Path $notesFile)) { throw "Missing ${notesFile}: the update dialog shows its Highlights." }
    # A draft must never become the update dialog's "What's new" (the DRAFT comment is deleted when the
    # notes are final).
    if ((Get-Content -Raw -Encoding UTF8 $notesFile) -match '<!--\s*DRAFT') { throw "$notesFile is still a DRAFT: finish the release notes (and delete the DRAFT comment) before a release build." }
    # The release notes link to CHANGELOG.md at the tag: it must have the release date by then.
    $changelogHeading = '(?m)^## \[' + [regex]::Escape($version) + '\] - \d{4}-\d{2}-\d{2}\s*$'
    if ((Get-Content -Raw -Encoding UTF8 (Join-Path $root "CHANGELOG.md")) -notmatch $changelogHeading) { throw "CHANGELOG.md has no '## [$version] - <YYYY-MM-DD>' heading (still 'unreleased'?): put in the release date before a release build." }
    $signNow = $Sign -or ([version]$version -le $bridgeVersion)
    # A signed tsmedia-update.json is what 2.2.0 users get (for 2.2.1, for good): it needs both architectures,
    # or 2.2.0 users on 32-bit TeamSpeak are never updated.
    if ($signNow -and $No32) { throw "-No32: $version gets a signed tsmedia-update.json for users of 2.2.0, so it must contain the 32-bit files too. Build both architectures." }
    $signTool = Join-Path $SigningDir "tsmedia-keys.ps1"
    if ($signNow) {
        $haveKey = (Test-Path $signTool) -and ((Test-Path (Join-Path $SigningDir "release-key-$KeyId.dpapi")) -or (Test-Path (Join-Path $SigningDir "release-key-$KeyId.pkey")))
        if (-not $haveKey) {
            if ($Sign) { throw "-Sign: release key $KeyId not found in $SigningDir." }
            throw "$version is the bridge release: users of 2.2.0 only accept a tsmedia-update.json signed with key $KeyId, which is not in $SigningDir. Build this release on the PC that has the key."
        }
        Write-Host "Release $version`: tsmedia-update.json will be signed with key $KeyId (users of 2.2.0 update to it directly)."
    } else {
        # The published, signed manifest of the bridge release, again in this release: 2.2.0 users
        # update to it (its files stay in its own GitHub release), and from there to this version.
        $keys = Get-TrustedKeys
        if ($BridgeManifest) {
            $bridgeBytes = [System.IO.File]::ReadAllBytes((Resolve-Path -LiteralPath $BridgeManifest).Path)
            $bridgeFrom = $BridgeManifest
        } else {
            $bridgeFrom = "https://github.com/$repoSlug/releases/download/v$bridgeVersion/$legacyName"
            $tmp = [System.IO.Path]::GetTempFileName()
            try {
                Write-Host "Fetching the signed bridge manifest: $bridgeFrom"
                try {
                    $got = Save-Url $bridgeFrom $tmp
                } catch {
                    throw "Couldn't download the bridge manifest ($($_.Exception.Message)). Check the internet connection and run the build again, or pass -BridgeManifest <file>."
                }
                if (-not $got) { throw "Release v$bridgeVersion has no $legacyName on GitHub. Publish $bridgeVersion first (signed), or pass -BridgeManifest <file>." }
                $bridgeBytes = [System.IO.File]::ReadAllBytes($tmp)
            } finally {
                Remove-Item -LiteralPath $tmp -ErrorAction SilentlyContinue
            }
        }
        $bridge = Read-Manifest ([System.Text.Encoding]::UTF8.GetString($bridgeBytes)) $keys
        if (-not $bridge.Signed) { throw "The bridge manifest ($bridgeFrom) is not signed: 2.2.0 couldn't read it." }
        if ($bridge.Version -lt $bridgeVersion -or $bridge.Version -ge [version]$version) { throw "The bridge manifest ($bridgeFrom) is for $($bridge.Version); it must be $bridgeVersion or later and older than $version." }
        foreach ($arch in @("win64", "win32") | Where-Object { (Get-PluginArchs $bridge.Payload) -notcontains $_ }) { Write-Warning "The bridge manifest has no $arch DLL: users of 2.2.0 on $arch TeamSpeak are not updated (build.ps1 -Sign signs this version for them)." }
        Write-Host "Release $version`: tsmedia-update.json is the signed manifest of $($bridge.Version) (key $($bridge.KeyId)): users of 2.2.0 update to $($bridge.Version) first, then to $version. No key needed."
    }
} else {
    Write-Host "Building without the update check (a copy you build yourself never replaces itself). For a release: build.ps1 -Release"
}
$updater = if ($Release) { "ON" } else { "OFF" }

# ---- build --------------------------------------------------------------------------------------------

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
    & $cmake -S $root -B $buildDir -G "Visual Studio 17 2022" -A $platform "-DCMAKE_PREFIX_PATH=$qtDir" -DTSMEDIA_TESTHOOKS=OFF "-DTSMEDIA_VERSION_OVERRIDE=" "-DTSMEDIA_UPDATER=$updater" @extra | Out-Host
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

# ---- the release folder: update files and both manifests ---------------------------------------------

if ($Release) {
    # Up to 5 "- " bullets under "### Highlights" in docs\release-notes\v<version>.md, markdown removed.
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

    # Exactly the files of the GitHub release, nothing else.
    $upload = Join-Path $dist "upload-$version"
    if (Test-Path $upload) { Remove-Item -LiteralPath $upload -Recurse -Force }
    New-Item -ItemType Directory -Force $upload | Out-Null
    Copy-Item -LiteralPath $plugin -Destination $upload
    $files = [ordered]@{}
    foreach ($arch in @("win64", "win32")) {
        if (-not $dlls.ContainsKey($arch)) { continue }
        $machine = if ($arch -eq "win64") { "x64" } else { "x86" }
        $dllAsset = Join-Path $upload "TSMedia-$version-$arch.update"
        Copy-Item -LiteralPath $dlls[$arch] -Destination $dllAsset -Force
        $files["plugins/tsmedia_$arch.dll"] = [ordered]@{ size = (Get-Item $dllAsset).Length; sha256 = (Get-Sha256 $dllAsset); machine = $machine }
        $helper = Join-Path $buildDirs[$arch] "$Config\tsmedia_update_helper.exe"
        if (-not (Test-Path $helper)) { throw "Missing $helper" }
        $helperAsset = Join-Path $upload "TSMedia-$version-helper-$arch.update"
        Copy-Item -LiteralPath $helper -Destination $helperAsset -Force
        $files["helper/tsmedia_update_helper_$arch.exe"] = [ordered]@{ size = (Get-Item $helperAsset).Length; sha256 = (Get-Sha256 $helperAsset); machine = $machine }
    }
    function New-Payload([int]$format) {
        $p = [ordered]@{
            format    = $format
            product   = "tsmedia"
            version   = $version
            tag       = "v$version"
            published = [DateTime]::UtcNow.ToString("yyyy-MM-dd", [Globalization.CultureInfo]::InvariantCulture) # Gregorian in every locale
            package   = [ordered]@{ name = "TSMedia-$version.ts3_plugin"; size = (Get-Item $plugin).Length; sha256 = (Get-Sha256 $plugin) }
            files     = $files
            notes     = @($notes)
        }
        if ($format -eq 1 -and $RevokeKeys.Count -gt 0) { $p["revokeKeys"] = @($RevokeKeys) }
        $bytes = (New-Object System.Text.UTF8Encoding($false)).GetBytes(($p | ConvertTo-Json -Compress -Depth 6))
        if ($bytes.Length -gt 48KB) { throw "The manifest is larger than 48 KiB." }
        return , $bytes
    }
    $keys = Get-TrustedKeys

    # tsmedia-update-v2.json: unsigned. 2.2.1 and later trust it because it comes from this repository's
    # latest release over HTTPS, and check every file against its size and SHA-256.
    $v2File = Join-Path $upload $v2Name
    [System.IO.File]::WriteAllBytes($v2File, (New-Payload 2))

    # tsmedia-update.json: signed, for 2.2.0.
    $legacyFile = Join-Path $upload $legacyName
    if ($signNow) {
        $payloadBytes = New-Payload 1
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
        [System.IO.File]::WriteAllText($legacyFile, $manifest, (New-Object System.Text.UTF8Encoding($false)))
    } else {
        [System.IO.File]::WriteAllBytes($legacyFile, $bridgeBytes)
    }

    # Self-check, as -VerifyPublished checks the published files: both manifests read back, and every
    # file this release lists matches what is in the folder.
    $v2 = Read-Manifest (Read-Utf8 $v2File) $keys
    $legacy = Read-Manifest (Read-Utf8 $legacyFile) $keys
    if ($v2.Signed -or $v2.Version -ne [version]$version) { throw "Self-check of $v2Name failed." }
    if (-not $legacy.Signed) { throw "Self-check of ${legacyName}: not signed." }
    $own = @($v2)
    if ($signNow) {
        if ($legacy.Version -ne [version]$version) { throw "Self-check of $legacyName read version $($legacy.Version)." }
        $own += $legacy
    }
    foreach ($m in $own) {
        foreach ($p in $m.Payload.files.PSObject.Properties) {
            $assetFile = Join-Path $upload (Get-AssetName $p.Name $version)
            if (-not (Test-Path -LiteralPath $assetFile) -or (Get-Item -LiteralPath $assetFile).Length -ne [int64]$p.Value.size -or (Get-Sha256 $assetFile) -ne $p.Value.sha256) { throw "Self-check: $assetFile doesn't match the manifest." }
        }
        if ((Get-Sha256 (Join-Path $upload $m.Payload.package.name)) -ne $m.Payload.package.sha256) { throw "Self-check: the package doesn't match the manifest." }
    }
    $legacyText = if ($signNow) { "signed with key $KeyId" } else { "the signed manifest of $($legacy.Version), unchanged" }
    Write-Host "Manifests: $v2Name (unsigned, $($notes.Count) highlights) and $legacyName ($legacyText); self-check passed."

    $uploadRel = "dist\upload-$version"
    $assetArgs = (Get-ChildItem -LiteralPath $upload | Sort-Object Name | ForEach-Object { "`"$uploadRel\$($_.Name)`"" }) -join " "
    Write-Host ""
    Write-Host "Release folder: $upload"
    Get-ChildItem -LiteralPath $upload | Sort-Object Name | ForEach-Object { Write-Host "  $($_.Name)" }
    Write-Host ""
    Write-Host "Publish (in the repository folder, after pushing main and the tag v$version; draft first, so 'latest' never lacks the manifests):"
    Write-Host "  gh release create v$version --verify-tag --draft --title `"TS Media chat $version`" --notes-file docs\release-notes\v$version.md $assetArgs"
    Write-Host "  gh release edit v$version --draft=false --latest      (a backport: --latest=false)"
    Write-Host "  powershell -ExecutionPolicy Bypass -File scripts\build.ps1 -VerifyPublished"
    Write-Host "Or on github.com: Releases > Draft a new release > tag v$version > drag ALL files of $uploadRel into the box, paste the release notes, Publish release."
}

if ($Install) {
    $target = Join-Path $env:APPDATA "TS3Client\plugins"
    New-Item -ItemType Directory -Force $target | Out-Null
    Copy-Item $dlls["win64"] $target -Force
    Write-Host "Installed: $target\tsmedia_win64.dll"
}
