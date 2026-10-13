include($ENV{IDF_PATH}/tools/cmake/version.cmake)
set(bench_requires esp_wifi esp_event esp_netif esp_http_server esp_timer esp_driver_uart nvs_flash)
# IDF 6 moved cJSON to the Component Registry; the manifest adds it there.
if(IDF_VERSION_MAJOR LESS 6)
    list(APPEND bench_requires json)
endif()

idf_component_register(SRCS "main.c" "radar.c" "protocol.c"
    INCLUDE_DIRS "."
    REQUIRES ${bench_requires}
    EMBED_TXTFILES "web.html")
