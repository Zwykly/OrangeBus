idf_component_register(
    SRCS "bluebus.c" "audio_output.c" "avrcp_controller.c" "a2dp_sink.c" "hfp_client.c" "bt_manager.c" "cli.c"
    PRIV_REQUIRES bt nvs_flash driver esp_driver_gpio esp_driver_i2s
    INCLUDE_DIRS ".")
