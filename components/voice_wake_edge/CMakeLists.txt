set(EI_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/ei-sdk/edge-impulse-sdk")
set(EI_SDK_PARENT "${CMAKE_CURRENT_SOURCE_DIR}/ei-sdk")
set(EI_MODEL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/model")

# Collect Edge Impulse SDK sources (only what's needed for compiled/EON model)
file(GLOB_RECURSE EI_SDK_SRCS
    "${EI_SDK_DIR}/tensorflow/lite/micro/*.cc"
    "${EI_SDK_DIR}/tensorflow/lite/micro/*.cpp"
    "${EI_SDK_DIR}/tensorflow/lite/core/api/*.cc"
    "${EI_SDK_DIR}/tensorflow/lite/core/api/*.cpp"
    "${EI_SDK_DIR}/tensorflow/lite/kernels/*.cc"
    "${EI_SDK_DIR}/tensorflow/lite/kernels/*.cpp"
    "${EI_SDK_DIR}/tensorflow/lite/c/*.c"
    "${EI_SDK_DIR}/CMSIS/DSP/Source/BasicMathFunctions/*.c"
    "${EI_SDK_DIR}/CMSIS/DSP/Source/CommonTables/*.c"
    "${EI_SDK_DIR}/CMSIS/DSP/Source/ComplexMathFunctions/*.c"
    "${EI_SDK_DIR}/CMSIS/DSP/Source/FastMathFunctions/*.c"
    "${EI_SDK_DIR}/CMSIS/DSP/Source/MatrixFunctions/*.c"
    "${EI_SDK_DIR}/CMSIS/DSP/Source/StatisticsFunctions/*.c"
    "${EI_SDK_DIR}/CMSIS/DSP/Source/TransformFunctions/*.c"
    "${EI_SDK_DIR}/CMSIS/DSP/Source/SupportFunctions/*.c"
    "${EI_SDK_DIR}/CMSIS/NN/Source/ActivationFunctions/*.c"
    "${EI_SDK_DIR}/CMSIS/NN/Source/BasicMathFunctions/*.c"
    "${EI_SDK_DIR}/CMSIS/NN/Source/ConcatenationFunctions/*.c"
    "${EI_SDK_DIR}/CMSIS/NN/Source/ConvolutionFunctions/*.c"
    "${EI_SDK_DIR}/CMSIS/NN/Source/FullyConnectedFunctions/*.c"
    "${EI_SDK_DIR}/CMSIS/NN/Source/NNSupportFunctions/*.c"
    "${EI_SDK_DIR}/CMSIS/NN/Source/PoolingFunctions/*.c"
    "${EI_SDK_DIR}/CMSIS/NN/Source/ReshapeFunctions/*.c"
    "${EI_SDK_DIR}/CMSIS/NN/Source/SoftmaxFunctions/*.c"
    "${EI_SDK_DIR}/CMSIS/NN/Source/SVDFunctions/*.c"
    "${EI_SDK_DIR}/porting/espressif/esp-dsp/modules/fft/esp_fft_impl.c"
    "${EI_SDK_DIR}/porting/espressif/esp-dsp/modules/fft/dsps_fft2r_fc32_ansi.c"
    "${EI_SDK_DIR}/porting/espressif/esp-dsp/modules/fft/dsps_fft2r_sc16_ansi.c"
    "${EI_SDK_DIR}/porting/espressif/esp-dsp/modules/fft/dsps_bit_rev_fc32_ansi.c"
    "${EI_SDK_DIR}/porting/espressif/esp-dsp/modules/fft/dsps_bit_rev_sc16_ansi.c"
    # NOTE: ei_classifier_porting.cpp from SDK is NOT included;
    # our custom ei_porting_esp32.cpp replaces it with PSRAM-aware allocators.
)

# Model source
set(EI_MODEL_SRCS
    "${EI_MODEL_DIR}/tflite_learn_982269_4_compiled.cpp"
)

# Wrapper source (custom porting replaces SDK's porting layer)
set(WRAPPER_SRCS
    "src/voice_wake_edge.c"
    "src/ei_classifier_wrapper.cpp"
    "src/ei_porting_esp32.cpp"
)

idf_component_register(
    SRCS ${WRAPPER_SRCS} ${EI_MODEL_SRCS} ${EI_SDK_SRCS}
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS
        "${EI_SDK_PARENT}"
        "${EI_SDK_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/model-parameters"
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${EI_SDK_DIR}/porting/espressif/esp-dsp/modules/common/include"
        "${EI_SDK_DIR}/porting/espressif/esp-dsp/modules/fft/include"
        "${EI_SDK_DIR}/classifier"
        "${EI_SDK_DIR}/CMSIS/Core/Include"
        "${EI_SDK_DIR}/CMSIS/DSP/Include"
        "${EI_SDK_DIR}/CMSIS/DSP/PrivateInclude"
        "${EI_SDK_DIR}/CMSIS/NN/Include"
        "${EI_SDK_DIR}/porting"
        "${EI_SDK_DIR}/tensorflow"
        "${EI_SDK_DIR}/tensorflow/lite"
        "${EI_SDK_DIR}/tensorflow/lite/micro"
        "${EI_SDK_DIR}/tensorflow/lite/micro/kernels"
        "${EI_SDK_DIR}/tensorflow/lite/micro/memory_planner"
        "${EI_SDK_DIR}/tensorflow/lite/c"
        "${EI_MODEL_DIR}"
    REQUIRES
        voice_frontend
        freertos
        esp_timer
        heap
)

# Compile Edge Impulse SDK sources separately (C++ required)
# Note: We only compile the wrapper and model, not the full SDK,
# because the model is compiled (EON) and has fewer dependencies.
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-missing-field-initializers
    -Wno-unused-parameter
    -Wno-sign-compare
    -Wno-type-limits
    -Wno-implicit-fallthrough
    -Wno-maybe-uninitialized
    -Wno-deprecated-declarations
    -Wno-unused-variable
    -Wno-unused-function
)

# Suppress Arduino include for ESP-IDF
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    ARDUINO_H=
)
