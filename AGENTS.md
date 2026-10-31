# AGENTS.md — AI Agent Development Guide

## Project

ESP32-IDF-Template — ESP-IDF v5.5.4 development template for ESP32-S3.

## Architecture

```
main/           Application entry point (app_main.c)
components/     Reusable drivers and modules
docs/           Design documentation (hardware, architecture, pin map)
.claude/        Claude Code environment hook (do not modify)
```

## Code Placement Rules

| Location | Purpose |
|----------|---------|
| `main/` | Application logic, task creation, state machines |
| `components/` | Hardware drivers, protocol libraries, reusable modules |

Each component structure:
```
components/xxx/
├── CMakeLists.txt
├── include/xxx.h
└── xxx.c
```

## Development Principles

1. **Read before write** — understand existing code structure before modifying
2. **Use ESP-IDF APIs** — prefer official drivers (`driver/`, `esp_system.h`) over raw register access
3. **Log with ESP_LOG** — use `ESP_LOGI/W/E(TAG, ...)`, not `printf`
4. **Handle errors** — check `esp_err_t` returns, log failures
5. **Keep main lean** — `app_main()` should initialize and delegate, not contain hardware logic
6. **Isolate drivers** — put reusable hardware code in `components/`, not `main/`

## Build

```bash
idf.py build
```

Must pass before any commit. Report result.

## Git Conventions

- One logical change per commit
- Commit message format: `<type>: <description>`
  - Types: `feat`, `fix`, `refactor`, `docs`, `chore`
- Do not commit: `build/`, `managed_components/`, `sdkconfig.old`, `dependencies.lock`
- Run `idf.py build` before committing code changes

## Documentation

- `docs/hardware.md` — GPIO assignments, power supply, wiring
- `docs/software_arch.md` — software architecture, component relationships
- `docs/pin_map.md` — GPIO resource table
- Update docs when changing hardware interfaces or architecture

## Safety

- Do not modify `.claude/settings.local.json` or `.claude/esp-idf-activate.ps1`
- Do not modify `sdkconfig` directly — use `idf.py menuconfig`
- Do not remove `idf_build_set_property` or CMake boilerplate from root `CMakeLists.txt`
