#include "voice_prompt_audio.h"

#include <string.h>

/* Embedded WAV symbols from EMBED_FILES in CMakeLists.txt. */
extern const uint8_t accepted_start[] asm("_binary_accepted_wav_start");
extern const uint8_t accepted_end[] asm("_binary_accepted_wav_end");
extern const uint8_t rejected_start[] asm("_binary_rejected_wav_start");
extern const uint8_t rejected_end[] asm("_binary_rejected_wav_end");
extern const uint8_t error_start[] asm("_binary_error_wav_start");
extern const uint8_t error_end[] asm("_binary_error_wav_end");
extern const uint8_t backend_xiaojing_start[] asm("_binary_backend_xiaojing_wav_start");
extern const uint8_t backend_xiaojing_end[] asm("_binary_backend_xiaojing_wav_end");
extern const uint8_t backend_esp_sr_start[] asm("_binary_backend_esp_sr_wav_start");
extern const uint8_t backend_esp_sr_end[] asm("_binary_backend_esp_sr_wav_end");
extern const uint8_t ptt_ready_start[] asm("_binary_ptt_ready_wav_start");
extern const uint8_t ptt_ready_end[] asm("_binary_ptt_ready_wav_end");
extern const uint8_t chat_thinking_start[] asm("_binary_chat_thinking_wav_start");
extern const uint8_t chat_thinking_end[] asm("_binary_chat_thinking_wav_end");
extern const uint8_t need_confirmation_start[] asm("_binary_need_confirmation_wav_start");
extern const uint8_t need_confirmation_end[] asm("_binary_need_confirmation_wav_end");
extern const uint8_t network_unavailable_start[] asm("_binary_network_unavailable_wav_start");
extern const uint8_t network_unavailable_end[] asm("_binary_network_unavailable_wav_end");
extern const uint8_t task_done_start[] asm("_binary_task_done_wav_start");
extern const uint8_t task_done_end[] asm("_binary_task_done_wav_end");

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Robust RIFF/WAV parser that correctly traverses chunks. */
static bool parse(const uint8_t *start, const uint8_t *end,
                  voice_prompt_pcm_t *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));

    size_t length = (size_t)(end - start);
    if (length < 12U) return false;

    /* Check RIFF/WAVE header */
    if (memcmp(start, "RIFF", 4) != 0 || memcmp(start + 8, "WAVE", 4) != 0)
        return false;

    /* Parse chunks */
    bool fmt_found = false;
    bool data_found = false;
    uint16_t format = 0, channels = 0, bits = 0;
    uint32_t sample_rate = 0;
    const uint8_t *pcm_data = NULL;
    uint32_t pcm_bytes = 0;

    size_t offset = 12;
    while (offset + 8 <= length) {
        uint32_t chunk_id = le32(start + offset);
        uint32_t chunk_size = le32(start + offset + 4);
        size_t chunk_data_start = offset + 8;

        /* Check for integer overflow */
        if (chunk_data_start + chunk_size < chunk_data_start)
            return false;

        /* Check chunk doesn't exceed file */
        if (chunk_data_start + chunk_size > length)
            return false;

        if (chunk_id == 0x20746D66U) { /* "fmt " */
            if (chunk_size < 16U) return false;
            format = le16(start + chunk_data_start);
            channels = le16(start + chunk_data_start + 2U);
            sample_rate = le32(start + chunk_data_start + 4U);
            bits = le16(start + chunk_data_start + 14U);
            fmt_found = true;
        } else if (chunk_id == 0x61746164U) { /* "data" */
            pcm_data = start + chunk_data_start;
            pcm_bytes = chunk_size;
            data_found = true;
        }

        /* Move to next chunk (align to even boundary) */
        offset = chunk_data_start + chunk_size + (chunk_size & 1U);

        /* If both fmt and data found, we can stop early */
        if (fmt_found && data_found) break;
    }

    /* Validate format */
    if (!fmt_found || !data_found) return false;
    if (format != 1U) return false;        /* PCM */
    if (channels != 1U) return false;      /* Mono */
    if (bits != 16U) return false;         /* 16-bit */
    if (sample_rate != 24000U) return false; /* 24kHz */

    /* Validate data */
    if (pcm_bytes == 0U || (pcm_bytes & 1U) != 0U) return false;
    if ((size_t)(pcm_data - start) + pcm_bytes > length) return false;

    out->pcm = (const int16_t *)pcm_data;
    out->pcm_bytes = pcm_bytes;
    out->sample_rate = sample_rate;
    out->duration_ms = (uint32_t)(((uint64_t)pcm_bytes * 1000U) /
                                  ((uint64_t)sample_rate * 2U));
    return true;
}

/* Asset mapping table — each prompt ID maps to exactly one embedded WAV. */
typedef struct {
    voice_prompt_id_t id;
    const uint8_t *start;
    const uint8_t *end;
} prompt_map_entry_t;

static const prompt_map_entry_t PROMPT_MAP[] = {
    {VOICE_PROMPT_ACCEPTED,           accepted_start,            accepted_end},
    {VOICE_PROMPT_REJECTED,           rejected_start,            rejected_end},
    {VOICE_PROMPT_ERROR,              error_start,               error_end},
    {VOICE_PROMPT_BACKEND_EDGE,       backend_xiaojing_start,    backend_xiaojing_end},
    {VOICE_PROMPT_BACKEND_ESP_SR,     backend_esp_sr_start,      backend_esp_sr_end},
    {VOICE_PROMPT_PTT_READY,          ptt_ready_start,           ptt_ready_end},
    {VOICE_PROMPT_CHAT_THINKING,      chat_thinking_start,       chat_thinking_end},
    {VOICE_PROMPT_NEED_CONFIRMATION,  need_confirmation_start,   need_confirmation_end},
    {VOICE_PROMPT_NETWORK_UNAVAILABLE,network_unavailable_start, network_unavailable_end},
};

#define PROMPT_MAP_SIZE (sizeof(PROMPT_MAP) / sizeof(PROMPT_MAP[0]))

bool voice_prompt_audio_get(voice_prompt_id_t id, voice_prompt_pcm_t *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));

    /* Validate ID range */
    if (id >= PROMPT_MAP_SIZE) return false;

    /* Find matching entry */
    for (size_t i = 0; i < PROMPT_MAP_SIZE; i++) {
        if (PROMPT_MAP[i].id == id) {
            return parse(PROMPT_MAP[i].start, PROMPT_MAP[i].end, out);
        }
    }

    return false;
}

/* Special accessor for task_done.wav (not mapped to any VOICE_PROMPT_* enum).
 * Used by voice_service for task completion notification. */
bool voice_prompt_audio_get_task_done(voice_prompt_pcm_t *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    return parse(task_done_start, task_done_end, out);
}
