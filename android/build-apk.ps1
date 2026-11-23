# Builds the Android apps from the Android build (out\build\android-arm64-h and the SDK's out\linux-arm64) without
# Gradle: javac + d8 + aapt2 + zipalign + apksigner. Signed with a local debug key (out\debug.keystore, created on
# first use).
#   out\project-gestalt.apk  the installer that is shipped (android\project\AndroidManifest.xml): the game's code
#                            and native libraries (as assets), the game app's manifest/resources (game_base.apk),
#                            the Portuguese translation and the signing key; on the phone it makes and installs the
#                            "NieR Gestalt" app from the player's copy (GestaltBuilder.java).
#   out\nier-test.apk        the game app itself (android\AndroidManifest.xml), for development.
# -GameIcon <png>: the development game app (nier-test.apk) with that icon (e.g. the tile from your own disc) instead of
# Project Gestalt's; never shipped.
param([string]$Build = 'out\build\android-arm64', [string]$GameIcon = '',[string]$Sdk = 'H:\Android\sdk', [string]$Jdk = 'H:\Android\jdk17',
      [string]$BuildTools = '34.0.0', [string]$Platform = 'android-34',
      [string]$Ndk = 'H:\Android\sdk\ndk\27.2.12479018')
# Tools print warnings to stderr; failures are checked with $LASTEXITCODE.
$ErrorActionPreference = 'Continue'
$root = Split-Path $PSScriptRoot
$here = $PSScriptRoot
$bt = "$Sdk\build-tools\$BuildTools"
$androidJar = "$Sdk\platforms\$Platform\android.jar"
$work = "$root\out\apk"
if (Test-Path $work) { Remove-Item $work -Recurse -Force }
New-Item -ItemType Directory -Force "$work\classes", "$work\dex", "$work\lib", "$work\apksig", "$work\res\mipmap-xxxhdpi" | Out-Null
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem, System.Drawing

# Java: SDL3's activity classes (same SDL as the native side) + ours, and apksig (Apache 2.0, from the build tools'
# apksigner.jar) for the signing on the phone.
$sdlJava = "$root\..\sdk-src\thirdparty\sdl3\android-project\app\src\main\java"
$sources = @(Get-ChildItem "$sdlJava\org\libsdl\app\*.java") + @(Get-ChildItem "$here\java" -Recurse -Filter *.java)
$jar = [IO.Compression.ZipFile]::OpenRead("$bt\lib\apksigner.jar")
foreach ($e in $jar.Entries) {
    if ($e.FullName -like 'com/android/apksig/*' -and $e.FullName.EndsWith('.class')) {
        $dest = Join-Path "$work\apksig" $e.FullName
        New-Item -ItemType Directory -Force (Split-Path $dest) | Out-Null
        [IO.Compression.ZipFileExtensions]::ExtractToFile($e, $dest, $true)
    }
}
$jar.Dispose()
& "$Jdk\bin\javac.exe" --release 11 -encoding UTF-8 -nowarn -classpath "$androidJar;$work\apksig" -d "$work\classes" ($sources | % FullName)
if ($LASTEXITCODE) { throw 'javac failed' }
& "$Jdk\bin\jar.exe" cf "$work\classes.jar" -C "$work\classes" . -C "$work\apksig" .
if ($LASTEXITCODE) { throw 'jar failed' }
& "$bt\d8.bat" --release --min-api 30 --lib $androidJar --output "$work\dex" "$work\classes.jar"
if ($LASTEXITCODE) { throw 'd8 failed' }

# The launcher icon (192x192): Project Gestalt's (docs\images\icone.png). The NieR Gestalt app gets the player's
# disc tile in its place on the phone.
function Icon([string]$from, [string]$dir) {
    New-Item -ItemType Directory -Force "$work\$dir\mipmap-xxxhdpi" | Out-Null
    $src = [Drawing.Image]::FromFile($from)
    $icon = New-Object Drawing.Bitmap 192, 192
    $g = [Drawing.Graphics]::FromImage($icon)
    $g.InterpolationMode = 'HighQualityBicubic'; $g.PixelOffsetMode = 'HighQuality'
    $g.DrawImage($src, 0, 0, 192, 192)
    $icon.Save("$work\$dir\mipmap-xxxhdpi\ic_launcher.png", [Drawing.Imaging.ImageFormat]::Png)
    $g.Dispose(); $icon.Dispose(); $src.Dispose()
    & "$bt\aapt2.exe" compile --dir "$work\$dir" -o "$work\$dir.zip"
    if ($LASTEXITCODE) { throw 'aapt2 compile failed' }
}
Icon "$root\docs\images\icone.png" 'res'
Icon $(if ($GameIcon) { $GameIcon } else { "$root\docs\images\icone.png" }) 'res_dev'

function Link([string]$manifest, [string]$out, [string]$res = 'res') {
    & "$bt\aapt2.exe" link -o $out --manifest $manifest -I $androidJar -R "$work\$res.zip" --auto-add-overlay
    if ($LASTEXITCODE) { throw "aapt2 link failed: $manifest" }
}
Link "$here\AndroidManifest.xml" "$work\game_base.apk"
Link "$here\AndroidManifest.xml" "$work\game_dev_base.apk" 'res_dev'
Link "$here\project\AndroidManifest.xml" "$work\project_base.apk"

# Native libraries (stripped).
$libs = @("$root\$Build\libmain.so") +
        @(Get-ChildItem "$root\$Build\libadrenotools\src\hook\*.so" -ErrorAction SilentlyContinue | % FullName) +
        @(Get-ChildItem "$root\..\sdk-src\out\linux-arm64\*.so" | % FullName) +
        @("$Ndk\toolchains\llvm\prebuilt\windows-x86_64\sysroot\usr\lib\aarch64-linux-android\libc++_shared.so")
$strip = "$Ndk\toolchains\llvm\prebuilt\windows-x86_64\bin\llvm-strip.exe"
foreach ($l in $libs) {
    & $strip --strip-debug -o "$work\lib\$(Split-Path $l -Leaf)" $l
    if ($LASTEXITCODE) { throw "strip failed: $l" }
}

# The signing key, also exported for the phone (assets/sign).
$keystore = "$root\out\debug.keystore"
if (-not (Test-Path $keystore)) {
    & "$Jdk\bin\keytool.exe" -genkeypair -keystore $keystore -storepass android -keypass android `
        -alias nierdebug -keyalg RSA -keysize 2048 -validity 10000 -dname 'CN=NieR Port Debug'
}
& "$Jdk\bin\java.exe" "$here\ExportKey.java" $keystore android nierdebug "$work\sign"
if ($LASTEXITCODE) { throw 'key export failed' }

function Finish([string]$unsigned, [string]$out) {
    & "$bt\zipalign.exe" -f -p 4 $unsigned "$unsigned.aligned"
    if ($LASTEXITCODE) { throw 'zipalign failed' }
    & "$bt\apksigner.bat" sign --ks $keystore --ks-pass pass:android --key-pass pass:android --out $out "$unsigned.aligned"
    if ($LASTEXITCODE) { throw 'apksigner failed' }
    "APK: $out ($([int]((Get-Item $out).Length / 1MB)) MB)"
}

# The game app (development): its manifest and resources, the code, the libraries.
Copy-Item "$work\game_dev_base.apk" "$work\game.apk"
$zip = [IO.Compression.ZipFile]::Open("$work\game.apk", 'Update')
[void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, "$work\dex\classes.dex", 'classes.dex')
foreach ($so in Get-ChildItem "$work\lib") {
    [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, $so.FullName, "lib/arm64-v8a/$($so.Name)")
}
$zip.Dispose()
Finish "$work\game.apk" "$root\out\nier-test.apk"

# Project Gestalt: the code, and as assets the libraries, the game app's base, the key and the translation.
Copy-Item "$work\project_base.apk" "$work\project.apk"
$zip = [IO.Compression.ZipFile]::Open("$work\project.apk", 'Update')
[void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, "$work\dex\classes.dex", 'classes.dex')
foreach ($so in Get-ChildItem "$work\lib") {
    [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, $so.FullName, "assets/lib/$($so.Name)")
}
[void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, "$work\game_base.apk", 'assets/game_base.apk')
foreach ($f in Get-ChildItem "$work\sign" -File) {
    [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, $f.FullName, "assets/sign/$($f.Name)")
}
$text = "$root\traducao"
foreach ($f in Get-ChildItem $text -Recurse -File) {
    $rel = $f.FullName.Substring($text.Length + 1).Replace('\', '/')
    [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, $f.FullName, "assets/TEXT/$rel")
}
$zip.Dispose()
Finish "$work\project.apk" "$root\out\project-gestalt.apk"
