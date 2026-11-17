[CmdletBinding()]
param(
    [string]$Destination,
    [string]$ArchivePath,
    [switch]$ReplaceExisting
)
$ErrorActionPreference = 'Stop'
$repository = (Resolve-Path -LiteralPath (Split-Path -Parent $PSScriptRoot)).Path
$mediaLock = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'ffmpeg.lock.json') -Raw | ConvertFrom-Json
if (-not $Destination) { $Destination = Join-Path $repository 'src/NessStudio.App/Native/FFmpeg' }
$Destination = [IO.Path]::GetFullPath($Destination)
if ($mediaLock.version -ne 1 -or $mediaLock.architecture -ne 'windows-x64') { throw 'Unsupported media tool lock' }
$complete = $true
foreach ($item in $mediaLock.files) {
    if ([IO.Path]::GetFileName($item.name) -ne $item.name) { throw 'Media lock output must be a filename' }
    $target = Join-Path $Destination $item.name
    if (Test-Path -LiteralPath $target) {
        if ((Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash -ne $item.sha256) {
            if (-not $ReplaceExisting) { throw "Media tool differs from its pin: $target. Preserve or inspect it before an explicit -ReplaceExisting restore." }
            $complete = $false
        }
    } else { $complete = $false }
}
if ($complete) { Write-Host ('Pinned FFmpeg ' + $mediaLock.release + ' already restored'); exit 0 }
if (-not $ArchivePath) {
    $cache = Join-Path $repository '.out/media-cache'
    [IO.Directory]::CreateDirectory($cache) | Out-Null
    $ArchivePath = Join-Path $cache ('ffmpeg-' + $mediaLock.archiveSha256 + '.zip')
    if (-not (Test-Path -LiteralPath $ArchivePath)) {
        $download = $ArchivePath + '.' + [Guid]::NewGuid().ToString('N') + '.pending'
        Invoke-WebRequest -UseBasicParsing -Uri $mediaLock.archiveUrl -OutFile $download
        if ((Get-FileHash -LiteralPath $download -Algorithm SHA256).Hash -ne $mediaLock.archiveSha256) { throw 'Downloaded FFmpeg archive hash mismatch; untrusted file retained for inspection' }
        [IO.File]::Move($download, $ArchivePath)
    }
}
$ArchivePath = (Resolve-Path -LiteralPath $ArchivePath).Path
if ((Get-FileHash -LiteralPath $ArchivePath -Algorithm SHA256).Hash -ne $mediaLock.archiveSha256) { throw 'FFmpeg archive hash mismatch' }
[IO.Directory]::CreateDirectory($Destination) | Out-Null
$temporary = Join-Path $Destination ('.restore-' + [Guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($temporary) | Out-Null
Add-Type -AssemblyName System.IO.Compression.FileSystem
$archive = [IO.Compression.ZipFile]::OpenRead($ArchivePath)
try {
    foreach ($item in $mediaLock.files) {
        $entry = $archive.GetEntry($item.entry)
        if ($null -eq $entry) { throw ('Pinned archive entry missing: ' + $item.entry) }
        $path = Join-Path $temporary $item.name
        $inputStream = $entry.Open()
        try {
            $outputStream = [IO.File]::Open($path, [IO.FileMode]::CreateNew)
            try { $inputStream.CopyTo($outputStream) } finally { $outputStream.Dispose() }
        } finally { $inputStream.Dispose() }
        if ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $item.sha256) { throw ('Extracted FFmpeg file hash mismatch: ' + $item.name) }
    }
} finally { $archive.Dispose() }
# Publish only the three explicit, independently verified entries.
foreach ($item in $mediaLock.files) {
    $source = Join-Path $temporary $item.name
    $target = Join-Path $Destination $item.name
    if (Test-Path -LiteralPath $target) { [IO.File]::Replace($source, $target, [NullString]::Value) }
    else { [IO.File]::Move($source, $target) }
}
[IO.Directory]::Delete($temporary, $false)
Write-Host ('Restored pinned FFmpeg ' + $mediaLock.release + ' to ' + $Destination)
