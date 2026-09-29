param(
  [Parameter(Mandatory = $true)]
  [ValidateSet("linux-x64", "windows-x64")]
  [string]$Rid,
  [Parameter(Mandatory = $true)]
  [ValidateSet("CLIENT", "SERVER")]
  [string]$Product
)

$ErrorActionPreference = "Stop"

foreach ($required in @("GITHUB_TOKEN", "RUNNER_TEMP", "GITHUB_ENV", "GITHUB_PATH")) {
  if ([string]::IsNullOrWhiteSpace((Get-Item "Env:$required").Value)) {
    throw "$required is required"
  }
}

$config = Join-Path $env:RUNNER_TEMP "turbomedia-native-sdk.config"
$project = Join-Path $env:RUNNER_TEMP "turbomedia-native-sdk.csproj"
$packages = Join-Path $env:RUNNER_TEMP "turbomedia-native-sdk-packages"
if (Test-Path -LiteralPath $packages) {
  Remove-Item -LiteralPath $packages -Recurse -Force
}

'<configuration><packageSources><clear /></packageSources></configuration>' |
  Set-Content -LiteralPath $config -Encoding utf8

dotnet nuget add source https://nuget.pkg.github.com/qigao/index.json --name github --username qigao --password $env:GITHUB_TOKEN --store-password-in-clear-text --configfile $config
if ($LASTEXITCODE -ne 0) { throw "failed to configure GitHub Packages source" }

$serverReferences = if ($Product -eq "SERVER") {
@'
              <PackageReference Include="RulesForge.Native" Version="*" />
              <PackageReference Include="TurboDB.Native" Version="*" />
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
$serverReferences
  </ItemGroup>
</Project>
"@ | Set-Content -LiteralPath $project -Encoding utf8

dotnet restore $project --configfile $config --packages $packages
if ($LASTEXITCODE -ne 0) { throw "native SDK restore failed" }

function Resolve-SdkRoot([string]$PackageId) {
  $packageRoot = Join-Path $packages $PackageId.ToLowerInvariant()
  if (-not (Test-Path -LiteralPath $packageRoot -PathType Container)) {
    throw "restored package directory is missing: $packageRoot"
  }
  $versions = @(Get-ChildItem -LiteralPath $packageRoot -Directory)
  if ($versions.Count -ne 1) {
    throw "expected exactly one restored version for $PackageId, found $($versions.Count)"
  }
  $root = Join-Path $versions[0].FullName "sdk/$Rid"
  if (-not (Test-Path -LiteralPath $root -PathType Container)) {
    throw "RID SDK is missing for $PackageId: $root"
  }
  return $root.Replace('\', '/')
}

$roots = [ordered]@{
  SALTS_ROOT = Resolve-SdkRoot "Salts.Native"
  SALTS_UTILS_ROOT = Resolve-SdkRoot "SaltsUtils.Native"
  SALTSNET_ROOT = Resolve-SdkRoot "SaltsNet.Native"
  CHTTP_ROOT = Resolve-SdkRoot "CHttp.Native"
}
if ($Product -eq "SERVER") {
  $roots["RULES_FORGE_ROOT"] = Resolve-SdkRoot "RulesForge.Native"
  $roots["TURBODB_ROOT"] = Resolve-SdkRoot "TurboDB.Native"
}

foreach ($pair in $roots.GetEnumerator()) {
  "$($pair.Key)=$($pair.Value)" | Out-File -FilePath $env:GITHUB_ENV -Encoding utf8 -Append
}

$requiredFiles = @(
  (Join-Path $roots.SALTS_ROOT "lib/cmake/Salts/SaltsConfig.cmake"),
  (Join-Path $roots.SALTS_UTILS_ROOT "lib/cmake/SaltsUtils/SaltsUtilsConfig.cmake"),
  (Join-Path $roots.SALTSNET_ROOT "lib/cmake/SaltsNet/SaltsNetConfig.cmake"),
  (Join-Path $roots.CHTTP_ROOT "lib/cmake/Chttp/ChttpConfig.cmake")
)

$idlcName = if ($Rid -eq "windows-x64") { "salts-idlc.exe" } else { "salts-idlc" }
$idlc = Join-Path $roots.SALTS_UTILS_ROOT "bin/$idlcName"
$requiredFiles += $idlc

if ($Product -eq "CLIENT") {
  $requiredFiles += @(
    (Join-Path $roots.SALTS_UTILS_ROOT "include/salts_capture.h"),
    (Join-Path $roots.SALTS_UTILS_ROOT "include/salts_playback.h")
  )
} else {
  $requiredFiles += @(
    (Join-Path $roots.RULES_FORGE_ROOT "lib/cmake/RulesForge/RulesForgeConfig.cmake"),
    (Join-Path $roots.TURBODB_ROOT "lib/cmake/Orm/OrmConfig.cmake"),
    (Join-Path $roots.TURBODB_ROOT "include/shared/orm/orm_runtime.h")
  )
}

foreach ($file in $requiredFiles) {
  if (-not (Test-Path -LiteralPath $file -PathType Leaf)) {
    throw "missing released SDK contract: $file"
  }
}

if ($Product -eq "SERVER") {
  $postgresModules = @(Get-ChildItem -Path $roots.TURBODB_ROOT -Recurse -File | Where-Object {
    $_.Name -eq "turbodb_driver_postgresql.dll" -or $_.Name -eq "libturbodb_driver_postgresql.so"
  })
  if ($postgresModules.Count -ne 0) {
    throw "generic TurboDB.Native unexpectedly bundled PostgreSQL Driver"
  }
}

foreach ($root in $roots.Values) {
  foreach ($subdir in @("bin", "lib")) {
    $candidate = Join-Path $root $subdir
    if (Test-Path -LiteralPath $candidate -PathType Container) {
      $candidate | Out-File -FilePath $env:GITHUB_PATH -Encoding utf8 -Append
    }
  }
}

Write-Host "Resolved $Product native SDKs for $Rid"
Write-Host "salts-idlc: $idlc"
