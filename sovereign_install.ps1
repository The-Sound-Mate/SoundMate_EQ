$guid = '{d417d94e-8595-4022-aef1-5a00b183e2bd}';
$clsids = @('{EC1CC9CE-FAED-4822-828A-82A81A6F018F}', '{EACD2258-FCAC-4FF4-B36D-419E924A6D79}', '{EF3E9359-9E6D-4AD5-A4A9-3129532A55F5}');
$targetClsid = $clsids[0];
$basePath = "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render\$guid\FxProperties";
function Hook-Key($path) {
  try {
    $items = Get-ItemProperty -Path $path -ErrorAction SilentlyContinue;
    $items.PSObject.Properties | ForEach-Object {
      $name = $_.Name; $val = $_.Value;
      if ($val -is [array] -and ($val -join ' ') -like '*{*') {
        $newVal = @($targetClsid); foreach($v in $val) { if($v -ne $targetClsid) { $newVal += $v } };
        Set-ItemProperty -Path $path -Name $name -Value $newVal -Type MultiString -Force -ErrorAction SilentlyContinue;
      }
    };
    Get-ChildItem -Path $path -ErrorAction SilentlyContinue | ForEach-Object { Hook-Key $_.PSPath };
  } catch {}
}
Hook-Key $basePath;
Set-ItemProperty -Path $basePath -Name '{1da5d803-d492-4edd-8c23-e0c0ffee7f0e},5' -Value 0 -Type DWord -Force;
Set-ItemProperty -Path $basePath -Name 'DisableProtectedAudioDG' -Value 1 -Type DWord -Force;
$childBase = "HKLM:\SOFTWARE\EqualizerAPO\Child APOs\$guid";
if (!(Test-Path $childBase)) { New-Item -Path $childBase -Force | Out-Null };
Set-ItemProperty -Path $childBase -Name 'Pre-mix' -Value $clsids[0] -Force;
Set-ItemProperty -Path $childBase -Name 'Post-mix' -Value $clsids[1] -Force;
