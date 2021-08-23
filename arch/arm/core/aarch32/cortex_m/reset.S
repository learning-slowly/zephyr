/*
 * Copyright (c) 2013-2014 Wind River Systems, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Reset handler
 *
 * Reset handler that prepares the system for running C code.
 */

#include <toolchain.h>
#include <linker/sections.h>
#include <arch/cpu.h>
#include "vector_table.h"

_ASM_FILE_PROLOGUE

GTEXT(z_arm_reset)
GTEXT(memset)
GDATA(z_interrupt_stacks)
#if defined(CONFIG_PLATFORM_SPECIFIC_INIT)
GTEXT(z_arm_platform_init)
#endif
#if defined(CONFIG_INIT_ARCH_HW_AT_BOOT)
GTEXT(z_arm_init_arch_hw_at_boot)
GDATA(z_main_stack)
#endif


/**
 *
 * @brief Reset vector
 *
 * Ran when the system comes out of reset, or when the firmware image is chain-
 * loaded by another application (for instance, a bootloader). At minimum, the
 * processor must be in thread mode with privileged level. At this point, the
 * main stack pointer (MSP) should be already pointing to a valid area in SRAM.
 *
 * Locking interrupts prevents anything but NMIs and hard faults from
 * interrupting the CPU. A default NMI handler is already in place in the
 * vector table, and the boot code should not generate hard fault, or we're in
 * deep trouble.
 *
 * We want to use the process stack pointer (PSP) instead of the MSP, since the
 * MSP is to be set up to point to the one-and-only interrupt stack during
 * later boot. That would not be possible if in use for running C code.
 *
 * When these steps are completed, jump to z_arm_prep_c(), which will finish
 * setting up the system for running C code.
 *
 * @return N/A
 */

SECTION_SUBSEC_FUNC(TEXT,_reset_section,z_arm_reset)

/* LS: 시작지점(entry point)으로 z_arm_reset 이 지정되어 있지만 부트로더에서
__start 심볼을 찾는 경우도 있기 때문에 두 심볼 모두 동일한 주소로 맵핑 */

/*
 * The entry point is located at the z_arm_reset symbol, which
 * is fetched by a XIP image playing the role of a bootloader, which jumps to
 * it, not through the reset vector mechanism. Such bootloaders might want to
 * search for a __start symbol instead, so create that alias here.
 */
SECTION_SUBSEC_FUNC(TEXT,_reset_section,__start)

/* LS: 부트로더나 다른 어플리케이션으로부터 분기했을 수도 있기 때문에
`CONFIG_INIT_ARCH_HW_AT_BOOT` 옵션이 설정되어 있는 경우 core register 상태를
리셋 상태로 초기화함 */
#if defined(CONFIG_INIT_ARCH_HW_AT_BOOT)
    /* Reset CONTROL register */
    movs.n r0, #0
/* LS: CONTROL 레지스터 내부 구조는 레퍼런스 매뉴얼 1405쪽 D1.2.13 섹션 참고 */
    msr CONTROL, r0
/* LS: CONTROL 레지스터의 변경사항이 다음 명령에 바로 적용되도록 프로세서 메모리
배리어를 사용 */
    isb
#if defined(CONFIG_CPU_CORTEX_M_HAS_SPLIM)
/* LS: stack limit 을 0으로 설정. 스택은 아래로 자라기 때문에 결국 제한이
없도록 설정한 것 */
    /* Clear SPLIM registers */
    movs.n r0, #0
    msr MSPLIM, r0
    msr PSPLIM, r0
#endif /* CONFIG_CPU_CORTEX_M_HAS_SPLIM */

#endif /* CONFIG_INIT_ARCH_HW_AT_BOOT */

#if defined(CONFIG_PLATFORM_SPECIFIC_INIT)
    bl z_arm_platform_init
#endif

#if defined(CONFIG_INIT_ARCH_HW_AT_BOOT)
#if defined(CONFIG_CPU_HAS_ARM_MPU)
    /* Disable MPU */
/* LS: 인터럽트가 발생하면 안되므로 MPU 비활성화 */
    movs.n r0, #0
/* LS: MPU 관련 레지스터는 1373쪽 D1.1.12 Memory Protection Unit 참고 */
    ldr r1, =_SCS_MPU_CTRL
    str r0, [r1]
/* LS: 새로운 mpu 리전 설정이 다음 명령들에 영향을 미칠 수 있도록 dsb 사용 */
    dsb
#endif /* CONFIG_CPU_HAS_ARM_MPU */
/* LS: z_main_stack 메모리 배열로 선언되어 있음. 베이스 주소에 스택 사이즈를
더함으로써 스택 최상을 가리키도록 함. 스택은 아래로 자라기 때문 */
/* LS: 아래 C 코드로 분기하기 때문에 해당 함수에서 사용할 스택을 설정함. 나중에
커널 idle 스레드에서 사용됨 */
    ldr r0, =z_main_stack + CONFIG_MAIN_STACK_SIZE
    msr msp, r0

    /* Initialize core architecture registers and system blocks */
    bl z_arm_init_arch_hw_at_boot
#endif /* CONFIG_INIT_ARCH_HW_AT_BOOT */

    /* lock interrupts: will get unlocked when switch to main task */
/* LS: BASEPRI 를 지원하지 않는 아키텍처에서는 모든 인터럽트를 금지하는 cpsid
명령을 사용. 우선순위별로 선점을 지정할 수 있는 아키텍처에서는 BASEPRI 를 사용. NMI 와 hardfault 를 제외한 모든 인터럽트를 금지함 */
#if defined(CONFIG_ARMV6_M_ARMV8_M_BASELINE)
    cpsid i
#elif defined(CONFIG_ARMV7_M_ARMV8_M_MAINLINE)
    movs.n r0, #_EXC_IRQ_DEFAULT_PRIO
    msr BASEPRI, r0
#else
#error Unknown ARM architecture
#endif

#ifdef CONFIG_WDOG_INIT
    /* board-specific watchdog initialization is necessary */
    bl z_arm_watchdog_init
#endif

/*
 *
 * Note: in all Cortex-M variants the interrupt stack area is at
 * least equal to CONFIG_ISR_STACK_SIZE + MPU_GUARD_ALIGN_AND_SIZE
 * (may be larger due to rounding up for stack pointer aligning
 * purposes but this is sufficient during initialization).
 */

#ifdef CONFIG_INIT_STACKS
/* LS: memset(stack_area, 0xaa, stack_size)
 인터럽트 스택에 corruption 이 발생했는지 확인하기 위해 스택에 특정값을 설정함 */
    ldr r0, =z_interrupt_stacks
    ldr r1, =0xaa
    ldr r2, =CONFIG_ISR_STACK_SIZE + MPU_GUARD_ALIGN_AND_SIZE
    bl memset
#endif

    /*
     * Set PSP and use it to boot without using MSP, so that it
     * gets set to z_interrupt_stacks during initialization.
     */
/* LS: PSP 에 인터럽트 스택을 설정하고 메인 스택에서 프로세스 스택으로 전환함.
참고로 현재 privileged thread 모드임 */
    ldr r0, =z_interrupt_stacks
    ldr r1, =CONFIG_ISR_STACK_SIZE + MPU_GUARD_ALIGN_AND_SIZE
    adds r0, r0, r1
    msr PSP, r0
    mrs r0, CONTROL
    movs r1, #2
    orrs r0, r1 /* CONTROL_SPSEL_Msk */
    msr CONTROL, r0
    /*
     * When changing the stack pointer, software must use an ISB instruction
     * immediately after the MSR instruction. This ensures that instructions
     * after the ISB instruction execute using the new stack pointer.
     */
    isb

    /*
     * 'bl' jumps the furthest of the branch instructions that are
     * supported on all platforms. So it is used when jumping to z_arm_prep_c
     * (even though we do not intend to return).
     */
    bl z_arm_prep_c
