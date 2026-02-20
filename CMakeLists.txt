idf_component_register(
    SRCS "AP_DS18B20.cpp"
    INCLUDE_DIRS "."
    REQUIRES onewire_bus ds18b20 esp_timer esp_log
)
