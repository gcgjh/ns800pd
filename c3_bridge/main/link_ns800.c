/**
 * @file    link_ns800.c
 * @brief   ESP32-C3 → NS800RT7P65 的 UART 帧收发
 *
 * 设计要点：
 *
 *   1. 发送只在任务上下文，用互斥锁串行化 —— WiFi 事件、上报任务、心跳
 *      都可能发帧，不加锁会交错出半截帧。
 *   2. 接收用状态机 + 中断，而非「读一整块再解析」：串口数据是流式的，
 *      一次 read 可能拿到半帧，也可能拿到两帧半；状态机逐字节喂，
 *      天然处理任意切分。
 *   3. 不做接收超时复位：帧内字节间隔远小于接收任务的轮询周期（100 ms），
 *      状态机不会因等不到后续字节而卡住；若对端波特率不匹配导致收到乱码，
 *      也会由非法 LEN 分支重新对齐帧头，自行恢复。
 *
 * 接线（两块板，实测确定）：
 *
 *   C3 TX (GPIO4) → NS800 PB7 (UART2_RX)
 *   C3 RX (GPIO5) ← NS800 PB6 (UART2_TX)
 *   GND           ↔ GND
 *
 *   必须共地，否则电平漂浮，收到的全是乱码。
 *   C3 侧使用 UART_NUM_1（GPIO4/GPIO5），不用 UART0：后者是日志口，
 *   烧录与打印都走它。
 *   NS800 的 UART1（PA12/PA13）被调试控制台占用，板间链路走 UART2。
 *   NS800 的 PB6/PB7 在 SDK 里的全局编号是 GPIO_38 / GPIO_39
 *   （不是 GPIO_6/GPIO_7，后两者对应 PA6/PA7）。
 */

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_timer.h"      /* 探针的采样窗口计时 */
#include "esp_rom_sys.h"    /* esp_rom_delay_us：微秒级采样延时 */
#include "esp_log.h"

/* 引脚自检需直接读 IO_MUX / GPIO 矩阵寄存器（2026-10-11 加的审计通道）。
 * 这些寄存器头文件在 soc 组件里，属「半公开」API —— 仅用常量地址，
 * 不依赖 esp_rom 的私有函数，升级 IDF 影响较小。 */
#include "soc/io_mux_reg.h"
#include "soc/gpio_reg.h"

#include "ns800_link.h"
#include "link_ns800.h"

static const char *TAG = "link";

/* ── 硬件参数（改引脚只改这里）──
 * ESP32-C3 的可选 UART TX/RX 由 GPIO 矩阵决定，任意可用脚均可；
 * 这两个值须与实际接线一致。默认取 C3 上常见的 D4/D5 位（GPIO4/5），
 * 避开 strapping 脚（GPIO2/8/9）和 USB-JTAG 用的 GPIO18/19。 */
#define LINK_UART_NUM           UART_NUM_1   /* 板间链路所用 UART 外设 */
#define LINK_TX_GPIO            4            /* C3 发送脚，接 NS800 PB7 */
#define LINK_RX_GPIO            5            /* C3 接收脚，接 NS800 PB6 */

/* UART 驱动内部环形缓冲长度，接收中断写入、接收任务读出（单位：字节） */
#define LINK_RX_BUF             512
#define LINK_TX_BUF             256

#define LINK_READ_CHUNK         64           /* 接收任务单次读取上限（字节） */
#define LINK_MUTEX_TIMEOUT_MS   1000         /* 发送锁等待上限（毫秒） */

static bool             s_inited;
static SemaphoreHandle_t s_tx_mutex;
static QueueHandle_t    s_rx_queue;          /* 收到的完整帧队列 */

/* ── 收帧状态机 ── */
typedef enum {
    ST_SOF0 = 0,     /* 等待帧头首字节 0xAA */
    ST_SOF1,         /* 等待帧头次字节 0x55 */
    ST_LEN,          /* 读取 LEN */
    ST_PAYLOAD,      /* 逐字节收 PAYLOAD */
    ST_CRC,          /* 读取并校验 CRC8 */
} rx_state_t;

static rx_state_t s_state = ST_SOF0;
static uint8_t    s_len;
static uint8_t    s_payload[NS800_LINK_MAX_PAYLOAD];
static uint8_t    s_idx;
static uint8_t    s_rx_crc_buf[1 + NS800_LINK_MAX_PAYLOAD];   /* LEN + PAYLOAD */
static uint32_t   s_rx_bytes;                                  /* 统计：收了多少字节 */
static uint32_t   s_probe_tx_flips;                       /* 探针测得的翻转次数 */
static uint32_t   s_rx_frames;                                 /* 统计：凑齐多少帧 */
static uint32_t   s_rx_badcrc;                                 /* 统计：CRC 失败帧数 */
static uint32_t   s_tx_frames;                                 /* 统计：发出多少帧 */
static TickType_t s_last_rx_tick = 0;                           /* 最近一帧有效帧的时刻 */

/** 超过这个时间没收到有效帧，就认为对端（NS800）掉线了。
 *  30 秒：NS800 心跳是 5 秒一帧，留 6 倍余量，偶发丢帧不会误报。 */
#define LINK_PEER_ALIVE_MS    30000

/* 命令码 → 名字，只用于日志可读性 */
static const char *cmd_name(uint8_t cmd)
{
    switch (cmd)
    {
    case NS800_CMD_HELLO:       return "HELLO";
    case NS800_CMD_WIFI_STATE:  return "WIFI_STATE";
    case NS800_CMD_SENSOR:      return "SENSOR";
    case NS800_CMD_TEXT:        return "TEXT";
    case NS800_CMD_SERVER_RESP: return "SERVER_RESP";
    case NS800_CMD_PING:        return "PING";
    case NS800_CMD_REQ_WIFI:    return "REQ_WIFI";
    case NS800_CMD_SET_REPORT:  return "SET_REPORT";
    case NS800_CMD_WIFI_CFG:     return "WIFI_CFG";
    case NS800_CMD_SET_WIFI_CFG: return "SET_WIFI_CFG";
    case NS800_CMD_GET_WIFI_CFG: return "GET_WIFI_CFG";
    case NS800_CMD_OTA_PROGRESS: return "OTA_PROGRESS";
    case NS800_CMD_OTA_START:    return "OTA_START";
    /* 漏掉这两个会让日志显示成 UNKNOWN，联调时多花时间排查：
     *   TIME 是本机主动发出的上行帧（回环时能看到自己发的帧），
     *   WIFI_FACTORY_RST 是 NS800 下行的恢复出厂命令。 */
    case NS800_CMD_TIME:         return "TIME";
    case NS800_CMD_WIFI_FACTORY_RST: return "WIFI_RST";
    default:                    return "UNKNOWN";
    }
}

/**
 * @brief  读取接收统计：累计字节数、成功帧数、CRC 失败帧数
 * @param  bytes   输出，累计收到的字节数（可为 NULL）
 * @param  frames  输出，累计成功解析的帧数（可为 NULL）
 * @param  badcrc  输出，累计 CRC 校验失败的帧数（可为 NULL）
 */
void ns800_link_get_stats(uint32_t *bytes, uint32_t *frames, uint32_t *badcrc)
{
    if (bytes)  *bytes  = s_rx_bytes;
    if (frames) *frames = s_rx_frames;
    if (badcrc) *badcrc = s_rx_badcrc;
}

/**
 * @brief  物理层活性探针：统计 RX 引脚在给定窗口内的电平翻转次数
 * @param  ms     采样窗口（毫秒）
 * @param  flips  输出，翻转次数（高→低、低→高均计入）
 * @return ESP_OK 采样完成；ESP_FAIL 链路未初始化或引脚配置失败
 * @note   探针把 RX 引脚临时切成 GPIO 输入，会抢占 UART 复用，
 *         采样期间需让出 CPU，结束后必须恢复 uart_set_pin()。
 */
esp_err_t ns800_link_rx_pin_probe(uint32_t ms, uint32_t *flips)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << LINK_RX_GPIO,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,   /* 上拉：无信号时也能读到确定的高电平 */
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };

    if (flips) *flips = 0;
    if (!s_inited) return ESP_FAIL;

    /* 让 RX 任务先跑一轮，把 UART 缓冲里的残留读干净，
     * 否则会把"旧数据"误算成"翻转"。 */
    uart_flush_input(LINK_UART_NUM);
    vTaskDelay(pdMS_TO_TICKS(120));

    /* 只采样 RX 脚，TX 脚保持 UART 功能不动。
     *
     * 早期曾把 TX 脚（GPIO4）也切为 GPIO 输入做对照组，但有误：
     * GPIO4 切输入模式会从 UART 外设抢占引脚，UART 随即无法发送，
     * 采样期间本就无波形，翻转计数恒为 0，会误判成「探针坏了」。
     * 实际只需采 RX：TX-RX 短路时 TX 波形经短路出现在 RX 上，flips_rx > 0
     * 即证明探针正常；TX-RX 不短路时 flips_rx = 0，说明 RX 确无波形。
     * 该组合已是完整自检，无需额外对照组。 */

    /* 只碰 RX 脚；TX 完全不动，UART 保持发送能力 */
    gpio_reset_pin(LINK_RX_GPIO);
    if (gpio_config(&cfg) != ESP_OK) return ESP_FAIL;
    /* 切走复用后 UART 接收会失效，先把残留读掉避免误算 */
    uart_flush_input(LINK_UART_NUM);
    vTaskDelay(pdMS_TO_TICKS(120));

    int      last = gpio_get_level(LINK_RX_GPIO);
    int64_t  t0   = esp_timer_get_time();
    int64_t  span = (int64_t)ms * 1000;      /* 微秒 */

    /* 采样用 20µs 忙等 —— 115200 波特率一位宽只有 ~8.7µs，
     * 用 vTaskDelay(1)（=1ms）采样会跳过绝大多数沿，读到恒定电平。 */
    while (esp_timer_get_time() - t0 < span)
    {
        int lv = gpio_get_level(LINK_RX_GPIO);
        if (lv != last) { if (flips) (*flips)++; last = lv; }
        esp_rom_delay_us(20);
    }

    /* 恢复 UART 功能，否则链路失效 */
    uart_set_pin(LINK_UART_NUM, LINK_TX_GPIO, LINK_RX_GPIO,
                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    uart_flush_input(LINK_UART_NUM);

    /* 兼容旧字段：本字段名义为 TX 对照组，实际存入 RX 脚翻转数，保持网页字段名不变 */
    s_probe_tx_flips = flips ? *flips : 0;

    ESP_LOGW(TAG, "电平探针: RX(GPIO%d) 翻转 %lu 次 / %lu ms",
             LINK_RX_GPIO, (unsigned long)(flips ? *flips : 0),
             (unsigned long)ms);

    /* 将本次结果写入全局，供 ping 处理读取判定 */
    return ESP_OK;
}

/**
 * @brief  读取最近一次电平探针测得的翻转次数
 * @return 翻转次数；探针从未运行过时为 0
 * @note   复用历史上 TX 对照组的字段，网页侧字段名保持不变。
 */
uint32_t ns800_link_probe_tx_flips(void)
{
    return s_probe_tx_flips;
}

/**
 * @brief  判断对端（NS800）是否在线
 * @return true 表示 30 秒内收到过至少一帧 CRC 通过的帧
 * @note   网页的「板间链路」状态据此得出。必须用 xTaskGetTickCount()
 *         的差值比较，不能直接比 tick 值 —— 计数值回绕
 *         （ESP32 默认 32 bit @ 100 Hz，约 497 天）时差值比较仍正确。
 */
bool ns800_link_peer_alive(void)
{
    if (s_last_rx_tick == 0) return false;         /* 一帧都没收到过 */
    return (xTaskGetTickCount() - s_last_rx_tick)
           < pdMS_TO_TICKS(LINK_PEER_ALIVE_MS);
}

/* 喂一个字节进状态机；凑齐合法帧就返回 true 并填 out */
static bool link_feed(uint8_t b, ns800_frame_t *out)
{
    s_rx_bytes++;

    switch (s_state)
    {
    case ST_SOF0:
        if (b == NS800_LINK_SOF0) s_state = ST_SOF1;
        break;

    case ST_SOF1:
        /* 连续两个 SOF0 时留在本状态，等 SOF1 之后的那个字节，
         * 不能回到 ST_SOF0 —— 否则 "AA AA 55" 会被漏掉一帧。 */
        if (b == NS800_LINK_SOF1)      s_state = ST_LEN;
        else if (b != NS800_LINK_SOF0) s_state = ST_SOF0;
        break;

    case ST_LEN:
        if (b == 0 || b > NS800_LINK_MAX_PAYLOAD)
        {
            /* 长度非法 —— 前面那个 0xAA 0x55 是噪声凑出的假帧头。
             * 这一字节本身可能正是新帧的 0xAA，不能直接回 ST_SOF0 丢弃，
             * 否则叠加噪声 "…AA AA AA 55 | AA 55 …" 会连丢两帧；
             * 将其当作 SOF 序列首字节重新匹配。 */
            s_state = (b == NS800_LINK_SOF0) ? ST_SOF1 : ST_SOF0;
            break;
        }
        s_len = b;
        s_idx = 0;
        s_rx_crc_buf[0] = b;
        s_state = ST_PAYLOAD;
        break;

    case ST_PAYLOAD:
        s_payload[s_idx] = b;
        s_rx_crc_buf[1 + s_idx] = b;
        s_idx++;
        if (s_idx >= s_len) s_state = ST_CRC;
        break;

    case ST_CRC:
    {
        uint8_t expect = ns800_frame_crc(s_rx_crc_buf, (uint16_t)(1 + s_len));

        s_state = ST_SOF0;
        if (b == expect)
        {
            out->len = s_len;
            memcpy(out->payload, s_payload, s_len);
            /* cmd 取自 payload[0]（发送端把命令码编进 payload 段，见协议） */
            out->cmd = (s_len >= 1) ? s_payload[0] : 0;
            s_rx_frames++;
            s_last_rx_tick = xTaskGetTickCount();   /* 网页用来判断 NS800 是否在线 */
            return true;
        }
        s_rx_badcrc++;
        break;
    }
    }
    return false;
}

/* ── 接收任务：从 UART 驱动批量读，逐字节喂状态机 ── */
static void link_rx_task(void *arg)
{
    uint8_t buf[LINK_READ_CHUNK];

    (void)arg;

    while (1)
    {
        int n = uart_read_bytes(LINK_UART_NUM, buf, sizeof(buf),
                                pdMS_TO_TICKS(100));

        /* 联调关键计数：UART 驱动层真实交付的字节数（未经状态机）。
         * 有了它即可区分两类故障：
         *   · s_rx_bytes == 0 → 物理层无信号（接线/引脚/共地）
         *   · s_rx_bytes > 0 但 s_rx_frames == 0 → 字节已到但解析不出
         *     （波特率不匹配、帧格式错、CRC 域不对）
         * 仅有 frames/badcrc 两个数时无法区分上述情况（两者皆 0 的组合有多种含义）。 */
        if (n > 0) s_rx_bytes += (uint32_t)n;

        for (int i = 0; i < n; i++)
        {
            ns800_frame_t f;
            if (link_feed(buf[i], &f))
            {
                /* 收到帧不要在此做耗时处理（联网、写 flash 等）——
                 * 会拖慢接收任务、把 UART 环形缓冲顶爆。
                 * 此处只投递到队列，交给上层处理任务。 */
                if (s_rx_queue)
                    xQueueSend(s_rx_queue, &f, 0);   /* 队列满就丢，不阻塞接收 */
            }
        }
        /* 无数据也周期醒来一次 —— 目前不需要超时复位（帧内字节间隔
         * 远小于 100ms），但如果对端波特率不匹配打成乱码，
         * 状态机靠非法 LEN 自愈即可，不会永久卡死。 */
        vTaskDelay(1);
    }
}

/**
 * @brief  初始化 UART 并启动接收任务
 * @return ESP_OK 初始化成功；非 ESP_OK 表示资源分配或驱动安装失败
 * @note   幂等：重复调用安全，仅首次真正执行初始化。
 */
esp_err_t ns800_link_init(void)
{
    if (s_inited) return ESP_OK;

    s_tx_mutex = xSemaphoreCreateMutex();
    s_rx_queue = xQueueCreate(8, sizeof(ns800_frame_t));
    if (!s_tx_mutex || !s_rx_queue) return ESP_ERR_NO_MEM;

    const uart_config_t cfg = {
        .baud_rate  = NS800_LINK_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(uart_driver_install(LINK_UART_NUM, LINK_RX_BUF, LINK_TX_BUF,
                                        0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(LINK_UART_NUM, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(LINK_UART_NUM, LINK_TX_GPIO, LINK_RX_GPIO,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    xTaskCreate(link_rx_task, "link_rx", 3072, NULL, 6, NULL);

    s_inited = true;
    ESP_LOGI(TAG, "UART%d 就绪 TX=%d RX=%d @%d (协议帧 AA 55 LEN PAYLOAD CRC8)",
             LINK_UART_NUM, LINK_TX_GPIO, LINK_RX_GPIO, NS800_LINK_BAUD);
    return ESP_OK;
}

/**
 * @brief  引脚路由自检：把 IO_MUX / GPIO 矩阵寄存器打到日志（只读不写）
 * @note   须在 ns800_link_init() 之后调用。
 *
 * 源码只能证明「调用了 uart_set_pin」，证明不了「真的接上了」，
 * 这两个寄存器是最终裁判。
 *
 * ESP32-C3 上 GPIO4/GPIO5 的 IO MUX ALT1 恰好就是 UART1
 * （GPIO4=MTMS→U1TXD，GPIO5=MTDI→U1RXD），故 uart_set_pin() 在此走的是
 * IO-MUX 直连而非 GPIO 矩阵 —— MCU_SEL=1 才表示「已接上」，而非 0。
 * （"U1TXD_GPIO_NUM = -1" 的含义是「UART1 无出厂默认引脚」，
 *  不等于「无 IO-MUX 路由」，二者勿混。）
 *
 * 判定依据：
 *   - IO_MUX_GPIOx.MCU_SEL（bit[14:12]）：1 = 用 IO-MUX 的 FUNC1(ALT1)。
 *     对 GPIO4/5 而言 ALT1 = UART1，故 TX 脚应为 1。
 *   - GPIO_FUNCx_OUT_SEL（bit[6:0]）：输出信号源编号。
 *     C3 中 signal 9 = U1TXD_OUT_IDX，故 TX 脚应为 9。
 *     RX 脚为输入，不应有输出源；实测 0x80（bit7=OEN_SEL，低 7 位为 0）。
 *   - FUN_IE（bit9）：输入使能，RX 脚必须为 1。
 */
void ns800_link_dump_pin_mux(void)
{
    uint32_t mux4 = REG_READ(IO_MUX_GPIO4_REG);
    uint32_t mux5 = REG_READ(IO_MUX_GPIO5_REG);
    uint32_t f4   = REG_READ(GPIO_FUNC4_OUT_SEL_CFG_REG);
    uint32_t f5   = REG_READ(GPIO_FUNC5_OUT_SEL_CFG_REG);

    uint32_t sel4 = (mux4 >> 12) & 0x7;   /* MCU_SEL */
    uint32_t sel5 = (mux5 >> 12) & 0x7;
    uint32_t ie4  = (mux4 >> 9) & 1;      /* FUN_IE  */
    uint32_t ie5  = (mux5 >> 9) & 1;
    uint32_t out4 = f4 & 0x7F;            /* OUT_SEL（7 位） */
    uint32_t out5 = f5 & 0x7F;

    ESP_LOGI(TAG, "── 引脚路由自检（TX=GPIO%d RX=GPIO%d）──",
             LINK_TX_GPIO, LINK_RX_GPIO);
    ESP_LOGI(TAG, "IO_MUX_GPIO4 = 0x%08lx  MCU_SEL=%lu FUN_IE=%lu",
             (unsigned long)mux4, (unsigned long)sel4, (unsigned long)ie4);
    ESP_LOGI(TAG, "IO_MUX_GPIO5 = 0x%08lx  MCU_SEL=%lu FUN_IE=%lu",
             (unsigned long)mux5, (unsigned long)sel5, (unsigned long)ie5);
    ESP_LOGI(TAG, "GPIO_FUNC4_OUT_SEL = 0x%08lx  OUT_SEL=%lu (9=U1TXD)",
             (unsigned long)f4, (unsigned long)out4);
    ESP_LOGI(TAG, "GPIO_FUNC5_OUT_SEL = 0x%08lx  OUT_SEL=%lu (RX无输出源)",
             (unsigned long)f5, (unsigned long)out5);

    /* 判定：TX 脚 MCU_SEL==1 且 OUT_SEL==9(U1TXD)，即 UART1 已路由到位。
     * RX 脚 IE 必须为 1（能收），OUT_SEL 为 0 属正常（纯输入）。 */
    if (sel4 == 1 && out4 == 9 && ie5 == 1)
        ESP_LOGI(TAG, "判定：UART1 已路由 GPIO4(TX, ALT1=U1TXD)/GPIO5(RX) ✔");
    else
        ESP_LOGW(TAG, "判定：引脚路由与预期不符，检查 uart_set_pin ✘");
}

/**
 * @brief  构造并发送一帧（含 SOF/SOF/LEN/PAYLOAD/CRC8）
 * @param  cmd      命令码（NS800_CMD_*）
 * @param  payload  数据段（可为 NULL），不含命令码本身
 * @param  len      数据段长度，最大 NS800_LINK_MAX_PAYLOAD-1
 * @return ESP_OK 发送成功；ESP_ERR_INVALID_STATE 未初始化；
 *         ESP_ERR_INVALID_ARG 长度越界；ESP_ERR_TIMEOUT 取锁超时；
 *         ESP_FAIL 串口写失败
 * @note   发送受互斥锁保护，须仅在任务上下文调用。
 */
esp_err_t ns800_link_send(uint8_t cmd, const void *payload, uint8_t len)
{
    uint8_t frame[2 + 1 + NS800_LINK_MAX_PAYLOAD + 1];
    uint8_t pl[NS800_LINK_MAX_PAYLOAD + 1];      /* cmd + payload 连成一段算 CRC */
    uint8_t total;

    if (!s_inited) return ESP_ERR_INVALID_STATE;
    if (len > NS800_LINK_MAX_PAYLOAD - 1) return ESP_ERR_INVALID_ARG;

    /* payload 段 = cmd(1B) + 数据。
     * 把 cmd 放入 payload 是协议设计的一部分：收端以 payload[0] 作为命令码，
     * 使「命令」与「数据」处于同一 CRC 保护域，不会出现
     * 「命令正确但数据是上一帧」的撕裂。 */
    pl[0] = cmd;
    if (payload && len) memcpy(&pl[1], payload, len);
    total = (uint8_t)(1 + len);

    frame[0] = NS800_LINK_SOF0;
    frame[1] = NS800_LINK_SOF1;
    frame[2] = total;
    memcpy(&frame[3], pl, total);
    frame[3 + total] = ns800_frame_crc(&frame[2], (uint16_t)(1 + total));

    if (xSemaphoreTake(s_tx_mutex, pdMS_TO_TICKS(LINK_MUTEX_TIMEOUT_MS)) != pdTRUE)
    {
        ESP_LOGW(TAG, "取发送锁超时，丢帧 cmd=0x%02X", cmd);
        return ESP_ERR_TIMEOUT;
    }

    int written = uart_write_bytes(LINK_UART_NUM, (const char *)frame,
                                   4 + total);
    xSemaphoreGive(s_tx_mutex);

    if (written < 0)
    {
        ESP_LOGE(TAG, "串口写失败 cmd=0x%02X", cmd);
        return ESP_FAIL;
    }

    /* 发送日志联调期必打：否则无法判断帧是否真的发出。
     * cmd 用名字比十六进制好认；TEXT 类顺便把内容带出。 */
    s_tx_frames++;
    if (cmd == NS800_CMD_TEXT && payload && len)
    {
        /* 内容可能没有 '\0'，限长拷贝再打，避免越界读 */
        char tmp[NS800_LINK_MAX_PAYLOAD];
        uint8_t n = (len < sizeof(tmp) - 1) ? len : (uint8_t)(sizeof(tmp) - 1);
        memcpy(tmp, payload, n);
        tmp[n] = '\0';
        ESP_LOGI(TAG, "TX %-11s %uB  \"%s\"", cmd_name(cmd), (unsigned)(4 + total), tmp);
    }
    else
    {
        ESP_LOGI(TAG, "TX %-11s %uB  (累计 %lu)",
                 cmd_name(cmd), (unsigned)(4 + total), (unsigned long)s_tx_frames);
    }
    return ESP_OK;
}

/**
 * @brief  从接收队列取一帧（阻塞至多 timeout_ms）
 * @param  out         输出，解出的完整帧
 * @param  timeout_ms  等待超时（毫秒）
 * @return true 取到一帧；false 超时或链路未初始化/队列未创建
 */
bool ns800_link_recv(ns800_frame_t *out, uint32_t timeout_ms)
{
    if (!s_inited || !s_rx_queue) return false;
    return xQueueReceive(s_rx_queue, out, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

/* ── 便捷发送 ── */

/**
 * @brief  发送一段文本帧（自动计算长度）
 * @param  cmd   命令码（通常为 NS800_CMD_TEXT）
 * @param  text  以 '\0' 结尾的文本，最长 NS800_LINK_MAX_PAYLOAD-1 字节
 * @return 同 ns800_link_send() 的返回语义
 */
esp_err_t ns800_link_send_text(uint8_t cmd, const char *text)
{
    size_t n;

    if (!text) return ESP_ERR_INVALID_ARG;

    /* payload 段要留 1 字节给 cmd，故文本最长 NS800_LINK_MAX_PAYLOAD-1 */
    n = strlen(text);
    if (n > NS800_LINK_MAX_PAYLOAD - 1)
        n = NS800_LINK_MAX_PAYLOAD - 1;

    return ns800_link_send(cmd, text, (uint8_t)n);
}

/**
 * @brief  发送传感器定长载荷帧（NS800_CMD_SENSOR）
 * @param  temp_x100  芯片温度 ×100（℃，如 2563 表示 25.63℃）
 * @param  rssi       WiFi 信号强度（dBm，带符号）
 * @param  free_kb    空闲堆大小 /1024（KB）
 * @param  uptime_s   系统运行秒数
 * @return 同 ns800_link_send() 的返回语义
 * @note   内部按小端手工序列化，避免结构体对齐差异导致两端不一致。
 */
esp_err_t ns800_link_send_sensor(int16_t temp_x100, int16_t rssi,
                                 uint16_t free_kb, uint32_t uptime_s)
{
    uint8_t p[NS800_SENSOR_PAYLOAD_LEN];

    /* 手工按小端拆字节 —— 不用 memcpy(struct)：
     * 结构体有对齐填充，跨编译器/跨架构时布局不保证一致。
     * 定长手工序列化虽啰嗦，但两端一定对得上。 */
    p[0] = (uint8_t)(temp_x100 & 0xFF);
    p[1] = (uint8_t)((temp_x100 >> 8) & 0xFF);
    p[2] = (uint8_t)(rssi & 0xFF);
    p[3] = (uint8_t)((rssi >> 8) & 0xFF);
    p[4] = (uint8_t)(free_kb & 0xFF);
    p[5] = (uint8_t)((free_kb >> 8) & 0xFF);
    p[6] = (uint8_t)(uptime_s & 0xFF);
    p[7] = (uint8_t)((uptime_s >> 8) & 0xFF);
    p[8] = (uint8_t)((uptime_s >> 16) & 0xFF);
    p[9] = (uint8_t)((uptime_s >> 24) & 0xFF);

    return ns800_link_send(NS800_CMD_SENSOR, p, NS800_SENSOR_PAYLOAD_LEN);
}
