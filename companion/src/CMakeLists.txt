FILE(GLOB_RECURSE app_sources ${CMAKE_SOURCE_DIR}/src/*.*)

idf_component_register(
    SRCS ${app_sources}
    REQUIRES esp_wifi nvs_flash esp_netif esp_timer
)
