param(
    [string]$LogDir = 'E:\orange Pi 5 Pro\log',
    [int]$IntervalSeconds = 60,
    [switch]$Once
)

$ErrorActionPreference = 'Stop'
$createdNew = $false
$instanceMutex = [System.Threading.Mutex]::new($false, 'Local\OrangePiSpeechHealthMonitor', [ref]$createdNew)
if (-not $createdNew) { exit 0 }
New-Item -ItemType Directory -Force -Path $LogDir | Out-Null
while ($true) {
    $now = Get-Date
    $path = Join-Path $LogDir ('speech-board-health-{0}.jsonl' -f $now.ToString('yyyy-MM-dd'))
    try {
        $result = & ssh.exe -F none -o BatchMode=yes -o ConnectTimeout=8 orangepi@192.168.1.101 'python3 ~/workspace/speech-translation-gateway/scripts/board_status.py' 2>&1
        if ($LASTEXITCODE -ne 0) { throw "ssh exit $LASTEXITCODE`: $result" }
        $snapshot = $result | ConvertFrom-Json
        $entry = [ordered]@{ windows_time = $now.ToString('o'); status = 'ok'; board = $snapshot }
    } catch {
        $entry = [ordered]@{ windows_time = $now.ToString('o'); status = 'error'; error = $_.Exception.Message }
    }
    ($entry | ConvertTo-Json -Depth 12 -Compress) | Add-Content -LiteralPath $path -Encoding utf8
    if ($Once) { break }
    Start-Sleep -Seconds ([Math]::Max(5, $IntervalSeconds))
}
