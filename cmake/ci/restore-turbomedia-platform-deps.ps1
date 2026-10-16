param(
  [Parameter(Mandatory = $true)]
  [ValidateSet("linux-x64", "windows-x64", "android-arm64-v8a", "macos-arm64")]
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

# Exact historical #197 RED/GREEN consumer graph. Both are immutable
# published release tags, not mutable 'latest' RC selections. The matching
# public nupkg byte digests are verified immediately after restore below.
# This is diagnostic branch scope only; do not switch master or add fallback.
$saltsVersion = "2.3.0-rc.15"
$saltsUtilsVersion = "4.3.0-rc.10"
# RC sequences belong to each package, not to a shared release counter.
# For example, Salts 2.3.0-rc.10 and SaltsUtils 4.3.0-rc.7 are the current
# official releases. Keep both post-cutover; installed package contracts and
# the native build/CTest graph qualify their actual compatibility.
foreach ($version in @($saltsVersion, $saltsUtilsVersion)) {
  if ($version -match '-rc\.([1-9][0-9]*)$' -and [int]$Matches[1] -lt 2) {
    throw "Unicode owner cutover requires rc.2+ SDK releases: $version"
  }
}
Write-Host "Official SDKs: Salts.Native $saltsVersion, SaltsUtils.Native $saltsUtilsVersion"

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
    <PackageReference Include="Salts.Native" Version="$saltsVersion" />
    <PackageReference Include="SaltsUtils.Native" Version="$saltsUtilsVersion" />
    <PackageReference Include="SaltsNet.Native" Version="*" />
    <PackageReference Include="CHttp.Native" Version="*" />
$desktopReference
  </ItemGroup>
</Project>
"@ | Set-Content -LiteralPath $project -Encoding utf8

dotnet restore $project --configfile $config --packages $packages --no-cache --force-evaluate
if ($LASTEXITCODE -ne 0) { throw "platform SDK restore failed" }
# Verify restored bytes, not only versions. Do not allow NuGet or a moving
# tag to change the exact historical ABI basis of this test candidate.
$expectedDigest = @{
  "Salts.Native" = "fa73229c7b2506b393fe779bb56f8d42c387a79d30db54c0cf17b408b7ba7ad1"
  "SaltsUtils.Native" = "b4e5207b9e1f49fab6bddfcd36a4f27d9e99cabe09c28b41913c26f20e366491"
}
foreach ($entry in @(@("Salts.Native", $saltsVersion), @("SaltsUtils.Native", $saltsUtilsVersion))) {
  $id = [string]$entry[0]
  $version = [string]$entry[1]
  $name = "$($id.ToLowerInvariant()).$version.nupkg"
  $file = Join-Path (Join-Path (Join-Path $packages $id.ToLowerInvariant()) $version) $name
  if (-not (Test-Path -LiteralPath $file -PathType Leaf)) {
    throw "exact historical nupkg not restored: $file"
  }
  $digest = (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant()
  if ($digest -cne $expectedDigest[$id]) {
    throw "historical ABI nupkg checksum mismatch: $id $version expected=$($expectedDigest[$id]) actual=$digest"
  }
  Write-Host "Attested #232 historical SDK: $id $version sha256=$digest"
}

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
  $expectedVersion = switch ($PackageId) {
    "Salts.Native" { $saltsVersion }
    "SaltsUtils.Native" { $saltsUtilsVersion }
    default { $null }
  }
  if ($expectedVersion -and $versions[0].Name -cne $expectedVersion) {
    throw "restored unexpected $PackageId version $($versions[0].Name) (expected $expectedVersion)"
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

# Fail before CMake configuration if published SDK target ownership is wrong.
$saltsTargets = Join-Path $roots.SALTS_ROOT "lib/cmake/Salts/SaltsTargets.cmake"
$utilsTargets = Join-Path $roots.SALTS_UTILS_ROOT "lib/cmake/SaltsUtils/SaltsUtilsTargets.cmake"
if (-not (Test-Path -LiteralPath $saltsTargets -PathType Leaf) -or
    -not (Test-Path -LiteralPath $utilsTargets -PathType Leaf)) {
  throw "paired SDK target exports are missing"
}
if (-not (Select-String -LiteralPath $saltsTargets -SimpleMatch "add_library(Salts::Unicode" -Quiet)) {
  throw "Salts.Native must own Salts::Unicode in its installed CMake export"
}
if (Select-String -LiteralPath $utilsTargets -SimpleMatch "add_library(Salts::Unicode" -Quiet) {
  throw "SaltsUtils.Native cannot duplicate Salts::Unicode"
}
Write-Host "Verified single Salts::Unicode owner in paired published SDK exports"

$saltsUtilsHostRoot = $null
if ($Rid -eq "android-arm64-v8a") {
  $saltsUtilsHostRoot = Resolve-SdkRoot "SaltsUtils.Native" "linux-x64"
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
