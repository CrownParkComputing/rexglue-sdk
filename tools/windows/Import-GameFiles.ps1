# Import your own copy of the game into assets\ and check it against
# content\content.sha256. Nothing is downloaded. Sources that work:
#   - a .zip / .7z / .rar holding the disc tree (7-Zip is needed for .7z and .rar)
#   - a .iso disc image (read by rexiso.exe beside the launcher)
#   - an already-extracted folder (the one holding default.xex)
#
#   powershell -ExecutionPolicy Bypass -File tools\Import-GameFiles.ps1 [-Source <file-or-folder>] [-Quiet]
#
# Exit: 0 imported and verified, 2 imported but not the expected rip, 1 nothing imported.
param([string]$Source = "", [switch]$Quiet)
$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$Facts = @{}
if (Test-Path "$Root\bundle.txt") {
  Get-Content "$Root\bundle.txt" | ForEach-Object { if ($_ -match '^(\w+)=(.*)$') { $Facts[$Matches[1]] = $Matches[2] } }
}
$Name = if ($Facts.name) { $Facts.name } else { "the game" }
Add-Type -AssemblyName System.Windows.Forms
function Say($text, $icon = 'Information') {
  if ($Quiet) { Write-Host $text } else { [void][System.Windows.Forms.MessageBox]::Show($text, "$Name - Import game files", 'OK', $icon) }
}
function Fail($text) { Say $text 'Error'; exit 1 }

if (-not $Source) {
  $rec = if (Test-Path "$Root\content\SOURCE.txt") { Get-Content "$Root\content\SOURCE.txt" -First 1 } else { "see content\SOURCE.txt" }
  $q = "Import your own copy of $Name.`n`nRecommended source:`n$rec`n`nYes = an archive or disc image (.zip / .7z / .rar / .iso)`nNo = an already-extracted folder"
  $r = [System.Windows.Forms.MessageBox]::Show($q, "$Name - Import game files", 'YesNoCancel', 'Question')
  if ($r -eq 'Cancel') { exit 1 }
  if ($r -eq 'Yes') {
    $d = New-Object System.Windows.Forms.OpenFileDialog
    $d.Title = "Select the game archive or disc image"
    $d.Filter = "Game archives and disc images|*.zip;*.7z;*.rar;*.iso|All files|*.*"
    if ($d.ShowDialog() -ne 'OK') { exit 1 }
    $Source = $d.FileName
  } else {
    $d = New-Object System.Windows.Forms.FolderBrowserDialog
    $d.Description = "Select the extracted game folder (the one holding default.xex)"
    if ($d.ShowDialog() -ne 'OK') { exit 1 }
    $Source = $d.SelectedPath
  }
}
if (-not (Test-Path -LiteralPath $Source)) { Fail "Not found:`n$Source" }

$Work = $null
if (Test-Path -LiteralPath $Source -PathType Container) {
  $Tree = $Source
} else {
  $Work = Join-Path ([IO.Path]::GetTempPath()) ("rexglue-import-" + [IO.Path]::GetRandomFileName())
  New-Item -ItemType Directory -Path $Work | Out-Null
  $ext = [IO.Path]::GetExtension($Source).ToLower()
  switch ($ext) {
    ".zip" { Expand-Archive -LiteralPath $Source -DestinationPath $Work -Force }
    ".iso" {
      $rexiso = Join-Path $Root "rexiso.exe"
      if (-not (Test-Path $rexiso)) { Fail "rexiso.exe is missing beside the launcher, so a .iso cannot be read.`nExtract the image yourself and import the folder instead." }
      & $rexiso extract $Source $Work | Out-Null
      if ($LASTEXITCODE -ne 0) { Fail "Could not read the disc image:`n$Source`n`nIs it an Xbox 360 disc image (XDVDFS)?" }
    }
    default {
      $sevenZip = @("$env:ProgramFiles\7-Zip\7z.exe", "${env:ProgramFiles(x86)}\7-Zip\7z.exe", "7z.exe") |
        Where-Object { Get-Command $_ -ErrorAction SilentlyContinue } | Select-Object -First 1
      if (-not $sevenZip) { Fail "7-Zip is needed to open $ext archives.`nInstall it from 7-zip.org, or extract the archive yourself and import the folder." }
      & $sevenZip x -y -bso0 -bsp0 "-o$Work" $Source | Out-Null
      if ($LASTEXITCODE -ne 0) { Fail "Could not extract:`n$Source" }
    }
  }
  $Tree = $Work
}

# The disc tree is the directory holding default.xex, wherever the archive put it.
$xex = Get-ChildItem -LiteralPath $Tree -Recurse -File -Filter default.xex -ErrorAction SilentlyContinue |
  Sort-Object { $_.FullName.Length } | Select-Object -First 1
if (-not $xex) { Fail "No default.xex found in:`n$Source`n`nThis launcher needs the disc tree (the folder with default.xex)." }
$Assets = Join-Path $Root "assets"
New-Item -ItemType Directory -Path $Assets -Force | Out-Null
robocopy $xex.DirectoryName $Assets /E /NFL /NDL /NJH /NJS /NP /R:1 /W:1 | Out-Null
if ($LASTEXITCODE -ge 8) { Fail "Copying the game files into assets\ failed." }
if ($Work) { Remove-Item -LiteralPath $Work -Recurse -Force -ErrorAction SilentlyContinue }

# Verify against content\content.sha256: "hash  ./path" lines; the archive line is skipped.
$sums = Join-Path $Root "content\content.sha256"
$ok = 0; $bad = @(); $missing = @()
if (Test-Path $sums) {
  foreach ($line in Get-Content $sums) {
    if ($line -notmatch '^([0-9a-fA-F]{64})\s+\./(.+)$') { continue }
    $rel = $Matches[2] -replace '/', '\'
    $path = Join-Path $Assets $rel
    if (-not (Test-Path -LiteralPath $path)) { $missing += $rel; continue }
    $h = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
    if ($h -ieq $Matches[1]) { $ok++ } else { $bad += $rel }
  }
}
$summary = "$ok files verified"
if ($missing.Count) { $summary += ", $($missing.Count) missing" }
if ($bad.Count) { $summary += ", $($bad.Count) differ" }
if ($missing.Count -or $bad.Count) {
  $first = ($missing + $bad | Select-Object -First 6) -join "`n"
  Say "Imported into assets\, but it is not the expected rip:`n$summary`n`nFirst problems:`n$first`n`nThe game may still run. The recommended source is in content\SOURCE.txt." 'Warning'
  exit 2
}
Say "Game files imported and verified.`n$summary`n`nYou can Play now."
exit 0
