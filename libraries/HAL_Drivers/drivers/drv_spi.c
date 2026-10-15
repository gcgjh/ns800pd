/*
 * Copyright (c) 2006-2026, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2026-05-12     lugl         first version
 */

#include "board.h"
#include "drv_spi.h"
#include "drv_config.h"

#ifdef RT_USING_SPI

#define DRV_DEBUG
#define LOG_TAG             "drv.spi"
#include <drv_log.h>

#if !defined(BSP_USING_SPI1) && !defined(BSP_USING_SPI2) && !defined(BSP_USING_SPI3) && \
    !defined(BSP_USING_SPI4)
#error "Please define at least one BSP_USING_SPIx"
#endif

/* ── NS800: SPI 批量传输开关 ─────────────────────────────────────────
 *
 * ⚠️ **保持为 0（2026-10-11 实测踩坑）**
 *
 * 曾尝试用 SPI_pollingFifoTransaction() 把逐字节发送换成 FIFO 批量，
 * 结果**屏上出现噪点/条纹/错位**（`lcdcolor preview 0/1/2` 纯色块测试
 * 直接暴露：颜色不纯、有斜纹）。
 *
 * 根因：该 API 的 `pTxBuffer` 是 **uint32_t\***，按 32bit 字步进；
 * 而本驱动 data_width 是 8bit（SPI_BIT_WIDTH_8_BITS），
 * 传进去的却是 8bit 数据的字节长度 —— 长度与元素宽度不匹配，
 * 每次搬运 4 字节当 1 个 "word" 发出，数据整条错位。
 * 要正确用它必须先确认：
 *   ① data_width=8 时 FIFO 里 1 项到底是 1 字节还是 1 个 32bit 字；
 *   ② numOfWords 的语义（字节 / 字 / FIFO 项）。
 * 在把这两件事彻底核实（查手册或示波器打 SPI 波形）之前，不要打开。
 *
 * 真正的性能瓶颈在 UI 层（全屏 invalidate + 每帧写 gamma），
 * 已在 lcd_tune_ui.c 修掉，不需要动 SPI 驱动。
 */
#define BSP_SPI_XFER_FIFO   0

enum
{
#ifdef BSP_USING_SPI1
    SPI1_INDEX,
#endif
#ifdef BSP_USING_SPI2
    SPI2_INDEX,
#endif
#ifdef BSP_USING_SPI3
    SPI3_INDEX,
#endif
#ifdef BSP_USING_SPI4
    SPI4_INDEX,
#endif
};

static struct ns800_spi_config spi_config[] =
{
#ifdef BSP_USING_SPI1
    {
        .name = "spi1",
        .Instance = SPI1,
        .rx_irq_type = SPI1_RX_IRQn,
        .tx_irq_type = SPI1_TX_IRQn,
        .sck_port = GPIOA,
        .sck_pin = GPIO_PIN_18,
        .sck_mux = ALT1_FUNCTION,
        .mosi_port = GPIOA,
        .mosi_pin = GPIO_PIN_16,
        .mosi_mux = ALT1_FUNCTION,
        .miso_port = GPIOA,
        .miso_pin = GPIO_PIN_17,
        .miso_mux = ALT1_FUNCTION,
    },
#endif
#ifdef BSP_USING_SPI2
    {
        .name = "spi2",
        .Instance = SPI2,
        .rx_irq_type = SPI2_RX_IRQn,
        .tx_irq_type = SPI2_TX_IRQn,
        .sck_port = GPIOB,
        .sck_pin = GPIO_PIN_0,
        .sck_mux = ALT9_FUNCTION,
        .mosi_port = GPIOB,
        .mosi_pin = GPIO_PIN_1,
        .mosi_mux = ALT9_FUNCTION,
        .miso_port = GPIOB,
        .miso_pin = GPIO_PIN_2,
        .miso_mux = ALT9_FUNCTION,
    },
#endif
#ifdef BSP_USING_SPI3
    {
        /* ⚠️ 原模板写的是 GPIOC_0/1/2 + ALT7，实测**无效**：
         *    GPIOC_0/1/2 = GPIO_64/65/66，它们的复用表里根本没有 SPI3
         *    （ALT15 是 SPI2、ALT6 是 UART1），配了也不会出波形。
         * 正确做法：按 GPIO 复用表选"同一 ALT 值"的一组三个脚。
         *    SPI3 可选组：
         *      ALT14: GPIO_20/21/22 —— GPIO_22 已作 LCD 背光，排除
         *      ALT6 : GPIO_50/51/52 (GPIOB_18/19/20) —— 全空闲，采用 ✅
         *      ALT15: GPIO_69/70/71 —— GPIO_69 被 board.c 初始化成 LED2，排除
         * 注意别撞已占引脚 {1,6,7,12,13,40,41,68,69}。 */
        .name = "spi3",
        .Instance = SPI3,
        .rx_irq_type = SPI3_RX_IRQn,
        .tx_irq_type = SPI3_TX_IRQn,
        .sck_port = GPIOB,
        .sck_pin = GPIO_PIN_20,        /* GPIO_52 */
        .sck_mux = ALT6_FUNCTION,
        .mosi_port = GPIOB,
        .mosi_pin = GPIO_PIN_18,       /* GPIO_50 */
        .mosi_mux = ALT6_FUNCTION,
        .miso_port = GPIOB,
        .miso_pin = GPIO_PIN_19,       /* GPIO_51 */
        .miso_mux = ALT6_FUNCTION,
    },
#endif
#ifdef BSP_USING_SPI4
    {
        .name = "spi4",
        .Instance = SPI4,
        .rx_irq_type = SPI4_RX_IRQn,
        .tx_irq_type = SPI4_TX_IRQn,
        .sck_port = GPIOC,
        .sck_pin = GPIO_PIN_4,
        .sck_mux = ALT7_FUNCTION,
        .mosi_port = GPIOC,
        .mosi_pin = GPIO_PIN_5,
        .mosi_mux = ALT7_FUNCTION,
        .miso_port = GPIOC,
        .miso_pin = GPIO_PIN_6,
        .miso_mux = ALT7_FUNCTION,
    },
#endif
};

static struct ns800_spi spi_obj[sizeof(spi_config) / sizeof(spi_config[0])] = {0};

static void ns800_spi_gpio_init(const struct ns800_spi_config *config)
{
    RT_ASSERT(config != RT_NULL);

    GPIO_setPinConfig(config->sck_port, config->sck_pin, config->sck_mux);
    GPIO_setAnalogMode(config->sck_port, config->sck_pin, GPIO_ANALOG_DISABLED);
    GPIO_setPadConfig(config->sck_port, config->sck_pin, GPIO_PIN_TYPE_STD);
    GPIO_setQualificationMode(config->sck_port, config->sck_pin, GPIO_QUAL_SYNC);
    GPIO_setDirectionMode(config->sck_port, config->sck_pin, GPIO_DIR_MODE_OUT);

    GPIO_setPinConfig(config->mosi_port, config->mosi_pin, config->mosi_mux);
    GPIO_setAnalogMode(config->mosi_port, config->mosi_pin, GPIO_ANALOG_DISABLED);
    GPIO_setPadConfig(config->mosi_port, config->mosi_pin, GPIO_PIN_TYPE_STD);
    GPIO_setQualificationMode(config->mosi_port, config->mosi_pin, GPIO_QUAL_SYNC);
    GPIO_setDirectionMode(config->mosi_port, config->mosi_pin, GPIO_DIR_MODE_OUT);

    GPIO_setPinConfig(config->miso_port, config->miso_pin, config->miso_mux);
    GPIO_setAnalogMode(config->miso_port, config->miso_pin, GPIO_ANALOG_DISABLED);
    GPIO_setPadConfig(config->miso_port, config->miso_pin, GPIO_PIN_TYPE_PULLUP);
    GPIO_setQualificationMode(config->miso_port, config->miso_pin, GPIO_QUAL_SYNC);
    GPIO_setDirectionMode(config->miso_port, config->miso_pin, GPIO_DIR_MODE_IN);
}

static rt_err_t ns800_spi_configure(struct rt_spi_device *device, struct rt_spi_configuration *configuration)
{
    struct ns800_spi *spi;
    SPI_TransferProtocol protocol;
    SPI_BitWidth data_width;

    RT_ASSERT(device != RT_NULL);
    RT_ASSERT(configuration != RT_NULL);

    spi = rt_container_of(device->bus, struct ns800_spi, spi_bus);

    ns800_spi_gpio_init(spi->config);

    switch (configuration->mode & RT_SPI_MODE_3)
    {
    case RT_SPI_MODE_0:
        protocol = SPI_PROT_POL0PHA0;
        break;
    case RT_SPI_MODE_1:
        protocol = SPI_PROT_POL0PHA1;
        break;
    case RT_SPI_MODE_2:
        protocol = SPI_PROT_POL1PHA0;
        break;
    case RT_SPI_MODE_3:
        protocol = SPI_PROT_POL1PHA1;
        break;
    default:
        protocol = SPI_PROT_POL0PHA0;
        break;
    }

    switch (configuration->data_width)
    {
    case 8:
        data_width = SPI_BIT_WIDTH_8_BITS;
        break;
    case 16:
        data_width = SPI_BIT_WIDTH_16_BITS;
        break;
    default:
        data_width = SPI_BIT_WIDTH_8_BITS;
        break;
    }

    SPI_disableModule(spi->config->Instance);

    SPI_setConfig(spi->config->Instance,
                  protocol,
                  SPI_MASTER_MODE,
                  SPI_FULL_DUPLEX_COMM_MODE,
                  configuration->max_hz,
                  data_width);

    SPI_resetFifo(spi->config->Instance);
    SPI_enableModule(spi->config->Instance);

    return RT_EOK;
}

#if BSP_SPI_XFER_FIFO
/* ── NS800: 纯发送走 FIFO 批量（原逐字节实现见下方 #else 分支）─────────
 *
 * 为什么快：
 *   SPI_transmitReceive() 是「发一字节就等一字节」，发送期间 SPI 时钟被
 *   等待循环拖住；这里一次把 16 个字（FIFO 深度）灌进 TX FIFO，
 *   SPI 硬件持续发送，再整批收 RX FIFO，时钟利用率接近 100%。
 *
 * 为什么返回值无副作用：
 *   发送时我们从 SPI 读回的 RX 数据是 dummy（MOSI 上没有意义的回读），
 *   调用方本来就会丢弃，所以整批丢掉即可。
 *
 * ⚠️ 注意 numOfWords 是「字」数不是字节数：
 *   本驱动 data_width 会把 8bit 配成 SPI_BIT_WIDTH_8_BITS，此时 1 字 = 1 字节。
 *   SPI_pollingFifoTransaction 内部按 SPI_FIFO_TXEMPTY(=16) 分块，
 *   pRxBuffer 传 NULL 时只发不收（见 StdDriver Src/spi.c）。
 * ⚠️ FIFO 宽度在 8bit 模式下是否按 32bit 递增未在数据手册核实：
 *   若换 data_width=16 后再调用本路径，需实测确认 txBuffer_pos 步进正确。
 */
static void ns800_spi_xfer_fifo_write(SPI_TypeDef *instance,
                                      const rt_uint8_t *buf, rt_size_t length)
{
    /* 8bit 模式：1 字节 = 1 字，直接按字长喂 FIFO */
    SPI_pollingFifoTransaction(instance, (uint32_t *)(rt_ubase_t)buf, RT_NULL,
                               (uint32_t)length);
}
#endif /* BSP_SPI_XFER_FIFO */

static rt_ssize_t ns800_spi_xfer(struct rt_spi_device *device, struct rt_spi_message *message)
{
    struct ns800_spi *spi;
    SPI_TypeDef *instance;
    const rt_uint8_t *send_buf;
    rt_uint8_t *recv_buf;
    rt_size_t length;

    RT_ASSERT(device != RT_NULL);
    RT_ASSERT(device->bus != RT_NULL);

    spi = rt_container_of(device->bus, struct ns800_spi, spi_bus);
    instance = spi->config->Instance;
    length = message->length;
    send_buf = message->send_buf;
    recv_buf = message->recv_buf;

    if (message->cs_take)
    {
        rt_pin_write(device->cs_pin, PIN_LOW);
    }

#if BSP_SPI_XFER_FIFO
    /* 纯发送（LCD 刷屏 / Flash 写数据走这里）—— 批量灌 FIFO，不再逐字节空转。
     * 收发的场景（读 Flash ID 等）保留原逐字节路径，行为完全不变。 */
    if (send_buf && !recv_buf)
    {
        ns800_spi_xfer_fifo_write(instance, send_buf, length);
        length = 0;
    }
#endif

    while (length)
    {
        if (send_buf)
        {
            if (recv_buf)
            {
                recv_buf[0] = SPI_transmitReceive(instance, send_buf[0]);
                recv_buf++;
            }
            else
            {
                SPI_transmitReceive(instance, send_buf[0]);
            }
            send_buf++;
        }
        else
        {
            if (recv_buf)
            {
                recv_buf[0] = SPI_transmitReceive(instance, 0xFF);
                recv_buf++;
            }
            else
            {
                SPI_transmitReceive(instance, 0xFF);
            }
        }
        length--;
    }

    if (message->cs_release)
    {
        rt_pin_write(device->cs_pin, PIN_HIGH);
    }

    return message->length;
}

static const struct rt_spi_ops ns800_spi_ops =
{
    .configure = ns800_spi_configure,
    .xfer = ns800_spi_xfer,
};

static void ns800_spi_clock_init(SPI_TypeDef *Instance)
{
#ifdef BSP_USING_SPI1
    if (Instance == SPI1)
    {
        RCC_enableSpi1Clock();
        RCC_resetSpi1Module();
        RCC_releaseSpi1Module();
    }
#endif
#ifdef BSP_USING_SPI2
    if (Instance == SPI2)
    {
        RCC_enableSpi2Clock();
        RCC_resetSpi2Module();
        RCC_releaseSpi2Module();
    }
#endif
#ifdef BSP_USING_SPI3
    if (Instance == SPI3)
    {
        RCC_enableSpi3Clock();
        RCC_resetSpi3Module();
        RCC_releaseSpi3Module();
    }
#endif
#ifdef BSP_USING_SPI4
    if (Instance == SPI4)
    {
        RCC_enableSpi4Clock();
        RCC_resetSpi4Module();
        RCC_releaseSpi4Module();
    }
#endif
}

int rt_hw_spi_init(void)
{
    rt_size_t i;
    rt_err_t result;

    for (i = 0; i < sizeof(spi_config) / sizeof(spi_config[0]); i++)
    {
        ns800_spi_clock_init(spi_config[i].Instance);

        spi_obj[i].config = &spi_config[i];
        spi_obj[i].spi_bus.parent.user_data = &spi_config[i];

        result = rt_spi_bus_register(&spi_obj[i].spi_bus,
                                      spi_config[i].name,
                                      &ns800_spi_ops);
        if (result != RT_EOK)
        {
            LOG_E("rt_spi_bus_register(%s) failed: %d", spi_config[i].name, result);
        }
    }

    return RT_EOK;
}

INIT_BOARD_EXPORT(rt_hw_spi_init);

#endif

