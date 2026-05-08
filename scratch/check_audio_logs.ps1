$logName = "Microsoft-Windows-Audio/Operational"
Write-Host "--- Recent Audio Engine Events ---"
try {
    $events = Get-WinEvent -LogName $logName -MaxEvents 20
    foreach ($e in $events) {
        $time = $e.TimeCreated.ToString("HH:mm:ss")
        $msg = $e.Message
        Write-Host "[$time] $msg"
    }
} catch {
    Write-Host "Could not read event logs: $_"
}
