/*
 * GNU ld linker script for NS800RT7P65 (Cortex-M7)
 *
 * Memory layout derived from the existing MDK scatter file:
 *   FLASH      0x08000000  512K
 *   VT_DTCM    0x20000000    1K
 *   DTCM       0x20000400   71K  (general RW/ZI/stack region)
 *   RT_HEAP    0x20012000   56K  (reserved for rt_system_heap_init)
 *                              ^ 原为 16K，因 LVGL widgets demo 需大量堆内存，
 *                                已把原 DTCM_EXT(40K, 空置) 并入堆区。
 *                                须与 board/board.h 的 HEAP_END 保持同步。
 *   SRAM12     0x20100000  256K  SRAM1+SRAM2 物理连续，合并成一个 region：
 *                                 · 头部 64K  → LVGL 内存池（.lvgl_pool）
 *                                   （实测只用了 ~16K，msh `lvmem` 可查）
 *                                 · 其后      → 全屏壁纸缓冲（.wall_buf, 150K）
 *                                   240x320 RGB565，对应 wallpaper_store.h
 *                                 SRAM1/SRAM2 单独看各 128K，放不下跨界的 150K 缓冲，
 *                                 故必须合并 —— 见下面 MEMORY 段注释。
 *   BKPSRAM    0x400B7000    4K
 */

ENTRY(Reset_Handler)

_system_stack_size = 0x1000;

MEMORY
{
    FLASH    (rx)  : ORIGIN = 0x08000000, LENGTH = 512K
    VT_DTCM  (rwx) : ORIGIN = 0x20000000, LENGTH = 0x400
    DTCM     (rwx) : ORIGIN = 0x20000400, LENGTH = 0x11C00
    RT_HEAP  (rwx) : ORIGIN = 0x20012000, LENGTH = 0xE000
    SRAM12   (rwx) : ORIGIN = 0x20100000, LENGTH = 0x40000
    BKPSRAM  (rwx) : ORIGIN = 0x400B7000, LENGTH = 0x1000
}

SECTIONS
{
    .text :
    {
        . = ALIGN(4);
        _stext = .;
        KEEP(*(.isr_vector))

        . = ALIGN(4);
        *(.text)
        *(.text.*)
        *(.rodata)
        *(.rodata*)
        *(.glue_7)
        *(.glue_7t)
        *(.gnu.linkonce.t*)
        *(.itcm)
        *(.itcm*)

        . = ALIGN(4);
        __fsymtab_start = .;
        KEEP(*(FSymTab))
        __fsymtab_end = .;

        . = ALIGN(4);
        __vsymtab_start = .;
        KEEP(*(VSymTab))
        __vsymtab_end = .;

        . = ALIGN(4);
        __rt_init_start = .;
        KEEP(*(SORT(.rti_fn*)))
        __rt_init_end = .;

        . = ALIGN(4);
        PROVIDE(__ctors_start__ = .);
        KEEP(*(SORT(.init_array.*)))
        KEEP(*(.init_array))
        PROVIDE(__ctors_end__ = .);

        . = ALIGN(4);
        _etext = .;
    } > FLASH = 0

    __exidx_start = .;
    .ARM.exidx :
    {
        *(.ARM.exidx* .gnu.linkonce.armexidx.*)
        _sidata = .;
    } > FLASH
    __exidx_end = .;

    .data : AT (_sidata)
    {
        . = ALIGN(4);
        _sdata = .;

        *(.data)
        *(.data.*)
        *(.gnu.linkonce.d*)
        *(.dtcm)
        *(.dtcm*)

        PROVIDE(__dtors_start__ = .);
        KEEP(*(SORT(.dtors.*)))
        KEEP(*(.dtors))
        PROVIDE(__dtors_end__ = .);

        . = ALIGN(4);
        _edata = .;
    } > DTCM

    .stack :
    {
        . = ALIGN(8);
        _sstack = .;
        . = . + _system_stack_size;
        . = ALIGN(8);
        _estack = .;
    } > DTCM

    __bss_start = .;
    .bss (NOLOAD) :
    {
        . = ALIGN(4);
        _sbss = .;

        *(.bss)
        *(.bss.*)
        *(COMMON)

        . = ALIGN(4);
        _ebss = .;

        *(.bss.init)
    } > DTCM
    __bss_end = .;

    .vt_dtcm (NOLOAD) :
    {
        . = ALIGN(4);
        *(.vt_dtcm)
        *(.vt_dtcm*)
        . = ALIGN(4);
    } > VT_DTCM

    /* ── SRAM12 = SRAM1 + SRAM2（物理地址连续，0x20100000..0x2013FFFF，共 256K）──
     * 合并原因：全屏壁纸缓冲 240x320x2 = 150K，跨 0x20120000 边界，
     *           单独用 SRAM1 或 SRAM2（各 128K）都放不下。
     * 硬约束：本段必须是 SRAM12 里的第一个输出段 —— 池地址 0x20100000 是
     *         lv_conf.h 的 LV_MEM_ADR 硬编码值，偏一点就对不上。
     * 运行时由 lv_port_disp.c 的 lvgl_pool_selftest() 校验。 */
    .lvgl_pool (NOLOAD) :
    {
        . = ALIGN(8);
        PROVIDE(__lvgl_pool_start = .);
        . = . + 0x10000;                    /* 64K，见 lv_conf.h 的 LV_MEM_SIZE */
        . = ALIGN(8);
        PROVIDE(__lvgl_pool_end = .);
    } > SRAM12

    /* 全屏壁纸缓冲（240x320 RGB565 = 150K）。紧跟池之后，起始 0x20110000。
     * NOLOAD：不占 flash，启动也不清零（开机由 wallpaper_store 从外置 flash 填）。 */
    .wall_buf (NOLOAD) :
    {
        . = ALIGN(8);
        PROVIDE(__wall_buf_start = .);
        *(.wall_buf)
        *(.wall_buf*)
        . = ALIGN(8);
        PROVIDE(__wall_buf_end = .);
    } > SRAM12

    /* 通用 sram2 段（历史遗留，当前无使用者；保留以免别处引用了它） */
    .sram2 (NOLOAD) :
    {
        . = ALIGN(4);
        *(.sram2)
        *(.sram2*)
        . = ALIGN(4);
    } > SRAM12

    .bkupsram (NOLOAD) :
    {
        . = ALIGN(4);
        *(.bss.bkupsram)
        *(.bss.bkupsram*)
        . = ALIGN(4);
    } > BKPSRAM

    /* .dtcm_ext 段已随 DTCM_EXT region 一并撤销：该段原本为空，
     * 其 40K 空间已并入 RT_HEAP 供 LVGL 使用。 */

    .rt_heap (NOLOAD) :
    {
        . = ALIGN(8);
        PROVIDE(Image$$RW_RT_HEAD$$Base = .);
        PROVIDE(Image$$RW_RT_HEAD$$Limit = .);
        PROVIDE(__rt_heap_start = .);
        . = . + LENGTH(RT_HEAP);
        . = ALIGN(8);
        PROVIDE(Image$$RW_RT_HEAD$$End = .);
        PROVIDE(__rt_heap_end = .);
    } > RT_HEAP

    _end = .;

    .stab          0 : { *(.stab) }
    .stabstr       0 : { *(.stabstr) }
    .stab.excl     0 : { *(.stab.excl) }
    .stab.exclstr  0 : { *(.stab.exclstr) }
    .stab.index    0 : { *(.stab.index) }
    .stab.indexstr 0 : { *(.stab.indexstr) }
    .comment       0 : { *(.comment) }
    .debug         0 : { *(.debug) }
    .line          0 : { *(.line) }
    .debug_srcinfo 0 : { *(.debug_srcinfo) }
    .debug_sfnames 0 : { *(.debug_sfnames) }
    .debug_aranges 0 : { *(.debug_aranges) }
    .debug_pubnames 0 : { *(.debug_pubnames) }
    .debug_info    0 : { *(.debug_info .gnu.linkonce.wi.*) }
    .debug_abbrev  0 : { *(.debug_abbrev) }
    .debug_line    0 : { *(.debug_line) }
    .debug_frame   0 : { *(.debug_frame) }
    .debug_str     0 : { *(.debug_str) }
    .debug_loc     0 : { *(.debug_loc) }
    .debug_macinfo 0 : { *(.debug_macinfo) }
    .debug_weaknames 0 : { *(.debug_weaknames) }
    .debug_funcnames 0 : { *(.debug_funcnames) }
    .debug_typenames 0 : { *(.debug_typenames) }
    .debug_varnames  0 : { *(.debug_varnames) }
}
