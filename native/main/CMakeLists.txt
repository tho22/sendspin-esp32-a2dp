idf_component_register(
    SRCS "main.cpp" "a2dp_output.cpp"
    INCLUDE_DIRS "."
    REQUIRES bt esp_wifi esp_netif esp_event esp_timer esp_coex nvs_flash esp_ringbuf
)
