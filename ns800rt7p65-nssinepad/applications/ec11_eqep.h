/**
 * @file    ec11_eqep.h
 * @brief   EC11 旋转编码器驱动接口：片内 eQEP3 硬件正交解码
 *
 * 对外接口：
 *   ec11_eqep_init()      初始化（时钟 -> 引脚 -> 解码器 -> 计数器 -> 使能，带回读重试）
 *   ec11_is_ready()       初始化是否成功
 *   ec11_get_position()   原始计数（rt_uint32_t，调用方用无符号差值）
 *   ec11_get_steps()      格数（rt_int32_t，一格 = 1，含回绕处理，可直接做索引）
 *   ec11_get_direction()  1 = 正转，-1 = 反转
 *   ec11_reset_position() 清零
 *
 * 调用约定：由 lv_port_indev_init() 在 LCD 初始化之后调用 ec11_eqep_init()。
 */

#ifndef __EC11_EQEP_H__
#define __EC11_EQEP_H__

#include <rtthread.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  初始化 eQEP3（时钟 + 释放复位 + 引脚三步配置 + 解码器 + 位置计数器 + 使能）
 * @return RT_EOK 成功；-RT_ERROR 配置始终无法写入
 * @note   内部带回读校验，未生效时自动重试，最多 3 轮。
 */
rt_err_t ec11_eqep_init(void);

/**
 * @brief  查询初始化是否成功
 * @return RT_TRUE 已就绪；RT_FALSE 未就绪（此时 get_position 恒返回 0）
 */
rt_bool_t ec11_is_ready(void);

/**
 * @brief  读取原始位置计数（32 位自由累加，2 倍频）
 * @return 位置计数；未初始化时返回 0
 * @note   调用方用无符号差值取增量。
 */
rt_uint32_t ec11_get_position(void);

/**
 * @brief  读取当前格数（有符号）：一格棘爪 = 1，顺时针为正、逆时针为负
 * @return 格数；未初始化时返回 0
 * @note   由 eQEP 位置计数除以每格计数得到，无回绕处理；
 *         调用方需自行做取模或限幅，以适配菜单索引。
 */
rt_int32_t ec11_get_steps(void);

/**
 * @brief  读取当前旋转方向
 * @return 1 = 正转（QDIR = 1）；-1 = 反转
 */
rt_int16_t ec11_get_direction(void);

/**
 * @brief  位置清零
 * @note   同时手动清除 QEPCTL.SWI 位，原因见 ec11_eqep.c 内注释。
 */
void ec11_reset_position(void);

#ifdef __cplusplus
}
#endif

#endif /* __EC11_EQEP_H__ */
