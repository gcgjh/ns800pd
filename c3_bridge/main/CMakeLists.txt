# IDF v6.x 把 "driver" 拆成了 esp_driver_uart / esp_driver_gpio 等细粒度组件，
# 必须按实际用到的头文件逐个声明，写 "driver" 会在编译期报 component requirements 错误。
idf_component_register(SRCS "host_data.c" "web_server.c" "time_mgr.c" "wifi_cfg.c" "c3_bridge_main.c"
                            "wifi_mgr.c"
                            "link_ns800.c"
                            "reporter.c"
                            "ota_mgr.c"
                       PRIV_REQUIRES esp_wifi nvs_flash esp_event esp_netif
                                     esp_driver_uart esp_driver_gpio
                                     esp_timer esp_http_client
                                     app_update esp_partition
                                     esp_http_server
                       INCLUDE_DIRS ".")
