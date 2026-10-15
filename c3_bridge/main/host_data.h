/**
 * @file    host_data.h
 * @brief   NS800 → C3 通用数据上行通道的接收端存储（对应 NS800_CMD_HOST_DATA 0x88）
 *
 * 为何需要本模块：
 *   链路 0x81..0x87 均为控制命令（C3 收到后执行固定动作）。
 *   但 NS800 需把数据告诉 C3，且数据种类开放（温度设定、背光亮度、游戏分数…），
 *   不可能每种分配一个命令码。故协议定了一条通用通道 0x88，payload 为一段
 *   "key=value" 的 UTF-8 文本，本模块负责拆开存储。
 *
 * 存储策略（刻意保持简单）：
 *   - 覆盖语义：同一 key 再推一次即替换旧值，不留历史；推的都是当前状态，
 *     历史无意义。
 *   - 定长表 + 满则顶掉最旧：16 条槽，每条 key/value 各 32 字节。
 *     全程静态分配，不调 malloc，少一个分配点少一个失败源。
 *   - 不落 NVS：这是易失的调试/同步通道，掉电丢光无妨；
 *     需持久化的数据应走专门命令，不要塞进这里。
 */

#ifndef __HOST_DATA_H__
#define __HOST_DATA_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** KV 表槽位数。满了之后新 key 会顶掉最旧的一条。
 *  取 16 而非更小的 8：NS800 的 c3_report.c 一轮就推 13 个字段
 *  （背光 / 三项调色 / 游戏最高分 / 运行时长 / 温湿度 / 链路状态 / 版本）。
 *  槽位少于字段数时，“覆盖 + 顶掉最旧”会让关键字段互相挤掉，
 *  网页上看到的 key 集合随写入顺序漂移，难以排查；
 *  槽位不少于单轮字段数即无意外丢失。
 *  每槽约 72 字节（key32+val32+seq4），16 槽约 1.2KB 静态内存，
 *  C3 有 190KB 堆，开销可忽略。只加槽不用动协议，也不用动 NS800。 */
#define HOST_DATA_SLOTS        16
#define HOST_DATA_KEY_MAX      32      /* 不含结尾 '\0' */
#define HOST_DATA_VAL_MAX      32      /* 不含结尾 '\0' */

typedef struct
{
    char     key[HOST_DATA_KEY_MAX + 1];      /* 以 '\0' 结尾 */
    char     val[HOST_DATA_VAL_MAX + 1];      /* 以 '\0' 结尾 */
    uint32_t seq;                     /* 递增序号，网页按它排序 / 看新鲜度 */
} host_data_kv_t;

/**
 * @brief 初始化（清空整张表并把序号归零）
 */
void host_data_init(void);

/**
 * @brief 写入一条 key=value
 * @param text  "key=value" 形式的 UTF-8 文本，可不含结尾 '\0'
 *              （长度由 ns800_link 的 LEN 字段给出）
 * @param len   text 的实际字节数
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 参数非法或 key 为空
 * @note   自动裁剪：key / value 超长均从尾部截断，写入定长槽不越界，
 *         比直接返回错误更有用 —— 主机推了超长串，仍能记下能记下的部分。
 */
int host_data_put(const char *text, uint32_t len);

/**
 * @brief 按 key 取当前值
 * @param out 至少 HOST_DATA_VAL_MAX+1 字节
 * @return ESP_OK 找到；ESP_ERR_NOT_FOUND 没有这个 key
 */
int host_data_get(const char *key, char *out, uint32_t out_size);

/**
 * @brief 拷贝整张表出来（给 /api/host_data 用）
 * @param out   至少 HOST_DATA_SLOTS 个元素
 * @param max   out 能容纳的槽数
 * @return 实际有效槽数（key 非空的条数）
 */
uint32_t host_data_snapshot(host_data_kv_t *out, uint32_t max);

/**
 * @brief 最近一次写入的原始文本（调试用：能看到主机到底发了什么畸形串）
 */
const char *host_data_last_raw(void);

/**
 * @brief 累计收到的 HOST_DATA 帧数（区分"没推"和"推了但被丢了"）
 */
uint32_t host_data_rx_count(void);

#ifdef __cplusplus
}
#endif

#endif /* __HOST_DATA_H__ */
