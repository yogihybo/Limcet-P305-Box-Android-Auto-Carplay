#ifndef __ASM_ARCH_TIMER_H
#define __ASM_ARCH_TIMER_H

#ifdef CONFIG_SPL_BUILD

void timer_init_24M(void);

void timer_uninit(void);

void timer_delay_us(unsigned int us);

#endif

#endif /* __ASM_ARCH_TIMER_H */
