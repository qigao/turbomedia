param(
  [Parameter(Mandatory = $true)]
  [ValidateSet("linux-x64", "windows-x64", "android-arm64-v8a", "macos-arm64", "ios-arm64", "ios-simulator-arm64")]
  [string]$Rid
)

$ErrorActionPreference = "Stop"

foreach ($required in @("GITHUB_TOKEN", "RUNNER_TEMP", "GITHUB_ENV", "GITHUB_PATH")) {
  if ([string]::IsNullOrWhiteSpace((Get-Item "Env:$required").Value)) {
    throw "$required is required"
  }
}

$config = Join-Path $env:RUNNER_TEMP "turbomedia-platform-sdk.config"
$project = Join-Path $env:RUNNER_TEMP "turbomedia-platform-sdk.csproj"
$packages = Join-Path $env:RUNNER_TEMP "turbomedia-platform-sdk-packages"
if (Test-Path -LiteralPath $packages) {
  Remove-Item -LiteralPath $packages -Recurse -Force
}

'<configuration><packageSources><clear /></packageSources></configuration>' |
  Set-Content -LiteralPath $config -Encoding utf8

dotnet nuget add source https://nuget.pkg.github.com/qigao/index.json `
  --name github `
  --username qigao `
  --password $env:GITHUB_TOKEN `
  --store-password-in-clear-text `
  --configfile $config
if ($LASTEXITCODE -ne 0) { throw "failed to configure GitHub Packages source" }

$desktopServices = $Rid -eq "linux-x64" -or $Rid -eq "windows-x64"
$desktopReference = if ($desktopServices) {
@'
    <PackageReference Include="RulesForge.Native" Version="*" />
'@
} else {
  ""
}

@"
<Project Sdk="Microsoft.NET.Sdk">
  <PropertyGroup>
    <TargetFramework>net8.0</TargetFramework>
  </PropertyGroup>
  <ItemGroup>
    <PackageReference Include="Salts.Native" Version="*" />
    <PackageReference Include="SaltsUtils.Native" Version="*" />
    <PackageReference Include="SaltsNet.Native" Version="*" />
    <PackageReference Include="CHttp.Native" Version="*" />
$desktopReference
  </ItemGroup>
</Project>
"@ | Set-Content -LiteralPath $project -Encoding utf8

dotnet restore $project --configfile $config --packages $packages
if ($LASTEXITCODE -ne 0) { throw "platform SDK restore failed" }

function Resolve-SdkRoot([string]$PackageId, [string]$SdkRid = "") {
  if ([string]::IsNullOrWhiteSpace($SdkRid)) {
    $SdkRid = $Rid
  }
  $packageRoot = Join-Path $packages $PackageId.ToLowerInvariant()
  if (-not (Test-Path -LiteralPath $packageRoot -PathType Container)) {
    throw "restored package directory is missing: $packageRoot"
  }
  $versions = @(Get-ChildItem -LiteralPath $packageRoot -Directory)
  if ($versions.Count -ne 1) {
    throw "expected exactly one restored version for $PackageId, found $($versions.Count)"
  }
  $root = Join-Path $versions[0].FullName "sdk/$SdkRid"
  if (-not (Test-Path -LiteralPath $root -PathType Container)) {
    throw "RID SDK is missing for ${PackageId}: $root"
  }
  return $root.Replace('\', '/')
}

$roots = [ordered]@{
  SALTS_ROOT = Resolve-SdkRoot "Salts.Native"
  SALTS_UTILS_ROOT = Resolve-SdkRoot "SaltsUtils.Native"
  SALTSNET_ROOT = Resolve-SdkRoot "SaltsNet.Native"
  CHTTP_ROOT = Resolve-SdkRoot "CHttp.Native"
}
if ($desktopServices) {
  $roots["RULES_FORGE_ROOT"] = Resolve-SdkRoot "RulesForge.Native"
}

$saltsUtilsHostRoot = $null
if ($Rid -eq "android-arm64-v8a") {
  $saltsUtilsHostRoot = Resolve-SdkRoot "SaltsUtils.Native" "linux-x64"
} elseif ($Rid -eq "ios-arm64" -or $Rid -eq "ios-simulator-arm64") {
  $saltsUtilsHostRoot = Resolve-SdkRoot "SaltsUtils.Native" "macos-arm64"
}
if ($saltsUtilsHostRoot) {
  "SALTS_UTILS_HOST_ROOT=$saltsUtilsHostRoot" |
    Out-File -FilePath $env:GITHUB_ENV -Encoding utf8 -Append
}

foreach ($pair in $roots.GetEnumerator()) {
  "$($pair.Key)=$($pair.Value)" |
    Out-File -FilePath $env:GITHUB_ENV -Encoding utf8 -Append
}

$requiredFiles = @(
  (Join-Path $roots.SALTS_ROOT "lib/cmake/Salts/SaltsConfig.cmake"),
  (Join-Path $roots.SALTS_UTILS_ROOT "lib/cmake/SaltsUtils/SaltsUtilsConfig.cmake"),
  (Join-Path $roots.SALTSNET_ROOT "lib/cmake/SaltsNet/SaltsNetConfig.cmake"),
  (Join-Path $roots.CHTTP_ROOT "lib/cmake/Chttp/ChttpConfig.cmake"),
  (Join-Path $roots.SALTS_UTILS_ROOT "include/salts_capture.h"),
  (Join-Path $roots.SALTS_UTILS_ROOT "include/salts_playback.h")
)
if ($desktopServices) {
  $requiredFiles +=
    (Join-Path $roots.RULES_FORGE_ROOT "lib/cmake/RulesForge/RulesForgeConfig.cmake")
}

$idlcName = if ($Rid -eq "windows-x64") { "salts-idlc.exe" } else { "salts-idlc" }
$idlcRoot = if ($saltsUtilsHostRoot) { $saltsUtilsHostRoot } else { $roots.SALTS_UTILS_ROOT }
$idlc = Join-Path $idlcRoot "bin/$idlcName"
$requiredFiles += $idlc

foreach ($file in $requiredFiles) {
  if (-not (Test-Path -LiteralPath $file -PathType Leaf)) {
    throw "missing released SDK contract: $file"
  }
}

if ($saltsUtilsHostRoot) {
  (Join-Path $saltsUtilsHostRoot "bin") |
    Out-File -FilePath $env:GITHUB_PATH -Encoding utf8 -Append
} else {
  foreach ($root in $roots.Values) {
    foreach ($subdir in @("bin", "lib")) {
      $candidate = Join-Path $root $subdir
      if (Test-Path -LiteralPath $candidate -PathType Container) {
        $candidate | Out-File -FilePath $env:GITHUB_PATH -Encoding utf8 -Append
      }
    }
  }
}

Write-Host "Resolved TurboMedia platform dependencies for $Rid"
Write-Host "salts-idlc: $idlc"
