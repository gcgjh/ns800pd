/**
 * @file    c3_link.c
 * @brief   NS800 侧与 ESP32-C3 的板间链路实现（UART2）
 *
 * 收：串口设备线程读字节，喂给接收状态机；攒满的帧入消息队列，解析线程落地。
 * 发：拼帧（含 CRC8），串口 write，全程持发送互斥锁。
 *
 * 接收分两级（设备线程 + 解析线程）：设备线程只做读字节与喂状态机，
 * 保证 115200 下不会溢出 FIFO；解析中可能要做 kprintf、字符串拷贝，
 * 放在设备线程会拖慢采样。分两级最省心。
 */

#include <rtthread.h>
#include <rtdevice.h>
#include <board.h>              /* BOARD_C3_LINK_BAUDRATE / BOARD_C3_LINK */
#include <stdlib.h>             /* atoi */
#include "c3_link.h"

#define DBG_TAG "c3link"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

/* ---------------- 配置 ---------------- */

#define LINK_UART_NAME      "uart2"      /* 板间链路使用的串口设备名 */
#define LINK_RX_RING        256          /* 与 BSP_UART2_RX_BUFSIZE 一致 */
#define LINK_FRAME_QUEUE    8            /* 帧队列深度 */
#define LINK_RX_CHUNK       64           /* 每次从串口读的最大字节数 */
#define LINK_RX_THREAD_STK  1536         /* 接收线程栈大小（字节） */
#define LINK_RX_THREAD_PRIO 22           /* 比 UI 低，比空闲高 */
#define LINK_PARSE_STK      2048         /* 解析线程栈大小（字节） */
#define LINK_PARSE_PRIO     23           /* 优先级低于 LINK_RX_THREAD_PRIO */

/* C3 超过这个时间没消息就认为掉线（心跳 10s，给 3 倍余量） */
#define LINK_ONLINE_TIMEOUT_MS  30000

/* ---------------- 内部类型 ---------------- */

/* 状态机 */
enum
{
    ST_SOF0 = 0,    /* 等 0xAA */
    ST_SOF1,        /* 等 0x55 */
    ST_LEN,         /* 等长度字节 */
    ST_PAYLOAD,     /* 收负载 */
    ST_CRC          /* 等校验字节 */
};

/* 与 c3_link_state_t 同构，内部用 */
static c3_link_state_t s_state;
static struct rt_mutex s_lock;
static struct rt_mutex s_tx_lock;

static rt_device_t s_uart;
static struct rt_messagequeue s_frames;

/* 消息池按真实槽位大小（对齐后的 msg_size + 链表头）预留，
 * 使 LINK_FRAME_QUEUE 等于实际可用帧数，不被对齐吃掉。
 * 多留一个槽位，避免整除误差丢掉最后一格。 */
#define LINK_MSG_SLOT   (RT_ALIGN(sizeof(ns800_frame_t), RT_ALIGN_SIZE) + sizeof(void *))
static rt_uint8_t s_msg_pool[(LINK_FRAME_QUEUE + 1) * LINK_MSG_SLOT];

/* 联调计数器：用于定位帧在哪一步丢失
 *   dbg_sem   : 信号量被 post 次数
 *   dbg_read  : rt_device_read 返回的字节数累计
 *   dbg_feed  : link_feed 凑满帧次数
 *   dbg_mqfail: rt_mq_send 失败次数
 *   dbg_parsed: parse 线程成功取出次数
 */
static volatile rt_uint32_t s_dbg_sem, s_dbg_read, s_dbg_feed_ok, s_dbg_mq_fail, s_dbg_parsed;
/* CRC 错误明细打印次数闸门：只打印前 8 次，避免坏帧刷屏。
 * 与 s_dbg_parsed（成功解析帧数）语义不同，不可混用。 */
static volatile rt_uint8_t  s_dbg_badcrc_print;

/* 状态机私有 */
static rt_uint8_t s_st = ST_SOF0;
static rt_uint8_t s_len = 0;
static rt_uint8_t s_cnt = 0;
static rt_uint8_t s_payload[NS800_LINK_MAX_PAYLOAD];

/*
 * CRC 保护域缓冲：LEN + PAYLOAD。
 *
 * 协议规定 CRC 覆盖 LEN + PAYLOAD 两段，共 1 + pl_len 字节（pl_len 含 cmd）。
 * 接收侧与发送侧都必须按 1 + pl_len 计算，并与 C3 侧 link_ns800.c 的
 * s_rx_crc_buf[0] = LEN、[1..] = PAYLOAD 完全对称。
 *
 * 曾出现两类不一致：接收侧误写成 ns800_frame_crc(s_payload, s_len)，
 * 漏掉 LEN 字节，导致每帧 CRC 都算错（差值仅 1 个 bit 量级，
 * 容易误判成末字节被干扰）；发送侧误写成 pl_len，同样少算 1 字节，
 * 且只在 NS800 -> C3 方向暴露。结论：凡计算 CRC 之处，
 * 长度参数一律为 1 + pl_len，别无例外。
 */
static rt_uint8_t s_crc_buf[1 + NS800_LINK_MAX_PAYLOAD];

/* ---------------- 发送 ---------------- */

/**
 * @brief  组帧并发送一帧到 C3
 * @param  cmd      命令码（NS800_CMD_xxx）
 * @param  payload  负载（不含 cmd），可为 RT_NULL
 * @param  len      负载长度（不含 cmd），最大 NS800_LINK_MAX_PAYLOAD - 1
 * @return RT_EOK 成功；-RT_EINVAL 负载过长；-RT_ERROR 链路未初始化；-RT_EIO 发送不完整
 * @note   发送全程持 s_tx_lock，可从任意线程调用。
 */
static rt_err_t send_raw(rt_uint8_t cmd, const void *payload, rt_uint8_t len)
{
    rt_uint8_t frame[2 + 1 + NS800_LINK_MAX_PAYLOAD + 1];
    rt_uint8_t n = 0;
    rt_uint8_t pl_len;

    /* pl_len = 1(cmd) + len(data)，不能超过 MAX_PAYLOAD */
    if (len > NS800_LINK_MAX_PAYLOAD - 1)
    {
        return -RT_EINVAL;
    }
    pl_len = (rt_uint8_t)(len + 1);

    frame[n++] = NS800_LINK_SOF0;
    frame[n++] = NS800_LINK_SOF1;
    frame[n++] = pl_len;
    frame[n++] = cmd;
    if (len && payload != RT_NULL)
    {
        rt_memcpy(&frame[n], payload, len);
        n = (rt_uint8_t)(n + len);
    }
    /* CRC 覆盖 LEN + PAYLOAD(含 cmd)，即从 frame[2] 起的 1 + pl_len 字节。
     *
     * 此处曾误写成 ns800_frame_crc(&frame[2], pl_len)，少算 1 个字节，
     * 只覆盖到 payload 的倒数第二字节，最后一字节未参与 CRC。
     * 该错误只在 NS800 -> C3 方向暴露：
     *   C3 -> NS800 时收发两侧长度均正确，链路一直正常；
     *   NS800 -> C3 时 C3 每帧 CRC 都对不上，表现为 C3 收不到 NS800 的帧
     *   （c3ping 时 C3 报 bytes > 0 而 frames = 0）。
     * 正确长度 = 1 + pl_len，与接收侧（1 + s_len）及 C3 侧 s_rx_crc_buf 完全对称。 */
    frame[n] = ns800_frame_crc(&frame[2], (rt_uint8_t)(1 + pl_len));
    n++;

    if (s_uart == RT_NULL)
    {
        return -RT_ERROR;
    }

    rt_mutex_take(&s_tx_lock, RT_WAITING_FOREVER);
    /* 用 rt_device_write 而非 write_raw：让驱动去处理 FIFO 与总线忙 */
    rt_size_t w = rt_device_write(s_uart, 0, frame, n);
    rt_mutex_release(&s_tx_lock);

    if (w != n)
    {
        LOG_W("发送不完整 %d/%d", (int)w, (int)n);
        return -RT_EIO;
    }
    return RT_EOK;
}

/**
 * @brief  发送一帧到 C3（对外接口，内部转 send_raw）
 * @param  cmd     命令码（NS800_CMD_xxx）
 * @param  payload 负载（不含 cmd），可为 RT_NULL
 * @param  len     负载长度（不含 cmd），最大 NS800_LINK_MAX_PAYLOAD - 1
 * @return RT_EOK 成功，其他为失败
 */
rt_err_t c3_link_send(rt_uint8_t cmd, const void *payload, rt_uint8_t len)
{
    return send_raw(cmd, payload, len);
}

/**
 * @brief  发送纯文本帧，长度自动计算，超长截断
 * @param  cmd   命令码（通常 NS800_CMD_TEXT）
 * @param  text  UTF-8 文本，以 '\0' 结尾
 * @return RT_EOK 成功；-RT_EINVAL text 为 RT_NULL
 */
rt_err_t c3_link_send_text(rt_uint8_t cmd, const char *text)
{
    rt_size_t n;

    if (text == RT_NULL)
    {
        return -RT_EINVAL;
    }
    n = rt_strlen(text);
    if (n > NS800_LINK_MAX_PAYLOAD - 1)
    {
        n = NS800_LINK_MAX_PAYLOAD - 1;
    }
    return send_raw(cmd, text, (rt_uint8_t)n);
}

/**
 * @brief  请求 C3 重新联网
 * @return RT_EOK 成功，其他为失败
 */
rt_err_t c3_link_request_wifi(void)
{
    /* 无负载：payload[0] 就是 cmd 本身，len = 0 */
    return send_raw(NS800_CMD_REQ_WIFI, RT_NULL, 0);
}

/**
 * @brief  设置 C3 的上报周期
 * @param  seconds 上报周期，单位秒，有效范围 1..3600
 * @return RT_EOK 成功，其他为失败
 */
rt_err_t c3_link_set_report_period(rt_uint32_t seconds)
{
    rt_uint8_t b[2];
    b[0] = (rt_uint8_t)(seconds & 0xFF);
    b[1] = (rt_uint8_t)((seconds >> 8) & 0xFF);
    /* 秒数以 2 字节小端发送，与 ns800_link.h 的 SET_REPORT 定义及
     * C3 侧解析一致；协议规定上限 3600 秒，远小于 uint16 上限，不会截断。
     *
     * 此处曾发送 4 字节（32 位小端），多出的高 2 字节对不超过 65535 的秒数
     * 恒为 0，被 C3 侧忽略而表现为可用。这类依赖对方宽容度的不一致不可取，
     * 两端一律以共享头文件里的 2 字节定义为准。 */
    return send_raw(NS800_CMD_SET_REPORT, b, 2);
}

/* 配网：下发一份 WiFi 配置（见 ns800_link.h 的载荷格式）
 * 载荷 = mode(1) + ssid_len(1) + ssid + pass_len(1) + pass */
static rt_err_t c3_link_set_wifi_cfg_impl(rt_uint8_t mode, const char *ssid, const char *pass)
{
    rt_uint8_t b[2 + NS800_WIFI_SSID_MAX + 1 + NS800_WIFI_PASS_MAX];
    rt_size_t sl, pl;
    rt_uint8_t n = 0;

    if (ssid == RT_NULL || pass == RT_NULL) return -RT_EINVAL;

    sl = rt_strlen(ssid);
    pl = rt_strlen(pass);
    if (sl == 0 || sl > NS800_WIFI_SSID_MAX || pl > NS800_WIFI_PASS_MAX)
        return -RT_EINVAL;
    /* 载荷总长 = 1+1+sl+1+pl ≤ MAX_PAYLOAD-1（send_raw 会再加 1 字节 cmd） */
    if ((rt_size_t)(3 + sl + pl) > NS800_LINK_MAX_PAYLOAD - 1u)
        return -RT_EINVAL;

    b[n++] = mode;
    b[n++] = (rt_uint8_t)sl;
    rt_memcpy(&b[n], ssid, sl); n = (rt_uint8_t)(n + sl);
    b[n++] = (rt_uint8_t)pl;
    rt_memcpy(&b[n], pass, pl); n = (rt_uint8_t)(n + pl);

    return send_raw(NS800_CMD_SET_WIFI_CFG, b, n);
}

rt_err_t c3_link_set_wifi_cfg(const char *ssid, const char *pass)
{
    return c3_link_set_wifi_cfg_impl(NS800_WIFI_MODE_STA, ssid, pass);
}

rt_err_t c3_link_set_ap_cfg(const char *ssid, const char *pass)
{
    return c3_link_set_wifi_cfg_impl(NS800_WIFI_MODE_AP, ssid, pass);
}

rt_err_t c3_link_get_wifi_cfg(void)
{
    /* 无负载：请求 C3 回报当前配置 */
    return send_raw(NS800_CMD_GET_WIFI_CFG, RT_NULL, 0);
}

/**
 * @brief  触发 C3 执行 OTA
 * @param  url 固件 .bin 的完整 URL
 * @return RT_EOK 已发出；-RT_EINVAL URL 为空或超长
 * @note   异步接口：只负责把 URL 帧发出去，C3 在后台下载并写 flash，
 *         进度经 OTA_PROGRESS 帧回填到 s_state.ota_*。
 */
rt_err_t c3_link_ota_start(const char *url)
{
    rt_size_t n;

    if (url == RT_NULL || url[0] == '\0')
        return -RT_EINVAL;

    n = rt_strlen(url);
    /* payload = URL（不带结尾 '\0'）。send_raw 内部还会再加 1 字节 cmd，
     * 所以 URL 上限是 MAX_PAYLOAD - 1 = 95 字节。 */
    if (n > NS800_LINK_MAX_PAYLOAD - 1)
    {
        LOG_E("OTA URL 过长: %u > %d", (unsigned)n, NS800_LINK_MAX_PAYLOAD - 1);
        return -RT_EINVAL;
    }

    return send_raw(NS800_CMD_OTA_START, url, (rt_uint8_t)n);
}

/**
 * @brief  向 C3 上报一条 "key=value" 形式的自定义数据
 * @param  key   键名，ASCII，不能为空
 * @param  value 值的文本形式，可为 RT_NULL（按空串处理）
 * @return RT_EOK 已发出；-RT_EINVAL key 为空或总长超限
 * @note   通用上行入口，C3 侧不认识该 key 也会原样保存。
 */
rt_err_t c3_link_send_kv(const char *key, const char *value)
{
    rt_uint8_t buf[NS800_LINK_MAX_PAYLOAD];
    rt_size_t  kl, vl, total;

    if (key == RT_NULL || key[0] == '\0')
    {
        return -RT_EINVAL;
    }
    /* value 允许为 NULL 或空串：用于只通知在线、不带值的场景，
     * 内部统一按空串处理。 */
    if (value == RT_NULL) value = "";

    kl = rt_strlen(key);
    vl = rt_strlen(value);
    /* payload = "key" + '=' + "value"，不含结尾 '\0' */
    total = kl + 1 + vl;

    if (total > (rt_size_t)(NS800_LINK_MAX_PAYLOAD - 1))
    {
        LOG_E("KV 过长: %u+%u+%u > %d", (unsigned)kl, (unsigned)vl,
              1u, NS800_LINK_MAX_PAYLOAD - 1);
        return -RT_EINVAL;
    }

    rt_memcpy(&buf[0], key, kl);
    buf[kl] = '=';
    if (vl) rt_memcpy(&buf[kl + 1], value, vl);

    return send_raw(NS800_CMD_HOST_DATA, buf, (rt_uint8_t)total);
}

/* 命令 c3kv：向 C3 上报自定义数据，用法 c3kv <key> [value]
 *
 * 联调入口，业务代码应直接调用 c3_link_send_kv()。
 * 无需重新烧写 NS800 固件即可验证 NS800 -> C3 通用通道：
 * 在 msh 中执行后，可在 C3 网页 /api/host_data 看到新条目。
 */
static int c3kv(int argc, char **argv)
{
    if (argc < 2)
    {
        rt_kprintf("用法: c3kv <key> [value]\n");
        rt_kprintf("  例: c3kv backlight 80\n");
        rt_kprintf("      c3kv heartbeat      (无 value 也可，发 \"heartbeat=\")\n");
        rt_kprintf("提示: 发完看 C3 网页的 /api/host_data，或串口日志的"
                   "\"收到主机数据\"\n");
        return 0;
    }

    if (c3_link_send_kv(argv[1], (argc >= 3) ? argv[2] : RT_NULL) == RT_EOK)
    {
        rt_kprintf("已上行 %s=%s\n", argv[1], (argc >= 3) ? argv[2] : "");
        return 0;
    }

    rt_kprintf("发送失败（超长或链路未初始化）\n");
    return 1;
}
MSH_CMD_EXPORT(c3kv, 向 C3 上报自定义数据: c3kv <key> [value]);

/* ---------------- 接收状态机 ---------------- */

/*
 * 逐字节喂入。返回 1 表示攒满一帧（结果在 *out），0 表示还在接收。
 * 帧长最多 99 字节，115200 下一帧约 8.6ms，逐字节处理的开销可以接受，
 * 换来的是任意切分位置都能正确解析。
 */
static int link_feed(rt_uint8_t b, ns800_frame_t *out)
{
    switch (s_st)
    {
    case ST_SOF0:
        if (b == NS800_LINK_SOF0) s_st = ST_SOF1;
        break;

    case ST_SOF1:
        /* 连续 SOF0 时留在本状态，等待其后的 0x55；
         * 若回到 ST_SOF0，"AA AA 55" 会漏掉一帧。 */
        if (b == NS800_LINK_SOF1)      s_st = ST_LEN;
        else if (b != NS800_LINK_SOF0) s_st = ST_SOF0;
        break;

    case ST_LEN:
        if (b == 0 || b > NS800_LINK_MAX_PAYLOAD)
        {
            /* 长度非法：说明前面的 0xAA 0x55 是噪声凑出的假帧头。
             * 该字节本身可能正是新帧的 0xAA，不能简单回 ST_SOF0 丢弃，
             * 否则 "…AA AA AA 55 | AA 55 LEN…" 这类叠加噪声会连丢两帧。
             * 正确做法：把它当作 SOF 序列的首字节重新匹配。 */
            LOG_W("非法长度 0x%02X，按 SOF 重新匹配", b);
            s_st = (b == NS800_LINK_SOF0) ? ST_SOF1 : ST_SOF0;
            break;
        }
        s_len = b;
        s_cnt = 0;
        s_crc_buf[0] = b;    /* CRC 域第 0 字节 = LEN */
        s_st  = ST_PAYLOAD;
        break;

    case ST_PAYLOAD:
        s_payload[s_cnt]     = b;
        s_crc_buf[1 + s_cnt] = b;
        s_cnt++;
        if (s_cnt >= s_len) s_st = ST_CRC;
        break;

    case ST_CRC:
    {
        /* 覆盖 LEN + PAYLOAD（与发送端 send_raw 对称，见 s_crc_buf 注释） */
        rt_uint8_t calc = ns800_frame_crc(s_crc_buf, (rt_uint8_t)(1 + s_len));
        rt_uint8_t cmd  = s_payload[0];   /* payload[0] 即 cmd */

        s_st = ST_SOF0;                   /* 无论对错都复位，等下一帧 */

        if (calc != b)
        {
            rt_mutex_take(&s_lock, RT_WAITING_FOREVER);
            s_state.rx_badcrc++;
            rt_mutex_release(&s_lock);
            /* 前若干次把整帧原始字节打印出来：仅凭 got/calc 无法区分
             * 末字节出错还是整帧错位，必须看字节。
             * 限流到前 8 次，避免坏帧刷屏冲掉有用日志。 */
            if (s_dbg_badcrc_print < 8)
            {
                rt_uint8_t k;
                s_dbg_badcrc_print++;
                rt_kprintf("[c3link] CRC错 len=%u got=%02X calc=%02X :", s_len, b, calc);
                for (k = 0; k < s_len; k++) rt_kprintf(" %02X", s_payload[k]);
                rt_kprintf("\n");
            }
            return 0;
        }

        out->cmd = cmd;
        out->len = s_len;
        rt_memcpy(out->payload, s_payload, s_len);
        return 1;
    }

    default:
        s_st = ST_SOF0;
        break;
    }
    return 0;
}

/* ---------------- 解析 ---------------- */

/**
 * @brief  解析一帧并落地到共享状态
 * @param  f 已通过 CRC 校验的完整帧
 * @return 无
 * @note   在解析线程上下文执行；写共享状态一律持 s_lock。
 *         除统计与在线标志外，仅更新与本命令码对应的字段。
 */
static void handle_frame(const ns800_frame_t *f)
{
    const rt_uint8_t *d = f->payload;   /* d[0] = cmd */
    rt_uint8_t dlen = (rt_uint8_t)(f->len - 1);   /* 数据部分长度 */

    rt_mutex_take(&s_lock, RT_WAITING_FOREVER);
    s_state.rx_frames++;
    s_state.last_rx_tick = rt_tick_get();
    s_state.online = 1;
    rt_mutex_release(&s_lock);
    switch (f->cmd)
    {
    case NS800_CMD_HELLO:
    {
        /* 数据就是版本字符串，可能没有 '\0'，手工截断 */
        char ver[sizeof(s_state.c3_ver)];
        rt_uint8_t n = (dlen < sizeof(ver) - 1) ? dlen : (rt_uint8_t)(sizeof(ver) - 1);

        rt_memcpy(ver, d + 1, n);
        ver[n] = '\0';

        rt_mutex_take(&s_lock, RT_WAITING_FOREVER);
        rt_strncpy(s_state.c3_ver, ver, sizeof(s_state.c3_ver) - 1);
        s_state.c3_ver[sizeof(s_state.c3_ver) - 1] = '\0';
        rt_mutex_release(&s_lock);

        LOG_I("C3 上线: %s", ver);
        break;
    }

    case NS800_CMD_WIFI_STATE:
        if (dlen >= 1)
        {
            rt_mutex_take(&s_lock, RT_WAITING_FOREVER);
            s_state.wifi_state = d[1];
            rt_mutex_release(&s_lock);
            LOG_I("WiFi 状态 -> %u", d[1]);
        }
        break;

    case NS800_CMD_TEXT:
    {
        char txt[NS800_LINK_MAX_PAYLOAD];
        rt_uint8_t n = (dlen < sizeof(txt) - 1) ? dlen : (rt_uint8_t)(sizeof(txt) - 1);

        rt_memcpy(txt, d + 1, n);
        txt[n] = '\0';
        LOG_I("C3: %s", txt);

        /* C3 拿到 IP 时会用 TEXT 帧推过来，约定前缀 "ip " 便于识别 */
        if (n > 3 && txt[0] == 'i' && txt[1] == 'p' && txt[2] == ' ')
        {
            rt_mutex_take(&s_lock, RT_WAITING_FOREVER);
            rt_strncpy(s_state.ip, &txt[3], sizeof(s_state.ip) - 1);
            s_state.ip[sizeof(s_state.ip) - 1] = '\0';
            rt_mutex_release(&s_lock);
        }
        break;
    }

    case NS800_CMD_SENSOR:
        /* 负载长度必须严格等于约定值，防止版本错配时越界 */
        if (dlen >= NS800_SENSOR_PAYLOAD_LEN)
        {
            const rt_uint8_t *p = d + 1;

            /* 全部手工小端组包 —— 与 C3 侧逐个字节序列化对称。
             * 不用 memcpy(struct)：结构体填充在不同编译器下不一致。 */
            rt_int16_t  temp = (rt_int16_t)((rt_uint16_t)p[0] | ((rt_uint16_t)p[1] << 8));
            /* 该字段为带符号补码（C3 的 wifi_mgr_rssi() 直接返回负值，
             * 如 -30 序列化为 0xE2 0xFF）。直接按 int16 解释，不要取负：
             * 曾按 uint16 读出 65506 再取负得到 +30，负号丢失。 */
            rt_int16_t  rssi = (rt_int16_t)((rt_uint16_t)p[2] | ((rt_uint16_t)p[3] << 8));
            rt_uint16_t freek = (rt_uint16_t)((rt_uint16_t)p[4] | ((rt_uint16_t)p[5] << 8));
            rt_uint32_t up   = ((rt_uint32_t)p[6])
                             | ((rt_uint32_t)p[7]  << 8)
                             | ((rt_uint32_t)p[8]  << 16)
                             | ((rt_uint32_t)p[9]  << 24);

            rt_mutex_take(&s_lock, RT_WAITING_FOREVER);
            s_state.temp_x100 = temp;
            s_state.rssi_dbm = rssi;        /* 线上就是带符号值，直接存 */
            s_state.free_kb  = freek;
            s_state.uptime_s = up;
            rt_mutex_release(&s_lock);
        }
        else
        {
            LOG_W("SENSOR 帧长度不符: %u != %d", dlen, NS800_SENSOR_PAYLOAD_LEN);
        }
        break;

    case NS800_CMD_SERVER_RESP:
        if (dlen >= 3)
        {
            rt_mutex_take(&s_lock, RT_WAITING_FOREVER);
            s_state.last_http_code = (rt_int16_t)((rt_uint16_t)d[1] | ((rt_uint16_t)d[2] << 8));
            s_state.last_http_ok   = d[3];
            rt_mutex_release(&s_lock);
        }
        break;

    case NS800_CMD_WIFI_CFG:
    {
        /* 载荷（d[0] 是 cmd）：mode(1)+ssid_len(1)+ssid+pass_len(1)+pass */
        const rt_uint8_t *p;
        rt_uint8_t mode, sl, pl;
        char ssid[NS800_WIFI_SSID_MAX + 1];
        char pass[NS800_WIFI_PASS_MAX + 1];

        if (dlen < 3) { LOG_W("WIFI_CFG 太短"); break; }
        p    = d + 1;
        mode = p[0];
        sl   = p[1];
        if (sl > NS800_WIFI_SSID_MAX || dlen < 2u + 1u + sl + 1u) { LOG_W("WIFI_CFG ssid 越界"); break; }
        rt_memcpy(ssid, p + 2, sl); ssid[sl] = '\0';
        pl = p[2 + sl];
        if (pl > NS800_WIFI_PASS_MAX || dlen < 3u + sl + pl) { LOG_W("WIFI_CFG pass 越界"); break; }
        rt_memcpy(pass, p + 3 + sl, pl); pass[pl] = '\0';

        rt_mutex_take(&s_lock, RT_WAITING_FOREVER);
        s_state.wifi_mode = mode;
        rt_strncpy(s_state.cfg_ssid, ssid, sizeof(s_state.cfg_ssid) - 1);
        s_state.cfg_ssid[sizeof(s_state.cfg_ssid) - 1] = '\0';
        rt_strncpy(s_state.cfg_pass, pass, sizeof(s_state.cfg_pass) - 1);
        s_state.cfg_pass[sizeof(s_state.cfg_pass) - 1] = '\0';
        s_state.cfg_valid = 1;
        rt_mutex_release(&s_lock);

        LOG_I("C3 配置: mode=%s SSID=%s", mode == NS800_WIFI_MODE_AP ? "AP" : "STA", ssid);
        break;
    }

    case NS800_CMD_OTA_PROGRESS:
        /* 载荷（d[0] 是 cmd）：status(1) + percent(1) */
        if (dlen >= 2)
        {
            rt_uint8_t st = d[1];
            rt_uint8_t pc = d[2];

            rt_mutex_take(&s_lock, RT_WAITING_FOREVER);
            s_state.ota_status = st;
            s_state.ota_percent = pc;
            /* OK/FAIL 都表示这轮升级结束，清掉 "进行中" 标志 */
            s_state.ota_active = (st == NS800_OTA_ST_RUNNING) ? 1u : 0u;
            rt_mutex_release(&s_lock);

            if (st == NS800_OTA_ST_OK)
                LOG_I("OTA 完成 100%%（C3 将重启到新槽）");
            else if (st == NS800_OTA_ST_FAIL)
                LOG_E("OTA 失败（进度 %u%%）", pc);
            else
                LOG_I("OTA 进度 %u%%", pc);
        }
        else
        {
            LOG_W("OTA_PROGRESS 帧太短: %u", dlen);
        }
        break;

    case NS800_CMD_TIME:
        /* 载荷（d[0] 是 cmd）：Unix 时间戳 4 字节小端（已换算到 UTC+8） */
        if (dlen >= 4)
        {
            const rt_uint8_t *p = d + 1;
            rt_int32_t ts = (rt_int32_t)((rt_uint32_t)p[0]
                          | ((rt_uint32_t)p[1] << 8)
                          | ((rt_uint32_t)p[2] << 16)
                          | ((rt_uint32_t)p[3] << 24));

            rt_mutex_take(&s_lock, RT_WAITING_FOREVER);
            s_state.time_valid = 1;
            s_state.time_unix  = ts;
            s_state.time_tick  = rt_tick_get();
            rt_mutex_release(&s_lock);

            /* 每秒一帧，不打日志（否则控制台被刷屏）。
             * 想确认对时结果用 c3stat / c3time 命令。 */
        }
        else
        {
            LOG_W("TIME 帧长度不符: %u < 4", dlen);
        }
        break;

    case NS800_CMD_PING:
        /* 主动回 pong，方便在 C3 侧日志里验证链路 */
        c3_link_send_text(NS800_CMD_TEXT, "pong");
        break;

    default:
        LOG_W("未知命令 0x%02X (len=%u)", f->cmd, f->len);
        break;
    }
}

/* ---------------- 线程 ---------------- */

/* 在线看门狗（定义在下方）：必须由线程周期性调用，不能在中断里调用。
 * 前向声明，供 parse_thread 引用。 */
static void link_check_online_timeout(void);

/**
 * @brief  解析线程：从消息队列取帧并落地到共享状态
 * @param  arg 未使用
 * @return 无（常驻线程）
 * @note   队列为空时以 2 秒超时等待，超时后顺带巡检一次在线看门狗。
 */
static void parse_thread_entry(void *arg)
{
    ns800_frame_t f;
    rt_ssize_t    r;

    (void)arg;
    while (1)
    {
        /* rt_mq_recv 的返回类型是 rt_ssize_t，不是 rt_err_t：
         * 成功时返回拷走的字节数（正数），失败才返回负的错误码。
         * 判据必须是 > 0。此处曾误写为 == RT_EOK（0），而成功返回的是消息大小，
         * 条件恒假，导致帧已入队却一帧都未解析（mqfail = 0 而 parsed = 0）。 */
        r = rt_mq_recv(&s_frames, &f, sizeof(f), rt_tick_from_millisecond(2000));
        if (r > 0)
        {
            s_dbg_parsed++;
            handle_frame(&f);
        }
        link_check_online_timeout();
    }
}

/* RX 就绪信号量：由串口驱动的 rx_indicate 回调在中断里 post。
 * 该信号量是必需的，不是优化。
 *
 * 本 BSP 使用 RT_USING_SERIAL_V1，其 rt_device_read() -> _serial_int_rx()
 * 不阻塞：软件 FIFO 一空就 break 返回 0（见
 * components/drivers/serial/dev_serial.c）。若接收线程写成
 * while(1){ n = read(); if (!n) continue; }，就会变成纯空转，
 * c3rx 线程的 CPU 占用可达 95%；且 _serial_int_rx() 每轮都要
 * rt_spin_lock_irqsave 关中断，空转会让 UART ISR 被反复推迟、
 * 硬件 FIFO 溢出丢字节，最终表现为帧内字节错位、大量 CRC 错。
 *
 * 正确做法是等待信号量：rx_indicate 在中断上下文被调用，post 是安全的。 */
static struct rt_semaphore s_rx_sem;

/**
 * @brief  串口 RX 指示回调，收到数据时唤醒接收线程
 * @param  dev  串口设备
 * @param  size 本次到达的字节数（此处未使用）
 * @return RT_EOK
 * @note   在中断上下文被调用，只能做计数与 rt_sem_release。
 */
static rt_err_t link_rx_indicate(rt_device_t dev, rt_size_t size)
{
    (void)dev; (void)size;
    s_dbg_sem++;
    rt_sem_release(&s_rx_sem);   /* 中断里调用：rt_sem_release 可安全用于 ISR */
    return RT_EOK;
}

/**
 * @brief  接收线程：读串口字节并喂入状态机，攒满的帧投递到消息队列
 * @param  arg 未使用
 * @return 无（常驻线程）
 * @note   等待 RX 信号量后一次性抽干软件 FIFO；队列满时丢帧并计数，
 *         绝不在本线程阻塞。
 */
static void rx_thread_entry(void *arg)
{
    rt_uint8_t  buf[LINK_RX_CHUNK];
    ns800_frame_t f;
    rt_size_t   n;
    rt_size_t   i;

    (void)arg;
    while (1)
    {
        /* 先挂起等数据到达（中断 post），再一次性把软件 FIFO 抽干。
         * 抽干这一步很关键：FIFO 里可能已经攒了好几帧，
         * 只读一次会让下一帧等到下一次中断才被处理。 */
        rt_sem_take(&s_rx_sem, RT_WAITING_FOREVER);

        do
        {
            n = rt_device_read(s_uart, 0, buf, sizeof(buf));
            if (n == 0 || n == (rt_size_t)-1)
            {
                break;
            }
            s_dbg_read += (rt_uint32_t)n;

            for (i = 0; i < n; i++)
            {
                if (link_feed(buf[i], &f))
                {
                    s_dbg_feed_ok++;
                    /* 队列满就丢帧并计数，绝不在接收线程里阻塞 */
                    if (rt_mq_send(&s_frames, &f, sizeof(f)) != RT_EOK)
                    {
                        s_dbg_mq_fail++;
                        rt_mutex_take(&s_lock, RT_WAITING_FOREVER);
                        s_state.rx_badcrc++;   /* 记在统计里，便于发现异常 */
                        rt_mutex_release(&s_lock);
                        LOG_W("帧队列满，丢弃 cmd=0x%02X", f.cmd);
                    }
                }
            }
        } while (n == sizeof(buf));
    }
}

/*
 * 在线看门狗：超时标离线。
 *
 * 本函数必须在线程上下文调用，不能在定时器回调里取互斥锁。
 * 本 BSP 的 rtconfig.h 未定义 RT_USING_TIMER_SOFT，
 * RT_TIMER_FLAG_SOFT_TIMER 因而是空标志：timer.c 的 _get_timer_lock()
 * 会走硬定时器分支，回调直接在 SysTick 中断里执行
 * （rt_timer_check 从 ISR 调用）。此时回调里的 rt_mutex_take() 命中
 * RT_DEBUG_NOT_IN_INTERRUPT 断言（RT_DEBUGING_CONTEXT 开启时该断言有效），
 * 进入 rt_assert_handler 后整板停住。该症状不易定位：控制台在 UI 起来后
 * 突然静默，看不出与定时器有关，复位后紧跟调试器时也未必立刻复现。
 *
 * 因此改为由 parse_thread 轮询：每收完一帧或等待超时各检查一次，
 * 不额外开线程。好处是不碰中断上下文，并让 parse_thread 有一个超时唤醒源，
 * 不必为看门狗单独占用一个定时器资源。
 */
static void link_check_online_timeout(void)
{
    rt_mutex_take(&s_lock, RT_WAITING_FOREVER);
    if (s_state.online &&
        (rt_tick_get() - s_state.last_rx_tick) > rt_tick_from_millisecond(LINK_ONLINE_TIMEOUT_MS))
    {
        s_state.online = 0;
        s_state.wifi_state = NS800_WIFI_IDLE;
    }
    rt_mutex_release(&s_lock);
}

/* ---------------- 对外接口 ---------------- */

/**
 * @brief  取一份当前链路状态快照
 * @param  out 输出结构体，调用方分配；为 RT_NULL 时直接返回
 * @return 无
 * @note   加锁拷贝，可随时调用。
 */
void c3_link_get_state(c3_link_state_t *out)
{
    if (out == RT_NULL)
    {
        return;
    }
    rt_mutex_take(&s_lock, RT_WAITING_FOREVER);
    rt_memcpy(out, &s_state, sizeof(*out));
    rt_mutex_release(&s_lock);
}

/*
 * 断言钩子：把断言现场（表达式串、函数名、行号）保存在全局变量里，
 * 便于用 PyOCD 直接读内存定位，不依赖控制台串口输出；同时仍打印一次，
 * 控制台可用时可直接查看。
 * 默认的 rt_assert_handler 只打印一行然后 while(1) 死等，
 * 若断言发生在控制台发不出或串口被占用的时机，外部只能看到板子静默。
 * 钩子内不做任何可能阻塞的操作（不取锁、不 malloc），只记录并打印，最后自旋。
 */
volatile const char *g_assert_ex;
volatile const char *g_assert_func;
volatile unsigned    g_assert_line;
volatile unsigned    g_assert_hit;

static void c3_assert_hook(const char *ex, const char *func, rt_size_t line)
{
    g_assert_ex   = ex;
    g_assert_func = func;
    g_assert_line = (unsigned)line;
    g_assert_hit  = 1;

    rt_kprintf("\n[ASSERT] %s @ %s:%u\n", ex ? ex : "?", func ? func : "?", (unsigned)line);
    while (1)
    {
        /* 自旋，保留现场给调试器读 */
    }
}

static void c3_link_install_assert_hook(void)
{
    rt_assert_set_hook(c3_assert_hook);
}

/**
 * @brief  初始化板间链路：打开 uart2、注册 RX 回调、启动接收与解析线程
 * @return RT_EOK 成功；-RT_ERROR 串口未找到或配置失败；-RT_ENOMEM 线程创建失败
 * @note   重复调用直接返回 RT_EOK。本函数在启动阶段调用一次。
 */
rt_err_t c3_link_init(void)
{
    struct serial_configure cfg = RT_SERIAL_CONFIG_DEFAULT;
    rt_thread_t th;

    if (s_uart != RT_NULL)
    {
        return RT_EOK;   /* 已初始化 */
    }

    rt_memset(&s_state, 0, sizeof(s_state));
    s_state.wifi_state     = NS800_WIFI_IDLE;
    s_state.last_http_code = -1;
    /* OTA 字段：ota_status 清零后恰好等于 NS800_OTA_ST_RUNNING，
     * 因此不能只看 status 判断是否在升级，必须结合 ota_active 标志
     * （收到首条 RUNNING 帧时才置 1）。见 c3ota / c3stat 的判断逻辑。 */
    s_state.ota_status  = NS800_OTA_ST_RUNNING;
    s_state.ota_active  = 0;

    rt_mutex_init(&s_lock,    "c3lk",  RT_IPC_FLAG_PRIO);
    rt_mutex_init(&s_tx_lock, "c3tx",  RT_IPC_FLAG_PRIO);

    /* RX 就绪信号量：初值 0，上限不限（每来一批数据 post 一次，抽干后自然消费掉） */
    rt_sem_init(&s_rx_sem, "c3rx", 0, RT_IPC_FLAG_PRIO);

    s_uart = rt_device_find(LINK_UART_NAME);
    if (s_uart == RT_NULL)
    {
        LOG_E("找不到 %s —— 检查 rtconfig.h 是否打开了 BSP_USING_UART2", LINK_UART_NAME);
        return -RT_ERROR;
    }

    cfg.baud_rate = BOARD_C3_LINK_BAUDRATE;
    cfg.data_bits = DATA_BITS_8;
    cfg.stop_bits = STOP_BITS_1;
    cfg.parity    = PARITY_NONE;
    cfg.bufsz     = LINK_RX_RING;

    if (rt_device_control(s_uart, RT_DEVICE_CTRL_CONFIG, &cfg) != RT_EOK)
    {
        LOG_E("配置 %s 失败", LINK_UART_NAME);
        return -RT_ERROR;
    }

    /* rt_mq_init 第 4 个参数是单条消息的字节数，第 5 个是消息池总字节数。
     * 曾把第 5 个参数误写成条数，导致 pool_size 仅 8 字节，
     * max_msgs = 8 / (align(98, 8) + 4) = 8 / 108 = 0，建出一个零容量队列，
     * 所有 rt_mq_send 都返回 -RT_EFULL，表现为帧收到了却一帧都解析不出来。
     *
     * rt_mq_init 在 max_msgs == 0 时会返回 -RT_EINVAL，此处必须判返回值。
     *
     * 另注意：槽位实际大小 = RT_ALIGN(msg_size, RT_ALIGN_SIZE) + sizeof(rt_mq_message)，
     * 池内实际可容纳的消息数可能少于 LINK_FRAME_QUEUE，故下面打印真实值。 */
    if (rt_mq_init(&s_frames, "c3fr", s_msg_pool, sizeof(ns800_frame_t),
                   sizeof(s_msg_pool), RT_IPC_FLAG_FIFO) != RT_EOK)
    {
        LOG_E("帧队列初始化失败（池太小？）");
        return -RT_ERROR;
    }

    LOG_I("帧队列: pool=%u slot_size=%u max_msgs=%u (请求深度 %d)",
          (unsigned)sizeof(s_msg_pool),
          (unsigned)(RT_ALIGN(sizeof(ns800_frame_t), RT_ALIGN_SIZE) + sizeof(void *)),
          (unsigned)s_frames.max_msgs, LINK_FRAME_QUEUE);

    /* 注册 RX 指示回调 —— 必须在 open 之前，否则 open 期间来的字节
     * 不会 post 信号量，接收线程会一直睡到下一批数据。 */
    rt_device_set_rx_indicate(s_uart, link_rx_indicate);

    /* 必须用 RT_DEVICE_FLAG_INT_RX：让驱动在收满中断里 post 我们的信号量。
     * 注意 V1 的 rt_device_read() 本身不阻塞，所以线程侧必须靠
     * s_rx_sem 睡着（见 rx_thread_entry 的注释）。 */
    if (rt_device_open(s_uart, RT_DEVICE_FLAG_INT_RX) != RT_EOK)
    {
        LOG_E("打开 %s 失败", LINK_UART_NAME);
        return -RT_ERROR;
    }

    th = rt_thread_create("c3rx", rx_thread_entry, RT_NULL,
                          LINK_RX_THREAD_STK, LINK_RX_THREAD_PRIO, 10);
    if (th == RT_NULL) { LOG_E("创建接收线程失败"); return -RT_ENOMEM; }
    rt_thread_startup(th);

    th = rt_thread_create("c3parse", parse_thread_entry, RT_NULL,
                          LINK_PARSE_STK, LINK_PARSE_PRIO, 10);
    if (th == RT_NULL) { LOG_E("创建解析线程失败"); return -RT_ENOMEM; }
    rt_thread_startup(th);

    /* 在线看门狗不在这里开定时器 —— 见 link_check_online_timeout() 的注释，
     * 定时器回调在本 BSP 会跑在 SysTick 中断里，take 互斥量必死。
     * 改由 c3parse 线程周期性轮询。 */

    LOG_I("板间链路就绪: %s @%u (TX=PB6 RX=PB7)", LINK_UART_NAME,
          (unsigned)BOARD_C3_LINK_BAUDRATE);

    /* 链路初始化成功后再装断言钩子：失败路径要保留原始 assert 行为便于定位 */
    c3_link_install_assert_hook();
    return RT_EOK;
}

/* ---------------- msh 调试命令 ---------------- */

/* WiFi 状态码转中文名，供 c3stat 打印 */
static const char *wifi_state_name(rt_uint8_t s)
{
    switch (s)
    {
    case NS800_WIFI_IDLE:         return "空闲";
    case NS800_WIFI_CONNECTING:   return "连接中";
    case NS800_WIFI_CONNECTED:    return "已连接";
    case NS800_WIFI_GOT_IP:       return "已获IP";
    case NS800_WIFI_DISCONNECTED: return "断开";
    case NS800_WIFI_FAIL:         return "失败";
    default:                      return "?";
    }
}

/* 命令 c3stat：查看 ESP32-C3 链路状态，用法 c3stat（无参数）。
 * 打印在线状态、固件版本、网络信息、收帧统计以及联调计数。 */
static int c3stat(int argc, char **argv)
{
    c3_link_state_t st;
    (void)argc; (void)argv;

    c3_link_get_state(&st);
    rt_kprintf("---- ESP32-C3 链路 ----\n");
    rt_kprintf("  在线    : %s\n", st.online ? "是" : "否");
    rt_kprintf("  固件    : %s\n", st.c3_ver[0] ? st.c3_ver : "(未握手)");
    rt_kprintf("  WiFi    : %s\n", wifi_state_name(st.wifi_state));
    rt_kprintf("  IP      : %s\n", st.ip[0] ? st.ip : "-");
    rt_kprintf("  RSSI    : %d dBm\n", st.rssi_dbm);
    rt_kprintf("  温度    : %d.%02d C\n", st.temp_x100 / 100,
               (st.temp_x100 < 0 ? -st.temp_x100 : st.temp_x100) % 100);
    rt_kprintf("  堆剩余  : %u KB\n", st.free_kb);
    rt_kprintf("  运行    : %u 秒\n", (unsigned)st.uptime_s);
    rt_kprintf("  上报    : code=%d ok=%u\n", st.last_http_code, st.last_http_ok);
    rt_kprintf("  收帧    : %u (CRC错 %u)\n", (unsigned)st.rx_frames,
               (unsigned)st.rx_badcrc);
    /* 配网配置快照（由 WIFI_CFG 帧回填，见 c3cfg 命令） */
    if (st.cfg_valid)
    {
        rt_kprintf("  -- C3 配置快照 --\n");
        rt_kprintf("  模式    : %s\n", st.wifi_mode == NS800_WIFI_MODE_AP ? "AP" : "STA");
        rt_kprintf("  SSID    : %s\n", st.cfg_ssid);
        rt_kprintf("  密码    : %s\n", st.cfg_pass[0] ? st.cfg_pass : "(空)");
    }
    else
    {
        rt_kprintf("  配置    : (未回读，发 c3cfg 请求)\n");
    }
    /* OTA 快照（由 OTA_PROGRESS 帧回填，见 c3ota 命令） */
    {
        const char *on = st.ota_active ? "进行中"
                       : (st.ota_status == NS800_OTA_ST_OK ? "成功"
                       : (st.ota_status == NS800_OTA_ST_FAIL ? "失败" : "空闲"));
        rt_kprintf("  OTA     : %s, 进度 %u%%\n", on, st.ota_percent);
    }
    /* 时间快照（由 TIME 帧回填，见 c3time 命令） */
    if (st.time_valid)
    {
        rt_int32_t now = st.time_unix
                       + (rt_int32_t)((rt_tick_get() - st.time_tick) / RT_TICK_PER_SECOND);
        rt_kprintf("  时间    : %d (unix, UTC+8)\n", (int)now);
    }
    else
    {
        rt_kprintf("  时间    : 未对时\n");
    }
    rt_kprintf("  -- 联调计数 --\n");
    rt_kprintf("  sem=%u read=%u feed=%u mqfail=%u parsed=%u\n",
               (unsigned)s_dbg_sem, (unsigned)s_dbg_read,
               (unsigned)s_dbg_feed_ok, (unsigned)s_dbg_mq_fail,
               (unsigned)s_dbg_parsed);
    return 0;
}
MSH_CMD_EXPORT(c3stat, 查看 ESP32-C3 链路状态);

/* 命令 c3dbg：清零 C3 链路联调计数，用法 c3dbg（无参数） */
static int c3dbg(int argc, char **argv)
{
    (void)argc; (void)argv;
    s_dbg_sem = s_dbg_read = s_dbg_feed_ok = s_dbg_mq_fail = s_dbg_parsed = 0;
    s_dbg_badcrc_print = 0;
    rt_kprintf("联调计数已清零\n");
    return 0;
}
MSH_CMD_EXPORT(c3dbg, 清零 C3 链路联调计数);

/* 命令 c3ping：向 ESP32-C3 发 PING，用法 c3ping（无参数）。
 *
 * 必须发送 NS800_CMD_PING（0x81），不能发 NS800_CMD_TEXT：
 * C3 的 cmd_task 只对 PING / REQ_WIFI / SET_REPORT / SET_WIFI_CFG /
 * GET_WIFI_CFG / OTA_START 有分支，TEXT 落入 default 被静默丢弃，不会回 pong，
 * 表现为发 ping 无响应，容易误判成链路不通。
 * C3 收到 PING 后会回一个 TEXT "pong"。 */
static int c3ping(int argc, char **argv)
{
    rt_uint8_t seq = 0;
    (void)argc; (void)argv;

    if (c3_link_send(NS800_CMD_PING, &seq, 1) == RT_EOK)
    {
        rt_kprintf("已发送 PING，C3 收到会回 pong（NS800 收帧 +1，看 c3stat）\n");
        return 0;
    }
    rt_kprintf("发送失败（链路未初始化？）\n");
    return -1;
}
MSH_CMD_EXPORT(c3ping, 向 ESP32-C3 发 PING（C3 会回 pong）);

/* 命令 c3wifirst：让 C3 恢复出厂 WiFi 配置，用法 c3wifirst（无参数）。
 *
 * 用于救回网页失联的设备：C3 的 HTTP 管理页可改 WiFi，
 * 若 STA 的 SSID 被填成一个不存在的 AP，设备会连不上网，网页随之失联，
 * 浏览器里无法改回；此时唯一还能操作的入口是 UART 直连的这块屏。
 * 本命令让 C3 擦除 NVS 里的 wificfg 命名空间，下次启动写回出厂默认值
 * （STA: gpm / 12345678）。
 *
 * 无 payload（f.len = 1，只有 cmd 字节）。
 * 会丢弃用户在网页上填过的全部 WiFi 配置，不可撤销。 */
static int c3wifirst(int argc, char **argv)
{
    (void)argc; (void)argv;

    rt_kprintf("即将让 C3 恢复出厂 WiFi 配置（STA: %s / 出厂密码）\n",
               "gpm");
    rt_kprintf("网页上填过的 SSID/密码会全部丢失，确定请继续\n");
    rt_kprintf("已发送 WIFI_FACTORY_RST，C3 会先回一条 TEXT 再重启\n");

    if (c3_link_send(NS800_CMD_WIFI_FACTORY_RST, RT_NULL, 0) == RT_EOK)
        return 0;
    rt_kprintf("发送失败（链路未初始化？）\n");
    return -1;
}
MSH_CMD_EXPORT(c3wifirst, 让 C3 恢复出厂 WiFi 配置（网页失联时的救生索）);

/* 命令 c3wifi：让 ESP32-C3 重连 WiFi，用法 c3wifi（无参数） */
static int c3wifi(int argc, char **argv)
{
    (void)argc; (void)argv;
    if (c3_link_request_wifi() == RT_EOK)
    {
        rt_kprintf("已请求 C3 重新联网\n");
        return 0;
    }
    rt_kprintf("发送失败\n");
    return -1;
}
MSH_CMD_EXPORT(c3wifi, 让 ESP32-C3 重连 WiFi);

/* 命令 c3period：设置 ESP32-C3 上报周期，用法 c3period <秒>，下限 5 秒 */
static int c3period(int argc, char **argv)
{
    rt_uint32_t sec;

    if (argc < 2)
    {
        rt_kprintf("用法: c3period <秒>\n");
        return -1;
    }
    sec = (rt_uint32_t)atoi(argv[1]);
    if (sec < 5) { sec = 5; }
    if (c3_link_set_report_period(sec) == RT_EOK)
    {
        rt_kprintf("已把 C3 上报周期设为 %u 秒\n", (unsigned)sec);
        return 0;
    }
    rt_kprintf("发送失败\n");
    return -1;
}
MSH_CMD_EXPORT(c3period, 设置 ESP32-C3 上报周期(秒));

/* 命令 c3cfg：C3 配网与配置回读。
 * 用法：
 *   c3cfg                      请求 C3 回报当前配置（打印到控制台并填快照）
 *   c3cfg sta <ssid> <pass>    让 C3 切 STA 连接指定 AP
 *   c3cfg ap  <ssid> [pass]    让 C3 切 AP 开热点（省略密码即开放）
 *
 * 调试入口；屏上的「配网」App 走同一条协议。 */
static int c3cfg(int argc, char **argv)
{
    if (argc == 1)
    {
        if (c3_link_get_wifi_cfg() == RT_EOK) { rt_kprintf("已请求 C3 回报配置（几秒后看日志）\n"); return 0; }
        rt_kprintf("发送失败\n");
        return -1;
    }

    if (argc >= 3 && rt_strcmp(argv[1], "sta") == 0)
    {
        const char *pass = (argc >= 4) ? argv[3] : "";
        if (c3_link_set_wifi_cfg(argv[2], pass) == RT_EOK)
        {
            rt_kprintf("已下发 STA: SSID=%s\n", argv[2]);
            return 0;
        }
    }
    else if (argc >= 3 && rt_strcmp(argv[1], "ap") == 0)
    {
        const char *pass = (argc >= 4) ? argv[3] : "";
        if (c3_link_set_ap_cfg(argv[2], pass) == RT_EOK)
        {
            rt_kprintf("已下发 AP: SSID=%s\n", argv[2]);
            return 0;
        }
    }
    rt_kprintf("用法: c3cfg [sta <ssid> <pass> | ap <ssid> [pass]]\n");
    return -1;
}
MSH_CMD_EXPORT(c3cfg, 配网: c3cfg sta <ssid> <pass> / c3cfg ap <ssid> [pass]);

/* 命令 c3ota：让 ESP32-C3 执行 OTA，用法 c3ota <固件URL>；
 * 不带参数时打印最近一次的 OTA 状态。 */
static int c3ota(int argc, char **argv)
{
    /* 无参数：打印最近一次的 OTA 状态（来自 OTA_PROGRESS 帧） */
    if (argc == 1)
    {
        c3_link_state_t st;
        const char *n;

        c3_link_get_state(&st);
        if (!st.ota_active && st.ota_percent == 0 && st.ota_status == NS800_OTA_ST_RUNNING)
            n = "未开始";
        else if (st.ota_status == NS800_OTA_ST_OK)
            n = "成功";
        else if (st.ota_status == NS800_OTA_ST_FAIL)
            n = "失败";
        else
            n = st.ota_active ? "进行中" : "空闲";

        rt_kprintf("OTA 状态: %s, 进度 %u%%\n", n, st.ota_percent);
        rt_kprintf("用法: c3ota <固件URL>   例: c3ota http://192.168.1.100:8000/c3_bridge.bin\n");
        return 0;
    }

    if (c3_link_ota_start(argv[1]) == RT_EOK)
    {
        rt_kprintf("已下发 OTA: %s\n", argv[1]);
        rt_kprintf("提示: C3 将在后台下载并写入备用槽，完成后自动重启。\n");
        rt_kprintf("      进度通过 OTA_PROGRESS 帧回报（看本控制台日志，或再执行 c3ota 查看）。\n");
        return 0;
    }

    rt_kprintf("OTA 下发失败（URL 为空或超过 %d 字节）\n", NS800_LINK_MAX_PAYLOAD - 1);
    return -1;
}
MSH_CMD_EXPORT(c3ota, 让 ESP32-C3 执行 OTA: c3ota <固件URL>（不带参数查看进度）);

/* 命令 c3time：查看 C3 对时结果，用法 c3time（无参数） */
static int c3time(int argc, char **argv)
{
    c3_link_state_t st;

    (void)argc;
    (void)argv;

    c3_link_get_state(&st);
    if (!st.time_valid)
    {
        rt_kprintf("时间: 未对时（C3 还没拿到 NTP 时间）\n");
        return 0;
    }

    {
        rt_int32_t now = st.time_unix
                       + (rt_int32_t)((rt_tick_get() - st.time_tick) / RT_TICK_PER_SECOND);
        /* 只需要年月日时分秒，这里做最简拆解（含闰年判断，不做时区换算，
         * 因为 time_unix 已是 UTC+8）；精确格式化在 UI 侧 sb_gmtime。 */
        rt_int32_t days = now / 86400;
        rt_int32_t rem  = now % 86400;
        int y = 1970, m = 1;
        static const int md[12] = {31,28,31,30,31,30,31,31,30,31,30,31};

        while (days >= (365 + (((y % 4 == 0 && y % 100 != 0) || y % 400 == 0) ? 1 : 0)))
        {
            days -= 365 + (((y % 4 == 0 && y % 100 != 0) || y % 400 == 0) ? 1 : 0);
            y++;
        }
        while (m <= 12)
        {
            int leap = ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0);
            int dm = md[m - 1] + ((m == 2 && leap) ? 1 : 0);
            if (days < dm) break;
            days -= dm;
            m++;
        }
        rt_kprintf("时间: %04d-%02d-%02d %02d:%02d:%02d (UTC+8)  [收到 %u 秒前]\n",
                   y, m, (int)days + 1,
                   (int)(rem / 3600), (int)((rem % 3600) / 60), (int)(rem % 60),
                   (unsigned)((rt_tick_get() - st.time_tick) / RT_TICK_PER_SECOND));
    }
    return 0;
}
MSH_CMD_EXPORT(c3time, 查看 C3 对时结果（桌面状态栏时间来源）);
