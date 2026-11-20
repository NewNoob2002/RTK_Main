// host 替身：web_module_check 专用 sdkconfig 常量（模拟 IDF v6.1 Wi-Fi 配置模式，不参与固件）。
#pragma once

#define CONFIG_RTK_WIFI_CONFIG_ENABLE             1
#define CONFIG_RTK_WIFI_AP_PASSWORD               "12345678"
#define CONFIG_RTK_WIFI_AP_CHANNEL                1
#define CONFIG_RTK_WIFI_IDLE_EXIT_S               300
#define CONFIG_RTK_FACTORY_KEY                    "123456"
#define CONFIG_HTTPD_WS_SUPPORT                   1
#define CONFIG_HTTPD_WS_POST_HANDSHAKE_CB_SUPPORT 1
#define CONFIG_RTK_GNSS_ENABLE                    1
#define CONFIG_RTK_GNSS_RATE_HZ                   1
#define CONFIG_RTK_LOG_RING_BYTES                 4096
#define CONFIG_RTK_CONSOLE_UART_NUM               0
#define CONFIG_RTK_CONSOLE_UART_BAUD              2000000
