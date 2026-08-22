#include "voice_prompt_catalog.h"

#include <stddef.h>

static const voice_prompt_asset_t CATALOG[] = {
    {VOICE_PROMPT_ACCEPTED,          "voice/accepted.wav",            2560,  80},
    {VOICE_PROMPT_REJECTED,          "voice/rejected.wav",            3200,  80},
    {VOICE_PROMPT_ERROR,             "voice/error.wav",               4000,  80},
    {VOICE_PROMPT_BACKEND_EDGE,      "voice/backend_xiaojing.wav",    3200,  80},
    {VOICE_PROMPT_BACKEND_ESP_SR,    "voice/backend_esp_sr.wav",      3200,  80},
    {VOICE_PROMPT_PTT_READY,         "voice/ptt_ready.wav",           1760,  80},
    {VOICE_PROMPT_CHAT_THINKING,     "voice/chat_thinking.wav",       2560,  80},
    {VOICE_PROMPT_NEED_CONFIRMATION, "voice/need_confirmation.wav",   3840,  80},
    {VOICE_PROMPT_NETWORK_UNAVAILABLE,
                                      "voice/network_unavailable.wav", 3680,  80},
};

const voice_prompt_asset_t *voice_prompt_catalog_get(
    voice_prompt_id_t id)
{
    for (size_t i = 0; i < sizeof(CATALOG) / sizeof(CATALOG[0]); i++) {
        if (CATALOG[i].id == id) return &CATALOG[i];
    }
    return NULL;
}
