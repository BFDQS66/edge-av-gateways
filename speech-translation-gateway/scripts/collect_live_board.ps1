param(
    [string]$LogDir = 'E:\orange Pi 5 Pro\log',
    [int]$IntervalSeconds = 3,
    [switch]$Once
)

$ErrorActionPreference = 'Stop'
$createdNew = $false
$instanceMutex = [System.Threading.Mutex]::new($false, 'Local\OrangePiSpeechLiveCollector', [ref]$createdNew)
if (-not $createdNew) { exit 0 }
$utf8 = New-Object System.Text.UTF8Encoding($false)
[Console]::OutputEncoding = $utf8
$OutputEncoding = $utf8
New-Item -ItemType Directory -Force -Path $LogDir | Out-Null
$cursorPath = Join-Path $LogDir 'speech-live-cursor.json'
$instance = ''
$sequence = 0
if (Test-Path -LiteralPath $cursorPath) {
    try {
        $cursor = Get-Content -LiteralPath $cursorPath -Raw | ConvertFrom-Json
        $instance = [string]$cursor.instance_id
        $sequence = [int]$cursor.seq
    } catch {
        $instance = ''
        $sequence = 0
    }
}
$lastError = ''

while ($true) {
    $now = Get-Date
    $path = Join-Path $LogDir ('speech-live-events-{0}.jsonl' -f $now.ToString('yyyy-MM-dd'))
    try {
        $command = "curl -fsS --max-time 5 'http://127.0.0.1:8770/api/state?since=$sequence'"
        $raw = & ssh.exe -F none -o BatchMode=yes -o ConnectTimeout=5 orangepi@192.168.1.101 $command 2>&1
        if ($LASTEXITCODE -ne 0) { throw "ssh exit $LASTEXITCODE`: $raw" }
        try { $state = ($raw -join "`n") | ConvertFrom-Json }
        catch { throw 'invalid board JSON (check SSH UTF-8 decoding)' }
        if ($instance -ne [string]$state.instance_id) {
            $instance = [string]$state.instance_id
            $sequence = 0
            $raw = & ssh.exe -F none -o BatchMode=yes -o ConnectTimeout=5 orangepi@192.168.1.101 "curl -fsS --max-time 5 'http://127.0.0.1:8770/api/state?since=0'" 2>&1
            if ($LASTEXITCODE -ne 0) { throw "ssh exit $LASTEXITCODE`: $raw" }
            try { $state = ($raw -join "`n") | ConvertFrom-Json }
            catch { throw 'invalid board JSON (check SSH UTF-8 decoding)' }
            $instance = [string]$state.instance_id
        }
        foreach ($event in $state.events) {
            if ([int]$event.seq -le $sequence) { continue }
            $entry = [ordered]@{ windows_time = (Get-Date).ToString('o'); instance_id = $instance; board = $event }
            ($entry | ConvertTo-Json -Depth 8 -Compress) | Add-Content -LiteralPath $path -Encoding utf8
            $sequence = [int]$event.seq
        }
        if ([int]$state.seq -gt $sequence) { $sequence = [int]$state.seq }
        @{ instance_id = $instance; seq = $sequence } | ConvertTo-Json -Compress |
            Set-Content -LiteralPath $cursorPath -Encoding utf8
        if ($lastError) {
            ([ordered]@{ windows_time = (Get-Date).ToString('o'); status = 'recovered'; error = $lastError } |
                ConvertTo-Json -Compress) | Add-Content -LiteralPath $path -Encoding utf8
            $lastError = ''
        }
    } catch {
        $errorText = $_.Exception.Message
        if ($errorText.Length -gt 250) { $errorText = $errorText.Substring(0, 250) }
        if ($errorText -ne $lastError) {
            ([ordered]@{ windows_time = (Get-Date).ToString('o'); status = 'collector_error'; error = $errorText } |
                ConvertTo-Json -Compress) | Add-Content -LiteralPath $path -Encoding utf8
            $lastError = $errorText
        }
    }
    if ($Once) { break }
    Start-Sleep -Seconds ([Math]::Max(1, $IntervalSeconds))
}
