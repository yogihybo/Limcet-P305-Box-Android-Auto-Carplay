// SPDX-License-Identifier: GPL-2.0+
/*
 * Basic support for the pwm module on Arkmicro.
 */

#include <common.h>
#include <div64.h>
#include <pwm.h>
#include <asm/io.h>

#define PWM_EN		0x0
#define PWM_DUTY	0x4
#define PWM_CNTR	0x8

#define PWM_REG(x)	(CONFIG_PWM_BASEADDR + 0x10 * (x)) 

int pwm_config(int pwm_id, int duty_ns, int period_ns)
{
	unsigned int clk_mhz = CONFIG_PWM_CLKFREQ / 1000000;
	unsigned int duty = (unsigned long long)duty_ns * clk_mhz / 1000;
	unsigned int period = (unsigned long long)period_ns * clk_mhz / 1000;
	
	writel(0, PWM_REG(pwm_id) + PWM_EN);
	writel(duty, PWM_REG(pwm_id) + PWM_DUTY);
	writel(period, PWM_REG(pwm_id) + PWM_CNTR);

	return 0;
}

int pwm_enable(int pwm_id)
{
	writel(1, PWM_REG(pwm_id) + PWM_EN);
	return 0;
}

void pwm_disable(int pwm_id)
{
	writel(0, PWM_REG(pwm_id) + PWM_EN);
}
