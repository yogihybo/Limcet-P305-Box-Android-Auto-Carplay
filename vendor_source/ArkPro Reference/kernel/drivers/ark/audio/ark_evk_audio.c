/* 
 * ark_sddac.c -- ALSA SoC audio for ark
 *
 * Copyright ....
 *
 * Author: Jack Tang <jacktang@astri.org>
 *
 */

#include <linux/module.h>
#include <linux/platform_device.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/soc.h>
#include <sound/soc-dapm.h>
#include <linux/clk.h>
#include <asm/io.h>

#include "ark_pcm.h"
#include "ark_i2s.h"

#include <linux/ark/sddac.h>

#define SDDAC_TEST

#undef ARK_SDDAC_DEBUG
#ifdef ARK_SDDAC_DEBUG
#define	DBG(f, a...)	printk(KERN_DEBUG "ASOC: %s-%d: "f, __FUNCTION__, __LINE__, ##a)
#else
#define DBG(...)
#endif

#define	ERR(f, a...)	printk(KERN_ERR "ASOC: %s-%d: "f, __FUNCTION__, __LINE__, ##a)

static int ark_sddac_startup(struct snd_pcm_substream *substream)
{
	int ret = 0;
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *codec_dai = rtd->codec_dai;

	DBG("-->\n");

	/* variable MCLK */
	ret = snd_soc_dai_set_sysclk(codec_dai, 0, 0, SND_SOC_CLOCK_IN);

	return ret;
}

static int ark_sddac_hw_params(
	struct snd_pcm_substream *substream, struct snd_pcm_hw_params *params)
{
	void __iomem 	*Sys_base;
	unsigned int val;
	
	DBG("-->%d -------\n",params_rate(params));

	Sys_base = ioremap(0xe4900000,0x1ff);

	val = readl(Sys_base+0x6c);
	val &= ~(1<<2);
	writel(val,Sys_base+0x6c);
	
	switch (params_rate(params)) {

		case 192000:
			val = (768 << 16) | 1875;
			break;

		case 96000:
			val = (384 << 16) | 1875;
			break;
			
		case 48000:
			val = (192 << 16) | 1875;
		    break;
			
		case 44100:
			val = (1764 << 16) | 18750;
		    break;
			
		case 32000:
			val = (128 << 16) | 1875;
		    break;

		case 24000:
			val = (96 << 16) | 1875;
		    break;
			
		case 22050:
			val = (882 << 16) | 18750;
		    break;

		case 16000:
			val = (64 << 16) | 1875;
		    break;

		case 12000:
			val = (48 << 16) | 1875;
		    break;
			
		case 11025:
			val = (441 << 16) | 18750;
			break;

 		case 8000:
			val = (32 << 16) | 1875;
			break;

 		case 4000:
			val = (16 << 16) | 1875;
			break;
		default:
			val = (1764 << 16) | 18750;
	}

	writel(val,Sys_base+0x174);
	iounmap(Sys_base);
	return 0;
}

static int ark_cs4334_hw_params(
	struct snd_pcm_substream *substream, struct snd_pcm_hw_params *params)
{
	void __iomem 	*Sys_base;
	unsigned int val;
	
	DBG("-->%d -------\n",params_rate(params));

	Sys_base = ioremap(0xe4900000,0x1ff);

	val = readl(Sys_base+0x6c);
	val &= ~(1<<0);
	writel(val,Sys_base+0x6c);
	
	switch (params_rate(params)) {

		case 192000:
			val = (768 << 16) | 1875;
			break;

		case 96000:
			val = (384 << 16) | 1875;
			break;
			
		case 48000:
			val = (192 << 16) | 1875;
		    break;
			
		case 44100:
			val = (1764 << 16) | 18750;
		    break;
			
		case 32000:
			val = (128 << 16) | 1875;
		    break;

		case 24000:
			val = (96 << 16) | 1875;
		    break;
			
		case 22050:
			val = (882 << 16) | 18750;
		    break;

		case 16000:
			val = (64 << 16) | 1875;
		    break;

		case 12000:
			val = (48 << 16) | 1875;
		    break;
			
		case 11025:
			val = (441 << 16) | 18750;
			break;

 		case 8000:
			val = (32 << 16) | 1875;
			break;

 		case 4000:
			val = (16 << 16) | 1875;
			break;
		default:
			val = (1764 << 16) | 18750;
	}

	writel(val,Sys_base+0x178);
	iounmap(Sys_base);
	return 0;
}

static struct snd_soc_ops ark_sddac_ops = {
	.startup	= ark_sddac_startup,
	.hw_params	= ark_sddac_hw_params,
};

static struct snd_soc_ops ark_cs4334_ops = {
	.startup	= ark_sddac_startup,
	.hw_params	= ark_cs4334_hw_params,
};

static struct snd_soc_dai_link ark_sddac_dai_link[] = {
	{
		.name 			= "SDDAC",
		.stream_name 	= "SDDAC",
		.cpu_dai_name	= "ark_i2s_dev.0",
		.codec_dai_name	= "sddac-hifi",
		.platform_name 	= "ark-audio",
		.codec_name 	= "ark_sddac_dev",
		.ops 			= &ark_sddac_ops,
	},
};

static struct snd_soc_card ark_sddac = {
	.name 			= "ARK-SDDAC",
	.owner			= THIS_MODULE,
	.dai_link 		= &ark_sddac_dai_link,
	.num_links 		= 1,// ARRAY_SIZE(ark_sddac_dai_link),
};

static struct snd_soc_dai_link ark_cs4334_dai_link[] = {
	{
		.name 			= "SDDAC1",
		.stream_name 	= "SDDAC1",
		.cpu_dai_name	= "ark_i2s_dev.1",
		.codec_dai_name	= "cs4334",
		.platform_name 	= "ark-audio",
		.codec_name 	= "ark_cs4334_dev",
		.ops 			= &ark_cs4334_ops,
	},
};

static struct snd_soc_card ark_cs4334 = {
	.name 			= "ARK-CS4334",
	.owner			= THIS_MODULE,
	.dai_link 		= &ark_cs4334_dai_link,
	.num_links 		= 1,// ARRAY_SIZE(ark_sddac_dai_link),
};

static struct platform_device *ark_sddac_snd_device;
static struct platform_device *ark_cs4334_snd_device;

static int ark_sddac_probe(struct platform_device *pdev)
{
	struct sddac_platform_data *pd = pdev->dev.platform_data;
	int ret = 0;

	DBG("-->\n");

	if (pd == NULL) {
		ERR("sddac_platform_data data not found\n");
		return -ENODEV;
	}

	ark_sddac_snd_device = platform_device_alloc("soc-audio", 0);
	if (!ark_sddac_snd_device) {
		ERR("Fail to allocate platform device\n");
		return -ENOMEM;
	}

	ark_cs4334_snd_device = platform_device_alloc("soc-audio", 1);
	if (!ark_cs4334_snd_device) {
		ERR("Fail to allocate platform device\n");
		return -ENOMEM;
	}

	platform_set_drvdata(ark_sddac_snd_device, &ark_sddac);
	platform_device_add_data(ark_sddac_snd_device, pd, sizeof(struct sddac_platform_data));
	ret = platform_device_add(ark_sddac_snd_device);

	if (ret) {
		ERR(">>>>>>>>>>>>>>>>>platform device add fail\n");
		platform_device_put(ark_sddac_snd_device);
		return ret;
	}

	platform_set_drvdata(ark_cs4334_snd_device, &ark_cs4334);
	platform_device_add_data(ark_cs4334_snd_device, pd, sizeof(struct sddac_platform_data));
	ret = platform_device_add(ark_cs4334_snd_device);

	if (ret) {
		ERR(">>>>>>>>>>>>>>>>>platform device add fail\n");
		platform_device_put(ark_cs4334_snd_device);
		return ret;
	}

	return ret;
}

static int ark_sddac_remove(struct platform_device *pdev)
{
	DBG("-->\n");

	platform_device_unregister(ark_sddac_snd_device);

	return 0;
}

static struct platform_driver ark_sddac_driver = {
	.probe 		= ark_sddac_probe,
	.remove		= ark_sddac_remove,
	.driver 	= {
			.name	= "ark_sddac",
			.owner	= THIS_MODULE,
	},
};

module_platform_driver(ark_sddac_driver);




MODULE_AUTHOR("Jack Tang, jacktang@astri.org");
MODULE_DESCRIPTION("ALSA SoC ARK SDDAC");
MODULE_LICENSE("GPL");

