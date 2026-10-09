param(
    [Parameter(Mandatory=$true)][string]$Clang,
    [Parameter(Mandatory=$true)][string]$SDK,
    [Parameter(Mandatory=$true)][string]$BuildDirectory
)
$ErrorActionPreference='Stop'
$repo=Split-Path $PSScriptRoot -Parent
$sdkPath=(Resolve-Path -LiteralPath $SDK).Path
$compiler=(Resolve-Path -LiteralPath $Clang).Path
$buildPath=[IO.Path]::GetFullPath($BuildDirectory)
# Keep intermediate files and compiler temporary state under the named project.
$projectRoot=[IO.Path]::GetFullPath((Join-Path $repo '../..')).TrimEnd('\')+'\'
if (!$buildPath.StartsWith($projectRoot,[StringComparison]::OrdinalIgnoreCase)) { throw 'Build directory must be inside this project.' }
New-Item -ItemType Directory -Force -Path $buildPath | Out-Null
$env:TEMP=$buildPath; $env:TMP=$buildPath
foreach ($source in @('guest_package_service','sha256')) {
    & $compiler -target arm-apple-ios3.0 -arch armv6 -isysroot $sdkPath -Wno-incompatible-sysroot -Os -Wall -Wextra -Werror -I "$repo/tools" -c "$repo/tools/$source.c" -o "$buildPath/$source.o"
    if ($LASTEXITCODE) { throw "Compile failed: $source" }
}
$output=Join-Path $repo 'app/Resources/GuestPackages/s5lbox-package-service'
# The open-source SDK's libSystem stub omits several real iOS 3 POSIX exports.
# Dynamic lookup binds those imports to the guest's libSystem at launch; this
# is a guest binary only. Physical firmware validation remains mandatory.
& "$sdkPath/usr/bin/ld.exe" -arch armv6 -iphoneos_version_min 3.0 -undefined dynamic_lookup -syslibroot $sdkPath -o $output "$sdkPath/usr/lib/crt1.o" "$buildPath/guest_package_service.o" "$buildPath/sha256.o" -lSystem
if ($LASTEXITCODE) { throw 'Guest helper link failed.' }
Get-FileHash -Algorithm SHA256 -LiteralPath $output
