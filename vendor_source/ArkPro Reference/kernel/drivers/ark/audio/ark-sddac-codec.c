/*
 * SDDAC - Sigma-delta digital-to-analog converter 
 *
 */
#include <linux/module.h>
#include <sound/soc.h>
#include <linux/io.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/gpio.h>
#include <linux/pwm.h>
#include <mach/va_map.h>
#include "ark_i2s_sddac_regs.h"
#include "sddac.h"

#define SOUND_CTRL_GPIO  1
#define SOUND_PWM_ID         1
#undef SDDAC_DEBUG
#ifdef SDDAC_DEBUG
#define DBG(f, a...) 	printk(KERN_DEBUG "ASOC: %s-%d: "f, __FUNCTION__, __LINE__, ##a)
#else
#define DBG(...)
#endif

#define ERR(f, a...) 	printk(KERN_ERR "ASOC: %s-%d: "f, __FUNCTION__, __LINE__, ##a)


#define SDDAC_RATES \
	(SNDRV_PCM_RATE_11025 | SNDRV_PCM_RATE_16000 | SNDRV_PCM_RATE_22050 | \
	SNDRV_PCM_RATE_32000 | SNDRV_PCM_RATE_44100 | SNDRV_PCM_RATE_48000 | \
	SNDRV_PCM_RATE_64000 | SNDRV_PCM_RATE_88200 | SNDRV_PCM_RATE_96000 | \
	SNDRV_PCM_RATE_176400 | SNDRV_PCM_RATE_192000 |SNDRV_PCM_RATE_8000)

#define SDDAC_FORMATS	(SNDRV_PCM_FMTBIT_S16_LE)

struct sddac_priv {
	void __iomem	*base;
	int ctrl_gpio;
	struct pwm_device *ctrl_pwm;
	unsigned int 	vol_l;
	unsigned int 	vol_r;
	void *private;
};

static struct sddac_priv *g_sddac_priv = NULL;

static int sddac_read(
	struct snd_soc_codec *codec, unsigned int reg)
{
	struct sddac_priv *sddac = snd_soc_codec_get_drvdata(codec);

	return readl(sddac->base + reg);
}

static int sddac_write(
	struct snd_soc_codec *codec, unsigned int reg, unsigned int value)
{
	struct sddac_priv *sddac = snd_soc_codec_get_drvdata(codec);
	writel(value, sddac->base + reg);

	return 0;
}


void ark_audio_mute(int mute)
{
	struct sddac_priv *sddac = g_sddac_priv;
	struct snd_soc_codec *codec = (struct snd_soc_codec *)(sddac->private);
	unsigned int reg = 0, vol_l = sddac->vol_l, vol_r= sddac->vol_r;

	if (mute) {
		sddac->vol_l = vol_l;
		sddac->vol_r = vol_r;
		sddac_write(codec, ARK_I2SSDDAC_DACR0, 0);
		printk("%s:%d mute=%d reg=0x%08x\n", __func__, __LINE__, mute, reg);
	} else {
		sddac_write(codec, ARK_I2SSDDAC_DACR0, (vol_l | (vol_r << 8)));
		reg = readl(sddac->base + ARK_I2SSDDAC_DACR0);
		printk("%s:%d mute=%d reg=0x%08x\n", __func__, __LINE__, mute, reg);
	}
}

static int sddac_mute(struct snd_soc_dai *dai, int mute)
{
	struct snd_soc_codec *codec = dai->codec;
	struct sddac_priv *sddac = snd_soc_codec_get_drvdata(codec);;
	unsigned int reg,vol_l=100,vol_r=100;

	DBG("-->%d---\n",mute);printk("%s:%d mute=%d\n", __func__, __LINE__, mute);

	if (mute) { //gpio1
		gpio_direction_output(sddac->ctrl_gpio, 0);
	} else {
		gpio_direction_output(sddac->ctrl_gpio, 1);
		pwm_config(sddac->ctrl_pwm, 3333, 50000);
	}

	return 0;
}

#ifdef CODEC_SDDAC
static int sddac_set_bias_level(
	struct snd_soc_codec *codec, enum snd_soc_bias_level level)
{
	
	DBG("-->%d--------\n",level);ds
	return 0;
}
#else
static int sddac_set_bias_level(
	struct snd_soc_codec *codec, enum snd_soc_bias_level level)
{
	unsigned int sacr0;

	DBG("-->%d-------\n",level);

	sacr0 = sddac_read(codec, ARK_I2SSDDAC_SACR0);
	sacr0 &= ~(ARK_I2SSDDAC_SACR0_VREF_PD | 
			ARK_I2SSDDAC_SACR0_DAC_PD | 
			ARK_I2SSDDAC_SACR0_SARADC_EN |
			ARK_I2SSDDAC_SACR0_SARADC_POW_EN);

	switch (level) {
		case SND_SOC_BIAS_ON:
		case SND_SOC_BIAS_PREPARE:
			/*TODO */
			sacr0 |= (ARK_I2SSDDAC_SACR0_SARADC_POW_EN);
			break;
		case SND_SOC_BIAS_STANDBY:
		case SND_SOC_BIAS_OFF:
			/*TODO */
			sacr0 |= (ARK_I2SSDDAC_SACR0_VREF_PD |
					ARK_I2SSDDAC_SACR0_DAC_PD |
					ARK_I2SSDDAC_SACR0_SARADC_EN);
			break;
		default:
			ERR("Invalid bias level\n");
			return -EINVAL;
	}

	DBG("-->%x-------\n",sacr0);

	sddac_write(codec, ARK_I2SSDDAC_SACR0, sacr0);

	codec->dapm.bias_level = level;
	return 0;
		
}
#endif

static const struct snd_kcontrol_new  sddac_snd_controls[] = {
	/* DAC volume control */
	SOC_SINGLE("Left Playback Volume", ARK_I2SSDDAC_DACR0, 0, 100, 0),
	SOC_SINGLE("Right Playback Volume", ARK_I2SSDDAC_DACR0, 8, 100, 0),
};

static int sddac_startup(struct snd_pcm_substream *substream,
	struct snd_soc_dai *dai)
{
	DBG("-->play init start---SDDAC----------\n");
	DBG("-->play init end-------------\n");
	return 0 ;
}



static void sddac_shutdown(struct snd_pcm_substream *substream,
	struct snd_soc_dai *dai)
{
	DBG("-->-------\n");
	return 0;
}

static int sddac_hw_params(struct snd_pcm_substream *substream,
	struct snd_pcm_hw_params *params,
	struct snd_soc_dai *dai)
{
	DBG("-->-------\n");
	return 0;
}


static int sddac_set_dai_sysclk(struct snd_soc_dai *codec_dai,
				  int clk_id, unsigned int freq, int dir)
{
	DBG("-->-------\n");
	return 0;
}

static int sddac_set_dai_fmt(struct snd_soc_dai *codec_dai,
			       unsigned int fmt)
{
	DBG("-->-------\n");
	return 0;
}

static const struct snd_soc_dai_ops sddac_dai_ops = {
	.startup	=  sddac_startup,
	.shutdown	=  sddac_shutdown,
	.hw_params	=  sddac_hw_params,
	.set_sysclk	=  sddac_set_dai_sysclk,
	.set_fmt	=  sddac_set_dai_fmt,
	.digital_mute	= sddac_mute,
};


static struct snd_soc_dai_driver sddac_dai = {
	.name 		= "sddac-hifi",
	.playback 	= {
			.stream_name 	= "Playback",
			.channels_min	= 1,
			.channels_max	= 2,
			.rates			= SDDAC_RATES,
			.formats		= SDDAC_FORMATS,
	},
	.capture = {
		.stream_name = "Capture",
		.channels_min = 1,
		.channels_max = 2,
		.rates = SDDAC_RATES,
		.formats = SDDAC_FORMATS,
	},
	.ops 		= &sddac_dai_ops,
};


static int sddac_probe(struct snd_soc_codec *codec)
{
	struct sddac_priv *sddac;
	struct sddac_platform_data *pd = codec->card->dev->platform_data;
	int ret = -1;
		
	DBG("-->\n");
	sddac = kzalloc(sizeof(struct sddac_priv), GFP_KERNEL);
	if (sddac == NULL)
		return -ENOMEM;
	sddac->ctrl_gpio = SOUND_CTRL_GPIO;
	if(0 && 0 > gpio_request(SOUND_CTRL_GPIO, "sound_ctrl")) {
		goto err_free_mem;
	}
	gpio_direction_output(sddac->ctrl_gpio, 0);

	sddac->ctrl_pwm = pwm_request(SOUND_PWM_ID, "sound_player_pwm0");
	if (IS_ERR(sddac->ctrl_pwm)) {
		ERR("Failed to request pwm devices\n");
		goto err_free_gpio;
	}
	sddac->base = ioremap(pd->reg_base, pd->reg_size);
	if (!sddac->base) {
		ERR("Failed to remap register space\n");
		ret = -ENOMEM;
		goto err_free_pwm;
	}
	sddac->vol_l = sddac->vol_r = 100;
	sddac->private = (void *)codec;

	ret = snd_soc_add_codec_controls(codec, sddac_snd_controls, 
				ARRAY_SIZE(sddac_snd_controls));
	if (ret < 0) {
		ERR("failed to register controls\n");
		goto err_iounmap;
	}
	snd_soc_codec_set_drvdata(codec, sddac);
	g_sddac_priv = sddac;

	sddac_set_bias_level(codec, SND_SOC_BIAS_ON);
	return 0;
err_iounmap:
	iounmap(sddac->base);
err_free_pwm:
	pwm_free(sddac->ctrl_pwm);
err_free_gpio:
	gpio_free(sddac->ctrl_gpio);
err_free_mem:
	kfree(sddac);
	return ret;
}

static int sddac_remove(struct snd_soc_codec *codec)
{
	struct sddac_priv *sddac = snd_soc_codec_get_drvdata(codec);

	DBG("-->\n");

	sddac_set_bias_level(codec, SND_SOC_BIAS_OFF);
	gpio_free(sddac->ctrl_gpio);
	pwm_free(sddac->ctrl_pwm);
	iounmap(sddac->base);
	kfree(sddac);

	return 0;
}

static struct snd_soc_codec_driver soc_codec_dev_sddac = {
	.probe 				= sddac_probe,
	.remove				= sddac_remove,
	.read				= sddac_read,
	.write				= sddac_write,
	.set_bias_level 	= sddac_set_bias_level,
};

static int sddac_codec_probe(struct platform_device *pdev)
{
	DBG("-->\n");
	return snd_soc_register_codec(&pdev->dev, 
		&soc_codec_dev_sddac, &sddac_dai, 1);
}

static int sddac_codec_remove(struct platform_device *pdev)
{
	DBG("-->\n");

	snd_soc_unregister_codec(&pdev->dev);
	return 0;
}

static struct platform_driver sddac_codec_driver = {
	.driver = {
		.name 	= "ark_sddac_dev",
		.owner 	= THIS_MODULE,
	},
	.probe 	= sddac_codec_probe,
	.remove = sddac_codec_remove,
};

module_platform_driver(sddac_codec_driver);


MODULE_DESCRIPTION("ARK SDDAC codec driver");
MODULE_AUTHOR("jacktang <jacktang@astri.org>");
MODULE_LICENSE("GPL");

