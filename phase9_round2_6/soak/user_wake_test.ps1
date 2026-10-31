# User wake test monitor
$env:IDF_PATH = "D:\Program\esp\.espressif\v5.5.4\esp-idf"
$env:PATH = "D:\Program\esp\.espressif\python_env\idf5.5_py3.10_env\Scripts;$env:PATH"

$logFile = "phase9_round2_6/soak/user_wake_test_log.txt"
$startTime = Get-Date

# Start monitoring - run indefinitely until user stops
python -m esp_idf_monitor -p COM8 --print_filter '*:I' 2>&1 | ForEach-Object {
    $line = $_
    $elapsed = (Get-Date) - $startTime
    
    # Log all lines with timestamp
    $timestamp = $elapsed.ToString("mm\:ss")
    Add-Content -Path $logFile -Value "[$timestamp] $line"
}
