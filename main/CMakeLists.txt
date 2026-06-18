idf_component_register(
SRCS "comfort.c" "ui_bmbt.c" "ui_mid.c" "ui_mir.c" "ui_cd53.c" "tel.c" "cdc.c" "ibus.c" "ibus_config.c" "orangebus.c" "audio_output.c" "avrcp_controller.c" "a2dp_sink.c" "hfp_client.c" "bt_manager.c" "cli.c" "eq_processor.c" "spp_server.c"
PRIV_REQUIRES bt nvs_flash driver esp_driver_gpio esp_driver_i2s esp_driver_uart
INCLUDE_DIRS ".")
