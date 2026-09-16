# The launcher window: Import game files / Play / Close, with the port's facts.
# Started by Launcher.bat; everything it needs sits in the bundle folder.
$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$Facts = @{}
if (Test-Path "$Root\bundle.txt") {
  Get-Content "$Root\bundle.txt" | ForEach-Object { if ($_ -match '^(\w+)=(.*)$') { $Facts[$Matches[1]] = $Matches[2] } }
}
$Name = if ($Facts.name) { $Facts.name } else { "Game" }
$Slug = if ($Facts.slug) { $Facts.slug } else { (Get-ChildItem "$Root\*.exe" | Where-Object { $_.Name -ne "rexiso.exe" } | Select-Object -First 1).BaseName }
$Exe = Join-Path $Root "$Slug.exe"
$Native = ""
if (Test-Path "$Root\NATIVE_COVERAGE.md") {
  $m = Select-String -Path "$Root\NATIVE_COVERAGE.md" -Pattern '(\d+)%' | Select-Object -First 1
  if ($m) { $Native = "Console layer native: " + $m.Matches[0].Groups[1].Value + "%" }
}
$Source = if (Test-Path "$Root\content\SOURCE.txt") { Get-Content "$Root\content\SOURCE.txt" -First 1 } else { "" }

Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
$form = New-Object System.Windows.Forms.Form
$form.Text = $Name
$form.ClientSize = New-Object System.Drawing.Size(560, 330)
$form.StartPosition = 'CenterScreen'
$form.FormBorderStyle = 'FixedDialog'
$form.MaximizeBox = $false
$form.Font = New-Object System.Drawing.Font("Segoe UI", 10)

$title = New-Object System.Windows.Forms.Label
$title.Location = New-Object System.Drawing.Point(20, 15)
$title.Size = New-Object System.Drawing.Size(520, 30)
$title.Font = New-Object System.Drawing.Font("Segoe UI", 14, [System.Drawing.FontStyle]::Bold)
$title.Text = "$Name - native Windows port"

$info = New-Object System.Windows.Forms.Label
$info.Location = New-Object System.Drawing.Point(20, 55)
$info.Size = New-Object System.Drawing.Size(520, 190)
function Refresh-Info {
  $have = Test-Path "$Root\assets\default.xex"
  $state = if ($have) { "imported (assets\)" } else { "not imported yet - press Import game files" }
  $lines = @(
    "Game files:  $state",
    "",
    "Recompiled functions:  $($Facts.functions)   in $($Facts.files) files",
    "Built:  $($Facts.built)",
    $Native,
    "",
    "Expected source:",
    "  $Source",
    "",
    "Saves and settings live in user-data\.  Details: CONVERSION.md, NATIVE_COVERAGE.md"
  )
  $info.Text = ($lines -join "`r`n")
  $script:HaveGame = $have
}
Refresh-Info

function Import-Game {
  $p = Start-Process -FilePath "powershell" -ArgumentList @("-NoProfile", "-STA", "-ExecutionPolicy", "Bypass", "-File", "`"$Root\tools\Import-GameFiles.ps1`"") -Wait -PassThru -WindowStyle Hidden
  Refresh-Info
  return ($p.ExitCode -ne 1)
}

$btnImport = New-Object System.Windows.Forms.Button
$btnImport.Text = "Import game files"
$btnImport.Location = New-Object System.Drawing.Point(20, 270)
$btnImport.Size = New-Object System.Drawing.Size(170, 40)
$btnImport.Add_Click({ [void](Import-Game) })

$btnPlay = New-Object System.Windows.Forms.Button
$btnPlay.Text = "Play"
$btnPlay.Location = New-Object System.Drawing.Point(205, 270)
$btnPlay.Size = New-Object System.Drawing.Size(170, 40)
$btnPlay.Add_Click({
  if (-not $script:HaveGame) { if (-not (Import-Game)) { return } }
  if (-not (Test-Path "$Root\assets\default.xex")) { return }
  Start-Process -FilePath $Exe -WorkingDirectory $Root -ArgumentList @(
    "--game_data_root=assets", "--gpu_plugin", "xenos", "--user_data_root=user-data", "--license_mask=1", "--mnk_mode")
  $form.Close()
})

$btnClose = New-Object System.Windows.Forms.Button
$btnClose.Text = "Close"
$btnClose.Location = New-Object System.Drawing.Point(390, 270)
$btnClose.Size = New-Object System.Drawing.Size(150, 40)
$btnClose.Add_Click({ $form.Close() })

$form.Controls.AddRange(@($title, $info, $btnImport, $btnPlay, $btnClose))
$form.AcceptButton = $btnPlay
[void]$form.ShowDialog()
