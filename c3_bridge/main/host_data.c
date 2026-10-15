/**
 * @file    host_data.c
 * @brief   NS800 → C3 通用数据上行通道的接收端存储
 *
 * 详见 host_data.h 的设计说明。核心是一张 16 槽的定长 KV 表，采用覆盖写入。
 */

#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "host_data.h"

/* 槽位：key 空串 = 未使用。seq 用于判断"最旧"（挑最小的顶掉）。 */
static host_data_kv_t s_tbl[HOST_DATA_SLOTS];
static uint32_t       s_seq;
static uint32_t       s_rx_count;

/* 最近一次收到的原始文本，留一小段便于网页上直接看到畸形串 */
static char           s_last_raw[HOST_DATA_KEY_MAX + HOST_DATA_VAL_MAX + 4];

static SemaphoreHandle_t s_lock;

/* 定长截断拷贝：显式在结尾写 '\0'，调用方永远可按 C 字符串安全使用。
 * 不用 strncpy，因为它在源长于 n 时不补 '\0'，是定长槽越界的经典来源。 */
static void copy_clamped(char *dst, uint32_t dst_size, const char *src, uint32_t n)
{
    if (n >= dst_size) n = dst_size - 1;   /* 留 1 字节给 '\0' */
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/**
 * @brief  初始化：清空整张表、序号归零，并创建互斥锁（仅首次）
 */
void host_data_init(void)
{
    memset(s_tbl, 0, sizeof(s_tbl));
    s_seq      = 0;
    s_rx_count = 0;
    s_last_raw[0] = '\0';

    if (s_lock == NULL)
    {
        s_lock = xSemaphoreCreateMutex();
    }
}

/* 找 key 相同的槽；找不到返回 -1。调用者须已持锁。 */
static int find_slot(const char *key)
{
    for (int i = 0; i < HOST_DATA_SLOTS; i++)
    {
        if (s_tbl[i].key[0] != '\0' && strcmp(s_tbl[i].key, key) == 0) return i;
    }
    return -1;
}

/* 挑一个空槽；全满则挑 seq 最小的（最旧）。调用者须已持锁。 */
static int pick_slot(void)
{
    int oldest = 0;
    for (int i = 0; i < HOST_DATA_SLOTS; i++)
    {
        if (s_tbl[i].key[0] == '\0') return i;      /* 空槽优先 */
        if (s_tbl[i].seq < s_tbl[oldest].seq) oldest = i;
    }
    return oldest;
}

/**
 * @brief  写入一条 "key=value"
 * @param  text  UTF-8 文本，可不含结尾 '\0'（长度由 len 给出）
 * @param  len   text 实际字节数
 * @return ESP_OK 成功；ESP_ERR_INVALID_ARG 参数非法或 key 为空
 * @note   取第一个 '=' 分隔；无 '=' 时整串当 key、value 为空。
 *         key / value 超长从尾部截断，写入定长槽不越界。
 */
int host_data_put(const char *text, uint32_t len)
{
    const char *eq;
    char        key[HOST_DATA_KEY_MAX + 1];
    int         slot;
    uint32_t    klen, vlen;

    if (text == NULL || len == 0) return ESP_ERR_INVALID_ARG;

    /* 先把原始文本留档（在截断之前），方便调试时看到主机发的原样 */
    copy_clamped(s_last_raw, sizeof(s_last_raw), text, len);
    s_rx_count++;

    /* 找第一个 '='，使 value 内可再含 '='（如 "url=http://x?a=1" 不会被截断） */
    eq = memchr(text, '=', len);
    if (eq == NULL)
    {
        /* 无 '='：整串当作 key，value 置空，不返回错误。
         * 收到即存下，优于丢弃；网页上也能看到主机到底发了什么。 */
        copy_clamped(key, sizeof(key), text, len);
        klen = (uint32_t)strlen(key);
        vlen = 0;
    }
    else
    {
        klen = (uint32_t)(eq - text);
        copy_clamped(key, sizeof(key), text, klen);
        klen = (uint32_t)strlen(key);      /* 截断后的真实长度 */

        vlen = len - (uint32_t)(eq - text) - 1;
        if (klen == 0) return ESP_ERR_INVALID_ARG;   /* "=value" 空 key 无意义 */
    }

    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);

    slot = find_slot(key);
    if (slot < 0) slot = pick_slot();

    copy_clamped(s_tbl[slot].key, HOST_DATA_KEY_MAX + 1, key, klen);
    if (eq)
    {
        copy_clamped(s_tbl[slot].val, HOST_DATA_VAL_MAX + 1, eq + 1, vlen);
    }
    else
    {
        s_tbl[slot].val[0] = '\0';
    }
    s_tbl[slot].seq = ++s_seq;

    if (s_lock) xSemaphoreGive(s_lock);
    return ESP_OK;
}

/**
 * @brief  按 key 取当前值
 * @param  key      键名
 * @param  out      输出缓冲，至少 HOST_DATA_VAL_MAX+1 字节
 * @param  out_size out 大小
 * @return ESP_OK 找到；ESP_ERR_NOT_FOUND 无此 key；ESP_ERR_INVALID_ARG 参数非法
 */
int host_data_get(const char *key, char *out, uint32_t out_size)
{
    int slot, ret = ESP_ERR_NOT_FOUND;

    if (key == NULL || out == NULL || out_size == 0) return ESP_ERR_INVALID_ARG;

    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);

    slot = find_slot(key);
    if (slot >= 0)
    {
        copy_clamped(out, out_size, s_tbl[slot].val, (uint32_t)strlen(s_tbl[slot].val));
        ret = ESP_OK;
    }

    if (s_lock) xSemaphoreGive(s_lock);
    return ret;
}

/**
 * @brief  拷贝整张表（供 /api/host_data 使用）
 * @param  out  输出数组，至少 HOST_DATA_SLOTS 个元素
 * @param  max  out 可容纳的槽数
 * @return 实际有效槽数（key 非空的条数）
 */
uint32_t host_data_snapshot(host_data_kv_t *out, uint32_t max)
{
    uint32_t n = 0;

    if (out == NULL || max == 0) return 0;
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);

    for (uint32_t i = 0; i < HOST_DATA_SLOTS && n < max; i++)
    {
        if (s_tbl[i].key[0] != '\0') out[n++] = s_tbl[i];
    }

    if (s_lock) xSemaphoreGive(s_lock);
    return n;
}

/**
 * @brief  返回最近一次写入的原始文本（调试用）
 */
const char *host_data_last_raw(void)
{
    return s_last_raw;
}

/**
 * @brief  累计收到的 HOST_DATA 帧数
 * @note   用于区分“没推”和“推了但被丢弃”
 */
uint32_t host_data_rx_count(void)
{
    return s_rx_count;
}
