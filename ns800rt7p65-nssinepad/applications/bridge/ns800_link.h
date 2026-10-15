/**
 * @file    ns800_link.h
 * @brief   ESP32-C3 ⇄ NS800RT7P65 串口链路协议（两块板共用的唯一权威定义）
 *
 * 两块板（NS800 的 RT-Thread 工程与 C3 的 ESP-IDF 工程）各自持有一份本文件副本，
 * 工程间无共享路径，靠「内容一致」约束。修改协议时必须同步两份副本，
 * 否则会表现为「一端发得出、另一端认不出」或反之。
 *
 * 帧格式（字节序一律小端）：
 *
 *   0xAA  0x55  LEN  PAYLOAD  CRC8
 *   SOF   SOF    payload 见下   校验
 *
 * LEN = PAYLOAD 的字节数（不含 SOF/CRC），有效范围 1..NS800_LINK_MAX_PAYLOAD。
 * CRC8 对 [LEN..PAYLOAD 末尾] 计算，算法 CRC-8/ATM（poly 0x07, init 0x00）。
 *
 * 采用双字节 SOF（0xAA 0x55）而非单字节：单字节 SOF 在噪声或半包时容易在
 * 数据里撞出假帧头，双字节将误判率降到可忽略。
 *
 * LEN 不含自身的 1 字节，这是最易写错处：收帧状态机读完 LEN 后要再读 LEN 个
 * 字节的 payload。
 *
 * 方向约定：C3 → NS800 为传感器/网络数据（NS800 是显示端，被动接收）；
 * NS800 → C3 为控制命令（如「重连 WiFi」「开始上报」）。两方向共用同一帧格式，
 * 靠 CMD 区分。
 */

#ifndef __NS800_LINK_H__
#define __NS800_LINK_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── 物理层参数 ──
 * 波特率两端必须一致，否则收到的全是乱码，且很难一眼看出是波特率问题。 */
#define NS800_LINK_BAUD             115200

/* 帧头同步字：两字节 0xAA 0x55 连续出现 */
#define NS800_LINK_SOF0             0xAAu
#define NS800_LINK_SOF1             0x55u

/* 单帧最大 payload：96 字节足够放一条 JSON 片段，又不会撑爆发送端栈
 * 与接收端缓冲（两端都按此上限做定长分配）。 */
#define NS800_LINK_MAX_PAYLOAD      96

/* ── 命令码 ── */

/* C3 → NS800 */
#define NS800_CMD_HELLO             0x01u   /* 上线问候，payload = 固件版本字符串 */
#define NS800_CMD_WIFI_STATE        0x02u   /* WiFi 状态变化，payload = 状态枚举(1B) */
#define NS800_CMD_SENSOR            0x03u   /* 传感器数据，payload = 紧凑二进制 */
#define NS800_CMD_TEXT              0x04u   /* 通用文本（调试/公告），payload = UTF-8 */
#define NS800_CMD_SERVER_RESP       0x05u   /* 服务器回包摘要，payload=状态码(2B)+文本 */
#define NS800_CMD_WIFI_CFG          0x06u   /* 回报当前 WiFi 配置（配网 App 请求时发） */
#define NS800_CMD_OTA_PROGRESS      0x07u   /* OTA 进度/结果，状态(1B)+百分比(1B) */
#define NS800_CMD_TIME              0x08u   /* 时间同步（C3 NTP 对时后下发），载荷见下 */

/* NS800 → C3 */
#define NS800_CMD_PING              0x81u   /* 心跳探测，payload = 序号(1B) */
#define NS800_CMD_REQ_WIFI          0x82u   /* 请求重连 WiFi */
#define NS800_CMD_SET_REPORT        0x83u   /* 设置上报周期，payload = 秒(2B, 小端) */
#define NS800_CMD_SET_WIFI_CFG      0x84u   /* 下发 WiFi 配置并连接（见下方载荷格式） */
#define NS800_CMD_GET_WIFI_CFG      0x85u   /* 请求 C3 回报当前 WiFi 配置（无 payload） */
#define NS800_CMD_OTA_START         0x86u   /* 触发 OTA，固件 URL（UTF-8，无 '\0'） */
#define NS800_CMD_WIFI_FACTORY_RST  0x87u   /* 恢复出厂 WiFi 配置，C3 擦 NVS 后重启 */

/* 0x88：NS800 → C3 的通用数据上行通道。
 *
 * 0x81..0x87 均为控制命令：C3 收到后执行一个固定动作即结束。
 * 通用上行通道面向开放集合的数据（如温度设定、背光亮度、游戏分数），
 * 若每加一种就分配一个命令码，协议会迅速膨胀到几十条，且每次改协议都要
 * 同步两份头文件与两端代码。
 *
 * 故采用单条通用通道 + key/value 文本：0x88 的 payload 是一段 UTF-8 的
 * "key=value"（或 JSON 片段），C3 侧原样存入 host_data 模块，网页经
 * /api/host_data 可见。
 *
 * 选用文本而非二进制 KV（tag+type+len+value）：文本的最大好处是两端不必
 * 同步加代码 —— NS800 任意模块调用 c3_link_send_kv("backlight", "80") 即可上行，
 * C3 即便不认识该 key 也能原样存下。如需强类型，后续再加 tag 约定即可，
 * 旧数据仍兼容。在嵌入式链路上，可扩展性优先于极致紧凑。
 *
 * 载荷：无 header，纯文本，不含结尾 '\0'，长度由 LEN 决定，例如
 * "backlight=80"、"game_score=1280"，上限 NS800_LINK_MAX_PAYLOAD-1 = 95 字节。
 *
 * 单 key 只保留最新值（覆盖语义），不做历史队列 —— C3 侧仅维护一张
 * NVS/内存表，满则淘汰最旧项。这是刻意的：NS800 的状态语义本就是「当前值」
 * （亮度/音量/设定温度），存历史既无意义又占 flash。 */
#define NS800_CMD_HOST_DATA         0x88u

/* 0x87 用途：网页配置 WiFi 时若 SSID 填错，设备将连不上网、网页也随之失联，
 * 浏览器自身无法恢复。NS800 屏不依赖网络，故由其发出此命令让 C3 擦除 NVS
 * 并恢复出厂配置。C3 侧实现见 wifi_cfg_factory_reset()。 */

/* ── NS800_CMD_OTA_PROGRESS 载荷（C3 → NS800）──
 *
 *   offset  类型   含义
 *   0       uint8  status   0 = 进行中，1 = 成功，2 = 失败
 *   1       uint8  percent  0..100（status=2 时为失败时已完成的百分比）
 *
 * 失败的具体原因不在此帧内，而另发一条 NS800_CMD_TEXT（如 "OTA 失败: HTTP 连接失败"），
 * 原因文本长度不定，塞进定长进度帧会破坏格式；屏上把 TEXT 当提示行显示即可。
 */
#define NS800_OTA_ST_RUNNING        0u   /* 进行中 */
#define NS800_OTA_ST_OK             1u   /* 成功 */
#define NS800_OTA_ST_FAIL           2u   /* 失败 */

/* ── NS800_CMD_TIME 载荷（C3 → NS800）──
 *
 *   offset  类型   含义
 *   0       uint32 Unix 时间戳（小端，已换算为本地时间 UTC+8）
 *
 * 共 4 字节。只发时间戳而不发「年月日时分秒」：NS800 需自行换算星期，
 * 直接给时间戳最简单；时区换算统一在 C3 侧做一次（C3 具备完整 libc 时间函数），
 * NS800 仅做 tm 拆解，无需关心时区。
 *
 * 32 位时间戳在 2038 年溢出 —— 本项目用不到那么久，且协议为 4 字节定长；
 * 若需延寿，将 offset 0..3 改为 8 字节并在两端同步修改即可。
 *
 * 每秒发送一帧（需求约定）。帧总长 = 2(SOF)+1(LEN)+1(cmd)+4(ts)+1(CRC) = 9 字节，
 * 115200 波特率下传输约 0.8 ms，对 96 字节上限的链路无压力。
 *
 * 本文件为两份拷贝之一（另一份在 NS800 工程的 applications/bridge/ns800_link.h），
 * 改动须两端同步，无软链。 */

/* ── NS800_CMD_OTA_START 载荷（NS800 → C3）──
 *   固件 .bin 的完整 URL 字符串，UTF-8，不含结尾 '\0'，
 *   长度由帧的 LEN 字段决定（上限 NS800_LINK_MAX_PAYLOAD-1 = 95 字节，
 *   足够放 "http://192.168.1.100:8000/c3_bridge.bin" 这类局域网地址）。
 */

/* ── 配网载荷格式（NS800_CMD_SET_WIFI_CFG / NS800_CMD_WIFI_CFG）──
 *
 *   offset  类型   含义
 *   0       uint8  mode      0 = STA（连路由器），1 = AP（自建热点）
 *   1       uint8  ssid_len  SSID 字节数（1..NS800_WIFI_SSID_MAX）
 *   2       ...    ssid      SSID（不含 '\0'）
 *   2+ssid_len  uint8 pass_len  密码字节数（0..NS800_WIFI_PASS_MAX，AP 模式允许 0）
 *   3+ssid_len  ...   pass      密码（不含 '\0'）
 *
 * 两个字符串均带独立长度字段而非靠 '\0' 分隔：SSID/密码是二进制安全的字节串，
 * SSID 中出现 '\0' 虽罕见但合法；带长度可精确切分，且两端解析逻辑对称，
 * 不依赖任何字符串函数。
 *
 * 长度上限来源：协议单帧 payload 上限 = 96 字节。
 * 开销 = 1(mode)+1(ssid_len)+ssid+1(pass_len)+pass ≤ 96 → ssid+pass ≤ 93。
 * 取 SSID_MAX=24 / PASS_MAX=32（合计 56，留足余量），覆盖绝大多数家用路由器
 * （SSID 通常 ≤ 24）与 WPA2 密码（8..63，取 32 够日常）。
 * 取舍为「限制长度以换单帧传输」，不做分帧。
 */
#define NS800_WIFI_SSID_MAX         24u  /* SSID 字节数上限 */
#define NS800_WIFI_PASS_MAX         32u  /* 密码字节数上限 */

/* WiFi 工作模式（配网载荷 offset 0） */
#define NS800_WIFI_MODE_STA         0u   /* 连路由器 */
#define NS800_WIFI_MODE_AP          1u   /* 自建热点 */

/* WiFi 状态枚举（NS800_CMD_WIFI_STATE 的 payload[0]）—— 与 NS800 端一一对应 */
#define NS800_WIFI_IDLE             0u   /* 空闲 */
#define NS800_WIFI_CONNECTING       1u   /* 连接中 */
#define NS800_WIFI_CONNECTED        2u   /* 已连接，尚未取得 IP */
#define NS800_WIFI_GOT_IP           3u   /* 已取得 IP */
#define NS800_WIFI_DISCONNECTED     4u   /* 已断开 */
#define NS800_WIFI_FAIL             5u   /* 连接失败 */

/* ── 传感器数据载荷（NS800_CMD_SENSOR）──
 * 定长二进制，不用字符串 —— 省带宽、避免 float 格式化分歧
 *
 *   offset  类型     含义
 *   0       int16   C3 芯片温度 × 100      （℃ ×100，例如 2563 = 25.63℃）
 *   2       int16   WiFi RSSI（dBm，带符号，如 -30）
 *   4       uint16  空闲堆 / 1024（KB）
 *   6       uint32  运行秒数
 *   ---- 共 10 字节 ----
 *
 * RSSI 符号约定：线上即为带符号补码。C3 的 wifi_mgr_rssi() 直接返回 -30，
 * send_sensor() 按 (rssi >> 8) & 0xFF 拆成两字节发出，即 -30 的补码 0xE2 0xFF。
 * 接收端须按 int16 直接解释、不得再取负（否则 -30 → 65506 → +30，
 * 屏上显示成 "30 dBm"，负号凭空消失）。
 *
 * 用 int16 存「温度×100」而非 float：两块芯片的 float 虽均为 IEEE754，
 * 但定长整数在任意一端都易于打印、比对，且不依赖字节对齐。 */
#define NS800_SENSOR_PAYLOAD_LEN    10

/* ── CRC8 (CRC-8/ATM: poly=0x07, init=0x00, no reflect, no xorout) ── */
static inline uint8_t ns800_crc8(const uint8_t *data, uint16_t len)
{
    uint8_t crc = 0x00;
    uint16_t i;
    int b;

    for (i = 0; i < len; i++)
    {
        crc ^= data[i];
        for (b = 0; b < 8; b++)
            crc = (uint8_t)((crc & 0x80) ? ((crc << 1) ^ 0x07) : (crc << 1));
    }
    return crc;
}

/* 把一帧的 LEN + PAYLOAD 拼出来后算 CRC（收/发两端都用这个，保证一致） */
static inline uint8_t ns800_frame_crc(const uint8_t *len_and_payload, uint16_t n)
{
    return ns800_crc8(len_and_payload, n);
}

/* ── 收到的一帧 ──
 * payload[0] 即命令码（发送端把 cmd 编入 payload 段首字节，使 cmd 与数据
 * 共享同一 CRC 保护域）。cmd 字段仅为便利别名。
 *
 * 该类型定义在共享头中而非各自工程，以避免出现多份不一致的协议类型定义。 */
typedef struct
{
    uint8_t cmd;                              /* = payload[0] */
    uint8_t len;                              /* payload 总长（含 cmd 那 1 字节） */
    uint8_t payload[NS800_LINK_MAX_PAYLOAD];  /* 命令码 + 数据，长度由 len 决定 */
} ns800_frame_t;

#ifdef __cplusplus
}
#endif

#endif /* __NS800_LINK_H__ */
