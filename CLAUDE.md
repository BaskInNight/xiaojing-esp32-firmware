# CLAUDE.md — ESP32-IDF-Template

## Project Overview

ESP32 development template based on ESP-IDF v5.5.4. Used as a base for peripheral testing and application development.

Target chip: ESP32-S3 (configurable via `idf.py menuconfig`).

## Project Structure

```
.
├── main/           Application entry (app_main.c)
├── components/     Reusable drivers and modules
├── docs/           Design documentation
├── .claude/        Claude Code hook (auto-activates ESP-IDF env)
├── CMakeLists.txt  Top-level build config
└── sdkconfig       Build configuration
```

## Development Rules

### Before Modifying Code

1. Read existing files to understand the current structure
2. Check `components/` for reusable modules before writing new code
3. Check `docs/` for hardware and architecture context

### Code Placement

- **`main/app_main.c`**: Application logic only (task creation, state machines, initialization flow)
- **`components/`**: Hardware drivers, protocol implementations, reusable modules
- Each component has its own `CMakeLists.txt`, source files, and `include/` directory

### ESP-IDF Conventions

- Use official ESP-IDF APIs (`esp_log.h`, `driver/`, `esp_system.h`)
- Use `ESP_LOGI/W/E` for logging, never `printf`
- Use `esp_err_t` return values for error handling
- Include `freertos/` headers for task/delay functions
- Prefer ESP-IDF component dependencies over direct `#include` of absolute paths

### Build Verification

- Run `idf.py build` after every code modification
- Report build result (success/failure + any warnings)
- If build fails, fix the issue before proceeding

### Modifying Components

- Do not change a component's public API without discussing the impact first
- Update the component's `README.md` if behavior changes
- Keep component interfaces minimal and well-defined

### Committing Changes

- Run `idf.py build` and confirm success before committing
- Use descriptive commit messages (what changed and why)
- Do not commit build artifacts (`build/`, `managed_components/`)
