# Codec backend is a Kconfig CHOICE (see Kconfig.projbuild). Exactly one
# of codec_lc3.c / codec_codec2.c implements codec.h per build; the
# unselected file would produce duplicate-symbol link errors so we
# include only the chosen one.
if(CONFIG_BIKE_CODEC_CODEC2)
    set(CODEC_SRCS "codec_codec2.c" "resampler_16_8.c")
else()
    set(CODEC_SRCS "codec_lc3.c")
endif()

idf_component_register(
    SRCS
        "main.c"
        "audio_pipeline.c"
        ${CODEC_SRCS}
        "mesh_mac.c"
        "mesh_proto.c"
        "mesh_crypto.c"
        "mixer.c"
        "mixer_jb.c"
        "bt_classic.c"
        "session_fsm.c"
        "ui.c"
        "nvs_cfg.c"
        "coex.c"
    INCLUDE_DIRS "."
    REQUIRES
        nvs_flash
        esp_wifi
        esp_coex
        bt
        driver
        esp_adc
        esp_driver_gpio
        esp_driver_i2c
        esp_driver_i2s
        esp_event
        esp_timer
        liblc3
        codec2
        esp_codec_dev
        webrtc_ns
        mbedtls
)
