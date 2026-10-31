# False wake monitor - runs for 10+ minutes
$env:IDF_PATH = "D:\Program\esp\.espressif\v5.5.4\esp-idf"
$env:PATH = "D:\Program\esp\.espressif\python_env\idf5.5_py3.10_env\Scripts;$env:PATH"

$startTime = Get-Date
$duration = 660  # 11 minutes
$logFile = "phase9_round2_6/soak/false_wake_log.txt"

# Start monitoring
python -m esp_idf_monitor -p COM8 --print_filter '*:I' 2>&1 | ForEach-Object {
    $line = $_
    $elapsed = (Get-Date) - $startTime
    
    # Log all lines
    Add-Content -Path $logFile -Value "[$([int]$elapsed.TotalSeconds)s] $line"
    
    # Check for wake detection
    if ($line -match "DETECTED xiao_jing") {
        Add-Content -Path $logFile -Value "FALSE_WAKE_DETECTED at $([int]$elapsed.TotalSeconds)s"
    }
    
    # Stop after duration
    if ($elapsed.TotalSeconds -ge $duration) {
        break
    }
}
