option(VOICE_CLOUD_DIAGNOSTIC "Enable transport diagnostic (LAN + MiMo auto-test)" OFF)

idf_component_register(
    SRCS
        "voice_core.c"
        "voice_tool_parser.c"
        "voice_cloud.c"
        "voice_cloud_adapter.c"
        "voice_cloud_transport_idf.c"
        "voice_tts_idf.c"
        "voice_capture_idf.c"
        "voice_vad.c"
        "voice_profile.c"
        "voice_route.c"
        "voice_provider.c"
        "voice_prompt_catalog.c"
        "voice_prompt_audio.c"
        "voice_service.c"
        "voice_recorder.c"
    INCLUDE_DIRS "include"
    REQUIRES
        machine_types
        machine_config
        wash_planner
        wash_executor
        freertos
        esp_http_client
        esp_coex
        mbedtls
        esp_timer
    EMBED_FILES
        "assets/mimo_generated/accepted.wav"
        "assets/mimo_generated/rejected.wav"
        "assets/mimo_generated/error.wav"
        "assets/mimo_generated/backend_xiaojing.wav"
        "assets/mimo_generated/backend_esp_sr.wav"
        "assets/mimo_generated/ptt_ready.wav"
        "assets/mimo_generated/chat_thinking.wav"
        "assets/mimo_generated/need_confirmation.wav"
        "assets/mimo_generated/network_unavailable.wav"
        "assets/legacy/task_done.wav"
)

if(VOICE_CLOUD_DIAGNOSTIC)
    target_compile_definitions(${COMPONENT_LIB} PUBLIC VOICE_CLOUD_DIAGNOSTIC_MODE=1)
    message(STATUS "voice_agent: VOICE_CLOUD_DIAGNOSTIC enabled")
endif()
