/*
 * Copyright (c) 2014, Wind River Systems, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Software-managed ISR table
 *
 * Data types for a software-managed ISR table, with a parameter per-ISR.
 */

#ifndef ZEPHYR_INCLUDE_SW_ISR_TABLE_H_
#define ZEPHYR_INCLUDE_SW_ISR_TABLE_H_

#if !defined(_ASMLANGUAGE)
#include <zephyr/types.h>
#include <toolchain.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Note the order: arg first, then ISR. This allows a table entry to be
 * loaded arg -> r0, isr -> r3 in _isr_wrapper with one ldmia instruction,
 * on ARM Cortex-M (Thumb2).
 */
struct _isr_table_entry {
	const void *arg;
	void (*isr)(const void *);
};

/* The software ISR table itself, an array of these structures indexed by the
 * irq line
 */
extern struct _isr_table_entry _sw_isr_table[];

/*
 * Data structure created in a special binary .intlist section for each
 * configured interrupt. gen_irq_tables.py pulls this out of the binary and
 * uses it to create the IRQ vector table and the _sw_isr_table.
 *
 * More discussion in include/linker/intlist.ld
 */
struct _isr_list {
	/** IRQ line number */
	int32_t irq;
	/** Flags for this IRQ, see ISR_FLAG_* definitions */
	int32_t flags;
	/** ISR to call */
	void *func;
	/** Parameter for non-direct IRQs */
	const void *param;
};

/** This interrupt gets put directly in the vector table */
#define ISR_FLAG_DIRECT BIT(0)

#define _MK_ISR_NAME(x, y) __MK_ISR_NAME(x, y)
#define __MK_ISR_NAME(x, y) __isr_ ## x ## _irq_ ## y

/* Create an instance of struct _isr_list which gets put in the .intList
 * section. This gets consumed by gen_isr_tables.py which creates the vector
 * and/or SW ISR tables.
 */
/* 
 * LS:
 *
 * Z_ISR_DECLARE(irq_p, 0, isr_p, isr_param_p); \
 *
 * irq number : 12
 * irq priority : 0
 * isr func : adc_stm32_isr
 * isr_param_p :&(__device_dts_ord_10)
 * flags_p : 0
 *
 * __aligned(__alignof(struct _isr_list)) \
 * :  _isr_list 인스턴스 생성시 16바이트 배수로 정렬한다.
 *
 * __attribute__((section(STRINGIFY(segment)))) (.intList) \
 * : .intList 섹션에 위치하도록 설정한다.
 *
 *   gen_isr_tables.py 스크립트에서 prebuilt elf 내부의 .intList섹션에 위치한
 *   _isr_list 데이터를 가져와서 Vector와 SW ISR 테이블을 정의하는 C파일을 생성하게 된다.
 *
 * __used \
 * : 참조되지 않거나 사용되지 않는 경우에도 유지하도록 컴파일러에게 알리는 지시어 
 *
 * _MK_ISR_NAME(adc_stm32_isr, __LINE__) 
 * : isr name 은 __isr_adc_stm32_isr_irq___LINE__ 으로 확장된다.
 *
 * {
 *     .irq = 12, 
 *     .flags = 0, 
 *     .func = (void *)&(adc_stm32_isr), 
 *     .param = (const void *)(&(__device_dts_ord_10))
 * }
 *
 * zephyr_prebuilt.elf 파일의 .intList 섹션을 objdump로 확인해 보면,
 * 0x20005038 주소에 0x10 크기의 __isr_adc_stm32_isr_irq_0.0 이름으로
 * _isr_list 인스턴스가 등록되어 있는걸 확인할 수 있다.
 *
 * $ objdump -t build/zephyr/zephyr_prebuilt.elf |grep .intList
 *  20005000 l    d  .intList	00000000 .intList
 *  20005000 l       .intList	00000000 $d
 *  20005008 l       .intList	00000000 $d
 *  20005008 l     O .intList	00000010 __isr___stm32_exti_isr_4_15_irq_2.0
 *  20005018 l     O .intList	00000010 __isr___stm32_exti_isr_2_3_irq_1.1
 *  20005028 l     O .intList	00000010 __isr___stm32_exti_isr_0_1_irq_0.2
 *  20005038 l       .intList	00000000 $d
 *  20005038 l     O .intList	00000010 __isr_adc_stm32_isr_irq_0.0
 *  20005048 l       .intList	00000000 $d
 *  20005048 l     O .intList	00000010 __isr_uart_stm32_isr_irq_0.0
 *  20005000 g     O .intList	00000008 _iheader
 * 
 * .intList섹션만 추출되어 isrList.bin이 생성된다. gen_irq_tables.py 스크립트 입력으로 사용된다.
 *  include/linker/intlist.ld 링커에 의해 final binary에 포함되지 않는 .intlist섹션만 생성된다.
 * 
 * $ hexdump -C build/zephyr/isrList.bin
 * 00000000  20 00 00 00 00 00 00 00  07 00 00 00 00 00 00 00  | ...............|
 * 00000010  b7 2e 00 08 b8 37 00 08  06 00 00 00 00 00 00 00  |.....7..........|
 * 00000020  c5 2e 00 08 b8 37 00 08  05 00 00 00 00 00 00 00  |.....7..........|
 * 00000030  d3 2e 00 08 b8 37 00 08  0c 00 00 00 00 00 00 00  |.....7..........|
 * 00000040  03 30 00 08 00 38 00 08  1c 00 00 00 00 00 00 00  |.0...8..........|
 * 00000050  f1 32 00 08 d0 37 00 08                           |.2...7..|
 * 00000058
 *
 * gen_irq_tables.py 스크립트에서 벡터 테이블과 소프트웨어 ISR 테이블을 정의하는 isr_tables.c 파일을 생성한다.
 *  
 */
#define Z_ISR_DECLARE(irq, flags, func, param) \
	static Z_DECL_ALIGN(struct _isr_list) Z_GENERIC_SECTION(.intList) \
		__used _MK_ISR_NAME(func, __COUNTER__) = \
			{irq, flags, (void *)&func, (const void *)param}

#define IRQ_TABLE_SIZE (CONFIG_NUM_IRQS - CONFIG_GEN_IRQ_START_VECTOR)

#ifdef CONFIG_DYNAMIC_INTERRUPTS
void z_isr_install(unsigned int irq, void (*routine)(const void *),
		   const void *param);
#endif

#ifdef __cplusplus
}
#endif

#endif /* _ASMLANGUAGE */

#endif /* ZEPHYR_INCLUDE_SW_ISR_TABLE_H_ */
