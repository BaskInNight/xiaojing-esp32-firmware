# Components

Reusable modules for this ESP-IDF project.

## Component Structure

```
components/
└── xxx/
    ├── CMakeLists.txt
    ├── include/
    │   └── xxx.h
    ├── xxx.c
    └── README.md
```

## Creating a New Component

1. Create a directory under `components/`:
   ```bash
   mkdir -p components/my_driver/include
   ```

2. Create `components/my_driver/CMakeLists.txt`:
   ```cmake
   idf_component_register(SRCS "my_driver.c"
                          INCLUDE_DIRS "include")
   ```

3. Create the header in `include/my_driver.h` and source in `my_driver.c`.

4. Add any ESP-IDF component dependencies:
   ```cmake
   idf_component_register(SRCS "my_driver.c"
                          INCLUDE_DIRS "include"
                          PRIV_REQUIRES driver)
   ```

5. No changes needed in the top-level `CMakeLists.txt` — ESP-IDF auto-discovers components.

## When to Use `components/` vs `main/`

| Use `components/` | Use `main/` |
|--------------------|-------------|
| Hardware drivers (GPIO, SPI, I2C, UART) | Application initialization |
| Protocol implementations | Task creation and scheduling |
| Sensor/actuator abstractions | State machine logic |
| Reusable utility modules | Top-level control flow |
| Code shared across projects | Project-specific orchestration |

## Example: Minimal Component

**`components/led/include/led.h`**
```c
#pragma once
#include "esp_err.h"

esp_err_t led_init(gpio_num_t pin);
esp_err_t led_set(bool on);
```

**`components/led/CMakeLists.txt`**
```cmake
idf_component_register(SRCS "led.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES driver)
```
