[CmdletBinding()]
param(
    [string] $RunDirectory = '',

    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[0-9A-Fa-f]{40}$')]
    [string] $SourceCommit,

    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[0-9A-Fa-f]{64}$')]
    [string] $BootkernelSha256,

    [Parameter(Mandatory = $true)]
    [long] $BootkernelBytes,

    # Retired-instruction cap. The default is the full SpringBoard-frontier
    # replay. A smaller cap is legitimate for a focused diagnostic -- for
    # example CommCenter's startup begins below 1e9 -- and costs
    # proportionally less wall clock. The profile window follows the cap so a
    # short run still profiles its own tail rather than a range it never
    # reaches.
    #
    # The upper bound is deliberately far above 2.1e9. Guest time, not host
    # time, is what bounds the stock software's own retry loops: the timebase
    # runs at 6 MHz against a 412 MHz CPU model, so one guest second costs
    # roughly 412 million retired instructions. CommCenter retries with
    # sleep(1) up to ten times before giving up, which is about 4.1e9
    # instructions of budget on its own -- more than the entire historical cap.
    # A run that stops at 2.1e9 has not observed a timeout; it has merely
    # stopped in the middle of one.
    [ValidateRange(1000000, 90000000000)]
    [long] $InstructionCap = 2100000000,

    # Take a checkpoint at this retired-instruction count. Zero means none.
    # bootkernel writes <file>, <file>.mdimage and <file>.mdstate; a later run
    # restores all three, which turns a 25-30 minute replay to the SpringBoard
    # frontier into seconds plus the delta. The checkpoint lands inside the run
    # directory so it inherits the same freshness and containment rules as every
    # other output.
    [ValidateRange(0, 90000000000)]
    [long] $SnapshotAt = 0,

    # Start from a checkpoint written by an earlier -SnapshotAt run instead of
    # from the kernel entry point. The three sidecars (<file>, .mdimage,
    # .mdstate) are read-only inputs and are hashed into the manifest exactly
    # like the kernel and the immutable rootfs, so a restored run states which
    # machine state it inherited. The trigger point of --snapshot-at is the
    # machine's own retired-instruction counter, which is part of the snapshot,
    # so -InstructionCap stays absolute across a restore: restoring a 2.4e9
    # checkpoint and asking for 12e9 runs 9.6e9 further, not 12e9 further.
    [string] $RestoreFrom = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$runDirectoryEstablished = $false
$runDir = $null
$launcherLog = $null
$launcherError = $null
$manifest = $null
$exitPath = $null
$endPath = $null
$launcherLogOwned = $false
$manifestOwned = $false

function ConvertTo-CanonicalPath {
    param(
        [Parameter(Mandatory = $true)]
        [string] $BasePath,

        [Parameter(Mandatory = $true)]
        [string] $Path
    )

    if ([IO.Path]::IsPathRooted($Path)) {
        return [IO.Path]::GetFullPath($Path)
    }
    return [IO.Path]::GetFullPath((Join-Path $BasePath $Path))
}

function Test-PathIsStrictlyBelow {
    param(
        [Parameter(Mandatory = $true)]
        [string] $ParentPath,

        [Parameter(Mandatory = $true)]
        [string] $CandidatePath
    )

    $separator = [IO.Path]::DirectorySeparatorChar
    $pathSeparators = [char[]]@(
        [IO.Path]::DirectorySeparatorChar,
        [IO.Path]::AltDirectorySeparatorChar
    )
    $parent = [IO.Path]::GetFullPath($ParentPath).TrimEnd(
        $pathSeparators
    ) + $separator
    $candidate = [IO.Path]::GetFullPath($CandidatePath)
    return $candidate.StartsWith(
        $parent, [StringComparison]::OrdinalIgnoreCase
    )
}

function Assert-PathIsStrictlyBelow {
    param(
        [Parameter(Mandatory = $true)]
        [string] $Label,

        [Parameter(Mandatory = $true)]
        [string] $ParentPath,

        [Parameter(Mandatory = $true)]
        [string] $CandidatePath
    )

    if (-not (Test-PathIsStrictlyBelow $ParentPath $CandidatePath)) {
        throw "$Label must remain strictly below $ParentPath ($CandidatePath)"
    }
}

function Assert-NotReparsePoint {
    param(
        [Parameter(Mandatory = $true)]
        [string] $Label,

        [Parameter(Mandatory = $true)]
        [string] $Path
    )

    if (-not (Test-Path -LiteralPath $Path)) {
        return
    }
    $item = Get-Item -LiteralPath $Path -Force
    if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw "$Label must not be a symlink, junction, or reparse point: $Path"
    }
}

function Assert-ContainedPathHasNoReparsePoint {
    param(
        [Parameter(Mandatory = $true)]
        [string] $RootPath,

        [Parameter(Mandatory = $true)]
        [string] $CandidatePath
    )

    $pathSeparators = [char[]]@(
        [IO.Path]::DirectorySeparatorChar,
        [IO.Path]::AltDirectorySeparatorChar
    )
    $root = [IO.Path]::GetFullPath($RootPath).TrimEnd(
        $pathSeparators
    )
    $candidate = [IO.Path]::GetFullPath($CandidatePath)
    Assert-PathIsStrictlyBelow 'contained path' $root $candidate
    Assert-NotReparsePoint 'containment root' $root

    $relative = $candidate.Substring($root.Length).TrimStart($pathSeparators)
    $current = $root
    foreach ($component in $relative.Split(
            $pathSeparators,
            [StringSplitOptions]::RemoveEmptyEntries)) {
        $current = Join-Path $current $component
        Assert-NotReparsePoint 'contained path component' $current
    }
}

function Write-NewUtf8File {
    param(
        [Parameter(Mandatory = $true)]
        [string] $Path,

        [Parameter(Mandatory = $true)]
        [AllowEmptyCollection()]
        [string[]] $Lines
    )

    $encoding = [Text.UTF8Encoding]::new($false)
    $stream = [IO.File]::Open(
        $Path,
        [IO.FileMode]::CreateNew,
        [IO.FileAccess]::Write,
        [IO.FileShare]::Read
    )
    try {
        $writer = [IO.StreamWriter]::new($stream, $encoding)
        try {
            foreach ($line in $Lines) {
                $writer.WriteLine($line)
            }
            $writer.Flush()
        }
        finally {
            $writer.Dispose()
        }
    }
    finally {
        $stream.Dispose()
    }
}

function Add-Utf8Lines {
    param(
        [Parameter(Mandatory = $true)]
        [string] $Path,

        [Parameter(Mandatory = $true)]
        [AllowEmptyCollection()]
        [string[]] $Lines
    )

    $encoding = [Text.UTF8Encoding]::new($false)
    $stream = [IO.File]::Open(
        $Path,
        [IO.FileMode]::Append,
        [IO.FileAccess]::Write,
        [IO.FileShare]::Read
    )
    try {
        $writer = [IO.StreamWriter]::new($stream, $encoding)
        try {
            foreach ($line in $Lines) {
                $writer.WriteLine($line)
            }
            $writer.Flush()
        }
        finally {
            $writer.Dispose()
        }
    }
    finally {
        $stream.Dispose()
    }
}

function Write-LauncherLog {
    param(
        [Parameter(Mandatory = $true)]
        [string] $Message
    )

    $line = '{0} {1}' -f (
        Get-Date
    ).ToUniversalTime().ToString('o'), $Message
    if (Test-Path -LiteralPath $launcherLog -PathType Leaf) {
        Add-Utf8Lines $launcherLog @($line)
    }
    else {
        Write-NewUtf8File $launcherLog @($line)
    }
}

function Get-FileEvidence {
    param(
        [Parameter(Mandatory = $true)]
        [string] $Label,

        [Parameter(Mandatory = $true)]
        [string] $Path
    )

    Assert-NotReparsePoint $Label $Path
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Missing $Label file: $Path"
    }
    $item = Get-Item -LiteralPath $Path -Force
    $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $Path).Hash
    return [PSCustomObject]@{
        Label = $Label
        Path = $item.FullName
        Bytes = [long]$item.Length
        Sha256 = $hash.ToUpperInvariant()
    }
}

function Assert-ExactInput {
    param(
        [Parameter(Mandatory = $true)]
        [string] $Label,

        [Parameter(Mandatory = $true)]
        [string] $Path,

        [Parameter(Mandatory = $true)]
        [long] $ExpectedBytes,

        [Parameter(Mandatory = $true)]
        [string] $ExpectedSha256
    )

    Assert-NotReparsePoint $Label $Path
    $evidence = Get-FileEvidence $Label $Path
    if ($evidence.Bytes -ne $ExpectedBytes) {
        throw "$Label size mismatch: expected $ExpectedBytes, got $($evidence.Bytes)"
    }
    if (-not $evidence.Sha256.Equals(
            $ExpectedSha256,
            [StringComparison]::OrdinalIgnoreCase)) {
        throw "$Label SHA-256 mismatch: expected $ExpectedSha256, got $($evidence.Sha256)"
    }
    return $evidence
}

function ConvertTo-WindowsCommandLineArgument {
    param(
        [Parameter(Mandatory = $true)]
        [AllowEmptyString()]
        [string] $Argument
    )

    if ($Argument.Length -gt 0 -and $Argument -notmatch '[\s"]') {
        return $Argument
    }

    $builder = [Text.StringBuilder]::new()
    [void]$builder.Append('"')
    $backslashes = 0
    foreach ($character in $Argument.ToCharArray()) {
        if ($character -eq [char]'\') {
            $backslashes++
            continue
        }
        if ($character -eq [char]'"') {
            for ($i = 0; $i -lt (2 * $backslashes + 1); $i++) {
                [void]$builder.Append('\')
            }
            [void]$builder.Append('"')
            $backslashes = 0
            continue
        }
        for ($i = 0; $i -lt $backslashes; $i++) {
            [void]$builder.Append('\')
        }
        $backslashes = 0
        [void]$builder.Append($character)
    }
    for ($i = 0; $i -lt (2 * $backslashes); $i++) {
        [void]$builder.Append('\')
    }
    [void]$builder.Append('"')
    return $builder.ToString()
}

function Write-FailureEvidence {
    param(
        [Parameter(Mandatory = $true)]
        [System.Management.Automation.ErrorRecord] $Failure
    )

    if (-not $runDirectoryEstablished -or
        [string]::IsNullOrWhiteSpace($runDir) -or
        -not (Test-Path -LiteralPath $runDir -PathType Container)) {
        return
    }

    $errorText = $Failure | Out-String
    $failureExit = if ([string]::IsNullOrWhiteSpace($exitPath)) {
        Join-Path $runDir 'run23.exit.txt'
    }
    else {
        $exitPath
    }
    $failureError = if ([string]::IsNullOrWhiteSpace($launcherError)) {
        Join-Path $runDir 'run23.launcher-error.txt'
    }
    else {
        $launcherError
    }
    $failureEnd = if ([string]::IsNullOrWhiteSpace($endPath)) {
        Join-Path $runDir 'run23.end-utc.txt'
    }
    else {
        $endPath
    }
    if ((Test-Path -LiteralPath $failureExit) -or
        (Test-Path -LiteralPath $failureError) -or
        (Test-Path -LiteralPath $failureEnd)) {
        $stamp = '{0}-{1}-{2}' -f (
            Get-Date
        ).ToUniversalTime().ToString('yyyyMMddTHHmmssfffZ'),
            $PID,
            ([Guid]::NewGuid().ToString('N').Substring(0, 8))
        $failureExit = Join-Path $runDir "run23.failure-$stamp.exit.txt"
        $failureError = Join-Path $runDir "run23.failure-$stamp.error.txt"
        $failureEnd = Join-Path $runDir "run23.failure-$stamp.end-utc.txt"
    }

    try {
        Write-NewUtf8File $failureError @($errorText.TrimEnd())
    }
    catch {
        [Console]::Error.WriteLine(
            "Run23 could not write failure detail: $($_.Exception.Message)"
        )
    }
    try {
        Write-NewUtf8File $failureEnd @(
            (Get-Date).ToUniversalTime().ToString('o')
        )
    }
    catch {
        [Console]::Error.WriteLine(
            "Run23 could not write failure end time: $($_.Exception.Message)"
        )
    }
    try {
        Write-NewUtf8File $failureExit @('99')
    }
    catch {
        [Console]::Error.WriteLine(
            "Run23 could not write failure exit status: $($_.Exception.Message)"
        )
    }
    if ($manifestOwned -and $manifest -and
        (Test-Path -LiteralPath $manifest -PathType Leaf)) {
        try {
            Add-Utf8Lines $manifest @(
                'launcher_failure: true',
                'launcher_failure_wrapper_exit_code: 99',
                "launcher_failure_exit_marker: $failureExit",
                "launcher_failure_detail: $failureError"
            )
        }
        catch {
            [Console]::Error.WriteLine(
                "Run23 could not append failure manifest: $($_.Exception.Message)"
            )
        }
    }
    if ($launcherLogOwned -and $launcherLog -and
        (Test-Path -LiteralPath $launcherLog -PathType Leaf)) {
        try {
            Write-LauncherLog 'Run23 launcher failed closed with wrapper exit 99.'
        }
        catch {
            [Console]::Error.WriteLine(
                "Run23 could not append failure log: $($_.Exception.Message)"
            )
        }
    }
}

try {
    if ($BootkernelBytes -le 0) {
        throw 'BootkernelBytes must be positive.'
    }

    $scriptPath = [IO.Path]::GetFullPath($PSCommandPath)
    $toolsDirectory = [IO.Path]::GetFullPath($PSScriptRoot)
    $repoCandidate = [IO.Path]::GetFullPath(
        (Join-Path $toolsDirectory '..')
    )
    if (-not (Test-Path -LiteralPath $repoCandidate -PathType Container)) {
        throw "Repository root is missing: $repoCandidate"
    }
    $repo = (Resolve-Path -LiteralPath $repoCandidate).Path
    $expectedScriptPath = [IO.Path]::GetFullPath(
        (Join-Path $repo 'tools\run23-cold-replay.ps1')
    )
    if (-not $scriptPath.Equals(
            $expectedScriptPath,
            [StringComparison]::OrdinalIgnoreCase)) {
        throw "Run23 launcher must remain at $expectedScriptPath ($scriptPath)"
    }
    Assert-NotReparsePoint 'Run23 launcher' $scriptPath
    if (-not ([IO.Path]::GetPathRoot($repo)).Equals(
            'F:\',
            [StringComparison]::OrdinalIgnoreCase)) {
        throw "Repository and Run23 mutable state must remain on F: ($repo)"
    }
    Assert-NotReparsePoint 'repository root' $repo

    $workRoot = [IO.Path]::GetFullPath((Join-Path $repo 'work'))
    if (-not (Test-Path -LiteralPath $workRoot)) {
        [void][IO.Directory]::CreateDirectory($workRoot)
    }
    if (-not (Test-Path -LiteralPath $workRoot -PathType Container)) {
        throw "Repository-local work path is not a directory: $workRoot"
    }
    Assert-NotReparsePoint 'repository work root' $workRoot

    if ([string]::IsNullOrWhiteSpace($RunDirectory)) {
        $runDir = [IO.Path]::GetFullPath(
            (Join-Path $workRoot 'run23-commcenter-baseband')
        )
    }
    else {
        $runDir = ConvertTo-CanonicalPath $repo $RunDirectory
    }
    Assert-PathIsStrictlyBelow 'Run23 directory' $workRoot $runDir
    if (-not ([IO.Path]::GetPathRoot($runDir)).Equals(
            'F:\',
            [StringComparison]::OrdinalIgnoreCase)) {
        throw "Run23 directory must remain on F: ($runDir)"
    }

    if (-not (Test-Path -LiteralPath $runDir)) {
        [void][IO.Directory]::CreateDirectory($runDir)
    }
    if (-not (Test-Path -LiteralPath $runDir -PathType Container)) {
        throw "Run23 path is not a directory: $runDir"
    }
    $runDir = (Resolve-Path -LiteralPath $runDir).Path
    Assert-PathIsStrictlyBelow 'resolved Run23 directory' $workRoot $runDir
    Assert-ContainedPathHasNoReparsePoint $workRoot $runDir
    $runDirectoryEstablished = $true

    $binDirectory = Join-Path $runDir 'bin'
    $screenDirectory = Join-Path $runDir 'firmware'
    $tempDirectory = Join-Path $runDir 'tmp'
    foreach ($directory in @(
            $binDirectory,
            $screenDirectory,
            $tempDirectory)) {
        if (-not (Test-Path -LiteralPath $directory)) {
            [void][IO.Directory]::CreateDirectory($directory)
        }
        if (-not (Test-Path -LiteralPath $directory -PathType Container)) {
            throw "Required Run23 path is not a directory: $directory"
        }
        Assert-ContainedPathHasNoReparsePoint $workRoot $directory
    }

    $sourceCommitNormalized = $SourceCommit.ToLowerInvariant()
    $bootkernelShaNormalized = $BootkernelSha256.ToUpperInvariant()
    $runBin = Join-Path $binDirectory 'bootkernel.exe'
    $kernel = Join-Path $repo 'firmware\kernel.macho'
    $tree = Join-Path $repo 'firmware\devicetree.bin'
    $sourceRoot = Join-Path $repo 'firmware\rootfs.img'
    $workRootImage = Join-Path $runDir (
        'rootfs-7e18-run23-{0}-{1}.img' -f
            $sourceCommitNormalized.Substring(0, 12),
            $bootkernelShaNormalized.Substring(0, 12).ToLowerInvariant()
    )
    $screen = Join-Path $screenDirectory 'screen.ppm'
    $stdout = Join-Path $runDir 'run23.stdout.log'
    $stderr = Join-Path $runDir 'run23.stderr.log'
    $launcherLog = Join-Path $runDir 'run23.launcher.log'
    $launcherError = Join-Path $runDir 'run23.launcher-error.txt'
    $manifest = Join-Path $runDir 'manifest.txt'
    $exitPath = Join-Path $runDir 'run23.exit.txt'
    $startPath = Join-Path $runDir 'run23.start-utc.txt'
    $endPath = Join-Path $runDir 'run23.end-utc.txt'

    $env:TEMP = $tempDirectory
    $env:TMP = $tempDirectory
    $env:TMPDIR = $tempDirectory

    $mutablePaths = @(
        $runBin,
        $workRootImage,
        $screen,
        $stdout,
        $stderr,
        $launcherLog,
        $launcherError,
        $manifest,
        $exitPath,
        $startPath,
        $endPath,
        $env:TEMP,
        $env:TMP,
        $env:TMPDIR
    )
    foreach ($path in $mutablePaths) {
        Assert-PathIsStrictlyBelow 'Run23 mutable path' $runDir $path
    }

    $freshOutputs = @(
        $workRootImage,
        $screen,
        $stdout,
        $stderr,
        $launcherLog,
        $launcherError,
        $manifest,
        $exitPath,
        $startPath,
        $endPath
    )
    $preexisting = @(
        $freshOutputs | Where-Object {
            Test-Path -LiteralPath $_
        }
    )
    if ($preexisting.Count -ne 0) {
        throw (
            "Refusing pre-existing Run23 output(s): {0}" -f
            ($preexisting -join ', ')
        )
    }

    Write-LauncherLog 'Run23 exact-input preflight started.'
    $launcherLogOwned = $true

    $headOutput = (& git -C $repo rev-parse HEAD 2>&1 | Out-String).Trim()
    if ($LASTEXITCODE -ne 0) {
        throw "Cannot resolve repository HEAD: $headOutput"
    }
    if (-not $headOutput.Equals(
            $sourceCommitNormalized,
            [StringComparison]::OrdinalIgnoreCase)) {
        throw (
            "Run23 source mismatch: requested {0}, HEAD {1}" -f
            $sourceCommitNormalized, $headOutput
        )
    }
    $bootBuildInputStatus = (
        & git -C $repo status --porcelain=v1 --untracked-files=all -- `
            CMakeLists.txt core tools 2>&1 |
        Out-String
    ).Trim()
    if ($LASTEXITCODE -ne 0) {
        throw (
            "Cannot inspect tracked Run23 boot build inputs: {0}" -f
            $bootBuildInputStatus
        )
    }
    if ($bootBuildInputStatus.Length -ne 0) {
        throw (
            "Run23 boot build inputs must match HEAD: {0}" -f
            $bootBuildInputStatus
        )
    }
    $branch = (
        & git -C $repo branch --show-current 2>&1 |
        Out-String
    ).Trim()
    if ($LASTEXITCODE -ne 0) {
        throw "Cannot resolve repository branch: $branch"
    }

    Assert-ContainedPathHasNoReparsePoint $runDir $runBin
    $bootkernelEvidence = Assert-ExactInput `
        'bootkernel' $runBin $BootkernelBytes $bootkernelShaNormalized

    $kernelBytes = [long]7942144
    $kernelSha256 =
        '0D8CDB339D37CF37A1DB2638FFF79272ECD63A17764BF7666EFA1618725DF70C'
    $treeBytes = [long]40544
    $treeSha256 =
        '4867C95FEDF544BDA2ECAA2626AE14C01A60D7771DC53FFE6FD3A6AAC8B8BA57'
    $sourceRootBytes = [long]433274880
    $sourceRootSha256 =
        'C3251E7F092C939D5818E92086CB47680981CFB03731DE7B55D238C942EB5E82'
    $expectedWorkRootBytes = [long]466825216

    $kernelEvidence = Assert-ExactInput `
        'kernel' $kernel $kernelBytes $kernelSha256
    $treeEvidence = Assert-ExactInput `
        'device tree' $tree $treeBytes $treeSha256
    $sourceRootEvidence = Assert-ExactInput `
        'immutable rootfs' $sourceRoot $sourceRootBytes $sourceRootSha256
    $scriptEvidence = Get-FileEvidence 'Run23 launcher' $scriptPath

    $profileSpan = [long]200000000
    $profileStart = $InstructionCap - $profileSpan
    if ($profileStart -lt 0) { $profileStart = [long]0 }
    $profileWindow = '{0}:{1}' -f $profileStart, $InstructionCap

    $bootArguments = @(
        $kernel,
        '-p', '0x08000000',
        '-V', '0xc0000000',
        '-d', $tree,
        '-c', 'debug=0x8 serial=1 nand-enable-adm=0',
        '--external-md', $sourceRoot, $workRootImage,
        '--grow', '32',
        '--fstab', '/dev/md0 / hfs rw,update 0 1',
        '-R', '128',
        '-F',
        '-H', '0x3d200000',
        '-W', $profileWindow,
        '-Z', '100000000',
        '-n', [string]$InstructionCap
    )
    if ($SnapshotAt -gt 0) {
        if ($SnapshotAt -ge $InstructionCap) {
            throw ("SnapshotAt {0} must be below the instruction cap {1}" -f
                   $SnapshotAt, $InstructionCap)
        }
        $snapshotPath = Join-Path $runDir ('run.snapshot-{0}' -f $SnapshotAt)
        foreach ($p in @($snapshotPath,
                         ($snapshotPath + '.mdimage'),
                         ($snapshotPath + '.mdstate'))) {
            Assert-PathIsStrictlyBelow 'Run23 checkpoint path' $runDir $p
            if (Test-Path -LiteralPath $p) {
                throw "Refusing pre-existing checkpoint output: $p"
            }
        }
        $bootArguments += @('--snapshot-at', [string]$SnapshotAt, $snapshotPath)
    }

    $restoreEvidenceLines = @('restore_from: none; cold boot from the kernel entry point')
    if ($RestoreFrom -ne '') {
        if ($SnapshotAt -gt 0) {
            throw 'Taking a checkpoint during a restored run is not supported yet; use one of -SnapshotAt or -RestoreFrom'
        }
        $restorePath = ConvertTo-CanonicalPath $repo $RestoreFrom
        # A checkpoint is machine state, not firmware, so it must live in the
        # workspace's work root like every other mutable artefact -- but it is
        # an INPUT here, so it is read, never written, and never placed under
        # this run's own directory.
        Assert-PathIsStrictlyBelow 'Run23 restore source' $workRoot $restorePath
        $restoreEvidenceLines = @()
        foreach ($suffix in @('', '.mdimage', '.mdstate')) {
            $part = $restorePath + $suffix
            if (-not (Test-Path -LiteralPath $part -PathType Leaf)) {
                throw "Restore source is missing its '$suffix' sidecar: $part"
            }
            $evidence = Get-FileEvidence "restore$suffix" $part
            $restoreEvidenceLines += @(
                "restore_from$($suffix)_path: $part",
                "restore_from$($suffix)_bytes: $($evidence.Bytes)",
                "restore_from$($suffix)_sha256: $($evidence.Sha256)"
            )
        }
        $bootArguments += @('--restore', $restorePath)
    }
    $childCommandLine = (
        $bootArguments |
        ForEach-Object {
            ConvertTo-WindowsCommandLineArgument ([string]$_)
        }
    ) -join ' '
    $fullArgv = @($runBin) + $bootArguments
    $argvJson = ConvertTo-Json -InputObject @($fullArgv) -Compress
    $exactCommand = '{0} {1}' -f (
        ConvertTo-WindowsCommandLineArgument $runBin
    ), $childCommandLine

    $manifestLines = @(
        'run: run23-commcenter-baseband',
        'purpose: exact cold replay of fail-closed queue, per-thread wait, and AppleBaseband notification diagnostics',
        "source_commit: $sourceCommitNormalized",
        "source_commit_head_verified: true",
        "tracked_boot_build_inputs_clean: true",
        "branch: $branch",
        "launcher_path: $scriptPath",
        "launcher_bytes: $($scriptEvidence.Bytes)",
        "launcher_sha256: $($scriptEvidence.Sha256)",
        "bootkernel_path: $runBin",
        "bootkernel_bytes: $($bootkernelEvidence.Bytes)",
        "bootkernel_sha256: $($bootkernelEvidence.Sha256)",
        "kernel_path: $kernel",
        "kernel_bytes: $($kernelEvidence.Bytes)",
        "kernel_sha256: $($kernelEvidence.Sha256)",
        "devicetree_path: $tree",
        "devicetree_bytes: $($treeEvidence.Bytes)",
        "devicetree_sha256: $($treeEvidence.Sha256)",
        "rootfs_source_path: $sourceRoot",
        "rootfs_source_bytes: $($sourceRootEvidence.Bytes)",
        "rootfs_source_sha256: $($sourceRootEvidence.Sha256)",
        "rootfs_source_role: immutable; external-md creates a unique writable F:-local work image",
        "rootfs_work_path: $workRootImage",
        "rootfs_expected_work_bytes: $expectedWorkRootBytes",
        "executable: $runBin",
        "argv_json: $argvJson",
        "windows_command_line: $exactCommand",
        "instruction_cap: $InstructionCap",
        "profile_window: $profileWindow",
        "snapshot_at: $SnapshotAt",
        'heartbeat_interval: 100000000',
        'restore_note: --snapshot-at and --restore are both keyed on the machine''s own retired-instruction counter, so the cap is absolute across a restore',
        'hot_page: 0x3d200000',
        'guest_ram_mib: 128',
        'display_enabled: true',
        'snapshots: -SnapshotAt writes a checkpoint; -RestoreFrom starts from one. Taking a checkpoint during a restored run is not supported yet.',
        "working_directory: $runDir",
        "stdout: $stdout",
        "stderr: $stderr",
        "launcher_log: $launcherLog",
        "launcher_error: $launcherError",
        "screen: $screen",
        "TEMP: $env:TEMP",
        "TMP: $env:TMP",
        "TMPDIR: $env:TMPDIR",
        'prelaunch_status: exact HEAD, clean tracked core/tools/CMake build inputs, launcher, copied binary, kernel, device tree, and immutable rootfs verified; all outputs fresh',
        'claim_gate: no owner, wait, baseband-causality, reply, or render claim until the terminal report and immutable hashes are reviewed'
    ) + $restoreEvidenceLines
    Write-NewUtf8File $manifest $manifestLines
    $manifestOwned = $true

    $startUtc = (Get-Date).ToUniversalTime().ToString('o')
    Write-NewUtf8File $startPath @($startUtc)
    Write-LauncherLog 'Run23 cold boot starting.'
    $process = Start-Process `
        -FilePath $runBin `
        -ArgumentList $childCommandLine `
        -WorkingDirectory $runDir `
        -RedirectStandardOutput $stdout `
        -RedirectStandardError $stderr `
        -WindowStyle Hidden `
        -Wait `
        -PassThru
    $childExitCode = [int]$process.ExitCode
    $endUtc = (Get-Date).ToUniversalTime().ToString('o')
    Write-LauncherLog "Run23 child exited with code $childExitCode."
    Add-Utf8Lines $manifest @(
        "postrun_start_utc: $startUtc",
        "postrun_end_utc: $endUtc",
        "postrun_child_exit_code: $childExitCode"
    )

    $postScript = Get-FileEvidence 'post-run launcher' $scriptPath
    $postBootkernel = Get-FileEvidence 'post-run bootkernel' $runBin
    $postKernel = Get-FileEvidence 'post-run kernel' $kernel
    $postTree = Get-FileEvidence 'post-run device tree' $tree
    $postSourceRoot = Get-FileEvidence 'post-run immutable rootfs' $sourceRoot
    $postStdout = Get-FileEvidence 'Run23 stdout' $stdout
    $postStderr = Get-FileEvidence 'Run23 stderr' $stderr
    $postWorkRoot = Get-FileEvidence 'Run23 rootfs work image' $workRootImage
    $postScreen = Get-FileEvidence 'Run23 screen' $screen

    $scriptUnchanged =
        $postScript.Bytes -eq $scriptEvidence.Bytes -and
        $postScript.Sha256.Equals(
            $scriptEvidence.Sha256,
            [StringComparison]::OrdinalIgnoreCase
        )
    $bootkernelUnchanged =
        $postBootkernel.Bytes -eq $bootkernelEvidence.Bytes -and
        $postBootkernel.Sha256.Equals(
            $bootkernelEvidence.Sha256,
            [StringComparison]::OrdinalIgnoreCase
        )
    $sourceUnchanged =
        $postKernel.Bytes -eq $kernelEvidence.Bytes -and
        $postKernel.Sha256.Equals(
            $kernelEvidence.Sha256,
            [StringComparison]::OrdinalIgnoreCase
        ) -and
        $postTree.Bytes -eq $treeEvidence.Bytes -and
        $postTree.Sha256.Equals(
            $treeEvidence.Sha256,
            [StringComparison]::OrdinalIgnoreCase
        ) -and
        $postSourceRoot.Bytes -eq $sourceRootEvidence.Bytes -and
        $postSourceRoot.Sha256.Equals(
            $sourceRootEvidence.Sha256,
            [StringComparison]::OrdinalIgnoreCase
        )

    Add-Utf8Lines $manifest @(
        "postrun_launcher_bytes: $($postScript.Bytes)",
        "postrun_launcher_sha256: $($postScript.Sha256)",
        "postrun_launcher_unchanged: $($scriptUnchanged.ToString().ToLowerInvariant())",
        "postrun_bootkernel_bytes: $($postBootkernel.Bytes)",
        "postrun_bootkernel_sha256: $($postBootkernel.Sha256)",
        "postrun_bootkernel_unchanged: $($bootkernelUnchanged.ToString().ToLowerInvariant())",
        "postrun_kernel_bytes: $($postKernel.Bytes)",
        "postrun_kernel_sha256: $($postKernel.Sha256)",
        "postrun_devicetree_bytes: $($postTree.Bytes)",
        "postrun_devicetree_sha256: $($postTree.Sha256)",
        "postrun_rootfs_source_bytes: $($postSourceRoot.Bytes)",
        "postrun_rootfs_source_sha256: $($postSourceRoot.Sha256)",
        "postrun_source_hashes_unchanged: $($sourceUnchanged.ToString().ToLowerInvariant())",
        "postrun_stdout_bytes: $($postStdout.Bytes)",
        "postrun_stdout_sha256: $($postStdout.Sha256)",
        "postrun_stderr_bytes: $($postStderr.Bytes)",
        "postrun_stderr_sha256: $($postStderr.Sha256)",
        "postrun_rootfs_work_bytes: $($postWorkRoot.Bytes)",
        "postrun_rootfs_work_sha256: $($postWorkRoot.Sha256)",
        "postrun_screen_bytes: $($postScreen.Bytes)",
        "postrun_screen_sha256: $($postScreen.Sha256)"
    )

    if (-not $scriptUnchanged) {
        throw 'Run23 launcher changed during execution.'
    }
    if (-not $bootkernelUnchanged) {
        throw 'Run23 bootkernel changed during execution.'
    }
    if (-not $sourceUnchanged) {
        throw 'Run23 immutable firmware source changed during execution.'
    }
    if ($postWorkRoot.Bytes -ne $expectedWorkRootBytes) {
        throw (
            "Run23 work image size mismatch: expected {0}, got {1}" -f
            $expectedWorkRootBytes, $postWorkRoot.Bytes
        )
    }

    Add-Utf8Lines $manifest @(
        "postrun_wrapper_exit_code: $childExitCode",
        'postrun_postflight_passed: true'
    )
    Write-NewUtf8File $endPath @($endUtc)
    Write-NewUtf8File $exitPath @([string]$childExitCode)
    Write-LauncherLog (
        "Run23 postflight passed; wrapper returning child code $childExitCode."
    )
    exit $childExitCode
}
catch {
    Write-FailureEvidence $_
    [Console]::Error.WriteLine(
        "Run23 launcher failed closed: $($_.Exception.Message)"
    )
    exit 99
}
