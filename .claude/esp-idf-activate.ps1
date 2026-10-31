# ESP-IDF Environment Activation for Claude Code
# Auto-detects ESP-IDF installation. No hardcoded paths required.
#
# Expected installation layout (ESP-IDF Windows Installer):
#   {IDF_TOOLS_PATH}/
#   ├── python_env/{idfNN_pyM.N_env}/    Python venv
#   └── v{version}/
#       ├── esp-idf/                      IDF_PATH
#       └── tools/                        Toolchain binaries
#
# Detection order for IDF_PATH:
#   1. Already set in environment
#   2. Windows registry (set by ESP-IDF Windows Installer)
#   3. Default fallback: D:\Program\esp\.espressif\v5.5.4\esp-idf

# --- Locate IDF_PATH ---

if (-not $env:IDF_PATH) {
    $regKey = 'HKCU:\SOFTWARE\Espressif\ESP-IDF'
    if (Test-Path $regKey) {
        $regVal = (Get-ItemProperty $regKey -ErrorAction SilentlyContinue).'IDF_PATH'
        if ($regVal -and (Test-Path "$regVal\tools\idf.py")) {
            $env:IDF_PATH = $regVal
        }
    }
}

if (-not $env:IDF_PATH) {
    $defaultPath = 'D:\Program\esp\.espressif\v5.5.4\esp-idf'
    if (Test-Path "$defaultPath\tools\idf.py") {
        $env:IDF_PATH = $defaultPath
    } else {
        Write-Error "ESP-IDF not found. Set IDF_PATH or run the ESP-IDF installer."
        exit 1
    }
}

# --- Derive IDF_TOOLS_PATH and version ---

$idfDir = Split-Path $env:IDF_PATH          # ...\v5.5.4
$version = Split-Path $idfDir -Leaf         # v5.5.4

if (-not $env:IDF_TOOLS_PATH) {
    $candidate = Split-Path $idfDir         # ...\.espressif
    if (Test-Path "$candidate\python_env") {
        $env:IDF_TOOLS_PATH = $candidate
    }
}

$toolsRoot = "$idfDir\tools"

# --- Locate Python venv ---

if (-not $env:IDF_PYTHON_ENV_PATH -and $env:IDF_TOOLS_PATH) {
    $pyBase = "$($env:IDF_TOOLS_PATH)\python_env"
    if (Test-Path $pyBase) {
        $pyDirs = [System.IO.Directory]::GetDirectories($pyBase)
        $pyMatch = $pyDirs | Where-Object { (Split-Path $_ -Leaf) -match 'idf.*_py' } |
                   Sort-Object { [System.IO.Directory]::GetLastWriteTime($_) } -Descending |
                   Select-Object -First 1
        if ($pyMatch) {
            $env:IDF_PYTHON_ENV_PATH = $pyMatch
        }
    }
}

# --- Build PATH ---

$espPaths = @()

if ($env:IDF_PYTHON_ENV_PATH -and (Test-Path "$($env:IDF_PYTHON_ENV_PATH)\Scripts")) {
    $espPaths += "$($env:IDF_PYTHON_ENV_PATH)\Scripts"
}

if (Test-Path $toolsRoot) {
    # Auto-discover tool directories under toolsRoot.
    # Uses .NET APIs for reliable directory enumeration (avoids Get-ChildItem issues
    # when running from Git Bash/MSYS2).
    #
    # ESP-IDF tools use three layouts:
    #   1. tools/{name}/{version}/bin/              (cmake)
    #   2. tools/{name}/{version}/{name}/bin/       (xtensa-esp-elf, riscv32-esp-elf)
    #   3. tools/{name}/{version}/{name}.exe        (ninja)

    $toolBaseDirs = [System.IO.Directory]::GetDirectories($toolsRoot)
    foreach ($toolDir in $toolBaseDirs) {
        $versionDirs = [System.IO.Directory]::GetDirectories($toolDir) |
                       Sort-Object -Descending
        foreach ($verDir in $versionDirs) {
            # 1. tools/{name}/{version}/bin/
            $binPath = Join-Path $verDir 'bin'
            if (Test-Path $binPath) {
                $espPaths += $binPath
                break
            }
            # 2. tools/{name}/{version}/{name}/bin/
            $nestedDirs = [System.IO.Directory]::GetDirectories($verDir)
            if ($nestedDirs.Count -gt 0) {
                $nestedBin = Join-Path $nestedDirs[0] 'bin'
                if (Test-Path $nestedBin) {
                    $espPaths += $nestedBin
                    break
                }
            }
            # 3. tools/{name}/{version}/*.exe  (flat layout, e.g. ninja)
            $exes = [System.IO.Directory]::GetFiles($verDir, '*.exe')
            if ($exes.Count -gt 0) {
                $espPaths += $verDir
                break
            }
        }
    }
}

if ($espPaths.Count -gt 0) {
    $env:PATH = (($espPaths -join ';') + ';' +
        (($env:PATH -split ';') | Where-Object { $_ -notmatch 'MiniConda|Miniconda' }) -join ';')
}
