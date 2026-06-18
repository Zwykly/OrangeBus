idf_component_register(
SRCS
    "orangebus.c"
    "ibus/ibus.c"
    "ibus/ibus_config.c"
    "bt/bt_manager.c"
    "bt/a2dp_sink.c"
    "bt/avrcp_controller.c"
    "bt/hfp_client.c"
    "bt/spp_server.c"
    "audio/audio_output.c"
    "audio/eq_processor.c"
    "ui/ui_bmbt.c"
    "ui/ui_cd53.c"
    "ui/ui_mid.c"
    "ui/ui_mir.c"
    "emulator/cdc.c"
    "emulator/tel.c"
    "comfort/comfort.c"
    "cli/cli.c"
PRIV_REQUIRES bt nvs_flash driver esp_driver_gpio esp_driver_i2s esp_driver_uart
INCLUDE_DIRS "." "board" "common" "ibus" "bt" "audio" "ui" "emulator" "comfort" "cli")
