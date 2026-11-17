[CmdletBinding()]
param([Parameter(Mandatory)][string]$PublishedDirectory)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path -LiteralPath $PublishedDirectory).Path
$notices = Join-Path $root 'Licenses/ThirdParty'
$manifest = Get-Content -LiteralPath (Join-Path $notices 'manifest.json') -Raw | ConvertFrom-Json
$dependencies = Get-Content -LiteralPath (Join-Path $root 'NessStudio.deps.json') -Raw | ConvertFrom-Json
if ($manifest.schemaVersion -ne 1) { throw 'Unknown third-party notice schema' }
$recorded = @{}
foreach ($entry in $manifest.files) {
    if ([IO.Path]::GetFileName($entry.file) -ne $entry.file) { throw 'Notice path must be a direct child' }
    $path = Join-Path $notices $entry.file
    if ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() -ne $entry.sha256) {
        throw ('Third-party notice differs from its recorded source: ' + $entry.file)
    }
    $recorded[$entry.file] = $true
}
$packages = @{}
foreach ($package in $manifest.packages) {
    $identity = $package.id + '/' + $package.version
    if ($packages.ContainsKey($identity)) { throw ('Duplicate notice identity: ' + $identity) }
    $packages[$identity] = $true
    foreach ($file in $package.licenses) {
        if (-not $recorded.ContainsKey($file)) { throw ('Unverified license reference: ' + $file) }
    }
}
foreach ($dependency in $dependencies.libraries.PSObject.Properties) {
    if ($dependency.Value.type -eq 'project') { continue }
    if (-not $packages.ContainsKey($dependency.Name)) {
        throw ('Published dependency has no matching version in the notice inventory: ' + $dependency.Name)
    }
    $packages.Remove($dependency.Name)
}
if ($packages.Count -ne 0) { throw ('Notice inventory differs from the published dependency graph: ' + ($packages.Keys -join ', ')) }
foreach ($excluded in $manifest.excludedNativePackages) {
    $identity = $excluded.id + '/' + $excluded.version
    if ($dependencies.libraries.PSObject.Properties[$identity]) { throw ('Excluded obsolete native package reappeared in the publish: ' + $identity) }
    foreach ($file in $excluded.licenses) {
        if (-not $recorded.ContainsKey($file)) { throw ('Unverified excluded-package notice reference: ' + $file) }
    }
}
$runtime = Get-Content -LiteralPath (Join-Path $root 'sqlcipher-runtime.json') -Raw | ConvertFrom-Json
$nativeComponents = @{}
if (-not $manifest.nativeComponents) { throw 'The pinned database native component inventory is missing' }
foreach ($component in $manifest.nativeComponents) {
    if ($nativeComponents.ContainsKey($component.id)) { throw ('Duplicate native component: ' + $component.id) }
    $nativeComponents[$component.id] = $true
    if ([IO.Path]::GetFileName($component.artifact) -ne $component.artifact) { throw 'Database runtime artifact must be a direct child' }
    foreach ($file in $component.licenses) {
        if (-not $recorded.ContainsKey($file)) { throw ('Unverified native license reference: ' + $file) }
    }
    $path = Join-Path $root $component.artifact
    $actualHash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actualHash -ne $component.artifactSha256) { throw ('Native component differs from notice inventory: ' + $component.id) }
    $runtimeFile = @($runtime.files | Where-Object { $_.path -eq $component.artifact })
    if ($runtimeFile.Count -ne 1 -or $runtimeFile[0].sha256 -ne $actualHash) { throw 'Database runtime provenance differs from published bytes' }
    $version = $runtime.sources.PSObject.Properties[$component.runtimeVersionField]
    if (-not $version -or $version.Value -ne $component.version) { throw ('Database component version differs from build provenance: ' + $component.id) }
}
[pscustomobject]@{status='PASS';noticeFiles=$manifest.files.Count;dependencyVersions=$manifest.packages.Count;nativeComponents=$nativeComponents.Count;releaseApproved=$false}
