#pragma once

/*
 * Copy to voice_secrets.h (ignored by git) when credentials are available.
 * AI Gateway authentication is a single API key. The default routed model
 * is mimo-v2.5, so this is the only field normally required.
 */
#define XIAOJING_VOICE_PROVIDER VOICE_PROVIDER_MIMO_DIRECT
#define XIAOJING_VOICE_API_KEY  ""
/* Optional only when the console exposes another routed model alias. */
/* #define XIAOJING_VOICE_MODEL "your-console-model-alias" */

/* MiMo direct alternative:
 * #define XIAOJING_VOICE_PROVIDER VOICE_PROVIDER_ESPRESSIF_AI_GATEWAY
 * #define XIAOJING_VOICE_API_KEY  "..."
 * #define XIAOJING_VOICE_MODEL    "mimo-v2.5" // optional
 */
