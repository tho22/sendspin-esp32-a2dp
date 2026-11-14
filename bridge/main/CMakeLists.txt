# The A2DP output is shared with the single-chip native firmware
idf_component_register(
    SRCS "main.cpp" "../../native/main/a2dp_output.cpp"
    INCLUDE_DIRS "." "../../native/main"
    REQUIRES bt nvs_flash esp_ringbuf esp_timer esp_driver_i2s esp_driver_uart esp_driver_gpio
)
