
$guid = "{d417d94e-8595-4022-aef1-5a00b183e2bd}"
$sfx = "{C9453E73-8C5C-4463-9984-AF8BAB2F5447}"
$mfx = "{13AB3EBD-137E-4903-9D89-60BE8277FD17}"
$key = "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render\$guid\FxProperties"

cmd.exe /c "reg add `"$key`" /v `"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},5`" /t REG_SZ /d `"$sfx`" /f"
cmd.exe /c "reg add `"$key`" /v `"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},6`" /t REG_SZ /d `"$mfx`" /f"

