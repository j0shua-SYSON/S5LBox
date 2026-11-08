param(
    [Parameter(Mandatory=$true)][string]$Clang,
    [Parameter(Mandatory=$true)][string]$SDK,
    [Parameter(Mandatory=$true)][string]$OutputDirectory
)
$ErrorActionPreference='Stop'
$compiler=(Resolve-Path -LiteralPath $Clang).Path
$sdkRoot=(Resolve-Path -LiteralPath $SDK).Path
$linker=Join-Path $sdkRoot 'usr/bin/ld.exe'
if (!(Test-Path -LiteralPath $linker)) { $linker=Join-Path $sdkRoot 'usr/bin/ld' }
$outputRoot=[IO.Path]::GetFullPath($OutputDirectory)
$repoRoot=(Resolve-Path "$PSScriptRoot/../../..").Path
$allowedRoot=[IO.Path]::GetFullPath((Join-Path $repoRoot 'work'))+[IO.Path]::DirectorySeparatorChar
if (!$outputRoot.StartsWith($allowedRoot,[StringComparison]::OrdinalIgnoreCase)) {
    throw 'Fixture output must be inside this checkout/work.'
}
$bundle=Join-Path $outputRoot 'InterpreterSmoke.app'
New-Item -ItemType Directory -Force -Path $bundle | Out-Null
$env:TEMP=$outputRoot; $env:TMP=$outputRoot; $env:TMPDIR=$outputRoot
$common=@('-target','arm-apple-ios3.0','-arch','armv6','-isysroot',$sdkRoot,
    '-Wno-incompatible-sysroot','-fno-stack-protector','-mfpu=vfp','-mfloat-abi=softfp',
    '-O1','-Wall','-Wextra','-Werror')
& $compiler @common -marm -c "$PSScriptRoot/smoke.c" -o "$outputRoot/smoke.o"
if ($LASTEXITCODE) { throw 'ARM fixture compile failed' }
& $compiler @common -mthumb -c "$PSScriptRoot/thumb.c" -o "$outputRoot/thumb.o"
if ($LASTEXITCODE) { throw 'Thumb fixture compile failed' }
& $linker -arch armv6 -iphoneos_version_min 3.0 -syslibroot $sdkRoot `
    -undefined dynamic_lookup -o "$bundle/InterpreterSmoke" "$sdkRoot/usr/lib/crt1.o" `
    "$outputRoot/smoke.o" "$outputRoot/thumb.o" -lSystem
if ($LASTEXITCODE) { throw 'Fixture link failed' }
Copy-Item -LiteralPath "$PSScriptRoot/Info.plist" -Destination "$bundle/Info.plist"
$visual=Join-Path $outputRoot 'HLEVisual.app'
New-Item -ItemType Directory -Force -Path $visual | Out-Null
& $compiler @common -marm -c "$PSScriptRoot/visual.m" -o "$outputRoot/visual.o"
if ($LASTEXITCODE) { throw 'Visual fixture compile failed' }
& $linker -arch armv6 -iphoneos_version_min 3.0 -syslibroot $sdkRoot `
    -undefined dynamic_lookup -o "$visual/HLEVisual" "$sdkRoot/usr/lib/crt1.o" `
    "$outputRoot/visual.o" -lSystem -lobjc -framework Foundation -framework UIKit -framework OpenAL
if ($LASTEXITCODE) { throw 'Visual fixture link failed' }
Copy-Item -LiteralPath "$PSScriptRoot/visual.plist" -Destination "$visual/Info.plist"
Copy-Item -LiteralPath "$PSScriptRoot/Fixture.bundle" -Destination $visual -Recurse -Force
$startup=Join-Path $outputRoot 'StartupSmoke.app'
New-Item -ItemType Directory -Force -Path $startup | Out-Null
& $compiler @common -marm -c "$PSScriptRoot/startup.m" -o "$outputRoot/startup.o"
if ($LASTEXITCODE) { throw 'Startup fixture compile failed' }
& $linker -arch armv6 -iphoneos_version_min 3.0 -syslibroot $sdkRoot `
    -undefined dynamic_lookup -o "$startup/InterpreterSmoke" "$sdkRoot/usr/lib/crt1.o" `
    "$outputRoot/startup.o" -lSystem -lobjc -framework Foundation -framework CoreFoundation
if ($LASTEXITCODE) { throw 'Startup fixture link failed' }
Copy-Item -LiteralPath "$PSScriptRoot/Info.plist" -Destination "$startup/Info.plist"
$audio=Join-Path $outputRoot 'AudioSmoke.app'
$arrays=Join-Path $outputRoot 'GLESArrays.app'
New-Item -ItemType Directory -Force -Path $arrays | Out-Null
& $compiler @common -marm -c "$PSScriptRoot/gles_arrays.m" -o "$outputRoot/gles_arrays.o"
if ($LASTEXITCODE) { throw 'GLES array fixture compile failed' }
& $linker -arch armv6 -iphoneos_version_min 3.0 -syslibroot $sdkRoot `
    -undefined dynamic_lookup -o "$arrays/InterpreterSmoke" "$sdkRoot/usr/lib/crt1.o" `
    "$outputRoot/gles_arrays.o" -lSystem -lobjc -framework Foundation -framework OpenGLES
if ($LASTEXITCODE) { throw 'GLES array fixture link failed' }
Copy-Item -LiteralPath "$PSScriptRoot/Info.plist" -Destination "$arrays/Info.plist"
$alerts=Join-Path $outputRoot 'AlertsSmoke.app'
New-Item -ItemType Directory -Force -Path $alerts | Out-Null
& $compiler @common -marm -c "$PSScriptRoot/alerts.m" -o "$outputRoot/alerts.o"
if ($LASTEXITCODE) { throw 'Alert fixture compile failed' }
& $linker -arch armv6 -iphoneos_version_min 3.0 -syslibroot $sdkRoot `
    -undefined dynamic_lookup -o "$alerts/InterpreterSmoke" "$sdkRoot/usr/lib/crt1.o" `
    "$outputRoot/alerts.o" -lSystem -lobjc -framework Foundation -framework UIKit
if ($LASTEXITCODE) { throw 'Alert fixture link failed' }
Copy-Item -LiteralPath "$PSScriptRoot/Info.plist" -Destination "$alerts/Info.plist"
New-Item -ItemType Directory -Force -Path $audio | Out-Null
& $compiler @common -marm -c "$PSScriptRoot/audio.m" -o "$outputRoot/audio.o"
if ($LASTEXITCODE) { throw 'Audio fixture compile failed' }
& $linker -arch armv6 -iphoneos_version_min 3.0 -syslibroot $sdkRoot `
    -undefined dynamic_lookup -o "$audio/InterpreterSmoke" "$sdkRoot/usr/lib/crt1.o" `
    "$outputRoot/audio.o" -lSystem -lobjc -framework Foundation -framework CoreFoundation -framework AudioToolbox -framework AVFoundation
if ($LASTEXITCODE) { throw 'Audio fixture link failed' }
Copy-Item -LiteralPath "$PSScriptRoot/Info.plist" -Destination "$audio/Info.plist"
$dylibs=Join-Path $outputRoot 'LibrarySmoke.app'
New-Item -ItemType Directory -Force -Path $dylibs | Out-Null
& $compiler @common -marm -c "$PSScriptRoot/dylibs.c" -o "$outputRoot/dylibs.o"
if ($LASTEXITCODE) { throw 'Library fixture compile failed' }
# The old fat image's ARMv6 slice is not aligned to a 16 KiB host page.
# Avoid the community linker's slice mmap failure by thinning the link input.
# The runtime still loads/selects the original bundled fat library.
$fat=[IO.File]::ReadAllBytes("$repoRoot/third_party/touchhle/touchHLE_dylibs/libstdc++.6.0.9.dylib")
function Read-Big32([int]$offset) {
    if($offset -lt 0 -or $offset+4 -gt $fat.Length) { throw 'Invalid fat header' }
    return [uint32]($fat[$offset]*16777216L + $fat[$offset+1]*65536L +
                   $fat[$offset+2]*256L + $fat[$offset+3])
}
if((Read-Big32 0) -ne [uint32]3405691582) { throw 'Expected the bundled fat C++ library' }
$count=Read-Big32 4; $thin=$null
if($count -gt ($fat.Length-8)/20) { throw 'Invalid fat architecture count' }
for($i=0; $i -lt $count; $i++) {
    $entry=8+$i*20
    if((Read-Big32 $entry) -ne 12 -or (Read-Big32 ($entry+4)) -ne 6) { continue }
    $offset=Read-Big32 ($entry+8); $size=Read-Big32 ($entry+12)
    if([long]$offset+$size -gt $fat.Length) { throw 'Invalid ARMv6 slice range' }
    $thin=New-Object byte[] $size; [Array]::Copy($fat,$offset,$thin,0,$size); break
}
if(!$thin) { throw 'No ARMv6 C++ link input' }
[IO.File]::WriteAllBytes("$outputRoot/libstdcxx-armv6.dylib",$thin)
& $linker -arch armv6 -iphoneos_version_min 3.0 -syslibroot $sdkRoot `
    -undefined dynamic_lookup -o "$dylibs/InterpreterSmoke" "$sdkRoot/usr/lib/crt1.o" `
    "$outputRoot/dylibs.o" -lSystem `
    "$repoRoot/third_party/touchhle/touchHLE_dylibs/libz.1.2.3.dylib" `
    "$repoRoot/third_party/touchhle/touchHLE_dylibs/libsqlite3.dylib" `
    "$repoRoot/third_party/touchhle/touchHLE_dylibs/libxml2.2.dylib" `
    "$outputRoot/libstdcxx-armv6.dylib"
if ($LASTEXITCODE) { throw 'Library fixture link failed' }
Copy-Item -LiteralPath "$PSScriptRoot/library.plist" -Destination "$dylibs/Info.plist"
Write-Output $bundle
