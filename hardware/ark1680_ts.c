// SPDX-License-Identifier: GPL-2.0
/*
 * ARK1680 on-SoC resistive ADC touchscreen controller driver.
 *
 * Port of the stock 3.4.0 vendor driver (ark1680_ts.ko) to 4.19, based on
 * static disassembly of the compiled module plus the board-file resource
 * registration recovered from the stock vmlinux — see
 * docs/ARK1680_TS_REVERSE_ENGINEERING.md for the full derivation. Register
 * offsets, the init sequence, the 4-sample median coordinate filter, and
 * the input event protocol below are a faithful reproduction of that
 * disassembly. Not yet hardware-tested.
 *
 * This is the resistive touch path this specific unit's stock firmware
 * actually selects (via the /msnprofile/ark1680_ts marker file) — not
 * the GT911 I2C capacitive path.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/moduleparam.h>
#include <linux/platform_device.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/input.h>
#include <linux/slab.h>
#include <linux/of.h>

/*
 * debug=1 (module param, or /sys/module/ark1680_ts/parameters/debug)
 * turns on per-IRQ tracing of raw ADC status/samples and filtered
 * coordinates — off by default since it fires every touch IRQ.
 * Probe-time milestones and all error paths always log via dev_info/
 * dev_err regardless of this flag, so `dmesg | grep ark1680_ts` is
 * useful for bring-up debugging even without turning it on.
 */
static bool ark1680_ts_debug;
module_param_named(debug, ark1680_ts_debug, bool, 0644);
MODULE_PARM_DESC(debug, "Trace raw ADC samples/status and filtered coordinates per IRQ (default: off)");

#define ark_ts_dbg(ts, fmt, ...) \
	do { \
		if (ark1680_ts_debug) \
			dev_info((ts)->dev, fmt, ##__VA_ARGS__); \
	} while (0)

/* ADC/TSC register block offsets (base: DT "reg", phys 0xe4500000 stock) */
#define ARK_TS_CH_ENABLE	0x00	/* channel/mode enable bits */
#define ARK_TS_CH_CONFIG	0x08	/* per-channel config/threshold */
#define ARK_TS_IRQ_STATUS	0x0c	/* IRQ cause bits; write to ack */
#define ARK_TS_ADC_RESULT	0x14	/* per-channel conversion result */
#define ARK_TS_RAW_X		0x24	/* raw X sample (>>1 to use) */
#define ARK_TS_RAW_Y		0x28	/* raw Y sample (>>1 to use) */
#define ARK_TS_DBCNT		0x2c	/* debounce count */
#define ARK_TS_DETINTER		0x30	/* detect interval */

/* Shared SoC syscon/pinctrl block offsets (fixed phys 0xe4900000, 0x200
 * bytes — same block as the "sregs"/pinctrl0 DT node; stock driver maps
 * it directly rather than going through the pinctrl subsystem, and this
 * port keeps that literal behaviour for a straightforward first pass). */
#define ARK_SYS_MMIO_PHYS	0xe4900000
#define ARK_SYS_MMIO_SIZE	0x200

#define ARK_SYS_CLKEN		0x48	/* OR bit 0x8: ADC/TSC clock enable */
#define ARK_SYS_PADCFG		0x50	/* OR bit 0x800: pad/pull config */
#define ARK_SYS_ADCCLKDIV	0x64	/* ADC clock divider */
#define ARK_SYS_PINMUX0		0x140	/* clear bit 0x400000: pinmux */
#define ARK_SYS_PINMUX1		0x144	/* clear bit 0x4000: pinmux */

/* Stock init constants */
#define ARK_TS_ADC_CLKDIV_VAL	239
#define ARK_TS_DBCNT_VAL	40000
#define ARK_TS_DETINTER_VAL	130

/* Median-of-4 stability threshold (ADC units) — from TSP_GetXY */
#define ARK_TS_STABLE_THRESH	80

struct ark1680_ts {
	struct device *dev;
	void __iomem *adc_base;
	void __iomem *sys_base;
	struct input_dev *input;
	int irq;

	/* 4-slot circular sample buffer + filter state, mirrors the stock
	 * point_x[]/point_y[]/count globals in TSP_GetXY. */
	u32 point_x[4];
	u32 point_y[4];
	unsigned int count;
	bool pressed;
};

static void ark1680_ts_sys_setbits(struct ark1680_ts *ts, unsigned int off, u32 bits)
{
	u32 v = readl(ts->sys_base + off);
	writel(v | bits, ts->sys_base + off);
}

static void ark1680_ts_sys_clrbits(struct ark1680_ts *ts, unsigned int off, u32 bits)
{
	u32 v = readl(ts->sys_base + off);
	writel(v & ~bits, ts->sys_base + off);
}

static void ark1680_ts_div_adc_clk(struct ark1680_ts *ts, u32 div)
{
	u32 v = readl(ts->sys_base + ARK_SYS_ADCCLKDIV);

	v &= ~(0xff00 | 0xfe);
	v |= (div << 1) & 0xffff;
	writel(v, ts->sys_base + ARK_SYS_ADCCLKDIV);
}

/*
 * Push a new (raw_x, raw_y) sample into the circular buffer and, once
 * warmed up (5+ samples), return the median-of-4 filtered coordinate and
 * whether it's stable. enable == false resets the filter (call on
 * touch-release), matching TSP_GetXY(count=0) in the stock driver.
 */
static bool ark1680_ts_get_xy(struct ark1680_ts *ts, bool enable, int *out_x, int *out_y)
{
	u32 xs[4], ys[4];
	u32 tmp;
	int i, j;
	unsigned int idx;

	if (!enable) {
		ts->count = 0;
		return false;
	}

	idx = ts->count & 3;
	ts->point_x[idx] = readl(ts->adc_base + ARK_TS_RAW_X) >> 1;
	ts->point_y[idx] = readl(ts->adc_base + ARK_TS_RAW_Y) >> 1;
	ts->count++;

	ark_ts_dbg(ts, "ark1680_ts: sample #%u raw=(%u,%u)\n",
		   ts->count, ts->point_x[idx], ts->point_y[idx]);

	if (ts->count <= 4) {
		ark_ts_dbg(ts, "ark1680_ts: warming up (%u/5 samples)\n", ts->count);
		return false;
	}

	memcpy(xs, ts->point_x, sizeof(xs));
	memcpy(ys, ts->point_y, sizeof(ys));

	/* Simple insertion sort, ascending, independently per axis --
	 * matches the stock compare-swap network (X and Y are not sorted
	 * jointly, each axis is filtered on its own). */
	for (i = 1; i < 4; i++) {
		tmp = xs[i];
		for (j = i - 1; j >= 0 && xs[j] > tmp; j--)
			xs[j + 1] = xs[j];
		xs[j + 1] = tmp;

		tmp = ys[i];
		for (j = i - 1; j >= 0 && ys[j] > tmp; j--)
			ys[j + 1] = ys[j];
		ys[j + 1] = tmp;
	}

	*out_x = (xs[1] + xs[2] + 1) >> 1;
	*out_y = (ys[1] + ys[2] + 1) >> 1;

	{
		bool stable = (xs[2] - xs[1] <= ARK_TS_STABLE_THRESH) &&
			      (ys[2] - ys[1] <= ARK_TS_STABLE_THRESH);

		ark_ts_dbg(ts, "ark1680_ts: filtered=(%d,%d) spread=(%u,%u) %s\n",
			   *out_x, *out_y, xs[2] - xs[1], ys[2] - ys[1],
			   stable ? "stable" : "unstable");
		return stable;
	}
}

static irqreturn_t ark1680_ts_interrupt(int irq, void *dev_id)
{
	struct ark1680_ts *ts = dev_id;
	u32 status = readl(ts->adc_base + ARK_TS_IRQ_STATUS);
	int x, y;

	if (!status)
		return IRQ_HANDLED;

	ark_ts_dbg(ts, "ark1680_ts: irq status=0x%08x\n", status);

	/* Ack every asserted cause bit. */
	writel(readl(ts->adc_base + ARK_TS_IRQ_STATUS) & ~status,
	       ts->adc_base + ARK_TS_IRQ_STATUS);

	if (ark1680_ts_get_xy(ts, true, &x, &y)) {
		ts->pressed = true;
		ark_ts_dbg(ts, "ark1680_ts: DOWN x=%d y=%d\n", x, y);
		input_report_abs(ts->input, ABS_X, x);
		input_report_abs(ts->input, ABS_Y, y);
		input_report_abs(ts->input, ABS_PRESSURE, 4095);
		input_report_key(ts->input, BTN_TOUCH, 1);
		input_sync(ts->input);
	} else if (ts->pressed) {
		ts->pressed = false;
		ark_ts_dbg(ts, "ark1680_ts: UP\n");
		ark1680_ts_get_xy(ts, false, &x, &y);
		input_report_abs(ts->input, ABS_PRESSURE, 0);
		input_report_key(ts->input, BTN_TOUCH, 0);
		input_sync(ts->input);
	}

	return IRQ_HANDLED;
}

/*
 * Per-write register tracing for ark1680_setup_tsc(). Always logs (not
 * gated by the `debug` param) since this only runs once at probe, not
 * per-IRQ. Added after v7 hardware testing showed SYS clken/padcfg
 * reading back as 0xffffffff (bus-fault pattern) post-setup, to isolate
 * exactly which write in the sequence causes it — see
 * docs/ARK1680_TS_REVERSE_ENGINEERING.md "Hardware-tested".
 */
static void ark1680_ts_trace(struct ark1680_ts *ts, const char *step)
{
	dev_info(ts->dev,
		 "setup[%-18s]: ADC[en=0x%08x cfg=0x%08x] SYS[clken=0x%08x padcfg=0x%08x clkdiv=0x%08x pmux0=0x%08x pmux1=0x%08x]\n",
		 step,
		 readl(ts->adc_base + ARK_TS_CH_ENABLE),
		 readl(ts->adc_base + ARK_TS_CH_CONFIG),
		 readl(ts->sys_base + ARK_SYS_CLKEN),
		 readl(ts->sys_base + ARK_SYS_PADCFG),
		 readl(ts->sys_base + ARK_SYS_ADCCLKDIV),
		 readl(ts->sys_base + ARK_SYS_PINMUX0),
		 readl(ts->sys_base + ARK_SYS_PINMUX1));
}

static void ark1680_setup_tsc(struct ark1680_ts *ts)
{
	ark1680_ts_trace(ts, "initial");

	ark1680_ts_div_adc_clk(ts, ARK_TS_ADC_CLKDIV_VAL);
	ark1680_ts_trace(ts, "after clkdiv");

	ark1680_ts_sys_setbits(ts, ARK_SYS_CLKEN, BIT(3));
	ark1680_ts_trace(ts, "after clken OR");

	ark1680_ts_sys_setbits(ts, ARK_SYS_PADCFG, BIT(11));
	ark1680_ts_trace(ts, "after padcfg OR");

	ark1680_ts_sys_clrbits(ts, ARK_SYS_PINMUX0, BIT(22));
	ark1680_ts_trace(ts, "after pinmux0 clr");

	writel(readl(ts->adc_base + ARK_TS_CH_ENABLE) | BIT(0),
	       ts->adc_base + ARK_TS_CH_ENABLE);
	writel(readl(ts->adc_base + ARK_TS_CH_ENABLE) & ~0x7e,
	       ts->adc_base + ARK_TS_CH_ENABLE);
	ark1680_ts_trace(ts, "after ch_enable#1");

	writel(7, ts->adc_base + ARK_TS_CH_CONFIG);
	writel(0, ts->adc_base + ARK_TS_IRQ_STATUS);
	ark1680_ts_trace(ts, "after cfg/irqclr");

	writel(ARK_TS_DBCNT_VAL, ts->adc_base + ARK_TS_DBCNT);
	writel(ARK_TS_DETINTER_VAL, ts->adc_base + ARK_TS_DETINTER);
	ark1680_ts_trace(ts, "after dbcnt/detint");

	ark1680_ts_sys_clrbits(ts, ARK_SYS_PINMUX1, BIT(14));
	ark1680_ts_trace(ts, "after pinmux1 clr");

	writel(readl(ts->adc_base + ARK_TS_CH_ENABLE) | BIT(8),
	       ts->adc_base + ARK_TS_CH_ENABLE);
	writel(readl(ts->adc_base + ARK_TS_CH_ENABLE) | BIT(9),
	       ts->adc_base + ARK_TS_CH_ENABLE);
	writel(readl(ts->adc_base + ARK_TS_CH_ENABLE) | BIT(10),
	       ts->adc_base + ARK_TS_CH_ENABLE);
	writel(readl(ts->adc_base + ARK_TS_CH_ENABLE) | BIT(11),
	       ts->adc_base + ARK_TS_CH_ENABLE);
	ark1680_ts_trace(ts, "after ch_enable#2");

	/* Enable_ADC_Channel(2) — enable the touch X/Y sampling channel. */
	writel(readl(ts->adc_base + ARK_TS_CH_ENABLE) | BIT(2),
	       ts->adc_base + ARK_TS_CH_ENABLE);
	writel(readl(ts->adc_base + ARK_TS_CH_CONFIG) & ~0x7000,
	       ts->adc_base + ARK_TS_CH_CONFIG);
	ark1680_ts_trace(ts, "final");
}

static int ark1680_ts_probe(struct platform_device *pdev)
{
	struct ark1680_ts *ts;
	struct resource *res;
	int error;

	ts = devm_kzalloc(&pdev->dev, sizeof(*ts), GFP_KERNEL);
	if (!ts)
		return -ENOMEM;

	ts->dev = &pdev->dev;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		dev_err(ts->dev, "probe: Can't get memory resource\n");
		return -ENODEV;
	}
	dev_info(ts->dev, "probe: ADC/TSC MEM resource %pR\n", res);

	ts->adc_base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(ts->adc_base)) {
		dev_err(ts->dev, "probe: Can't map ADC/TSC registers (%ld)\n",
			PTR_ERR(ts->adc_base));
		return PTR_ERR(ts->adc_base);
	}

	ts->irq = platform_get_irq(pdev, 0);
	if (ts->irq < 0) {
		dev_err(ts->dev, "probe: Can't get interrupt resource (%d)\n", ts->irq);
		return ts->irq;
	}
	dev_info(ts->dev, "probe: irq=%d\n", ts->irq);

	/* Shared SoC syscon/pinmux block — fixed physical address, same as
	 * the stock driver (see file header). */
	ts->sys_base = devm_ioremap(&pdev->dev, ARK_SYS_MMIO_PHYS, ARK_SYS_MMIO_SIZE);
	if (!ts->sys_base) {
		dev_err(ts->dev, "probe: Can't map syscon/pinmux registers at 0x%x\n",
			ARK_SYS_MMIO_PHYS);
		return -ENOMEM;
	}
	dev_info(ts->dev, "probe: mapped syscon/pinmux block at phys 0x%x, size 0x%x\n",
		 ARK_SYS_MMIO_PHYS, ARK_SYS_MMIO_SIZE);

	ts->input = devm_input_allocate_device(&pdev->dev);
	if (!ts->input) {
		dev_err(ts->dev, "probe: failed allocating input device\n");
		return -ENOMEM;
	}

	ts->input->name = "ark1680-ts";
	ts->input->phys = "ark1680/input0";
	ts->input->id.bustype = BUS_HOST;
	ts->input->dev.parent = &pdev->dev;

	__set_bit(EV_ABS, ts->input->evbit);
	__set_bit(EV_KEY, ts->input->evbit);
	__set_bit(BTN_TOUCH, ts->input->keybit);
	input_set_abs_params(ts->input, ABS_X, 0, 4095, 5, 0);
	input_set_abs_params(ts->input, ABS_Y, 0, 4095, 5, 0);
	input_set_abs_params(ts->input, ABS_PRESSURE, 0, 4095, 5, 0);

	input_set_drvdata(ts->input, ts);
	platform_set_drvdata(pdev, ts);

	ark1680_setup_tsc(ts);

	error = devm_request_threaded_irq(&pdev->dev, ts->irq, NULL,
					   ark1680_ts_interrupt,
					   IRQF_ONESHOT, "ark1680_ts", ts);
	if (error) {
		dev_err(ts->dev, "probe: failed requesting irq %d (%d)\n", ts->irq, error);
		return error;
	}

	error = input_register_device(ts->input);
	if (error) {
		dev_err(ts->dev, "probe: failed registering input device (%d)\n", error);
		return error;
	}

	dev_info(ts->dev, "ARK1680 resistive touchscreen registered, irq=%d, debug=%d (echo 1 > /sys/module/ark1680_ts/parameters/debug to trace samples)\n",
		 ts->irq, ark1680_ts_debug);
	return 0;
}

static const struct of_device_id ark1680_ts_match[] = {
	{ .compatible = "arkmicro,ark1680-ts", },
	{ },
};
MODULE_DEVICE_TABLE(of, ark1680_ts_match);

static struct platform_driver ark1680_ts_driver = {
	.probe = ark1680_ts_probe,
	.driver = {
		.name = "ark1680_ts",
		.of_match_table = ark1680_ts_match,
	},
};
module_platform_driver(ark1680_ts_driver);

MODULE_AUTHOR("Reconstructed from stock ark1680_ts.ko disassembly");
MODULE_DESCRIPTION("ARK1680 resistive ADC touchscreen driver");
MODULE_LICENSE("GPL");
