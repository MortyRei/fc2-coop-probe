<#
Sets up a Far Cry 2 TEST copy for the co-op probe on this PC (copy "A" or "B").
Does not touch the real install, the real profile or the real saves.

  1. finds Far Cry 2 (Steam, app 19900) and checks bin\Dunia.dll is the Steam 1.03 build (SHA-256 6353936a...2765)
  2. copies the install to -TestDir (default: <game drive>\fc2-coop\test-<letter>)
  3. backs up the copy's Dunia.dll (Dunia.dll.orig) and patches three same-length strings in the copy
     (shown for -Letter B):
       0xE08950  FarCry2Instance      -> FarCry2InstancB       (own mutex, so it can run next to the normal game)
       0xE089A0  My Games\Far Cry 2\  -> My Games\FC2test_B\   (folder the save list is built from)
       0xE0E094  \Far Cry 2           -> \FC2test_B            (own profile + opened save: Documents\My Games\FC2test_B)
     then checks that exactly those 17 bytes changed
  4. puts the probe (-Probe, default dinput8.dll next to this script) into the copy's bin folder; if a
     SHA256SUMS.txt lies next to this script, the probe must match it
  5. creates Documents\My Games\FC2test_<letter> with GamerProfile.xml and Saved Games\<save>.sav from -PrivateDir
     (a GamerProfile.xml and the one .sav both PCs load; keep these private)

  powershell -ExecutionPolicy Bypass -File setup-copy.ps1 -Letter B -PrivateDir <folder with GamerProfile.xml and the .sav>
#>
param(
    [Parameter(Mandatory = $true)][string]$PrivateDir,
    [ValidateSet('A', 'B')][string]$Letter = 'B',
    [string]$Probe = (Join-Path $PSScriptRoot 'dinput8.dll'),
    [string]$GameDir,
    [string]$TestDir,
    [string]$Documents = [Environment]::GetFolderPath('MyDocuments')
)
$ErrorActionPreference = 'Stop'
$wantDunia = '6353936a54aa841350bb30ff005727859cdef1aa10c209209b220b399e862765'
$name = "FC2test_$Letter"
function Say($m) { Write-Output ("[setup] " + $m) }

# 1. game
if (-not $GameDir) {
    $libs = @()
    foreach ($s in "${env:ProgramFiles(x86)}\Steam", "$env:ProgramFiles\Steam") {
        $vdf = Join-Path $s 'steamapps\libraryfolders.vdf'
        if (Test-Path $vdf) { $libs += $s; $libs += (Select-String -Path $vdf -Pattern '"path"\s+"([^"]+)"' | ForEach-Object { $_.Matches[0].Groups[1].Value -replace '\\\\', '\' }) }
    }
    foreach ($l in ($libs | Select-Object -Unique)) {
        $m = Join-Path $l 'steamapps\appmanifest_19900.acf'
        if (Test-Path $m) {
            $dir = (Select-String -Path $m -Pattern '"installdir"\s+"([^"]+)"').Matches[0].Groups[1].Value
            $GameDir = Join-Path $l "steamapps\common\$dir"
            $build = (Select-String -Path $m -Pattern '"buildid"\s+"(\d+)"').Matches[0].Groups[1].Value
            Say "found Far Cry 2 in $GameDir (Steam build $build)"
            break
        }
    }
}
if (-not $GameDir -or -not (Test-Path (Join-Path $GameDir 'bin\Dunia.dll'))) { throw 'Far Cry 2 not found; pass -GameDir' }
$h = (Get-FileHash (Join-Path $GameDir 'bin\Dunia.dll')).Hash.ToLower()
if ($h -ne $wantDunia) { throw "bin\Dunia.dll is not the Steam 1.03 build (SHA-256 $h); the probe's addresses would not match" }
Say 'Dunia.dll is the expected Steam 1.03 build'

# probe
if (-not (Test-Path $Probe)) { throw "probe not found: $Probe (build dinput8.dll first or pass -Probe)" }
$sumsFile = Join-Path $PSScriptRoot 'SHA256SUMS.txt'
if (Test-Path $sumsFile) {
    $want = Get-Content $sumsFile | Where-Object { $_ -match '^([0-9a-f]{64})\s+\*?dinput8\.dll$' } | ForEach-Object { $Matches[1] }
    if ($want -and (Get-FileHash $Probe).Hash.ToLower() -ne $want) { throw 'dinput8.dll does not match SHA256SUMS.txt' }
}

# private files
$prof = Join-Path $PrivateDir 'GamerProfile.xml'
$saves = @(Get-ChildItem $PrivateDir -Filter *.sav -File -Recurse)
if (-not (Test-Path $prof)) { throw "no GamerProfile.xml in $PrivateDir" }
if ($saves.Count -ne 1) { throw "expected exactly one .sav in $PrivateDir, found $($saves.Count)" }

# 2. test copy
if (-not $TestDir) { $TestDir = Join-Path (Split-Path $GameDir -Qualifier) ("fc2-coop\test-" + $Letter.ToLower()) }
$bin = Join-Path $TestDir 'bin'
if (Test-Path (Join-Path $bin 'Dunia.dll.orig')) {
    Say "test copy already set up in $TestDir, not copying again"
}
else {
    if (Test-Path $TestDir) { throw "$TestDir exists but is not a finished test copy; remove it or pass another -TestDir" }
    Say "copying the game to $TestDir (about 3.7 GB) ..."
    robocopy $GameDir $TestDir /E /COPY:DAT /R:1 /W:1 /NFL /NDL /NP /NJH /NJS | Out-Null
    if ($LASTEXITCODE -ge 8) { throw "robocopy failed (exit $LASTEXITCODE)" }

    # 3. patch the copy
    $d = Join-Path $bin 'Dunia.dll'
    Copy-Item $d "$d.orig"
    if ((Get-FileHash "$d.orig").Hash.ToLower() -ne $wantDunia) { throw 'backup copy does not match' }
    # 0xE0E094: profile and the save file that is opened; 0xE089A0: the folder the save list (Continue / Load Game)
    # is built from. With only the first, a copy on a PC that never ran the game lists no saves.
    $patches = @(
        @{ off = 0xE08950; old = 'FarCry2Instance'; new = "FarCry2Instanc$Letter" },
        @{ off = 0xE089A0; old = 'My Games\Far Cry 2\'; new = "My Games\$name\" },
        @{ off = 0xE0E094; old = '\Far Cry 2'; new = "\$name" })
    $fs = [IO.File]::Open($d, 'Open', 'ReadWrite', 'None')
    try {
        foreach ($p in $patches) {
            if ($p.old.Length -ne $p.new.Length) { throw "length mismatch for $($p.new)" }
            $buf = New-Object byte[] ($p.old.Length + 1); $fs.Position = $p.off; [void]$fs.Read($buf, 0, $buf.Length)
            if ([Text.Encoding]::ASCII.GetString($buf, 0, $p.old.Length) -cne $p.old -or $buf[-1] -ne 0) { throw ("unexpected bytes at 0x{0:X}" -f $p.off) }
        }
        foreach ($p in $patches) { $nb = [Text.Encoding]::ASCII.GetBytes($p.new); $fs.Position = $p.off; $fs.Write($nb, 0, $nb.Length) }
    }
    finally { $fs.Close() }
    $a = [IO.File]::ReadAllBytes("$d.orig"); $b = [IO.File]::ReadAllBytes($d); $n = 0
    for ($i = 0; $i -lt $a.Length; $i++) { if ($a[$i] -ne $b[$i]) { $n++ } }
    if ($a.Length -ne $b.Length -or $n -ne 17) { throw "patch check failed: $n bytes differ (expected 17)" }
    Say 'patched the copy''s Dunia.dll (17 bytes in 3 strings; backup Dunia.dll.orig)'
}

# 4. probe
Copy-Item $Probe (Join-Path $bin 'dinput8.dll') -Force
Say 'probe dinput8.dll installed in the test copy'

# 5. test profile
$pdir = Join-Path $Documents "My Games\$name"
$sdir = Join-Path $pdir 'Saved Games'
New-Item -ItemType Directory -Force $sdir | Out-Null
foreach ($pair in @(@{ src = $prof; dst = (Join-Path $pdir 'GamerProfile.xml') }, @{ src = $saves[0].FullName; dst = (Join-Path $sdir $saves[0].Name) })) {
    if ((Test-Path $pair.dst) -and (Get-FileHash $pair.dst).Hash -ne (Get-FileHash $pair.src).Hash) { throw "$($pair.dst) exists and differs; not overwriting" }
    Copy-Item $pair.src $pair.dst -Force
}
Say "test profile ready: $pdir (save $($saves[0].Name))"
Say "done. Test copy: $TestDir"
