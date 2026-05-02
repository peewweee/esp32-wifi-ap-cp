param([int]$Seconds = 20)
$port = New-Object System.IO.Ports.SerialPort 'COM5', 115200, 'None', 8, 'One'
$port.NewLine = "`n"
$port.ReadTimeout = 200
try {
    $port.Open()
    # Pulse EN low via RTS to reset the chip and capture from start of boot
    $port.RtsEnable = $true
    $port.DtrEnable = $false
    Start-Sleep -Milliseconds 100
    $port.RtsEnable = $false
    Start-Sleep -Milliseconds 50

    $deadline = (Get-Date).AddSeconds($Seconds)
    while ((Get-Date) -lt $deadline) {
        if ($port.BytesToRead -gt 0) {
            $chunk = $port.ReadExisting()
            [Console]::Out.Write($chunk)
        } else {
            Start-Sleep -Milliseconds 50
        }
    }
} finally {
    if ($port.IsOpen) { $port.Close() }
    $port.Dispose()
}
