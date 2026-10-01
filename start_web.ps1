param(
    [string]$Url = "http://127.0.0.1:8080/",
    [int]$TimeoutSec = 15,
    [switch]$NoBrowser
)

# The batch file keeps RippleTrace.exe in the foreground. This short-lived helper
# only waits for the listener and opens the browser, so stopping the server
# cannot leave a child server process behind.
$ErrorActionPreference = "SilentlyContinue"
$deadline = (Get-Date).AddSeconds($TimeoutSec)
$healthUrl = $Url.TrimEnd('/') + "/api/health"

while ((Get-Date) -lt $deadline) {
    try {
        $response = Invoke-RestMethod -Uri $healthUrl -TimeoutSec 2
        if ($response.ok -eq $true) {
            if (-not $NoBrowser) {
                Start-Process $Url | Out-Null
            }
            exit 0
        }
    } catch {
        # The server may still be binding its socket.
    }
    Start-Sleep -Milliseconds 250
}

Write-Host "[RippleTrace] server was not ready in $TimeoutSec seconds; open $Url manually."
exit 1
