[CmdletBinding()]
param(
    [ValidateSet('Debug','Release')][string]$Configuration='Debug',
    [Parameter(Mandatory)][string]$StageDirectory,
    [string]$NativeBuildDirectory,
    [switch]$RunTests,
    [switch]$SkipHardwareTests,
    [switch]$Sdk,
    [string]$CMakePath
)
$ErrorActionPreference='Stop'
function Get-NativeFileSha256([string]$Path) {
    # Use the runtime API so staging also works when Windows PowerShell inherits
    # PSModulePath from a PowerShell 7 host such as a CI runner.
    $algorithm=[Security.Cryptography.SHA256]::Create()
    $stream=$null
    try {
        $stream=[IO.File]::OpenRead($Path)
        return [BitConverter]::ToString($algorithm.ComputeHash($stream)).Replace('-','').ToLowerInvariant()
    } finally {
        if($stream){$stream.Dispose()}
        $algorithm.Dispose()
    }
}
$repository=(Resolve-Path -LiteralPath (Split-Path -Parent $PSScriptRoot)).Path
if (-not $NativeBuildDirectory) { $NativeBuildDirectory=Join-Path $repository ('.out/native/windows-x64/'+$Configuration) }
$NativeBuildDirectory=[IO.Path]::GetFullPath($NativeBuildDirectory)
$StageDirectory=[IO.Path]::GetFullPath($StageDirectory)
if ($NativeBuildDirectory -eq $repository -or $NativeBuildDirectory.StartsWith((Join-Path $repository 'native')+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)) { throw 'Native outputs must stay outside native source directories' }
$vswhere=Join-Path ([Environment]::GetFolderPath('ProgramFilesX86')) 'Microsoft Visual Studio/Installer/vswhere.exe'
$visualStudio=(& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath)
if (-not $visualStudio) { throw 'Visual Studio 2026 C++ x64 workload is required' }
if (-not $CMakePath) { $CMakePath=Join-Path $visualStudio 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe' }
if (-not (Test-Path -LiteralPath $CMakePath)) { throw "CMake executable missing: $CMakePath" }
$gitExecutable=Join-Path $env:ProgramFiles 'Git/cmd/git.exe'
if(-not(Test-Path -LiteralPath $gitExecutable)) { $gitExecutable=(Get-Command git.exe -ErrorAction Stop).Source }
$sha=[Security.Cryptography.SHA256]::Create()
try { $mutexId=[BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($NativeBuildDirectory.ToLowerInvariant()))).Replace('-','') }
finally { $sha.Dispose() }
$mutex=[Threading.Mutex]::new($false,('Local\NessStudio.Native.'+$mutexId))
$acquired=$false
$start=[DateTimeOffset]::UtcNow
$attempt=$start.ToString('yyyyMMddTHHmmssfff')+'-'+$PID
$logDirectory=Join-Path $NativeBuildDirectory ('logs/'+$attempt)
function Run-Native([string]$Executable,[string[]]$Arguments,[string]$Name) {
    [IO.Directory]::CreateDirectory($logDirectory) | Out-Null
    $quoted=@(foreach ($argument in $Arguments) {
        if ($argument -ne '' -and $argument -notmatch '[\s"]') { $argument }
        else { '"'+[regex]::Replace([regex]::Replace($argument,'(\\*)"','$1$1\"'),'(\\+)$','$1$1')+'"' }
    })
    $info=[Diagnostics.ProcessStartInfo]::new();$info.FileName=$Executable;$info.Arguments=$quoted -join ' '
    $info.WorkingDirectory=$repository;$info.UseShellExecute=$false;$info.CreateNoWindow=$true
    $info.RedirectStandardOutput=$true;$info.RedirectStandardError=$true
    $process=[Diagnostics.Process]::new();$process.StartInfo=$info
    try {
        $process.Start() | Out-Null
        $stdout=$process.StandardOutput.ReadToEndAsync();$stderr=$process.StandardError.ReadToEndAsync()
        $process.WaitForExit()
        $text=$stdout.Result+$stderr.Result
        [IO.File]::WriteAllText((Join-Path $logDirectory ($Name+'.log')),$text,[Text.UTF8Encoding]::new($false))
        if ($process.ExitCode -ne 0) {
            [Console]::Error.WriteLine($text)
            $failure=[Exception]::new("Native $Name failed with exit $($process.ExitCode). Log: $logDirectory")
            $failure.Data['NativeExitCode']=$process.ExitCode;throw $failure
        }
    } finally { $process.Dispose() }
}
try {
    try { $acquired=$mutex.WaitOne([TimeSpan]::FromMinutes(10)) } catch [Threading.AbandonedMutexException] { $acquired=$true }
    if (-not $acquired) { throw 'Timed out waiting for the native configuration build lock' }
    $lockAcquired=[DateTimeOffset]::UtcNow
    Run-Native $CMakePath @('--preset','windows-x64','-S',$repository,'-B',$NativeBuildDirectory,('-DGIT_EXECUTABLE='+$gitExecutable)) 'configure'
    Run-Native $CMakePath @('--build',$NativeBuildDirectory,'--config',$Configuration,'--parallel','4') 'build'
    $binaryDirectory=Join-Path $NativeBuildDirectory ('bin/'+$Configuration)
    $manifest=Get-Content -LiteralPath (Join-Path $binaryDirectory 'native-build.json') -Raw | ConvertFrom-Json
    $runtimePattern=if($Configuration -eq 'Debug'){'Microsoft.VC*.DebugCRT'}else{'Microsoft.VC*.CRT'}
    $runtimeBase=if($Configuration -eq 'Debug'){'debug_nonredist/x64'}else{'x64'}
    $redist=$null
    $versions=@(Get-ChildItem -LiteralPath (Join-Path $visualStudio 'VC/Redist/MSVC') -Directory |
        Where-Object { $_.Name -match '^\d+(\.\d+){1,3}$' } |
        Sort-Object { [version]$_.Name } -Descending)
    $toolsetRuntime=Join-Path $visualStudio 'VC/Redist/MSVC/v145'
    if(Test-Path -LiteralPath $toolsetRuntime){$versions+=Get-Item -LiteralPath $toolsetRuntime}
    foreach($version in $versions){
        $candidate=Join-Path $version.FullName $runtimeBase
        if(Test-Path -LiteralPath $candidate){
            $runtime=Get-ChildItem -LiteralPath $candidate -Directory -Filter $runtimePattern | Select-Object -First 1
            if($runtime){$redist=$runtime.FullName;break}
        }
    }
    if (-not $redist) { throw 'The selected Visual Studio installation has no matching x64 CRT redistributable' }
    Copy-Item -LiteralPath (Join-Path $repository 'native/deps/NessMuxer/LICENSE') -Destination (Join-Path $binaryDirectory 'NessMuxer.LICENSE.txt')
    Copy-Item -LiteralPath (Join-Path $repository 'native/deps/json/LICENSE.MIT') -Destination (Join-Path $binaryDirectory 'json.LICENSE.txt')
    Copy-Item -LiteralPath (Join-Path $repository 'native/deps/dependencies.lock.json') -Destination (Join-Path $binaryDirectory 'native-dependencies.json')
    $encoderFiles=@(
        @('native/deps/x264-abi164/libx264-164.dll','libx264-164.dll'),
        @('native/deps/x264-abi164/licenses/COPYING','x264.LICENSE.txt'),
        @('native/deps/x264-abi164/licenses/AUTHORS','x264.AUTHORS.txt'),
        @('native/deps/x264-abi164/x264-eaa68fa.source.tar.gz','x264.source.tar.gz'),
        @('native/deps/NessMuxer/native-backends.lock.json','encoder-dependencies.json'),
        @('native/deps/nv-codec-headers/LICENSE.txt','NVENC-headers.LICENSE.txt'),
        @('native/deps/NessMuxer/third_party/amf/LICENSE.txt','AMF-headers.LICENSE.txt')
    )
    foreach($encoderFile in $encoderFiles) {
        Copy-Item -LiteralPath (Join-Path $repository $encoderFile[0]) -Destination (Join-Path $binaryDirectory $encoderFile[1])
    }
    # Report the API headers actually used by this engine, independently of the
    # older pinned NessMuxer source bundle's original dependency manifest.
    $encoderLock=Get-Content -LiteralPath (Join-Path $binaryDirectory 'encoder-dependencies.json') -Raw | ConvertFrom-Json
    $engineLock=Get-Content -LiteralPath (Join-Path $repository 'native/deps/dependencies.lock.json') -Raw | ConvertFrom-Json
    $encoderLock.nvenc=$engineLock.nvenc
    [IO.File]::WriteAllText((Join-Path $binaryDirectory 'encoder-dependencies.json'),($encoderLock|ConvertTo-Json -Depth 12),[Text.UTF8Encoding]::new($false))
    $vplLicense=Join-Path $NativeBuildDirectory 'native/vendor/libvpl-2.17.0/LICENSE'
    Copy-Item -LiteralPath $vplLicense -Destination (Join-Path $binaryDirectory 'libvpl.LICENSE.txt')
    $inputs=@(Get-Item -LiteralPath (Join-Path $binaryDirectory 'libnessstudio.dll'),(Join-Path $binaryDirectory 'nessstudio-cli.exe'),(Join-Path $binaryDirectory 'native-build.json'),(Join-Path $binaryDirectory 'NessMuxer.LICENSE.txt'),(Join-Path $binaryDirectory 'json.LICENSE.txt'),(Join-Path $binaryDirectory 'native-dependencies.json'))
    $inputs+=@(Get-ChildItem -LiteralPath $redist -Filter '*.dll' -File)
    $inputs+=@(foreach($encoderFile in $encoderFiles){Get-Item -LiteralPath (Join-Path $binaryDirectory $encoderFile[1])})
    $inputs+=Get-Item -LiteralPath (Join-Path $binaryDirectory 'libvpl.LICENSE.txt')
    foreach ($symbol in @('libnessstudio.pdb','nessstudio-cli.pdb')) { $file=Join-Path $binaryDirectory $symbol; if(Test-Path -LiteralPath $file){$inputs+=Get-Item -LiteralPath $file} }
    [IO.Directory]::CreateDirectory($StageDirectory) | Out-Null
    $files=@(foreach($file in $inputs){
        $destination=Join-Path $StageDirectory $file.Name
        $hash=Get-NativeFileSha256 $file.FullName
        if(-not(Test-Path -LiteralPath $destination) -or (Get-NativeFileSha256 $destination) -ne $hash){Copy-Item -LiteralPath $file.FullName -Destination $destination}
        # CTest/CLI also work without developer PATH or globally installed VC CRT.
        $nativeRuntime=Join-Path $binaryDirectory $file.Name
        if($file.DirectoryName -eq $redist -and (-not(Test-Path -LiteralPath $nativeRuntime) -or (Get-NativeFileSha256 $nativeRuntime) -ne $hash)){Copy-Item -LiteralPath $file.FullName -Destination $nativeRuntime}
        @{name=$file.Name;sha256=$hash;length=$file.Length}
    })
    if($RunTests){
        $fixtureDirectory=Join-Path $binaryDirectory 'session-test'
        if(Test-Path -LiteralPath $fixtureDirectory){foreach($runtime in Get-ChildItem -LiteralPath $redist -Filter '*.dll' -File){Copy-Item -LiteralPath $runtime.FullName -Destination $fixtureDirectory}}
        $testArguments=@('--test-dir',$NativeBuildDirectory,'-C',$Configuration,'--output-on-failure','--no-tests=error','--test-output-size-passed','65536','--output-junit',(Join-Path $logDirectory 'ctest-results.xml'))
        if($SkipHardwareTests){$testArguments+=@('--label-exclude','hardware')}
        Run-Native (Join-Path (Split-Path -Parent $CMakePath) 'ctest.exe') $testArguments 'ctest'
    }
    if($Sdk){
        [IO.Directory]::CreateDirectory((Join-Path $StageDirectory 'include/nessstudio')) | Out-Null
        [IO.Directory]::CreateDirectory((Join-Path $StageDirectory 'lib')) | Out-Null
        Copy-Item -LiteralPath (Join-Path $repository 'native/libnessstudio/include/nessstudio/nessstudio.h') -Destination (Join-Path $StageDirectory 'include/nessstudio/nessstudio.h')
        Copy-Item -LiteralPath (Join-Path $NativeBuildDirectory ('lib/'+$Configuration+'/libnessstudio.lib')) -Destination (Join-Path $StageDirectory 'lib/libnessstudio.lib')
        Copy-Item -LiteralPath (Join-Path $repository 'native/libnessstudio/README.md') -Destination (Join-Path $StageDirectory 'README.txt')
        Copy-Item -LiteralPath (Join-Path $repository 'LICENSE') -Destination (Join-Path $StageDirectory 'NessStudio.LICENSE.txt')
    }
    $entry=@{status='PASS';configuration=$Configuration;build=$manifest.build;sourceFingerprint=$manifest.sourceFingerprint;native_directory=$NativeBuildDirectory;stage_directory=$StageDirectory;started_at=$start.ToString('o');lock_acquired_at=$lockAcquired.ToString('o');finished_at=[DateTimeOffset]::UtcNow.ToString('o');log_directory=$logDirectory;files=$files}
    $runtimeJson=$entry | ConvertTo-Json -Depth 8
    [IO.File]::WriteAllText((Join-Path $StageDirectory 'native-runtime.json'),$runtimeJson,[Text.UTF8Encoding]::new($false))
    [IO.File]::AppendAllText((Join-Path $NativeBuildDirectory 'invocations.jsonl'),(($entry|ConvertTo-Json -Depth 8 -Compress)+[Environment]::NewLine),[Text.UTF8Encoding]::new($false))
    Write-Host ('Native '+$Configuration+': '+$manifest.build+' -> '+$StageDirectory)
} catch {
    [Console]::Error.WriteLine($_.Exception.Message)
    $exitCode=if($_.Exception.Data.Contains('NativeExitCode')){[int]$_.Exception.Data['NativeExitCode']}else{1}
    exit $exitCode
} finally { if($acquired){$mutex.ReleaseMutex()};$mutex.Dispose() }
