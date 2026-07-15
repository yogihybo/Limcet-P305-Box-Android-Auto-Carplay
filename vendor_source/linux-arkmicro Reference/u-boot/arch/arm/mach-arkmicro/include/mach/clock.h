/*
 * (C) Copyright 2009
 * Stefano Babic, DENX Software Engineering, sbabic@denx.de.
 *
 * See file CREDITS for list of people who contributed to this
 * project.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston,
 * MA 02111-1307 USA
 */

#ifndef __ASM_ARCH_CLOCK_H
#define __ASM_ARCH_CLOCK_H

int ark_clock_init(void);

unsigned long ark_get_cpu_clock(void);

unsigned long ark_get_axi_clock(void);

unsigned long ark_get_ahb_clock(void);

unsigned long ark_get_apb_clock(void);

unsigned long ark_get_ddr_clock(void);

unsigned long ark_get_syspll_clk(void);

unsigned long ark_get_timer_clock(void);

unsigned long ark_get_lcdpll_clock(void);

#endif /* __ASM_ARCH_CLOCK_H */
