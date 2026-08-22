#include "board_config.h"
#include "sdkconfig.h"

xiaojing_output_mode_t board_config_get_output_mode(void)
{
#if defined(CONFIG_XIAOJING_OUTPUT_MODE_FAKE)
    return XIAOJING_MODE_FAKE;
#elif defined(CONFIG_XIAOJING_OUTPUT_MODE_LOW_VOLTAGE)
    return XIAOJING_MODE_LOW_VOLTAGE;
#elif defined(CONFIG_XIAOJING_OUTPUT_MODE_REAL)
    return XIAOJING_MODE_REAL;
#else
    return XIAOJING_MODE_FAKE;
#endif
}

bool board_config_is_ptc_enabled(void)
{
#ifdef CONFIG_XIAOJING_PTC_ENABLED
    return true;
#else
    return false;
#endif
}

bool board_config_is_hot_air_module_enabled(void)
{
#ifdef CONFIG_XIAOJING_HOT_AIR_MODULE_ENABLED
    return true;
#else
    return false;
#endif
}

bool board_config_is_voice_enabled(void)
{
#ifdef CONFIG_XIAOJING_VOICE_ENABLED
    return true;
#else
    return false;
#endif
}

bool board_config_is_display_enabled(void)
{
#ifdef CONFIG_XIAOJING_DISPLAY_ENABLED
    return true;
#else
    return false;
#endif
}

bool board_config_is_legacy_ble_enabled(void)
{
#ifdef CONFIG_XIAOJING_LEGACY_BLE
    return true;
#else
    return false;
#endif
}

bool board_config_is_board_identity_confirmed(void)
{
#ifdef CONFIG_XIAOJING_BOARD_IDENTITY_CONFIRMED
    return true;
#else
    return false;
#endif
}

const char *board_config_get_output_mode_str(void)
{
    switch (board_config_get_output_mode()) {
    case XIAOJING_MODE_FAKE:         return "FAKE";
    case XIAOJING_MODE_LOW_VOLTAGE:  return "LOW_VOLTAGE";
    case XIAOJING_MODE_REAL:         return "REAL";
    default:                         return "UNKNOWN";
    }
}
