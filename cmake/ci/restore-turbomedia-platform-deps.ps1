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

# M16: enforce the observed release graph, not the latest mutable RC.
# The checked-in lock is review-candidate scoped; updating it needs a new
# exact-SHA CI qualification. No floating versions, fallback paths or retries.
$repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot "../..")).Path
$lockPath = Join-Path $PSScriptRoot "m16-sdk-lock.json"
if (-not (Test-Path -LiteralPath $lockPath -PathType Leaf)) {
  throw "M16 immutable SDK lock is missing: $lockPath"
}
$lock = Get-Content -LiteralPath $lockPath -Raw | ConvertFrom-Json
if ($lock.schema -ne 1) { throw "unsupported M16 SDK lock schema" }
$packageIds = @("Salts.Native", "SaltsUtils.Native", "SaltsNet.Native",
                "CHttp.Native", "RulesForge.Native")
if (@($lock.packages.PSObject.Properties).Count -ne $packageIds.Count) {
  throw "M16 lock package set changed"
}
function Get-LockedPackage([string]$PackageId) {
  $field = $lock.packages.PSObject.Properties[$PackageId]
  if ($null -eq $field) { throw "M16 SDK has no exact pin: $PackageId" }
  $candidate = $field.Value
  if ([string]$candidate.repository -cnotmatch '^qigao/[A-Za-z0-9-]+$' -or
      [string]$candidate.version -cnotmatch '^[0-9]+\.[0-9]+\.[0-9]+-rc\.[1-9][0-9]*$' -or
      [string]$candidate.tag_commit -cnotmatch '^[a-f0-9]{40}$' -or
      [string]$candidate.sha256 -cnotmatch '^[a-f0-9]{64}$') {
    throw "invalid immutable M16 SDK pin: $PackageId"
  }
  return $candidate
}
# Enforce action pins for every M16 workflow, including the ones which do
# not restore NuGet packages. This fails if a new unreviewed Action appears.
$workflowRoot = Join-Path $repositoryRoot ".github/workflows"
$workflowFiles = @(Get-ChildItem -LiteralPath $workflowRoot -Filter "streamgraph-*.yml" -File)
$workflowFiles += Get-Item -LiteralPath (Join-Path $workflowRoot "platform-build.yml")
if ($workflowFiles.Count -lt 11) { throw "M16 workflow audit coverage shrank" }
$actionUses = 0
foreach ($workflow in $workflowFiles) {
  foreach ($line in [System.IO.File]::ReadAllLines($workflow.FullName)) {
    $match = [regex]::Match($line, '^\s*(?:-\s*)?uses:\s*([^@\s]+)@([^\s#]+)')
    if (-not $match.Success) { continue }
    $actionName = $match.Groups[1].Value
    if ($actionName.StartsWith("./")) { continue }
    $actionSha = $match.Groups[2].Value
    $lockedAction = $lock.actions.PSObject.Properties[$actionName]
    if ($null -eq $lockedAction -or $actionSha -cne [string]$lockedAction.Value -or
        $actionSha -cnotmatch '^[a-f0-9]{40}$') {
      throw "unpinned/mismatched M16 Action $actionName@$actionSha in $($workflow.Name)"
    }
    $actionUses++
  }
}
if ($actionUses -lt 20) { throw "M16 immutable Action coverage incomplete" }
$vcpkg = Get-Content -LiteralPath (Join-Path $repositoryRoot "vcpkg.json") -Raw |
  ConvertFrom-Json
if ([string]$vcpkg.'builtin-baseline' -cne [string]$lock.vcpkg_baseline) {
  throw "vcpkg builtin baseline drift from M16 lock"
}
Write-Host "M16 locked Actions verified: $actionUses uses across $($workflowFiles.Count) workflows"
# Canonical vcpkg Action exports the actual bootstrap identities; both must
# match the same frozen toolchain whose source was audited for this M16 RC.
if ([string]$env:VCPKG_SCRIPTS_REVISION -cne [string]$lock.vcpkg_scripts_revision -or
    [string]$env:VCPKG_TOOL_REVISION -cne [string]$lock.vcpkg_tool_version) {
  throw "canonical vcpkg scripts/tool revision drift from M16 lock"
}
Write-Host "M16 canonical vcpkg scripts/toolchain attested: $($lock.vcpkg_scripts_revision), $($lock.vcpkg_tool_version)"


function Read-GitHubJson([string]$Endpoint) {
  $json = (& gh api $Endpoint) -join [Environment]::NewLine
  if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($json)) {
    throw "GitHub release/tag verification failed: $Endpoint"
  }
  return ConvertFrom-Json -InputObject $json
}
foreach ($id in $packageIds) {
  $pin = Get-LockedPackage $id
  $tag = "v$($pin.version)"
  $reference = Read-GitHubJson "repos/$($pin.repository)/git/ref/tags/$tag"
  if ([string]$reference.object.type -cne "commit" -or
      [string]$reference.object.sha -cne [string]$pin.tag_commit) {
    throw "published source tag moved or missing for $id $tag"
  }
  $release = Read-GitHubJson "repos/$($pin.repository)/releases/tags/$tag"
  $name = "$id.$($pin.version).nupkg"
  $asset = @($release.assets | Where-Object { $_.name -ceq $name })
  if ($release.draft -or -not $release.prerelease -or
      [string]$release.tag_name -cne $tag -or $asset.Count -ne 1 -or
      [int64]$asset[0].size -le 0 -or
      [string]$asset[0].digest -cne "sha256:$($pin.sha256)") {
    throw "immutable release asset provenance mismatch: $name"
  }
  Write-Host "Locked release attested: $id $($pin.version) source=$($pin.tag_commit) sha256=$($pin.sha256)"
}
$saltsVersion = (Get-LockedPackage "Salts.Native").version
$saltsUtilsVersion = (Get-LockedPackage "SaltsUtils.Native").version
$saltsNetVersion = (Get-LockedPackage "SaltsNet.Native").version
$chttpVersion = (Get-LockedPackage "CHttp.Native").version
$rulesForgeVersion = if ($Rid -eq "linux-x64" -or $Rid -eq "windows-x64") {
  (Get-LockedPackage "RulesForge.Native").version
} else { $null }

# RC sequences belong to each package, not to a shared release counter.
# For example, Salts 2.3.0-rc.10 and SaltsUtils 4.3.0-rc.7 are the current
# official releases. Keep both post-cutover; installed package contracts and
# the native build/CTest graph qualify their actual compatibility.
foreach ($version in @($saltsVersion, $saltsUtilsVersion)) {
  if ($version -match '-rc\.([1-9][0-9]*)$' -and [int]$Matches[1] -lt 2) {
    throw "Unicode owner cutover requires rc.2+ SDK releases: $version"
  }
}
Write-Host "Official SDK graph: Salts.Native $saltsVersion, SaltsUtils.Native $saltsUtilsVersion, SaltsNet.Native $saltsNetVersion, CHttp.Native $chttpVersion, RulesForge.Native $rulesForgeVersion"

$desktopServices = $Rid -eq "linux-x64" -or $Rid -eq "windows-x64"
$desktopReference = if ($desktopServices) {
@"
    <PackageReference Include="RulesForge.Native" Version="[$rulesForgeVersion]" />
"@
} else {
  ""
}

@"
<Project Sdk="Microsoft.NET.Sdk">
  <PropertyGroup>
    <TargetFramework>net8.0</TargetFramework>
  </PropertyGroup>
  <ItemGroup>
    <PackageReference Include="Salts.Native" Version="[$saltsVersion]" />
    <PackageReference Include="SaltsUtils.Native" Version="[$saltsUtilsVersion]" />
    <PackageReference Include="SaltsNet.Native" Version="[$saltsNetVersion]" />
    <PackageReference Include="CHttp.Native" Version="[$chttpVersion]" />
$desktopReference
  </ItemGroup>
</Project>
"@ | Set-Content -LiteralPath $project -Encoding utf8

dotnet restore $project --configfile $config --packages $packages --no-cache --force-evaluate
if ($LASTEXITCODE -ne 0) { throw "platform SDK restore failed" }

# Byte-for-byte NuGet provenance: reject registry or package re-packing,
# transitive version drift, missing original nupkg, or stale cache copies.
foreach ($id in $packageIds) {
  if ($id -eq "RulesForge.Native" -and -not $desktopServices) { continue }
  $pin = Get-LockedPackage $id
  $name = "$($id.ToLowerInvariant()).$($pin.version).nupkg"
  $packageFile = Join-Path (Join-Path (Join-Path $packages $id.ToLowerInvariant()) $pin.version) $name
  if (-not (Test-Path -LiteralPath $packageFile -PathType Leaf)) {
    throw "NuGet restore lacked original immutable package: $name"
  }
  $digest = (Get-FileHash -LiteralPath $packageFile -Algorithm SHA256).Hash.ToLowerInvariant()
  if ($digest -cne [string]$pin.sha256) {
    throw "NuGet package bytes drifted: $name expected=$($pin.sha256) actual=$digest"
  }
  Write-Host "Locked restored package bytes verified: $name sha256=$digest"
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
    "SaltsNet.Native" { $saltsNetVersion }
    "CHttp.Native" { $chttpVersion }
    "RulesForge.Native" { $rulesForgeVersion }
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

# Reject the known macOS Salts.Native 2.2.0 TinyTest ABI defect BEFORE
# compiling TurboMedia: GCC emulated TLS cannot satisfy AppleClang C11
# native Mach-O TLS references to ttest_active_config__. Corrected official
# 2.3.0 RC packages are verified by Salts' own installed C11 link gate.
if ($Rid -eq "macos-arm64") {
  $tinytestArchive = Join-Path $roots.SALTS_ROOT "lib/libtinytest.a"
  if (-not (Test-Path -LiteralPath $tinytestArchive -PathType Leaf)) {
    throw "official macOS Salts SDK does not contain TinyTest archive: $tinytestArchive"
  }
  $tinytestSymbols = (& nm -gU $tinytestArchive | Out-String)
  if ($LASTEXITCODE -ne 0) {
    throw "unable to inspect installed macOS TinyTest TLS symbols"
  }
  if ($tinytestSymbols.Contains("___emutls_v.ttest_active_config__")) {
    throw "installed macOS TinyTest uses incompatible GCC emulated TLS ABI"
  }
  if (-not $tinytestSymbols.Contains(" _ttest_active_config__")) {
    throw "installed macOS TinyTest lacks required native C11 TLS symbol"
  }
  Write-Host "Verified AppleClang-compatible native TinyTest TLS from official Salts SDK"
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
