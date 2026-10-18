/**
  * @file       board.h
  * @author     Haven-X
  * @brief      Header file of board.c
  *
  * <h2><center>&copy; Copyright (c) 2025 Novosense Limited.
  * All rights reserved.</center></h2>
  */

#ifndef __BOARD_H__
#define __BOARD_H__

#include <rtthread.h>
#include "NS800RTxxxx.h"
#include "drv_common.h"
#include "drv_gpio.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define NS800_FLASH_START_ADRESS     ((uint32_t)0x08000000)
#define NS800_FLASH_SIZE             (512 * 1024)
#define NS800_FLASH_END_ADDRESS      ((uint32_t)(NS800_FLASH_START_ADRESS + NS800_FLASH_SIZE))

#define NS800RT7P65_SRAM_SIZE           (256)

extern int Image$$RW_RT_HEAD$$Limit;
#define HEAP_BEGIN      (&Image$$RW_RT_HEAD$$Limit)
/* 堆大小 56K (0xE000)：原为 16K，LVGL widgets demo 需要更大的堆，
 * 已把原 DTCM_EXT(40K) 并入。改这里必须同步改 board/linker_scripts/link.lds 的 RT_HEAP LENGTH */
#define HEAP_END        ((uint32_t)&Image$$RW_RT_HEAD$$Limit + 0xE000)


/*******************************************************************************
 * Definitions
 ******************************************************************************/

/** @defgroup BOARD_LEDx_PIN Board LEDx PIN
  * @{
  * @brief  Defines configuration macros for board ledx pin.
  */
#define BOARD_LED1_PIN    GPIO_68    /* reserve */
#define BOARD_LED2_PIN    GPIO_69    /* reserve */

/**
  * @}
  */

/** @defgroup BOARD_SWITCHx_PIN Board SWITCHx PIN
  * @{
  * @brief  Defines configuration macros for board switchx pin.
  */
#define BOARD_KEY_PIN    GPIO_41


/**
  * @}
  */

/** @defgroup SERIAL_COM_PIN Serial Com port Pins
  * @{
  * @brief  Defines configuration macros for serial com port pin.
  */
#define BOARD_SERIALCOM_TX_PIN    GPIO_12
#define BOARD_SERIALCOM_RX_PIN    GPIO_13

/**
  * @}
  */

/** @defgroup C3_LINK_PIN ESP32-C3 板间链路串口引脚
  * @{
  * @brief  与 ESP32-C3 通信的专用 UART2（PB6/PB7，ALT1）。
  *         UART1 被控制台占用，因此另开一路；PB6/PB7 当前无其他外设使用。
  *         接线：NS800 PB6(TX) -> C3 GPIO5(RX)，NS800 PB7(RX) <- C3 GPIO4(TX)，
  *               两侧 GND 必须共地。
  *
  *   编号陷阱：本 SDK 的全局编号 `GPIO_n` **按物理封装引脚顺序**排，
  *    和 port 字母不是简单对齐的（中间夹了 GPIOF 的散脚）。注意别搞错：
  *      GPIO_6  = PA6   ← **不是** PB6！
  *      GPIO_7  = PA7   ← **不是** PB7！
  *      GPIO_38 = PB6   ✓
  *      GPIO_39 = PB7   ✓
  *    权威定义在 StdDriver/Inc/ti/NS800RT7xxx_TI_gpio.h。
  *
  *   另注：下面这两个宏**仅作文档用途，驱动不读它们** —— UART2 的实际引脚
  *    写死在 libraries/HAL_Drivers/drivers/drv_uart.c 的 uart_config[] 里
  *    （`.tx_port = GPIOB, .tx_pin = GPIO_PIN_6` 等）。要换脚得改那边。
  *    这里用「port + pin」两部分写，和 drv_uart.c 的写法保持一致，
  *    免得有人误拿它去 rt_pin_read()（那是 rt_pin 体系的单值编号 GPIO_38）。
  */
#define BOARD_C3_LINK_TX_PORT     GPIOB       /* UART2_TX，ALT1 */
#define BOARD_C3_LINK_TX_PIN      GPIO_PIN_6  /* = PB6  = rt_pin 编号 GPIO_38 */
#define BOARD_C3_LINK_RX_PORT     GPIOB       /* UART2_RX，ALT1 */
#define BOARD_C3_LINK_RX_PIN      GPIO_PIN_7  /* = PB7  = rt_pin 编号 GPIO_39 */

/**
  * @}
  */

#define BOARD_SERIALCOM_BAUDRATE    (115200UL)
#define BOARD_SERIALCOM             UART1

/* 板间链路波特率：与 C3 侧 NS800_LINK_BAUD 保持一致，改动必须两边同步 */
#define BOARD_C3_LINK_BAUDRATE      (115200UL)
#define BOARD_C3_LINK               UART2


/*******************************************************************************
 * Variables
 ******************************************************************************/

/*******************************************************************************
 * Definitions
 ******************************************************************************/

/** @defgroup HXTL_CONFIG High-Speed Crystal Oscillator (HXTL) Configuration
  * @{
  * @brief  Defines configuration macros for the High-Speed Crystal Oscillator (HXTL).
  */
#ifdef SYSCLK_SOURCE_USE_HXTL
#define HXTL_CONFIG    ((RCC_HXTL_FILT_ENABLE << 15) | \
                        (RCC_HXTL_FEEDBACK_RES_0_8M << 13) | \
                        (RCC_HXTL_FEEDBACK_ENABLE << 12) | \
                        (RCC_HXTL_STARTUP_16384 << 8) | \
                        RCC_HXTL_24M_AGC_DISABLE)

#else
#define HXTL_CONFIG    0U
#endif

/**
  * @}
  */

/** @defgroup PLL_CONFIG PLL Configuration
  * @{
  * @brief  Defines configuration macros for the Phase-Locked Loop (PLL).
  * @note   Will configure the clock as follows:
  *          PLL_SYSCLK = PLLSOURCE  * PLLM / ( (PLLN+1) * (2 * (PLLP+1)))
  *
  *          PLLSOURCE optional: \b HXTL_FREQ_VALUE
  *                              \b MIRC1_FREQ_VALUE
  *                              \b MIRC2_FREQ_VALUE
  *          Example:
  *           if PLLSOURCE = HXTL_FREQ_VALUE = 20Mhz, PLLM_40, PLLN_DIV1, PLLP_DIV2,
  *
  *             PLL_SYSCLK = 20Mhz * 40 / ( (0+1) * (2 * (0+1)) ) = 400MHz
  */
#ifdef SYSCLK_USE_PLL
#ifdef PLLCLK_SOURCE_USE_HXTL
#define PLL_CONFIG    ((RCC_PLLM_40 << 8) | \
                       (RCC_PLLN_DIV1 << 4) | \
                       (RCC_PLLP_DIV2 << 17) | \
                       (RCC_PLLSOURCE_HXTL))
#else  /* ndef PLLCLK_SOURCE_USE_HXTL */
#ifdef PLLCLK_SOURCE_USE_MIRC2
#define PLL_CONFIG    ((RCC_PLLM_80 << 8) | \
                       (RCC_PLLN_DIV1 << 4) | \
                       (RCC_PLLP_DIV2 << 17) | \
                       (RCC_PLLSOURCE_MIRC2))
#else  /* ndef PLLCLK_SOURCE_USE_MIRC2 */
#define PLL_CONFIG    ((RCC_PLLM_52 << 8) | \
                       (RCC_PLLN_DIV1 << 4) | \
                       (RCC_PLLP_DIV2 << 17) | \
                       (RCC_PLLSOURCE_MIRC1))
#endif  /* PLLCLK_SOURCE_USE_MIRC2 */
#endif  /* PLLCLK_SOURCE_USE_HXTL */
#else  /* ndef SYSCLK_USE_PLL */
#define PLL_CONFIG    0U
#endif  /* SYSCLK_USE_PLL */

/**
  * @}
  */



/** @defgroup PLL_EXT_CONFIG PLL Extended Configuration
  * @{
  * @brief  Defines configuration macros for the Phase-Locked Loop (PLL) extended settings.
  */
#ifdef SYSCLK_USE_PLL
#define PLL_EXT_CONFIG    ((RCC_PLLQ_DIV8 << 27) | \
                           (RCC_PLLR_DIV8 << 22) | \
                           (RCC_PLL_FORCELK_DISABLE << 18) | \
                           (RCC_PLL_LKDT_SEL_0 << 16) | \
                           (RCC_PLL_LOOP_FILTER_CAP_84PF << 8) | \
                           (RCC_PLL_CHARGE_PUMP_CURRENT_4UA << 4) | \
                           (RCC_PLL_LOOP_FILTER_RES_10K << 2) | \
                           (RCC_PLL_VCO_400_800M))
#else
#define PLL_EXT_CONFIG   0U
#endif

/**
  * @}
  */

 /** @defgroup SYS_CLOCK_DIV_CONFIG Systerm Clock Division Configuration
  * @{
  * @brief  Defines macros for configuring system clock division settings.
  */
#ifdef SYSCLK_USE_PLL
#define SYSCLOCK_CFGR_DIV_CONFIG    ((RCC_APB5_HCLK_DIV4 << 28) | \
                                     (RCC_APB2_4_HCLK_DIV1 << 24) | \
                                     (RCC_APB1_3_HCLK_DIV1 << 20) | \
                                     (RCC_HCLKSOURCE_CPUCLK_2 << 12) | \
                                     (RCC_SYSCLK_DIV1 << 8))
#define SYSCLOCK_CFGR2_DIV_CONFIG    ((RCC_FLASH_DIV_2 << 0) | \
                                      (RCC_TRACE_DIV_2 << 4) | \
                                      (RCC_EMIF_DIV_2 << 8) | \
                                      (RCC_EPWM_DIV_1 << 12))

#else
#define SYSCLOCK_CFGR_DIV_CONFIG    ((RCC_APB5_HCLK_DIV1 << 28) | \
                                     (RCC_APB2_4_HCLK_DIV1 << 24) | \
                                     (RCC_APB1_3_HCLK_DIV1 << 20) | \
                                     (RCC_HCLKSOURCE_CPUCLK << 12) | \
                                     (RCC_SYSCLK_DIV1 << 8))
#define SYSCLOCK_CFGR2_DIV_CONFIG    ((RCC_FLASH_DIV_1 << 0) | \
                                      (RCC_TRACE_DIV_1 << 4) | \
                                      (RCC_EMIF_DIV_1 << 8) | \
                                      (RCC_EPWM_DIV_1 << 12))
#endif


/**
  * @}
  */

#define SYSTICK_TIME_UNIT    (1000UL)    /*!< Uint: 1M */




/*******************************************************************************
 * Functions
 ******************************************************************************/

/**
 * \brief  Function to initialize the device.
 * \note   Mainly initialize the system clock and enable all peripheral clocks.
 */
void Device_init(void);

/**
 * \brief  Set up clock source selection, PLL control, and clock dividers.
 */
void System_setClock(void);

/**
 * \brief  Enable clocks for all peripherals.
 */
void Device_enableAllPeripherals(void);

/**
 * \brief  Unlock all peripheral registers.
 */
void Device_unlockPeriphReg (void);

/**
 * \brief  Disable clocks for all peripherals.
 */
void Device_disableAllPeripherals (void);

/**
 *\@brief  Lock all peripheral registers.
 */
void Device_lockPeriphReg (void);

/**
 *\@brief  All peripherals are hung on the CUP1
 */
void Device_enableAllPeripheralsInCpu1 (void);


/**
 * \brief  Board Initialization.
 * \note   Call this function in your application if you wish to do all module
 *         initialization.
 *         If you wish to not use some of the initializations, instead of the
 *         Board_init use the individual Module_inits.
 */
void Board_init (void);

/**
 * \brief  PinMux Initialization.
 * \note   Call this function in your application if you want all
 *         PinMux initialization to be done.
 */
void PinMux_init (void);

/**
 * \brief  Board Serial Communication Interface Initialization.
 * \note   Call this function in the application to initialize the SCI serial port
 *         on the board for outputting debugging information.
 */
void SerialCOM_init(void);

/**
 * \brief  Board LED Initialization.
 * \note   Call this function in the application to initialize the LED
 *         on the board.
 */
void LED_init (void);

/**
 * \brief  Board Switch Initialization.
 * \note   Call this function in the application to initialize the Switch
 *         on the board.
 */
void Switch_init (void);


#ifdef __cplusplus
}
#endif


#endif  /* __BOARD_H__ */

