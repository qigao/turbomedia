<#
.SYNOPSIS
Builds and runs the interactive TurboMedia MediaProjection frame test.

.DESCRIPTION
Builds a small APK without requiring Gradle, installs it on a connected Android
device, and waits for the app to receive and convert at least one screen frame.
The Android system consent dialog must be accepted on the device.
#>
[CmdletBinding()]
param(
    [string]$Preset = 'android-arm64-v8a-release-win',
    [string]$Serial,
    [string]$BuildDirectory = 'build/android-arm64-v8a-release',
    [ValidateRange(10, 300)]
    [int]$TimeoutSeconds = 90,
    [switch]$NoBuild,
    [switch]$KeepInstalled
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$buildRoot = [IO.Path]::GetFullPath((Join-Path $repoRoot $BuildDirectory))
$workRoot = [IO.Path]::GetFullPath((Join-Path $buildRoot 'screen-capture-apk'))
$repoPrefix = $repoRoot.TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
if (-not $workRoot.StartsWith($repoPrefix, [StringComparison]::OrdinalIgnoreCase)) {
    throw "Refusing to prepare APK outside the repository: $workRoot"
}

if (Test-Path -LiteralPath $workRoot) {
    Remove-Item -LiteralPath $workRoot -Recurse -Force
}
New-Item -ItemType Directory -Path $workRoot | Out-Null

$sdkRoot = if ($env:ANDROID_SDK_ROOT) {
    $env:ANDROID_SDK_ROOT
} elseif ($env:ANDROID_HOME) {
    $env:ANDROID_HOME
} else {
    Join-Path $env:LOCALAPPDATA 'Android/Sdk'
}
$sdkRoot = [IO.Path]::GetFullPath($sdkRoot)
if (-not (Test-Path -LiteralPath $sdkRoot -PathType Container)) {
    throw "Android SDK not found: $sdkRoot"
}

$buildTools = Get-ChildItem -LiteralPath (Join-Path $sdkRoot 'build-tools') -Directory |
    Where-Object { $_.Name -match '^\d+\.\d+\.\d+$' } |
    Sort-Object { [version]$_.Name } -Descending |
    Select-Object -First 1
if (-not $buildTools) {
    throw "No stable Android build-tools installation found under $sdkRoot"
}

$platform = Get-ChildItem -LiteralPath (Join-Path $sdkRoot 'platforms') -Directory |
    Where-Object { $_.Name -match '^android-(\d+)$' } |
    Sort-Object { [int]($_.Name -replace '^android-', '') } -Descending |
    Select-Object -First 1
if (-not $platform) {
    throw "No Android SDK platform found under $sdkRoot"
}

$androidJar = Join-Path $platform.FullName 'android.jar'
$adb = Join-Path $sdkRoot 'platform-tools/adb.exe'
$aapt2 = Join-Path $buildTools.FullName 'aapt2.exe'
$d8 = Join-Path $buildTools.FullName 'd8.bat'
$zipalign = Join-Path $buildTools.FullName 'zipalign.exe'
$apksigner = Join-Path $buildTools.FullName 'apksigner.bat'
$jar = (Get-Command jar -ErrorAction Stop).Source
$javac = (Get-Command javac -ErrorAction Stop).Source

foreach ($tool in @($androidJar, $adb, $aapt2, $d8, $zipalign, $apksigner, $jar, $javac)) {
    if (-not (Test-Path -LiteralPath $tool -PathType Leaf)) {
        throw "Required Android/Java tool not found: $tool"
    }
}

if (-not $NoBuild) {
    & cmake --build --preset $Preset --target turbo_media_android --parallel
    if ($LASTEXITCODE -ne 0) {
        throw "Android native build failed with exit code $LASTEXITCODE"
    }
}

$screenCaptureJava = Join-Path $repoRoot 'media/mobile/android/java/com/turbonet/media/ScreenCapture.java'
$activityJava = Join-Path $repoRoot 'tests/android/screen_capture/ScreenCaptureTestActivity.java'
$manifest = Join-Path $repoRoot 'tests/android/screen_capture/AndroidManifest.xml'
$androidLibrary = Join-Path $repoRoot 'media/mobile/android/libs/arm64-v8a/libturbo_media_android.so'
$deviceLibrary = Join-Path $buildRoot 'bin/libturbo_media_device.so'
foreach ($inputPath in @($screenCaptureJava, $activityJava, $manifest, $androidLibrary, $deviceLibrary)) {
    if (-not (Test-Path -LiteralPath $inputPath -PathType Leaf)) {
        throw "Screen test input not found: $inputPath"
    }
}

$classesDirectory = Join-Path $workRoot 'classes'
$dexDirectory = Join-Path $workRoot 'dex'
$stageDirectory = Join-Path $workRoot 'stage'
$nativeDirectory = Join-Path $stageDirectory 'lib/arm64-v8a'
New-Item -ItemType Directory -Path $classesDirectory, $dexDirectory, $nativeDirectory | Out-Null

& $javac -encoding UTF-8 -source 8 -target 8 -classpath $androidJar -d $classesDirectory `
    $screenCaptureJava $activityJava
if ($LASTEXITCODE -ne 0) {
    throw "javac failed with exit code $LASTEXITCODE"
}

$classesJar = Join-Path $workRoot 'classes.jar'
& $jar --create --file $classesJar -C $classesDirectory .
if ($LASTEXITCODE -ne 0) {
    throw "jar failed with exit code $LASTEXITCODE"
}

& $d8 --lib $androidJar --min-api 28 --output $dexDirectory $classesJar
if ($LASTEXITCODE -ne 0) {
    throw "d8 failed with exit code $LASTEXITCODE"
}

$unsignedApk = Join-Path $workRoot 'screen-capture-unsigned.apk'
& $aapt2 link -I $androidJar --manifest $manifest --min-sdk-version 28 `
    --target-sdk-version 28 -o $unsignedApk
if ($LASTEXITCODE -ne 0) {
    throw "aapt2 link failed with exit code $LASTEXITCODE"
}

Copy-Item -LiteralPath (Join-Path $dexDirectory 'classes.dex') -Destination $stageDirectory
Copy-Item -LiteralPath $androidLibrary, $deviceLibrary -Destination $nativeDirectory
& $jar --update --file $unsignedApk -C $stageDirectory classes.dex -C $stageDirectory lib
if ($LASTEXITCODE -ne 0) {
    throw "Failed to add DEX/native libraries to APK"
}

$alignedApk = Join-Path $workRoot 'screen-capture-aligned.apk'
$signedApk = Join-Path $workRoot 'screen-capture-test.apk'
& $zipalign -f 4 $unsignedApk $alignedApk
if ($LASTEXITCODE -ne 0) {
    throw "zipalign failed with exit code $LASTEXITCODE"
}

$debugKeystore = Join-Path $HOME '.android/debug.keystore'
if (-not (Test-Path -LiteralPath $debugKeystore -PathType Leaf)) {
    throw "Android debug keystore not found: $debugKeystore"
}
& $apksigner sign --ks $debugKeystore --ks-pass pass:android --key-pass pass:android `
    --out $signedApk $alignedApk
if ($LASTEXITCODE -ne 0) {
    throw "apksigner failed with exit code $LASTEXITCODE"
}

if (-not $Serial) {
    $deviceLines = @(& $adb devices | Select-Object -Skip 1 |
        Where-Object { $_ -match "\tdevice$" })
    if ($deviceLines.Count -ne 1) {
        throw "Expected exactly one connected Android device; pass -Serial explicitly"
    }
    $Serial = ($deviceLines[0] -split "\t")[0]
}

$packageName = 'com.turbonet.media.test'
$activityName = "$packageName/com.turbonet.media.ScreenCaptureTestActivity"
try {
    & $adb -s $Serial install -r $signedApk
    if ($LASTEXITCODE -ne 0) {
        throw "APK installation failed with exit code $LASTEXITCODE"
    }

    & $adb -s $Serial logcat -c
    & $adb -s $Serial shell am force-stop $packageName
    & $adb -s $Serial shell am start -W -n $activityName
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to launch screen-capture test activity"
    }

    Write-Host 'Approve the system screen-capture dialog on the device; waiting for a native I420 frame.'
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    $passed = $false
    while ([DateTime]::UtcNow -lt $deadline) {
        $log = (& $adb -s $Serial logcat -d -s 'TurboMediaScreenTest:I' '*:S' 2>$null) -join "`n"
        if ($log -match 'PASS frames=\d+') {
            $result = [regex]::Match($log, 'PASS frames=\d+').Value
            Write-Host "PASS: Android MediaProjection screen capture ($result)"
            Write-Host "APK: $signedApk"
            $passed = $true
            break
        }
        if ($log -match 'FAIL [^\r\n]+') {
            throw ([regex]::Match($log, 'FAIL [^\r\n]+').Value)
        }
        Start-Sleep -Milliseconds 500
    }

    if (-not $passed) {
        throw "Timed out after $TimeoutSeconds seconds waiting for MediaProjection test result"
    }
} finally {
    if (-not $KeepInstalled) {
        & $adb -s $Serial uninstall $packageName 2>$null | Out-Null
    }
}
