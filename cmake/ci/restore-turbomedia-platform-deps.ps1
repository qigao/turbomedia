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
$restoreRoot = Join-Path $env:RUNNER_TEMP "turbomedia-native-sdk-restore"
New-Item -ItemType Directory -Path $restoreRoot -Force | Out-Null
$project = Join-Path $restoreRoot "turbomedia-platform-sdk.csproj"
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
    <PackageReference Include="Salts.Native" Version="2.3.0-*" />
    <PackageReference Include="SaltsUtils.Native" Version="4.3.0-*" />
    <!-- Qualified CI selects a published stable or numeric RC without
         hardcoding a permanent version or admitting rc.sha snapshots. -->
    <PackageReference Update="Salts.Native" Version="`$(SaltsNativeQualifiedVersion)"
                      Condition="'`$(SaltsNativeQualifiedVersion)' != ''" />
    <PackageReference Update="SaltsUtils.Native" Version="`$(SaltsUtilsNativeQualifiedVersion)"
                      Condition="'`$(SaltsUtilsNativeQualifiedVersion)' != ''" />
    <PackageReference Include="SaltsNet.Native" Version="*" />
    <PackageReference Include="CHttp.Native" Version="*" />
$desktopReference
  </ItemGroup>
</Project>
"@ | Set-Content -LiteralPath $project -Encoding utf8

# NuGet's prerelease sort can incorrectly prefer an ephemeral Linux-only
# 2.3.0-rc.sha... over an immutable public 2.3.0-rc.N. Keep the repository's
# floating PackageReference, but qualify the highest officially tagged RC (or
# stable) from GitHub Releases and verify its exact restored identity.
function Get-OfficialNativeVersion([string]$repository, [string]$line,
                                   [string]$packageName) {
  $url = "https://api.github.com/repos/qigao/$repository/releases?per_page=100"
  $headers = @{
    Accept = "application/vnd.github+json"
    Authorization = "Bearer $env:GITHUB_TOKEN"
    "User-Agent" = "turbomedia-native-sdk-qualification"
  }
  $releases = Invoke-RestMethod -Method Get -Uri $url -Headers $headers
  $rcPattern = "^v" + [regex]::Escape($line) + "-rc\.([0-9]+)$"
  $choices = @(
    foreach ($release in $releases) {
      if ($release.draft) { continue }
      $tag = [string]$release.tag_name
      if ($tag -eq "v$line" -and -not $release.prerelease) {
        $priority = [int]::MaxValue
      } elseif ($release.prerelease -and $tag -match $rcPattern) {
        $priority = [int]$Matches[1]
      } else {
        continue
      }
      [pscustomobject]@{
        Priority = $priority
        Version = $tag.Substring(1)
        Release = $release
      }
    }
  )
  if ($choices.Count -eq 0) {
    throw "No published stable or numeric RC native release for $line in qigao/$repository"
  }
  $selected = $choices | Sort-Object Priority -Descending | Select-Object -First 1
  $asset = "$packageName.$($selected.Version).nupkg"
  if (@($selected.Release.assets | Where-Object { $_.name -eq $asset }).Count -ne 1) {
    throw "Official native release $($selected.Release.tag_name) lacks asset $asset"
  }
  return $selected.Version
}

$saltsExpected = Get-OfficialNativeVersion "salts" "2.3.0" "Salts.Native"
$saltsUtilsExpected = Get-OfficialNativeVersion "salts-utils" "4.3.0" "SaltsUtils.Native"
$properties = @(
  "-p:SaltsNativeQualifiedVersion=$saltsExpected",
  "-p:SaltsUtilsNativeQualifiedVersion=$saltsUtilsExpected"
)
Write-Host "Qualifying native SDK versions Salts.Native $saltsExpected / SaltsUtils.Native $saltsUtilsExpected"

dotnet restore $project --configfile $config --packages $packages --no-cache --force-evaluate @properties
if ($LASTEXITCODE -ne 0) { throw "platform SDK restore failed" }

$assetsPath = Join-Path $restoreRoot "obj/project.assets.json"
if (-not (Test-Path -LiteralPath $assetsPath -PathType Leaf)) {
  throw "NuGet resolved asset graph is missing: $assetsPath"
}
$assets = Get-Content -LiteralPath $assetsPath -Raw | ConvertFrom-Json -AsHashtable
function Check-ResolvedPackage([string]$name, [string]$expected) {
  $matched = @($assets.libraries.Keys | Where-Object {
    $_.StartsWith("$name/", [StringComparison]::OrdinalIgnoreCase)
  })
  if ($matched.Count -ne 1) { throw "Expected exactly one resolved $name package" }
  $version = $matched[0].Substring($name.Length + 1)
  if ($version -ne $expected) {
    throw "$name restored $version, expected qualified official version $expected"
  }
}
Check-ResolvedPackage "Salts.Native" $saltsExpected
Check-ResolvedPackage "SaltsUtils.Native" $saltsUtilsExpected
"SALTS_SDK_RESOLVED_VERSION=$saltsExpected" |
  Out-File -FilePath $env:GITHUB_ENV -Encoding utf8 -Append
"SALTS_UTILS_SDK_RESOLVED_VERSION=$saltsUtilsExpected" |
  Out-File -FilePath $env:GITHUB_ENV -Encoding utf8 -Append

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
