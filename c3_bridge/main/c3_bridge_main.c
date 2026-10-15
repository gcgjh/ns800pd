/**
 * @file    c3_bridge_main.c
 * @brief   ESP32-C3 联网桥 —— 主入口
 *
 * 与 NS800RT7P65 的分工：
 *   NS800RT7P65：屏幕、UI、实时显示（RT-Thread + LVGL）
 *   ESP32-C3   ：联网能力（WiFi），把网络数据交给 NS800 显示
 *
 * 两块板用 UART 相连，协议见 ns800_link.h。C3 是数据源，NS800 是显示端。
 *
 * 启动顺序（有依赖，不能乱）：
 *   1. UART 链路先起 —— 后面所有状态变化都要能立刻通知 NS800，
 *      起晚了会丢掉开机的状态
 *   2. 发 HELLO，让 NS800 知道联网模块在线
 *   3. WiFi 初始化（可能要几秒）
 *   4. 上报任务（内部会等 IP，但不会死等）
 *
 * 不要用 ESP_ERROR_CHECK 包 wifi_mgr_init —— SSID 没配时它会返回
 * ESP_ERR_INVALID_STATE，用 ERROR_CHECK 会直接 abort 重启，表现为板子反复重启。
 */

#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_timer.h"

#include "link_ns800.h"
#include "wifi_mgr.h"
#include "reporter.h"
#include "ota_mgr.h"
#include "time_mgr.h"
#include "web_server.h"
#include "ns800_link.h"

static const char *TAG = "c3";

/* 固件版本串已经移到 web_server.h 里（作为 C3_BRIDGE_VERSION 宏）——
 * 因为 HTTP 管理页也要显示它，而两处各写一份字符串必然会串版本
 * （网页显示 v2.0、串口 HELLO 报 v2.1）。单一出处 = 改一处两边都对。 */

/* ── 启动时打印芯片信息（排障第一手资料）── */
static void print_chip_info(void)
{
    esp_chip_info_t ci;
    char ver[32];

    esp_chip_info(&ci);
    snprintf(ver, sizeof(ver), "v%d.%d.%d",
             ESP_IDF_VERSION_MAJOR, ESP_IDF_VERSION_MINOR, ESP_IDF_VERSION_PATCH);

    ESP_LOGI(TAG, "芯片 %s rev%d, %d 核, IDF %s",
             CONFIG_IDF_TARGET, ci.revision, ci.cores, ver);
    ESP_LOGI(TAG, "空闲堆 %u 字节", (unsigned)esp_get_free_heap_size());
}

/* ── 心跳：周期性给 NS800 发一句话，证明链路活着 ── */
static void heartbeat_task(void *arg)
{
    (void)arg;

    while (1)
    {
        uint32_t up = (uint32_t)(esp_timer_get_time() / 1000000LL);
        char msg[48];

        snprintf(msg, sizeof(msg), "alive %lus", (unsigned long)up);
        ns800_link_send_text(NS800_CMD_TEXT, msg);

        /* 联调诊断：把 C3 的 UART 接收统计顺带报给 NS800。
         * 用来一次性区分两种完全不同的故障：
         *   ① rx_bytes 一直为 0  → 下行根本没到 C3（接线/引脚/电平问题）
         *   ② rx_bytes 在涨但 rx_frames 为 0 → 字节到了但帧解析失败（协议/CRC）
         * 仅凭两端日志无法判断卡在哪一环，加这行后一眼定位：曾据此读出
         * "bytes=24 frames=0 crc=4"，直接指向 NS800 发送侧 CRC 域少算 1 字节。
         *
         * 只在有下行流量时发（frames 或 bytes 较上次有变化），
         * 链路空闲时不发 —— 否则每 10 秒一条纯噪声，把 c3stat 刷花。 */
        {
            static uint32_t last_b = 0xFFFFFFFFu, last_f = 0xFFFFFFFFu;
            uint32_t rb = 0, rf = 0, bc = 0;

            ns800_link_get_stats(&rb, &rf, &bc);
            if (rb != last_b || rf != last_f)
            {
                char dbg[48];
                last_b = rb; last_f = rf;
                snprintf(dbg, sizeof(dbg), "c3rx bytes=%lu frames=%lu crc=%lu",
                         (unsigned long)rb, (unsigned long)rf, (unsigned long)bc);
                ns800_link_send_text(NS800_CMD_TEXT, dbg);
            }
        }

        /* 顺便周期性重发 HELLO。
         * 不能只在启动时发一次：NS800 端从复位到 c3_link_init()
         * 大约要 1 秒，而 C3 在 363ms 就把 HELLO 发出去了 ——
         * 对端那时还没开串口，HELLO 必然丢失，NS800 界面永远显示"(未握手)"。
         * 每 60 秒重发一次，代价可忽略，换来任意顺序复位都能自动补握手。 */
        static uint32_t tick = 0;
        if (++tick % 6 == 0)   /* 每 60 秒重发一次，避免每次心跳都刷 */
        {
            ns800_link_send_text(NS800_CMD_HELLO, C3_BRIDGE_VERSION);
        }

        /* 每 10 秒一次：太密会占满 NS800 的串口接收任务，
         * 太疏则 NS800 那边判断"离线"要等很久。 */
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== C3 联网桥启动 ===");
    print_chip_info();

    /* ⓪ OTA 分区信息 —— 最先打，方便一眼看出"这次跑的是哪个槽、
     *    是不是刚 OTA 完处于待验证状态"。 */
    ota_mgr_init();
    ota_mgr_dump_info();

    /* ① 链路最先起（见文件头说明） */
    if (ns800_link_init() != ESP_OK)
        ESP_LOGE(TAG, "串口链路初始化失败 —— 检查 UART 引脚/驱动");
    else
    {
        /* 引脚路由自检：读回 IO_MUX / GPIO 矩阵寄存器（只读不写）。
         * 证明 uart_set_pin() 真的生效，而不是"调用过但没接上"。 */
        ns800_link_dump_pin_mux();
        ns800_link_send_text(NS800_CMD_HELLO, C3_BRIDGE_VERSION);
    }

    /* ② WiFi —— 不用 ESP_ERROR_CHECK 包（未配 SSID 会 abort 重启） */
    esp_err_t werr = wifi_mgr_init();
    if (werr != ESP_OK)
    {
        ESP_LOGW(TAG, "WiFi 未启动: %s", esp_err_to_name(werr));
        ESP_LOGW(TAG, "→ 请在 menuconfig 里配置 C3_WIFI_SSID / C3_WIFI_PASSWORD");
    }

    /* ③ 上报 + 命令处理（含 OTA_START 的接收处理） */
    reporter_start();

    /* ③.5 NTP 对时 + 每秒把时间下发给 NS800
     *
     * 必须起独立任务，不能在 app_main 里同步等 NTP：
     *    time_mgr_start() 内部会等 IP（最多 60s）+ 等首次对时（最多 15s），
     *    直接调用会把 app_main 卡住 70 多秒，导致后面的心跳任务和
     *    ota_mgr_confirm_valid()（回滚确认）都被推迟 —— 而 confirm_valid()
     *    拖太久的话，万一期间看门狗/复位触发，新固件会被 bootloader 回滚。
     *    所以：任务里跑。
     *
     * 任务栈 4096：SNTP 走 lwIP，内部有 socket + 字符串格式化，3072 偏紧。 */
    xTaskCreate(time_mgr_task, "time", 4096, NULL, 4, NULL);

    /* ④ 心跳 */
    xTaskCreate(heartbeat_task, "hb", 2048, NULL, 4, NULL);

    /* ④.5 HTTP 管理页
     *
     * 必须在 WiFi 起来之后起（HTTP 服务要用网卡），但不能等 IP：
     *    wifi_mgr_wait_ip() 最长阻塞 60 秒，等到它返回才起网页的话，
     *    「SSID 填错了连不上 → 想改回来」这个最需要网页的场景就废了
     *    （设备在反复重连，网页根本没监听）。
     *    esp_http_server 绑定的是 netif 层，网卡 up 就够，不需要 IP。
     *    AP 模式下本机 IP 是 192.168.4.1，同样能访问。 */
    web_server_start();

    /* ⑤ OTA 回滚确认 —— 必须放在所有初始化成功之后！
     *
     * 语义：开了 CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE 后，OTA 刷进来的新固件
     * 第一次启动是 PENDING_VERIFY 状态。只有调用了本函数才算"自检通过"，
     * 否则下次重启 bootloader 会自动回滚到旧固件。
     *
     * 放在这里的含义 = "串口链路起了 + WiFi 起了 + 任务都建起来了"才算健康。
     * 若以后加了更严格的自检（比如必须连上服务器），把本调用挪到那之后。
     */
    ota_mgr_confirm_valid();

    ESP_LOGI(TAG, "=== 启动完成，进入主循环 ===");
    /* app_main 返回后 FreeRTOS 会回收它的栈，其余任务继续跑 */
}
