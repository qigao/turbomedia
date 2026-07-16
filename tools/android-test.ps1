#requires -Version 7.0

<#
.SYNOPSIS
Builds, deploys, runs, or debugs one TurboMedia test on an Android device.

.DESCRIPTION
Uses the repository's Android CMake preset to build one EXCLUDE_FROM_ALL test
target, deploys the executable and its non-system ELF dependencies through ADB,
and runs it from /data/local/tmp. With -Lldb, the script starts the NDK
lldb-server, forwards its localhost TCP listener through ADB, and launches the
matching host LLDB.

.PARAMETER Target
The CMake executable target to build and run, for example turbo_media_test_crypto.

.PARAMETER Preset
The Android build preset. Its associated configure preset and binaryDir are
resolved from CMake preset JSON. The default is the Windows-hosted ARM64
Release build preset.

.PARAMETER Serial
ADB device serial. Required when more than one online device is present.

.PARAMETER BuildDirectory
Optional configured CMake build directory override. Normally resolved from the
configure preset's binaryDir.

.PARAMETER RemoteDirectory
Absolute device working directory below /data/local/tmp.

.PARAMETER Filter
TinyTest filter passed as --filter.

.PARAMETER Tap
Requests TinyTest TAP output.

.PARAMETER JUnit
Local destination for TinyTest JUnit XML. The file is generated on the device
and pulled after the test finishes.

.PARAMETER Data
Additional test files or directories to push into the remote working directory.

.PARAMETER Library
Additional shared libraries to deploy. Their ELF dependencies are resolved too.

.PARAMETER LibraryDirectory
Additional directories searched for non-system shared-library dependencies.

.PARAMETER TestArgument
Additional arguments passed verbatim to the device test executable.

.PARAMETER Lldb
Runs the test under the NDK LLDB client/server instead of running it directly.

.PARAMETER LldbCommand
Additional LLDB commands executed after connecting. This is useful for scripted
debug sessions, for example -LldbCommand 'process continue','quit'.

.PARAMETER Port
Host and device localhost TCP port used for the LLDB connection.

.PARAMETER NoBuild
Uses an already-built test executable without invoking CMake.

.EXAMPLE
./tools/android-test.ps1 turbo_media_test_crypto -Tap

.EXAMPLE
./tools/android-test.ps1 turbo_media_test_crypto -Filter "known error" -JUnit artifacts/turbo_media_test_crypto.xml

.EXAMPLE
./tools/android-test.ps1 turbo_media_test_crypto -Lldb -Serial adb-DEVICE._adb-tls-connect._tcp
#>
param(
    [Parameter(Mandatory, Position = 0)]
    [ValidatePattern('^[A-Za-z0-9_.+-]+$')]
    [string]$Target,

    [ValidatePattern('^[A-Za-z0-9_.+-]+$')]
    [string]$Preset = 'android-arm64-v8a-release-win',

    [string]$Serial,
    [string]$BuildDirectory,

    [ValidatePattern('^/data/local/tmp/[A-Za-z0-9._/-]+$')]
    [string]$RemoteDirectory = '/data/local/tmp/turbomedia-tests',

    [string]$Filter,
    [switch]$Tap,
    [string]$JUnit,
    [string[]]$Data = @(),
    [string[]]$Library = @(),
    [string[]]$LibraryDirectory = @(),

    [Parameter(ValueFromRemainingArguments)]
    [string[]]$TestArgument = @(),

    [switch]$Lldb,
    [string[]]$LldbCommand = @(),

    [ValidateRange(1024, 65535)]
    [int]$Port = 5039,

    [switch]$NoBuild
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if ($RemoteDirectory -match '(^|/)\.\.(/|$)') {
    throw "RemoteDirectory must not contain '..': $RemoteDirectory"
}

$RepoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))

function Invoke-NativeCommand {
    param(
        [Parameter(Mandatory)][string]$FilePath,
        [Parameter()][string[]]$ArgumentList = @()
    )

    & $FilePath @ArgumentList
    if ($LASTEXITCODE -ne 0) {
        throw "Command failed with exit code ${LASTEXITCODE}: $FilePath $($ArgumentList -join ' ')"
    }
}

function Invoke-NativeCapture {
    param(
        [Parameter(Mandatory)][string]$FilePath,
        [Parameter()][string[]]$ArgumentList = @(),
        [switch]$AllowFailure
    )

    $output = & $FilePath @ArgumentList 2>&1
    $exitCode = $LASTEXITCODE
    if (-not $AllowFailure -and $exitCode -ne 0) {
        throw "Command failed with exit code ${exitCode}: $FilePath $($ArgumentList -join ' ')`n$($output -join "`n")"
    }
    return [pscustomobject]@{
        ExitCode = $exitCode
        Output = ($output -join "`n")
    }
}

function Get-CacheValue {
    param(
        [Parameter(Mandatory)][string]$CachePath,
        [Parameter(Mandatory)][string]$Name
    )

    $pattern = '^' + [regex]::Escape($Name) + ':[^=]*=(.*)$'
    foreach ($line in [IO.File]::ReadLines($CachePath)) {
        if ($line -match $pattern) {
            return $Matches[1]
        }
    }
    return $null
}

function Get-CommandPath {
    param([Parameter(Mandatory)][string]$Name)

    $command = Get-Command $Name -ErrorAction SilentlyContinue
    if ($null -eq $command) {
        return $null
    }
    return $command.Source
}

function Expand-PresetIncludePath {
    param(
        [Parameter(Mandatory)][string]$Value,
        [Parameter(Mandatory)][string]$FileDirectory
    )

    $expanded = $Value.Replace('${sourceDir}', $RepoRoot).Replace('${fileDir}', $FileDirectory)
    $expanded = [regex]::Replace($expanded, '\$penv\{([^}]+)\}', {
        param($match)
        $value = [Environment]::GetEnvironmentVariable($match.Groups[1].Value)
        if ($null -eq $value) {
            throw "Preset include references undefined parent environment variable '$($match.Groups[1].Value)'."
        }
        return $value
    })
    if ($expanded -match '\$(?:\{|penv\{|env\{)') {
        throw "Unsupported macro in preset include '$Value'."
    }
    if (-not [IO.Path]::IsPathRooted($expanded)) {
        $expanded = Join-Path $FileDirectory $expanded
    }
    return [IO.Path]::GetFullPath($expanded)
}

$ConfigurePresets = [Collections.Generic.Dictionary[string, object]]::new([StringComparer]::Ordinal)
$BuildPresets = [Collections.Generic.Dictionary[string, object]]::new([StringComparer]::Ordinal)
$VisitedPresetFiles = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)

function Add-PresetDefinition {
    param(
        [Parameter(Mandatory)][Collections.Generic.Dictionary[string, object]]$Registry,
        [Parameter(Mandatory)][object]$PresetObject,
        [Parameter(Mandatory)][string]$FilePath
    )

    if ($Registry.ContainsKey($PresetObject.name)) {
        throw "Duplicate CMake preset '$($PresetObject.name)' found while reading '$FilePath'."
    }
    $Registry.Add($PresetObject.name, [pscustomobject]@{
        Data = $PresetObject
        FileDirectory = Split-Path $FilePath -Parent
    })
}

function Import-PresetFile {
    param([Parameter(Mandatory)][string]$Path)

    $fullPath = [IO.Path]::GetFullPath($Path)
    if (-not $VisitedPresetFiles.Add($fullPath)) {
        return
    }
    if (-not (Test-Path -LiteralPath $fullPath -PathType Leaf)) {
        throw "Included CMake preset file does not exist: $fullPath"
    }

    try {
        $document = Get-Content -Raw -LiteralPath $fullPath | ConvertFrom-Json
    } catch {
        throw "Cannot parse CMake preset JSON '$fullPath': $($_.Exception.Message)"
    }

    $fileDirectory = Split-Path $fullPath -Parent
    $includeProperty = $document.PSObject.Properties['include']
    if ($null -ne $includeProperty -and $null -ne $includeProperty.Value) {
        foreach ($include in @($includeProperty.Value)) {
            Import-PresetFile (Expand-PresetIncludePath ([string]$include) $fileDirectory)
        }
    }
    $configureProperty = $document.PSObject.Properties['configurePresets']
    if ($null -ne $configureProperty -and $null -ne $configureProperty.Value) {
        foreach ($presetObject in @($configureProperty.Value)) {
            Add-PresetDefinition $ConfigurePresets $presetObject $fullPath
        }
    }
    $buildProperty = $document.PSObject.Properties['buildPresets']
    if ($null -ne $buildProperty -and $null -ne $buildProperty.Value) {
        foreach ($presetObject in @($buildProperty.Value)) {
            Add-PresetDefinition $BuildPresets $presetObject $fullPath
        }
    }
}

function Resolve-PresetProperty {
    param(
        [Parameter(Mandatory)][Collections.Generic.Dictionary[string, object]]$Registry,
        [Parameter(Mandatory)][string]$PresetName,
        [Parameter(Mandatory)][string]$PropertyName,
        [Collections.Generic.HashSet[string]]$Visiting = $null
    )

    if (-not $Registry.ContainsKey($PresetName)) {
        throw "CMake preset '$PresetName' was not found."
    }
    if ($null -eq $Visiting) {
        $Visiting = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    }
    if (-not $Visiting.Add($PresetName)) {
        throw "CMake preset inheritance cycle detected at '$PresetName'."
    }

    try {
        $definition = $Registry[$PresetName]
        $property = $definition.Data.PSObject.Properties[$PropertyName]
        if ($null -ne $property -and $null -ne $property.Value) {
            return [pscustomobject]@{
                Value = $property.Value
                OwnerName = $PresetName
                FileDirectory = $definition.FileDirectory
            }
        }

        $inheritsProperty = $definition.Data.PSObject.Properties['inherits']
        if ($null -ne $inheritsProperty -and $null -ne $inheritsProperty.Value) {
            foreach ($parentName in @($inheritsProperty.Value)) {
                $resolved = Resolve-PresetProperty $Registry ([string]$parentName) $PropertyName $Visiting
                if ($null -ne $resolved) {
                    return $resolved
                }
            }
        }
        return $null
    } finally {
        [void]$Visiting.Remove($PresetName)
    }
}

function Resolve-PresetMap {
    param(
        [Parameter(Mandatory)][Collections.Generic.Dictionary[string, object]]$Registry,
        [Parameter(Mandatory)][string]$PresetName,
        [Parameter(Mandatory)][string]$PropertyName,
        [Collections.Generic.HashSet[string]]$Visiting = $null
    )

    if (-not $Registry.ContainsKey($PresetName)) {
        throw "CMake preset '$PresetName' was not found."
    }
    if ($null -eq $Visiting) {
        $Visiting = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    }
    if (-not $Visiting.Add($PresetName)) {
        throw "CMake preset inheritance cycle detected at '$PresetName'."
    }

    try {
        $definition = $Registry[$PresetName]
        $resolved = [ordered]@{}
        $inheritsProperty = $definition.Data.PSObject.Properties['inherits']
        if ($null -ne $inheritsProperty -and $null -ne $inheritsProperty.Value) {
            $parents = @($inheritsProperty.Value)
            for ($index = $parents.Count - 1; $index -ge 0; --$index) {
                $parentMap = Resolve-PresetMap $Registry ([string]$parents[$index]) $PropertyName $Visiting
                foreach ($entry in $parentMap.GetEnumerator()) {
                    $resolved[$entry.Key] = $entry.Value
                }
            }
        }

        $property = $definition.Data.PSObject.Properties[$PropertyName]
        if ($null -ne $property -and $null -ne $property.Value) {
            foreach ($entry in $property.Value.PSObject.Properties) {
                $resolved[$entry.Name] = $entry.Value
            }
        }
        return $resolved
    } finally {
        [void]$Visiting.Remove($PresetName)
    }
}

function Expand-ConfigurePresetValue {
    param(
        [Parameter(Mandatory)][string]$Value,
        [Parameter(Mandatory)][Collections.IDictionary]$PresetEnvironment
    )

    $expanded = $Value.
        Replace('${sourceDir}', $RepoRoot).
        Replace('${sourceParentDir}', (Split-Path $RepoRoot -Parent)).
        Replace('${sourceDirName}', (Split-Path $RepoRoot -Leaf)).
        Replace('${presetName}', $ConfigurePresetName).
        Replace('${dollar}', '$').
        Replace('${pathListSep}', [IO.Path]::PathSeparator)
    $expanded = [regex]::Replace($expanded, '\$penv\{([^}]+)\}', {
        param($match)
        $value = [Environment]::GetEnvironmentVariable($match.Groups[1].Value)
        if ($null -eq $value) {
            throw "Preset references undefined parent environment variable '$($match.Groups[1].Value)'."
        }
        return $value
    })
    $expanded = [regex]::Replace($expanded, '\$env\{([^}]+)\}', {
        param($match)
        $name = $match.Groups[1].Value
        if ($PresetEnvironment.Contains($name) -and $null -ne $PresetEnvironment[$name]) {
            return [string]$PresetEnvironment[$name]
        }
        $value = [Environment]::GetEnvironmentVariable($name)
        if ($null -eq $value) {
            throw "Preset references undefined environment variable '$name'."
        }
        return $value
    })
    if ($expanded -match '\$(?:\{|penv\{|env\{)') {
        throw "Cannot fully expand configure preset value '$Value'."
    }
    return $expanded
}

function Expand-ConfigurePresetPath {
    param(
        [Parameter(Mandatory)][object]$Property,
        [Parameter(Mandatory)][string]$ConfigurePresetName,
        [AllowEmptyString()][string]$GeneratorName = ''
    )

    $sourceParent = Split-Path $RepoRoot -Parent
    $sourceName = Split-Path $RepoRoot -Leaf
    $hostSystemName = if ($IsWindows) { 'Windows' } elseif ($IsLinux) { 'Linux' } elseif ($IsMacOS) { 'Darwin' } else { [Environment]::OSVersion.Platform.ToString() }
    $expanded = ([string]$Property.Value).
        Replace('${sourceDir}', $RepoRoot).
        Replace('${sourceParentDir}', $sourceParent).
        Replace('${sourceDirName}', $sourceName).
        Replace('${presetName}', $ConfigurePresetName).
        Replace('${fileDir}', $Property.FileDirectory).
        Replace('${generator}', $GeneratorName).
        Replace('${hostSystemName}', $hostSystemName).
        Replace('${dollar}', '$').
        Replace('${pathListSep}', [IO.Path]::PathSeparator)
    $expanded = [regex]::Replace($expanded, '\$(?:p?env)\{([^}]+)\}', {
        param($match)
        $value = [Environment]::GetEnvironmentVariable($match.Groups[1].Value)
        if ($null -eq $value) {
            throw "Preset binaryDir references undefined environment variable '$($match.Groups[1].Value)'."
        }
        return $value
    })
    if ($expanded -match '\$(?:\{|penv\{|env\{)') {
        throw "Cannot expand preset binaryDir '$($Property.Value)'. Pass -BuildDirectory explicitly."
    }
    if (-not [IO.Path]::IsPathRooted($expanded)) {
        $expanded = Join-Path $RepoRoot $expanded
    }
    return [IO.Path]::GetFullPath($expanded)
}

$rootPresets = Join-Path $RepoRoot 'CMakePresets.json'
$userPresets = Join-Path $RepoRoot 'CMakeUserPresets.json'
if (Test-Path -LiteralPath $rootPresets -PathType Leaf) {
    Import-PresetFile $rootPresets
}
if (Test-Path -LiteralPath $userPresets -PathType Leaf) {
    Import-PresetFile $userPresets
}
if ($BuildPresets.Count -eq 0) {
    throw "No CMake build presets were found below $RepoRoot"
}

$configurePresetProperty = Resolve-PresetProperty $BuildPresets $Preset 'configurePreset'
if ($null -eq $configurePresetProperty) {
    throw "Build preset '$Preset' does not resolve a configurePreset."
}
$ConfigurePresetName = [string]$configurePresetProperty.Value
$ConfigureEnvironment = Resolve-PresetMap $ConfigurePresets $ConfigurePresetName 'environment'
$ConfigureCacheVariables = Resolve-PresetMap $ConfigurePresets $ConfigurePresetName 'cacheVariables'

$NdkRoot = $null
if ($ConfigureEnvironment.Contains('ANDROID_NDK_HOME') -and
    $null -ne $ConfigureEnvironment['ANDROID_NDK_HOME']) {
    $NdkRoot = Expand-ConfigurePresetValue ([string]$ConfigureEnvironment['ANDROID_NDK_HOME']) $ConfigureEnvironment
}
if ([string]::IsNullOrWhiteSpace($NdkRoot) -and
    $ConfigureCacheVariables.Contains('VCPKG_CHAINLOAD_TOOLCHAIN_FILE')) {
    $chainloadToolchain = Expand-ConfigurePresetValue ([string]$ConfigureCacheVariables['VCPKG_CHAINLOAD_TOOLCHAIN_FILE']) $ConfigureEnvironment
    if ((Split-Path $chainloadToolchain -Leaf) -eq 'android.toolchain.cmake') {
        $NdkRoot = Split-Path (Split-Path (Split-Path $chainloadToolchain -Parent) -Parent) -Parent
    }
}
if ([string]::IsNullOrWhiteSpace($NdkRoot)) {
    throw "Configure preset '$ConfigurePresetName' does not define ANDROID_NDK_HOME or an Android VCPKG_CHAINLOAD_TOOLCHAIN_FILE."
}
$NdkRoot = [IO.Path]::GetFullPath($NdkRoot)
$NdkToolchainFile = Join-Path $NdkRoot 'build/cmake/android.toolchain.cmake'
if (-not (Test-Path -LiteralPath $NdkToolchainFile -PathType Leaf)) {
    throw "Configure preset '$ConfigurePresetName' resolves an invalid Android NDK: $NdkRoot"
}

$generatorProperty = Resolve-PresetProperty $ConfigurePresets $ConfigurePresetName 'generator'
$GeneratorName = if ($null -eq $generatorProperty) { '' } else { [string]$generatorProperty.Value }
$configurationProperty = Resolve-PresetProperty $BuildPresets $Preset 'configuration'
$BuildConfiguration = if ($null -eq $configurationProperty) { '' } else { [string]$configurationProperty.Value }

if ([string]::IsNullOrWhiteSpace($BuildDirectory)) {
    $binaryDirProperty = Resolve-PresetProperty $ConfigurePresets $ConfigurePresetName 'binaryDir'
    if ($null -eq $binaryDirProperty) {
        throw "Configure preset '$ConfigurePresetName' does not resolve a binaryDir. Pass -BuildDirectory explicitly."
    }
    $BuildDirectory = Expand-ConfigurePresetPath $binaryDirProperty $ConfigurePresetName $GeneratorName
} elseif (-not [IO.Path]::IsPathRooted($BuildDirectory)) {
    $BuildDirectory = Join-Path $RepoRoot $BuildDirectory
}
$BuildDirectory = [IO.Path]::GetFullPath($BuildDirectory)
$CachePath = Join-Path $BuildDirectory 'CMakeCache.txt'

$CMake = Get-CommandPath 'cmake'
if ($null -eq $CMake) {
    throw 'cmake was not found on PATH.'
}

if (-not $NoBuild) {
    $queryDirectory = Join-Path $BuildDirectory '.cmake/api/v1/query/client-android-test'
    [void](New-Item -ItemType Directory -Path $queryDirectory -Force)
    $query = @{
        requests = @(@{ kind = 'codemodel'; version = @{ major = 2 } })
    } | ConvertTo-Json -Depth 5
    Set-Content -LiteralPath (Join-Path $queryDirectory 'query.json') -Value $query -Encoding utf8NoBOM

    Push-Location $RepoRoot
    try {
        Invoke-NativeCommand $CMake @('--preset', $ConfigurePresetName)
        Invoke-NativeCommand $CMake @('--build', '--preset', $Preset, '--target', $Target, '--parallel')
    } finally {
        Pop-Location
    }
}

if (-not (Test-Path -LiteralPath $CachePath -PathType Leaf)) {
    throw "CMake cache does not exist: $CachePath"
}

function Get-CMakeTargetInfo {
    param(
        [Parameter(Mandatory)][string]$CMakeBuildDirectory,
        [Parameter(Mandatory)][string]$TargetName,
        [AllowEmptyString()][string]$ConfigurationName = ''
    )

    $replyDirectory = Join-Path $CMakeBuildDirectory '.cmake/api/v1/reply'
    if (-not (Test-Path -LiteralPath $replyDirectory -PathType Container)) {
        throw "CMake File API reply is missing: $replyDirectory. Run once without -NoBuild."
    }
    $indexFile = Get-ChildItem -LiteralPath $replyDirectory -Filter 'index-*.json' -File |
        Sort-Object Name -Descending |
        Select-Object -First 1
    if ($null -eq $indexFile) {
        throw "CMake File API index is missing below $replyDirectory"
    }

    $index = Get-Content -Raw -LiteralPath $indexFile.FullName | ConvertFrom-Json
    $codemodelReference = @($index.objects) | Where-Object { $_.kind -eq 'codemodel' } | Select-Object -First 1
    if ($null -eq $codemodelReference) {
        throw "CMake File API index '$($indexFile.FullName)' has no codemodel reply."
    }
    $codemodelPath = Join-Path $replyDirectory $codemodelReference.jsonFile
    $codemodel = Get-Content -Raw -LiteralPath $codemodelPath | ConvertFrom-Json

    $selectedTargetReferences = @(
        foreach ($configuration in @($codemodel.configurations)) {
            if (-not [string]::IsNullOrWhiteSpace($ConfigurationName) -and
                $configuration.name -ne $ConfigurationName) {
                continue
            }
            foreach ($targetReference in @($configuration.targets)) {
                $targetReference
            }
        }
    )
    $targetReferences = @($selectedTargetReferences | Where-Object { $_.name -eq $TargetName })
    if ($targetReferences.Count -eq 0) {
        $configurationMessage = if ([string]::IsNullOrWhiteSpace($ConfigurationName)) { '' } else { " for configuration '$ConfigurationName'" }
        throw "Target '$TargetName' is absent$configurationMessage from CMake File API codemodel '$codemodelPath'."
    }

    $artifactPaths = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    $allArtifactPaths = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($targetReference in $selectedTargetReferences) {
        $targetPath = Join-Path $replyDirectory $targetReference.jsonFile
        $targetObject = Get-Content -Raw -LiteralPath $targetPath | ConvertFrom-Json
        if ($targetReference.name -eq $TargetName -and $targetObject.type -ne 'EXECUTABLE') {
            throw "CMake target '$TargetName' has type '$($targetObject.type)', expected EXECUTABLE."
        }
        $artifactsProperty = $targetObject.PSObject.Properties['artifacts']
        if ($null -eq $artifactsProperty -or $null -eq $artifactsProperty.Value) {
            continue
        }
        foreach ($artifact in @($artifactsProperty.Value)) {
            $candidate = if ([IO.Path]::IsPathRooted($artifact.path)) {
                [IO.Path]::GetFullPath($artifact.path)
            } else {
                [IO.Path]::GetFullPath((Join-Path $CMakeBuildDirectory $artifact.path))
            }
            if (Test-Path -LiteralPath $candidate -PathType Leaf) {
                [void]$allArtifactPaths.Add($candidate)
                if ($targetReference.name -eq $TargetName -and
                    [IO.Path]::GetExtension($candidate) -notin @('.pdb', '.dSYM')) {
                    [void]$artifactPaths.Add($candidate)
                }
            }
        }
    }
    if ($artifactPaths.Count -ne 1) {
        throw "Target '$TargetName' resolves $($artifactPaths.Count) executable artifacts: $($artifactPaths -join ', ')"
    }
    return [pscustomobject]@{
        Executable = @($artifactPaths)[0]
        Artifacts = @($allArtifactPaths)
        CodemodelPath = $codemodelPath
    }
}

$CMakeTargetInfo = Get-CMakeTargetInfo $BuildDirectory $Target $BuildConfiguration
$Executable = $CMakeTargetInfo.Executable
$CMakeArtifacts = @($CMakeTargetInfo.Artifacts)
$BinaryDirectory = Split-Path $Executable -Parent

$ReadElf = Get-CacheValue $CachePath 'CMAKE_READELF'
if ([string]::IsNullOrWhiteSpace($ReadElf) -or -not (Test-Path -LiteralPath $ReadElf -PathType Leaf)) {
    $readElfCandidates = @(
        Get-ChildItem -LiteralPath (Join-Path $NdkRoot 'toolchains/llvm/prebuilt') -Filter 'llvm-readelf*' -File -Recurse |
            Where-Object { $_.Name -in @('llvm-readelf', 'llvm-readelf.exe') }
    )
    if ($readElfCandidates.Count -ne 1) {
        throw "Expected one llvm-readelf below the preset NDK '$NdkRoot', found $($readElfCandidates.Count)."
    }
    $ReadElf = $readElfCandidates[0].FullName
}

$ToolchainBin = Split-Path $ReadElf -Parent
$ToolchainRoot = Split-Path $ToolchainBin -Parent

$header = (Invoke-NativeCapture $ReadElf @('-h', $Executable)).Output
$Architecture = switch -Regex ($header) {
    'Machine:\s+AArch64' { 'arm64-v8a'; break }
    'Machine:\s+ARM' { 'armeabi-v7a'; break }
    'Machine:\s+Advanced Micro Devices X86-64' { 'x86_64'; break }
    'Machine:\s+Intel 80386' { 'x86'; break }
    default { throw "Unsupported ELF machine in $Executable" }
}

$ArchDetails = @{
    'arm64-v8a' = @{ DeviceAbi = 'arm64-v8a'; RuntimeArch = 'aarch64'; Triple = 'aarch64-linux-android' }
    'armeabi-v7a' = @{ DeviceAbi = 'armeabi-v7a'; RuntimeArch = 'arm'; Triple = 'arm-linux-androideabi' }
    'x86_64' = @{ DeviceAbi = 'x86_64'; RuntimeArch = 'x86_64'; Triple = 'x86_64-linux-android' }
    'x86' = @{ DeviceAbi = 'x86'; RuntimeArch = 'i386'; Triple = 'i686-linux-android' }
}[$Architecture]

function Find-Adb {
    $candidates = @(
        (Get-CommandPath 'adb'),
        $(if ($env:ANDROID_SDK_ROOT) { Join-Path $env:ANDROID_SDK_ROOT 'platform-tools/adb.exe' }),
        $(if ($env:ANDROID_HOME) { Join-Path $env:ANDROID_HOME 'platform-tools/adb.exe' }),
        (Join-Path (Split-Path (Split-Path $NdkRoot -Parent) -Parent) 'platform-tools/adb.exe')
    ) | Where-Object { -not [string]::IsNullOrWhiteSpace($_) }

    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            return [IO.Path]::GetFullPath($candidate)
        }
    }
    throw 'adb was not found on PATH or beside the configured Android NDK.'
}

$Adb = Find-Adb

function Get-AdbArguments {
    param([string[]]$ArgumentList)

    if ([string]::IsNullOrWhiteSpace($script:Serial)) {
        return $ArgumentList
    }
    return @('-s', $script:Serial) + $ArgumentList
}

function Invoke-AdbCommand {
    param([Parameter(Mandatory)][string[]]$ArgumentList)
    Invoke-NativeCommand $Adb (Get-AdbArguments $ArgumentList)
}

function Invoke-AdbCapture {
    param(
        [Parameter(Mandatory)][string[]]$ArgumentList,
        [switch]$AllowFailure
    )
    return Invoke-NativeCapture $Adb (Get-AdbArguments $ArgumentList) -AllowFailure:$AllowFailure
}

$deviceList = (Invoke-NativeCapture $Adb @('devices')).Output -split "`r?`n"
$onlineDevices = @(
    foreach ($line in $deviceList) {
        if ($line -match '^(\S+)\s+device(?:\s|$)') {
            $Matches[1]
        }
    }
)

if ([string]::IsNullOrWhiteSpace($Serial)) {
    if ($onlineDevices.Count -eq 0) {
        throw 'No online ADB device was found. Reconnect the paired WiFi device and run adb devices.'
    }
    if ($onlineDevices.Count -gt 1) {
        throw "More than one online ADB device was found. Pass -Serial. Devices: $($onlineDevices -join ', ')"
    }
    $script:Serial = $onlineDevices[0]
} else {
    $script:Serial = $Serial
    $state = Invoke-AdbCapture @('get-state') -AllowFailure
    if ($state.ExitCode -ne 0 -or $state.Output.Trim() -ne 'device') {
        throw "ADB device '$Serial' is not online: $($state.Output)"
    }
}

$DeviceAbi = (Invoke-AdbCapture @('shell', 'getprop ro.product.cpu.abi')).Output.Trim()
if ($DeviceAbi -ne $ArchDetails.DeviceAbi) {
    throw "ELF ABI '$Architecture' does not match device ABI '$DeviceAbi'."
}

$SystemLibraries = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
@(
    'libandroid.so', 'libatomic.so', 'libc.so', 'libcamera2ndk.so', 'libdl.so',
    'libjnigraphics.so', 'liblog.so', 'libm.so', 'libmediandk.so',
    'libnativewindow.so', 'libOpenSLES.so',
    'libstdc++.so', 'libunwind.so', 'libvulkan.so', 'libz.so'
) | ForEach-Object { [void]$SystemLibraries.Add($_) }

function Get-ElfDependencies {
    param([Parameter(Mandatory)][string]$Path)

    $dynamic = (Invoke-NativeCapture $ReadElf @('-d', $Path)).Output
    return @(
        foreach ($line in $dynamic -split "`r?`n") {
            if ($line -match '\(NEEDED\).*Shared library: \[([^]]+)\]') {
                $Matches[1]
            }
        }
    )
}

$VcpkgInstalled = Get-CacheValue $CachePath 'VCPKG_INSTALLED_DIR'
$VcpkgTriplet = Get-CacheValue $CachePath 'VCPKG_TARGET_TRIPLET'
$LibrarySearchDirectories = @(
    foreach ($directory in $LibraryDirectory) {
        if (-not (Test-Path -LiteralPath $directory -PathType Container)) {
            throw "Additional library directory does not exist: $directory"
        }
        (Resolve-Path -LiteralPath $directory).Path
    }
)

function Test-MatchingElf {
    param([Parameter(Mandatory)][string]$Path)

    $result = Invoke-NativeCapture $ReadElf @('-h', $Path) -AllowFailure
    if ($result.ExitCode -ne 0) {
        return $false
    }
    $matchesArchitecture = switch ($Architecture) {
        'arm64-v8a' { $result.Output -match 'Machine:\s+AArch64' }
        'armeabi-v7a' { $result.Output -match 'Machine:\s+ARM' }
        'x86_64' { $result.Output -match 'Machine:\s+Advanced Micro Devices X86-64' }
        'x86' { $result.Output -match 'Machine:\s+Intel 80386' }
    }
    return [bool]$matchesArchitecture
}

function Find-SharedLibrary {
    param([Parameter(Mandatory)][string]$Name)

    $codemodelCandidates = @(
        foreach ($candidate in $CMakeArtifacts) {
            if ((Split-Path $candidate -Leaf) -eq $Name -and (Test-MatchingElf $candidate)) {
                $candidate
            }
        }
    )
    if ($codemodelCandidates.Count -gt 1) {
        throw "CMake codemodel contains multiple '$Name' artifacts: $($codemodelCandidates -join ', ')"
    }
    if ($codemodelCandidates.Count -eq 1) {
        return $codemodelCandidates[0]
    }

    $directCandidates = [Collections.Generic.List[string]]::new()
    $directCandidates.Add((Join-Path $BinaryDirectory $Name))
    foreach ($directory in $LibrarySearchDirectories) {
        $directCandidates.Add((Join-Path $directory $Name))
    }
    if ($VcpkgInstalled -and $VcpkgTriplet) {
        $directCandidates.Add((Join-Path $VcpkgInstalled "$VcpkgTriplet/bin/$Name"))
        $directCandidates.Add((Join-Path $VcpkgInstalled "$VcpkgTriplet/lib/$Name"))
    }
    $directCandidates.Add((Join-Path $ToolchainRoot "sysroot/usr/lib/$($ArchDetails.Triple)/$Name"))

    foreach ($candidate in $directCandidates) {
        if ((Test-Path -LiteralPath $candidate -PathType Leaf) -and (Test-MatchingElf $candidate)) {
            return [IO.Path]::GetFullPath($candidate)
        }
    }

    $clangRuntimeRoot = Join-Path $ToolchainRoot 'lib/clang'
    if (Test-Path -LiteralPath $clangRuntimeRoot -PathType Container) {
        foreach ($candidate in Get-ChildItem -LiteralPath $clangRuntimeRoot -Filter $Name -File -Recurse) {
            if (Test-MatchingElf $candidate.FullName) {
                return $candidate.FullName
            }
        }
    }
    return $null
}

$DeployFiles = [Collections.Generic.Dictionary[string, string]]::new([StringComparer]::OrdinalIgnoreCase)
$DependencyQueue = [Collections.Generic.Queue[string]]::new()
$ScannedFiles = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)

function Add-DeployFile {
    param([Parameter(Mandatory)][string]$Path)

    $resolved = (Resolve-Path -LiteralPath $Path).Path
    $name = Split-Path $resolved -Leaf
    if ($DeployFiles.ContainsKey($name) -and $DeployFiles[$name] -ne $resolved) {
        throw "Two deployment files have the same name '$name': '$resolved' and '$($DeployFiles[$name])'"
    }
    $DeployFiles[$name] = $resolved
    $DependencyQueue.Enqueue($resolved)
}

Add-DeployFile $Executable
foreach ($path in $Library) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Additional library does not exist: $path"
    }
    Add-DeployFile $path
}

while ($DependencyQueue.Count -gt 0) {
    $current = $DependencyQueue.Dequeue()
    if (-not $ScannedFiles.Add($current)) {
        continue
    }

    foreach ($needed in Get-ElfDependencies $current) {
        if ($SystemLibraries.Contains($needed) -or $DeployFiles.ContainsKey($needed)) {
            continue
        }
        $dependency = Find-SharedLibrary $needed
        if ($null -eq $dependency) {
            throw "Cannot resolve non-system dependency '$needed' required by '$current'. Pass -Library or -LibraryDirectory."
        }
        Add-DeployFile $dependency
    }
}

Write-Host "Device: $($script:Serial) ($DeviceAbi)"
Write-Host "Target: $Executable"
Write-Host "Remote: $RemoteDirectory"
Write-Host "Deploying $($DeployFiles.Count) ELF file(s)"

Invoke-AdbCommand @('shell', "mkdir -p '$RemoteDirectory'")
foreach ($entry in $DeployFiles.GetEnumerator() | Sort-Object Key) {
    Invoke-AdbCommand @('push', $entry.Value, "$RemoteDirectory/$($entry.Key)")
}
Invoke-AdbCommand @('shell', "chmod 755 '$RemoteDirectory/$Target'")

foreach ($path in $Data) {
    if (-not (Test-Path -LiteralPath $path)) {
        throw "Test data path does not exist: $path"
    }
    Invoke-AdbCommand @('push', (Resolve-Path -LiteralPath $path).Path, "$RemoteDirectory/")
}

function ConvertTo-ShellLiteral {
    param([AllowEmptyString()][string]$Value)
    return "'" + $Value.Replace("'", "'\''") + "'"
}

$RemoteArguments = [Collections.Generic.List[string]]::new()
if (-not [string]::IsNullOrWhiteSpace($Filter)) {
    $RemoteArguments.Add('--filter')
    $RemoteArguments.Add($Filter)
}
if ($Tap) {
    $RemoteArguments.Add('--tap')
}

$LocalJUnit = $null
$RemoteJUnit = $null
if (-not [string]::IsNullOrWhiteSpace($JUnit)) {
    $LocalJUnit = if ([IO.Path]::IsPathRooted($JUnit)) {
        [IO.Path]::GetFullPath($JUnit)
    } else {
        [IO.Path]::GetFullPath((Join-Path (Get-Location) $JUnit))
    }
    $RemoteJUnit = "$RemoteDirectory/$Target.junit.xml"
    $RemoteArguments.Add('--junit')
    $RemoteArguments.Add($RemoteJUnit)
}
foreach ($argument in $TestArgument) {
    $RemoteArguments.Add($argument)
}

$quotedArguments = @($RemoteArguments | ForEach-Object { ConvertTo-ShellLiteral $_ })
$RemoteExecutable = "$RemoteDirectory/$Target"
$RemoteCommand = "cd $(ConvertTo-ShellLiteral $RemoteDirectory) && export LD_LIBRARY_PATH=$(ConvertTo-ShellLiteral $RemoteDirectory) && exec $(ConvertTo-ShellLiteral $RemoteExecutable)"
if ($quotedArguments.Count -gt 0) {
    $RemoteCommand += ' ' + ($quotedArguments -join ' ')
}

function Pull-JUnitResult {
    if ($null -eq $LocalJUnit) {
        return
    }
    $remoteExists = Invoke-AdbCapture @('shell', "test -f $(ConvertTo-ShellLiteral $RemoteJUnit)") -AllowFailure
    if ($remoteExists.ExitCode -ne 0) {
        Write-Warning "JUnit result was not created on the device: $RemoteJUnit"
        return
    }
    $parent = Split-Path $LocalJUnit -Parent
    if (-not (Test-Path -LiteralPath $parent -PathType Container)) {
        [void](New-Item -ItemType Directory -Path $parent -Force)
    }
    Invoke-AdbCommand @('pull', $RemoteJUnit, $LocalJUnit)
    Write-Host "JUnit: $LocalJUnit"
}

if (-not $Lldb) {
    & $Adb @(Get-AdbArguments @('shell', $RemoteCommand))
    $testExitCode = $LASTEXITCODE
    Pull-JUnitResult
    if ($testExitCode -ne 0) {
        throw "Device test '$Target' failed with exit code $testExitCode."
    }
    Write-Host "PASS: $Target"
    exit 0
}

$LldbExecutable = Join-Path $ToolchainBin 'lldb.cmd'
if (-not (Test-Path -LiteralPath $LldbExecutable -PathType Leaf)) {
    $LldbExecutable = Join-Path $ToolchainBin 'lldb.exe'
}
if (-not (Test-Path -LiteralPath $LldbExecutable -PathType Leaf)) {
    $LldbExecutable = Join-Path $ToolchainBin 'lldb'
}
if (-not (Test-Path -LiteralPath $LldbExecutable -PathType Leaf)) {
    throw "NDK LLDB was not found in $ToolchainBin"
}

$LldbServer = Get-ChildItem -LiteralPath (Join-Path $ToolchainRoot 'lib/clang') `
    -Filter 'lldb-server' -File -Recurse |
    Where-Object { $_.FullName -match "[\\/]linux[\\/]$([regex]::Escape($ArchDetails.RuntimeArch))[\\/]lldb-server$" } |
    Sort-Object FullName -Descending |
    Select-Object -First 1
if ($null -eq $LldbServer) {
    throw "NDK lldb-server for '$Architecture' was not found under $ToolchainRoot"
}

$RemoteLldbServer = "$RemoteDirectory/lldb-server"
$RemoteLldbPidFile = "$RemoteDirectory/lldb-server-$Port.pid"
$stopRemoteLldbTemplate = @'
if [ -f __PID_FILE__ ]; then server_pid=$(cat __PID_FILE__); if [ "$(readlink /proc/$server_pid/exe 2>/dev/null)" = __SERVER__ ]; then kill $server_pid 2>/dev/null || true; fi; rm -f __PID_FILE__; fi
'@
$StopRemoteLldbCommand = $stopRemoteLldbTemplate.Replace(
    '__PID_FILE__', (ConvertTo-ShellLiteral $RemoteLldbPidFile)).Replace(
    '__SERVER__', (ConvertTo-ShellLiteral $RemoteLldbServer)).Trim()
Invoke-AdbCommand @('push', $LldbServer.FullName, $RemoteLldbServer)
Invoke-AdbCommand @('shell', "chmod 755 $(ConvertTo-ShellLiteral $RemoteLldbServer)")
Invoke-AdbCommand @('shell', $StopRemoteLldbCommand)
Invoke-AdbCommand @('forward', "tcp:$Port", "tcp:$Port")

$ServerCommand = "cd $(ConvertTo-ShellLiteral $RemoteDirectory) && export LD_LIBRARY_PATH=$(ConvertTo-ShellLiteral $RemoteDirectory) && echo `$`$ > $(ConvertTo-ShellLiteral $RemoteLldbPidFile) && exec $(ConvertTo-ShellLiteral $RemoteLldbServer) gdbserver 127.0.0.1:$Port -- $(ConvertTo-ShellLiteral $RemoteExecutable)"
if ($quotedArguments.Count -gt 0) {
    $ServerCommand += ' ' + ($quotedArguments -join ' ')
}

$serverProcess = $null
$commandFile = $null
try {
    $startInfo = [Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $Adb
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    foreach ($argument in Get-AdbArguments @('shell', $ServerCommand)) {
        [void]$startInfo.ArgumentList.Add($argument)
    }
    $serverProcess = [Diagnostics.Process]::Start($startInfo)

    Start-Sleep -Milliseconds 500
    if ($serverProcess.HasExited) {
        throw "lldb-server exited before LLDB connected (exit code $($serverProcess.ExitCode))."
    }

    $localExecutable = $Executable.Replace('\', '/')
    $localBinaryDirectory = $BinaryDirectory.Replace('\', '/')
    $lldbCommands = @(
        "settings append target.exec-search-paths `"$localBinaryDirectory`"",
        "target create `"$localExecutable`"",
        "target modules search-paths add `"$RemoteDirectory`" `"$localBinaryDirectory`"",
        "gdb-remote 127.0.0.1:$Port"
    ) + $LldbCommand
    $commandFile = New-TemporaryFile
    Set-Content -LiteralPath $commandFile -Value $lldbCommands -Encoding utf8NoBOM

    Write-Host "Starting LLDB on localhost:$Port"
    & $LldbExecutable '--source' $commandFile
    $lldbExitCode = $LASTEXITCODE
    Pull-JUnitResult
    if ($lldbExitCode -ne 0) {
        throw "LLDB exited with code $lldbExitCode."
    }
} finally {
    if ($null -ne $commandFile) {
        Remove-Item -LiteralPath $commandFile -Force -ErrorAction SilentlyContinue
    }
    & $Adb @(Get-AdbArguments @('forward', '--remove', "tcp:$Port")) 2>$null
    if ($null -ne $serverProcess -and -not $serverProcess.HasExited) {
        $serverProcess.Kill($true)
    }
    & $Adb @(Get-AdbArguments @('shell', $StopRemoteLldbCommand)) 2>$null
    if ($null -ne $serverProcess) {
        $serverProcess.Dispose()
    }
}
