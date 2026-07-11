#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/i2c.h>
#include <linux/errno.h>
#include <asm/uaccess.h>
#include <linux/delay.h>
#include <linux/platform_device.h>
#include <asm/setup.h>
#include <linux/uaccess.h>
#include <linux/gpio.h>
#include <linux/of_gpio.h>
#include <linux/of_device.h>
#include "ark1668_itu656.h"
#include "rn6752.h"


//#define RN6752_USE_TIMER
#define RN6752_LOW_POWER



//extern int ark_sys_pad_config_gpio_mode(int gpio);
//extern void dvr_set_sys_clk(int level);
extern int dvr_get_pragressive(void);
extern void dvr_restart(void);
static void rn6752_reset(int gpio);
static int rn6752_read_byte(unsigned char regaddr);
static int rn6752_write_byte(unsigned char regaddr, unsigned char regval);
static int rn6752_check_id(struct dvr_rn6752 *dvr_rn6752);
#ifdef RN6752_USE_TIMER
static void rn6752_start_timer(int timeout_100ms);
#endif


struct ark_private_data *g_itu656in_priv = NULL;
struct i2c_client *g_itu656in_client  = NULL;

static struct dvr_rn6752 *g_dvr_rn6752 = NULL;
//extern special_info_transfer specinfo_param;
static struct ark_carback_effect_para rn6752_effect;
static int enh_LFS = 1;
static bool rn6752_dbg = false;

/*****************************************************************************************************
 *	Before use RN6752 driver, we should complete some function as forllows.
 */

/* detect video signal */
static int rn6752_detect_signal(void)
{
	if(rn6752_dbg)
		printk(KERN_ALERT "### rn6752_detect_signal\n");
	if(g_dvr_rn6752)
	{
#ifdef RN6752_USE_TIMER
		if(!g_dvr_rn6752->timer_start)
		{
			rn6752_start_timer(50);
		}
#else
		queue_work(g_dvr_rn6752->eq_queue, &g_dvr_rn6752->eq_work);
#endif
		return ((g_dvr_rn6752->mode != RN6752_MODE_NONE) ? 1 : 0);
	}
	else
	{
		return 0;
	}
}

//Select itu656 input channel
static int rn6752_select_channel(int ch)
{
	if((ch >= 0) && (ch <= 1))
	{		
		if(g_dvr_rn6752)
			g_dvr_rn6752->curr_channel = ch;
		rn6752_write_byte(0xD3, ch);
	}
	
	return 0;
}

//confirm progressive or interlace based on in signal resolution.
static int rn6752_get_progressive(void)
{
	static int progressive = 1;

	if(g_dvr_rn6752)
	{
#if 0
		int progressive = 1;
		if(g_dvr_rn6752->mode == RN6752_MODE_NONE)
			progressive = 1;
		else if(g_dvr_rn6752->mode < RN6752_MODE_720P_PAL)
			progressive = 0;
		//dvr_set_sys_clk(0);
#else
		switch(g_dvr_rn6752->mode)
		{
			case RN6752_MODE_NONE:
				break;
			case RN6752_MODE_CVBS_PAL:
			case RN6752_MODE_CVBS_NTSC:
				progressive = 0;
				break;
			case RN6752_MODE_720P_PAL:
			case RN6752_MODE_720P_NTSC:
				progressive = 1;
				break;
			default:
				progressive = 1;
				break;
		}
#endif
	}

	if(rn6752_dbg)
		printk(KERN_ALERT "### itu656 get rn6752 progressive:%d\n", progressive);

	return progressive;
}

static int rn6752_enter_carback_cb(void)
{
#ifdef RN6752_LOW_POWER
	rn6752_write_byte(0x80, 0x30);	//set power mode

	if(g_dvr_rn6752)
	{
		g_dvr_rn6752->enter_carback = 1;
		queue_work(g_dvr_rn6752->eq_queue, &g_dvr_rn6752->eq_work);
	}
	if(rn6752_dbg)
		printk(KERN_ALERT "### rn6752 enter carback\n");
#endif
	return 0;
}

static int rn6752_exit_carback_cb(void)
{
#ifdef RN6752_LOW_POWER
	rn6752_write_byte(0x80, 0x34);	//set low power mode

	if(g_dvr_rn6752)
	{
		g_dvr_rn6752->enter_carback = 0;
	}
	if(rn6752_dbg)
		printk(KERN_ALERT "### rn6752 exit carback\n");
#endif
	return 0;
}

static int rn6752_set_display_effect(int cmd, unsigned long arg)
{
	int error = 0;

	switch(cmd)
	{
		case ARK_DVR_GET_BRIGHTNESS:
		{
			int brightness = rn6752_read_byte(RN6752_BRIGHTNESS_ADDR);
			if(brightness < 0)
			{
				error = brightness;
				break;
			}
			if(copy_to_user((void  *)arg, &brightness, sizeof(int))){
				printk("%s: copy to carback_brightness error\n", __func__);
				error = -EFAULT;
			}
			break;
		}
		case ARK_DVR_SET_BRIGHTNESS:
		{
			int brightness;
			if(copy_from_user(&brightness, (void *)arg, sizeof(int))){
				printk("%s: copy from user frame error\n", __func__);
				error = -EFAULT;
			}
			else
			{
				brightness &= 0xFF;
				if(rn6752_write_byte(RN6752_BRIGHTNESS_ADDR, brightness) == 0)
				{
					rn6752_effect.carback_mask |= ARK_DVR_BRIGHTNESS_MASK;
					rn6752_effect.carback_brightness = brightness;
				}
			}
			break;
		}
		case ARK_DVR_GET_CONTRAST:
		{
			int contrast = rn6752_read_byte(RN6752_CONTRAST_ADDR);
			if(contrast < 0)
			{
				error = contrast;
				break;
			}
			if(copy_to_user((void  *)arg, &contrast, sizeof(int))){
				printk("%s: copy to carback_contrast error\n", __func__);
				error = -EFAULT;
			}
			break;
		}
		case ARK_DVR_SET_CONTRAST:
		{
			int contrast;
			if(copy_from_user(&contrast, (void *)arg, sizeof(int))){
				printk("%s: copy from user frame error\n", __func__);
				error = -EFAULT;
			}
			else
			{
				contrast &= 0xFF;
				if(rn6752_write_byte(RN6752_CONTRAST_ADDR, contrast) == 0)
				{
					rn6752_effect.carback_mask |= ARK_DVR_CONTRAST_MASK;
					rn6752_effect.carback_contrast = contrast;
				}
			}
			break;
		}
		case ARK_DVR_GET_SATURATION:
		{
			int saturation = rn6752_read_byte(RN6752_SATURATION_ADDR);
			if(saturation < 0)
			{
				error = saturation;
				break;
			}
			if(copy_to_user((void  *)arg, &saturation, sizeof(int))){
				printk("%s: copy to carback_saturation error\n", __func__);
				error = -EFAULT;
			}
			break;
		}
		case ARK_DVR_SET_SATURATION:
		{
			int saturation;
			if(copy_from_user(&saturation, (void *)arg, sizeof(int))){
				printk("%s: copy from user frame error\n", __func__);
				error = -EFAULT;
			}
			else
			{
				saturation &= 0xFF;
				if(rn6752_write_byte(RN6752_SATURATION_ADDR, saturation) == 0)
				{
					rn6752_effect.carback_mask |= ARK_DVR_SATURATION_MASK;
					rn6752_effect.carback_saturation = saturation;
				}
			}
			break;
		}
		case ARK_DVR_GET_HUE:
		{
			int hue = rn6752_read_byte(RN6752_HUE_ADDR);
			if(hue < 0)
			{
				error = hue;
				break;
			}
			if(copy_to_user((void  *)arg, &hue, sizeof(int))){
				printk("%s: copy to carback_hue error\n", __func__);
				error = -EFAULT;
			}
			break;
		}
		case ARK_DVR_SET_HUE:
		{
			int hue;
			if(copy_from_user(&hue, (void *)arg, sizeof(int))){
				printk("%s: copy from user frame error\n", __func__);
				error = -EFAULT;
			}
			else
			{
				hue &= 0xFF;
				if(rn6752_write_byte(RN6752_HUE_ADDR, hue) == 0)
				{
					rn6752_effect.carback_mask |= ARK_DVR_HUE_MASK;
					rn6752_effect.carback_hue = hue;
				}
			}
			break;
		}
		case ARK_DVR_GET_SHARPNESS:
		{
			int shaprness = rn6752_read_byte(RN6752_SHARPNESS_ADDR);
			if(shaprness < 0)
			{
				error = shaprness;
				break;
			}
			shaprness &= 0x7F;
			if(copy_to_user((void  *)arg, &shaprness, sizeof(int))){
				printk("%s: copy to carback_sharpness error\n", __func__);
				error = -EFAULT;
			}
			break;
		}
		case ARK_DVR_SET_SHARPNESS:
		{
			int sharpness;
			if(copy_from_user(&sharpness, (void *)arg, sizeof(int))){
				printk("%s: copy from user frame error\n", __func__);
				error = -EFAULT;
			}
			else
			{
				unsigned char tmp = (enh_LFS ? 0x80 : 0);
				sharpness &= 0x7F;
				if(rn6752_write_byte(RN6752_SHARPNESS_ADDR, (sharpness | tmp)) == 0)
				{
					rn6752_effect.carback_mask |= ARK_DVR_SHARPNESS_MASK;
					rn6752_effect.carback_sharpness = sharpness;
				}
			}
			break;
		}
		default:
			error = -EFAULT;
			break;
	}

	return error;
}

/******************************************************************************************************/

static int rn6752_write_byte(unsigned char regaddr, unsigned char regval)
{
	struct i2c_client *client;
	struct i2c_msg msg;
	s32 ret = -1;
	s32 retries = 0;
	u8 buf[2] = {0};

	if(!g_dvr_rn6752)
		return -ENODEV;

	client = g_dvr_rn6752->client;

	buf[0] = regaddr;
	buf[1] = regval;

	msg.flags = 0;
	msg.addr  = client->addr;
	msg.len   = 2;
	msg.buf   = buf;

	while(retries < 5)
	{
		ret = i2c_transfer(client->adapter, &msg, 1);
		if (ret == 1)
			break;
		retries++;
	}
	if(retries >= 5)
	{
		printk(KERN_ALERT "### ERR: %s failure\n",__FUNCTION__);
		return -EBUSY;
	}

	return 0;
}

static int rn6752_read_byte(unsigned char regaddr)
{
	struct i2c_client *client;
	struct i2c_msg read_msgs[2];
	s32 ret = -1;
	s32 retries = 0;
	u8 regValue = 0x00;

	if(!g_dvr_rn6752)
		return -ENODEV;

	client = g_dvr_rn6752->client;
    read_msgs[0].flags = !I2C_M_RD;
    read_msgs[0].addr  = client->addr;
    read_msgs[0].len   = 1;
    read_msgs[0].buf   = &regaddr;

    read_msgs[1].flags = I2C_M_RD;
    read_msgs[1].addr  = client->addr;
    read_msgs[1].len   = 1;
    read_msgs[1].buf   = &regValue;//low byte

    while(retries < 5)
    {
        ret = i2c_transfer(client->adapter, read_msgs, 2);
        if(ret == 2)
			break;
        retries++;
    }

    if((retries >= 5))
    {
		printk(KERN_ALERT "### ERR: %s failure\n",__FUNCTION__);
		return -EBUSY;
	}
	return regValue;
}

static void rn6752_set_display_effect_default(void)
{
	unsigned int mask = rn6752_effect.carback_mask;

	if((mask & ARK_DVR_BRIGHTNESS_MASK))
		rn6752_write_byte(RN6752_BRIGHTNESS_ADDR, (rn6752_effect.carback_brightness & 0xFF));

	if((mask & ARK_DVR_CONTRAST_MASK))
		rn6752_write_byte(RN6752_CONTRAST_ADDR, (rn6752_effect.carback_contrast & 0xFF));

	if((mask & ARK_DVR_SATURATION_MASK))
		rn6752_write_byte(RN6752_SATURATION_ADDR, (rn6752_effect.carback_saturation & 0xFF));

	if((mask & ARK_DVR_HUE_MASK))
		rn6752_write_byte(RN6752_HUE_ADDR, (rn6752_effect.carback_hue & 0xFF));
	else
		rn6752_write_byte(RN6752_HUE_ADDR, 0x80);	//set default hue

	if((mask & ARK_DVR_SHARPNESS_MASK))
	{
		if(enh_LFS)
			rn6752_write_byte(RN6752_SHARPNESS_ADDR, ((rn6752_effect.carback_sharpness | 0x80) & 0xFF));
		else
			rn6752_write_byte(RN6752_SHARPNESS_ADDR, (rn6752_effect.carback_sharpness & 0x7F));
	}
	else	//set default sharpness
	{
		if(enh_LFS)
			rn6752_write_byte(RN6752_SHARPNESS_ADDR, 0x80);
		else
			rn6752_write_byte(RN6752_SHARPNESS_ADDR, 0x00);
	}
}

static void rn6752_write_reg(const char *buf, int len)
{
	int i;

	for(i=0; i<len; i++)
		rn6752_write_byte(buf[2*i],buf[2*i+1]);

	rn6752_set_display_effect_default();
}

static  char * rn6752_get_mode_string (int mode)
{
	if(mode == RN6752_MODE_NONE)
	{
		return "NONE";
	}
	else if(mode == RN6752_MODE_CVBS_PAL)
	{
		return "CVBS_PAL";
	}
	else if(mode == RN6752_MODE_CVBS_NTSC)
	{
		return "CVBS_NTSC";
	}
	else if(mode == RN6752_MODE_720P_PAL)
	{
		return "720_PAL";
	}
	else if(mode == RN6752_MODE_720P_NTSC)
	{
		return "720_NTSC";
	}

	return "NONE";
};

static int rn6752_signal_check(void)
{
	u8 signal_cnt = 0;
	u8 nosignal_cnt=0;
	u8 reg_0x75,reg_0x77, reg_0x78, reg_0x79;
	u16 counter1 = 0, counter2 = 0, counter3 = 0;
	int i;
	
	for(i=0; i<30; i++)
	{
		if((rn6752_read_byte(0x00)&0x10) == 0x00)
		{
			signal_cnt++;
		}
		else
		{
			nosignal_cnt++;	
		}

		if(signal_cnt > 10)
		{
			//printk(KERN_ALERT "### >i:%d\r\n", i);
			reg_0x77 = rn6752_read_byte(0x77);
			reg_0x78 = rn6752_read_byte(0x78);
			reg_0x79 = rn6752_read_byte(0x79);
			reg_0x75 = rn6752_read_byte(0x75);

			//counter1 = 0;
			counter1 = reg_0x77&0x03;
			counter1 <<= 8;
			counter1 |= reg_0x78;

			//counter2 = 0;
			counter2 = reg_0x77&0xc;
			counter2 >>= 2;
			counter2 <<= 8;
			counter2 |= reg_0x79;

		    counter3 = reg_0x75;

			//printk(KERN_ALERT ">reg 0x77:%x\r\n", reg_0x77);
			//printk(KERN_ALERT ">reg 0x78:%x\r\n", reg_0x78);
			//printk(KERN_ALERT ">reg 0x79:%x\r\n", reg_0x79);
			//printk(KERN_ALERT ">counter1:%d\r\n", counter1);
			//printk(KERN_ALERT ">counter2:%d\r\n", counter2);
			break;
		}

		if(nosignal_cnt > 20)
		{
			return RN6752_MODE_NONE;
		}
		msleep(1);
	}
	
	if( (counter1 > 700) ||(counter2 > 700))
	{
		//720p pal
	    if(counter3 > 0x8c)
		      return RN6752_MODE_720P_PAL;
		else
		     return RN6752_MODE_720P_NTSC;
	}
	else if( ((counter1>330) && (counter1<550)) || ((counter2>330) && (counter2<550)) )
	{	
		//cvbs pal
		return RN6752_MODE_CVBS_PAL;
	}
	else if( (counter1<330) && (counter2<330) )
	{
		//cvbs ntsc
		return RN6752_MODE_CVBS_NTSC;
	}
	
	return RN6752_MODE_NONE ;
}


static void rn6752_eq_work(struct work_struct *work)
{
	//struct dvr_rn6752 *dvr_rn6752 = container_of(work, struct dvr_rn6752, eq_work);
	struct dvr_rn6752 *dvr_rn6752 = g_dvr_rn6752;
	static int mode_cfg = RN6752_MODE_NONE;

	if(!dvr_rn6752)
		return;

	//mutex_lock(&dvr_rn6752->eq_lock);
	if(mode_cfg == RN6752_MODE_NONE)
	{
		printk(KERN_ALERT "### rn6752_eq_work reset\n");
		//reset
		rn6752_reset(dvr_rn6752->gpio_reset);
		//check id
		if(rn6752_check_id(dvr_rn6752))
			return;
		
		//720p cfg: before auto match, we must config 720p mode, because the default clk config is based on 720P
		rn6752_write_reg(rn6752_tiu656_720p_pal, sizeof(rn6752_tiu656_720p_pal)/2);
		mode_cfg = RN6752_MODE_720P_PAL;

#ifdef RN6752_LOW_POWER
		if(!g_dvr_rn6752->enter_carback)
		{
			rn6752_write_byte(0x80, 0x34);	//low power mode
		}
#endif
		return;
	}

#ifdef RN6752_LOW_POWER
	if(!g_dvr_rn6752->enter_carback)
	{
		if(rn6752_dbg)
			printk(KERN_ALERT "### rn6752_eq_work return when exit carback\n");
		return;
	}
#endif

	rn6752_write_byte(0x49, 0x81);
	rn6752_write_byte(0x33, 0x80);
	rn6752_write_byte(0x48, 0x1b);
	msleep(100);

	dvr_rn6752->mode = rn6752_signal_check();
	if(dvr_rn6752->mode == RN6752_MODE_NONE)
	{
		msleep(10);
		dvr_rn6752->mode = rn6752_signal_check();
	}
	rn6752_write_byte(0x48, 0x13);
	if(rn6752_dbg)
		printk(KERN_ALERT "### rn6752 mode:%s, mode_cfg:%s\n", rn6752_get_mode_string(dvr_rn6752->mode), rn6752_get_mode_string(mode_cfg));
	
	if((dvr_rn6752->mode == RN6752_MODE_NONE) || (mode_cfg != dvr_rn6752->mode))
	{
		if(rn6752_dbg)
			printk(KERN_ALERT "### rn6752 change mode to (%s)\n", rn6752_get_mode_string(dvr_rn6752->mode));
		switch(dvr_rn6752->mode)
		{
			case RN6752_MODE_CVBS_PAL:
				if(mode_cfg != RN6752_MODE_CVBS_PAL)
				{
					rn6752_write_reg(rn6752_tiu656_cvbs_pal, sizeof(rn6752_tiu656_cvbs_pal)/2);
					mode_cfg = RN6752_MODE_CVBS_PAL;
				}
				if(dvr_get_pragressive() == 1)
					dvr_restart();
				break;
			case RN6752_MODE_CVBS_NTSC:
				if(mode_cfg != RN6752_MODE_CVBS_NTSC)
				{
					rn6752_write_reg(rn6752_tiu656_cvbs_ntsc, sizeof(rn6752_tiu656_cvbs_ntsc)/2);
					mode_cfg = RN6752_MODE_CVBS_NTSC;
				}
				if(dvr_get_pragressive() == 1)
					dvr_restart();
				break;
			case RN6752_MODE_720P_NTSC:
				if(mode_cfg != RN6752_MODE_720P_NTSC)
				{
					rn6752_write_reg(rn6752_tiu656_720p_ntsc, sizeof(rn6752_tiu656_720p_ntsc)/2);
					mode_cfg = RN6752_MODE_720P_NTSC;
				}
				if(dvr_get_pragressive() == 0)
					dvr_restart();
				break;
			case RN6752_MODE_720P_PAL:
				if(mode_cfg != RN6752_MODE_720P_PAL)
				{
					rn6752_write_reg(rn6752_tiu656_720p_pal, sizeof(rn6752_tiu656_720p_pal)/2);
					mode_cfg = RN6752_MODE_720P_PAL;
				}
				if(dvr_get_pragressive() == 0)
					dvr_restart();
				break;
			case RN6752_MODE_NONE:
			default:
				if(mode_cfg != RN6752_MODE_720P_PAL)
				{
					rn6752_write_reg(rn6752_tiu656_720p_pal, sizeof(rn6752_tiu656_720p_pal)/2);
					mode_cfg = RN6752_MODE_720P_PAL;
				}
				break;
		}
	}
	else
	{
		int progressive = dvr_get_pragressive();
		int restart = 0;

		switch(dvr_rn6752->mode)
		{
			case RN6752_MODE_CVBS_PAL:
			case RN6752_MODE_CVBS_NTSC:
				if(progressive == 1)
					restart = 1;
				break;
			case RN6752_MODE_720P_PAL:
			case RN6752_MODE_720P_NTSC:
				if(progressive == 0)
					restart = 1;
				break;
			default:
				break;
		}

		if(restart)
		{
			if(rn6752_dbg)
				printk(KERN_ALERT "### mode(%s) progressive(%d) does not match, itu656 dvr_restart\n", rn6752_get_mode_string(dvr_rn6752->mode), progressive);
			dvr_restart();
		}
	}
	//mutex_unlock(&dvr_rn6752->eq_lock);
}

#ifdef RN6752_USE_TIMER
static void rn6752_timeout_timer(struct timer_list *t)
{
	struct dvr_rn6752 *dvr_rn6752 = from_timer(dvr_rn6752, t, timer);

	if(dvr_rn6752)
	{
		queue_work(dvr_rn6752->eq_queue, &dvr_rn6752->eq_work);

		if(dvr_rn6752->timer_timeout > 0)
		{
			dvr_rn6752->timer_timeout --;
			mod_timer(&dvr_rn6752->timer, jiffies +  msecs_to_jiffies(100));
		}
		else
		{
			dvr_rn6752->timer_start = false;
		}
	}
}

static void rn6752_start_timer(int timeout_100ms)
{
	//speed video recognise
	if(g_dvr_rn6752)
	{
		g_dvr_rn6752->timer_timeout = timeout_100ms;
		if(!g_dvr_rn6752->timer_start)
		{
			g_dvr_rn6752->timer_start = true;
			mod_timer(&g_dvr_rn6752->timer, jiffies + msecs_to_jiffies(10));
		}
	}
}
#endif

static irqreturn_t rn6752_intr_handler(int irq, void *dev_id)
{
#if 0
	struct dvr_rn6752 *dvr_rn6752 = (struct dvr_rn6752 *)dev_id;
	unsigned long flags;
	
	spin_lock_irqsave(&dvr_rn6752->spin_lock, flags);
	
	spin_unlock_irqrestore(&dvr_rn6752->spin_lock, flags);
#endif

	return IRQ_HANDLED;
}

static int rn6752_check_id(struct dvr_rn6752 *dvr_rn6752)
{
	int id = -1;
	int ret;	

	ret = rn6752_read_byte(0xfe);
	if(ret < 0)
		goto err_check_id;
	id = (ret<<8);

	ret = rn6752_read_byte(0xfd);
	if(ret < 0)
		goto err_check_id;
	id |= ret;

	if(id == 0x401) {
		dvr_rn6752->id = RN675X_ID_RN6752;
	} else if(id == 0x501) {
		dvr_rn6752->id = RN675X_ID_RN6752M;
	}

	return 0;
err_check_id:
	printk(KERN_ERR "***ERR: %s failed, id:%d, ret:%d\n", __FUNCTION__, id, ret);
	dvr_rn6752->id = RN675X_ID_UNKNOWN;
	return -1;
}

static void rn6752_reset(int gpio)
{
	if(gpio >= 0)
	{
		//hw reset
		gpio_direction_output(gpio,1);
		msleep(1);
		gpio_direction_output(gpio,0);
		msleep(10);
		gpio_direction_output(gpio,1);
		msleep(100);
	}
	else
	{
		//sw reset
		//rn6752_write_byte(0x80, 0x31); //soft reset
		//msleep(100);
		//rn6752_write_byte(0x80, 0x30); //reset complete
	}

	if(g_dvr_rn6752 && (g_dvr_rn6752->curr_channel >= 0))
		rn6752_write_byte(0xD3, g_dvr_rn6752->curr_channel);
	rn6752_write_byte(0x1A, 0x83);	//disable blue screen
	
}

static void rn6752m_pre_init(void) {
	u8 rom_byte1, rom_byte2, rom_byte3, rom_byte4, rom_byte5, rom_byte6;

	rn6752_write_byte(0xE1, 0x80);
	rn6752_write_byte(0xFA, 0x81);
	rom_byte1=rn6752_read_byte (0xFB);
	rom_byte2=rn6752_read_byte (0xFB);
	rom_byte3=rn6752_read_byte (0xFB);
	rom_byte4=rn6752_read_byte (0xFB);
	rom_byte5=rn6752_read_byte (0xFB);
	rom_byte6=rn6752_read_byte (0xFB);

	// config. decoder accroding to rom_byte5 and rom_byte6
	if ((rom_byte6 == 0x00) && (rom_byte5 == 0x00)) {
		rn6752_write_byte(0xEF, 0xAA);  
		rn6752_write_byte(0xE7, 0xFF);
		rn6752_write_byte(0xFF, 0x09);
		rn6752_write_byte(0x03, 0x0C);
		rn6752_write_byte(0xFF, 0x0B);
		rn6752_write_byte(0x03, 0x0C);
	}
	else if (((rom_byte6 == 0x34) && (rom_byte5 == 0xA9)) ||
         ((rom_byte6 == 0x2C) && (rom_byte5 == 0xA8))) {
		rn6752_write_byte(0xEF, 0xAA);  
		rn6752_write_byte(0xE7, 0xFF);
		rn6752_write_byte(0xFC, 0x60);
		rn6752_write_byte(0xFF, 0x09);
		rn6752_write_byte(0x03, 0x18);
		rn6752_write_byte(0xFF, 0x0B);
		rn6752_write_byte(0x03, 0x18);
	}
	else {
		rn6752_write_byte(0xEF, 0xAA);  
		rn6752_write_byte(0xFC, 0x60);
		rn6752_write_byte(0xFF, 0x09);
		rn6752_write_byte(0x03, 0x18);
		rn6752_write_byte(0xFF, 0x0B);
		rn6752_write_byte(0x03, 0x18);	
	}
}

static ssize_t rn6752_get(struct device *dev,
        struct device_attribute *attr, char *buf)
{
	int i;
	
	for(i=0; i<0xff; i++)
	{
		printk(KERN_ALERT "[0x%x]:0x%x\n",i,rn6752_read_byte(i));
	}

	return 0;
}

static ssize_t rn6752_set(struct device *dev,
        struct device_attribute *attr, const char *buf, size_t count)
{	
	if(!strncmp(buf, "write", 5))
	{
		unsigned int reg,val;
		sscanf(buf,"%*s%x%x",&reg,&val);
		rn6752_write_byte(reg,val);
		printk(KERN_ALERT "write reg[0x%02x]:0x%02x\n",reg,val);
	}
	
	if(!strncmp(buf, "read", 4))
	{
		unsigned int reg,val;
		sscanf(buf,"%*s%x",&reg);
		val = rn6752_read_byte(reg);
		printk(KERN_ALERT "read reg[0x%02x]:0x%02x\n",reg,val);
	}

	if(!strncmp(buf, "debug", 5))
	{
		unsigned int val;
		sscanf(buf,"%*s%x",&val);
		if(val)
		{
			rn6752_dbg = true;
			printk(KERN_ALERT "rn6752 debug on\n");
		}
		else
		{
			rn6752_dbg = false;
			printk(KERN_ALERT "rn6752 debug off\n");
		}
	}

	if(!strncmp(buf, "work", 4))
	{
		rn6752_eq_work(NULL);
		printk(KERN_ALERT "### eq_work\n");
	}


    return count;
}

static DEVICE_ATTR(rn6752, S_IWUSR | S_IRUGO,//static DEVICE_ATTR(dvr, S_IWUGO | S_IRUGO,
        rn6752_get, rn6752_set);

static struct attribute *rn6752_sysfs_attrs[] = {
        &dev_attr_rn6752.attr,
        NULL
};

static const struct attribute_group rn6752_sysfs = {
        .attrs  = rn6752_sysfs_attrs,
};

static const struct of_device_id rn6752_of_match[] = {
	{ .compatible = "arkmicro,arkn141_rn6752"},
	{ .compatible = "arkmicro,ark1668_rn6752"},
	{ }
};
MODULE_DEVICE_TABLE(of, ark7116_of_match);

static int dvr_rn6752_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
	struct dvr_rn6752 *dvr_rn6752;
	struct ark_private_data pdata;
	const struct of_device_id *match = NULL;
	int value;
	int ret = -1;

	dvr_rn6752 = devm_kzalloc(&client->dev, sizeof(struct dvr_rn6752), GFP_KERNEL);
	if (!dvr_rn6752)
	{
		printk(KERN_ERR "***ERR: %s %d: failed to allocate memory\n", __FUNCTION__, __LINE__);
		return -ENOMEM;
	}

	dvr_rn6752->gpio_reset = of_get_named_gpio(client->dev.of_node, "reset-gpio", 0);
	if (gpio_is_valid(dvr_rn6752->gpio_reset)) {
		//ark_sys_pad_config_gpio_mode(dvr_rn6752->gpio_reset);
		ret = devm_gpio_request_one(&client->dev, dvr_rn6752->gpio_reset, GPIOF_OUT_INIT_HIGH, "rn6752_reset");
		if (ret) {
			printk(KERN_ERR "***ERR: Failed to request rn6752 reset gpio:%d\n", dvr_rn6752->gpio_reset);
			return -EBUSY;
		}
	} else {
		dvr_rn6752->gpio_reset = -1;
	}

	dvr_rn6752->gpio_pdn = of_get_named_gpio(client->dev.of_node, "pdn-gpio", 0);
	if (gpio_is_valid(dvr_rn6752->gpio_pdn)) {
		//ark_sys_pad_config_gpio_mode(dvr_rn6752->gpio_pdn);
		ret = devm_gpio_request_one(&client->dev, dvr_rn6752->gpio_pdn, GPIOF_OUT_INIT_HIGH, "rn6752_pdn");
		if (ret) {
			printk(KERN_ERR "***ERR: Failed to request rn6752 pdn gpio:%d\n", dvr_rn6752->gpio_pdn);
			return -EBUSY;
		}
	} else {
		dvr_rn6752->gpio_pdn = -1;
	}

	dvr_rn6752->gpio_irq = of_get_named_gpio(client->dev.of_node, "irq-gpio", 0);
	if (gpio_is_valid(dvr_rn6752->gpio_irq)) {
		//ark_sys_pad_config_gpio_mode(dvr_rn6752->gpio_irq);
		ret = devm_gpio_request_one(&client->dev, dvr_rn6752->gpio_irq, GPIOF_OUT_INIT_HIGH, "rn6752_irq");
		if (ret) {
			printk(KERN_ERR "***ERR: Failed to request rn6752 pd gpio:%d\n", dvr_rn6752->gpio_irq);
			return -EBUSY;
		}
	} else {
		dvr_rn6752->gpio_irq = -1;
	}

	if(!of_property_read_u32(client->dev.of_node, "default-channel", &value)) {
		dvr_rn6752->curr_channel = value;
	} else {
		dvr_rn6752->curr_channel = 0;
	}

	if(!of_property_read_u32(client->dev.of_node, "camera-format", &value)) {
		dvr_rn6752->mode = value;
	} else {
		dvr_rn6752->mode = RN6752_MODE_NONE;
	}
	
	if(!of_property_read_u32(client->dev.of_node, "itu601in", &value)) {
		dvr_rn6752->itu601in = value;
	} else {
		dvr_rn6752->itu601in = 0;
	}

	dvr_rn6752->client = client;
	spin_lock_init(&dvr_rn6752->spin_lock);
	//mutex_init(&dvr_rn6752->mutex_lock);
	g_dvr_rn6752 = dvr_rn6752;
	//memcpy(&rn6752_effect, &specinfo_param.carback_mask, sizeof(struct ark_carback_effect_para));
	memset(&rn6752_effect, 0, sizeof(struct ark_carback_effect_para));

	match = of_match_device(&rn6752_of_match[0], &client->dev);
	if (match) {
		//match arkn141
		if(dvr_rn6752->mode != RN6752_MODE_NONE) {	//using static mode
			//reset
			rn6752_reset(dvr_rn6752->gpio_reset);
			//check id
			if(rn6752_check_id(dvr_rn6752))
			{
				printk(KERN_ERR "***ERR: %s rn6752_check_id failed\n", __FUNCTION__);
				return EINVAL;
			}

			if(dvr_rn6752->id == RN675X_ID_RN6752M) {
				rn6752m_pre_init();
			}

			switch(dvr_rn6752->mode) {
				case RN6752_MODE_CVBS_PAL:
					if(dvr_rn6752->id == RN675X_ID_RN6752) {
						if(dvr_rn6752->itu601in == 0)
							rn6752_write_reg(rn6752_tiu656_cvbs_pal, sizeof(rn6752_tiu656_cvbs_pal)/2);
					} else if(dvr_rn6752->id == RN675X_ID_RN6752M) {
						if(dvr_rn6752->itu601in == 1)
							rn6752_write_reg(rn6752m_bt601_cvbs_pal, sizeof(rn6752m_bt601_cvbs_pal)/2);
						else
							rn6752_write_reg(rn6752m_bt656_cvbs_pal, sizeof(rn6752m_bt656_cvbs_pal)/2);
					}
					break;
				case RN6752_MODE_CVBS_NTSC:
					if(dvr_rn6752->id == RN675X_ID_RN6752) {
						if(dvr_rn6752->itu601in == 0)
							rn6752_write_reg(rn6752_tiu656_cvbs_ntsc, sizeof(rn6752_tiu656_cvbs_ntsc)/2);
					} else if(dvr_rn6752->id == RN675X_ID_RN6752M) {
						if(dvr_rn6752->itu601in == 1)
							rn6752_write_reg(rn6752m_bt601_cvbs_ntsc, sizeof(rn6752m_bt601_cvbs_ntsc)/2);
						else
							rn6752_write_reg(rn6752m_bt656_cvbs_ntsc, sizeof(rn6752m_bt656_cvbs_ntsc)/2);
					}
					break;
				case RN6752_MODE_720P_PAL:
					if(dvr_rn6752->id == RN675X_ID_RN6752) {
						if(dvr_rn6752->itu601in == 0)
							rn6752_write_reg(rn6752_tiu656_720p_pal, sizeof(rn6752_tiu656_720p_pal)/2);
					} else {
						if(dvr_rn6752->itu601in == 1)
							rn6752_write_reg(rn6752m_bt601_720p_25pfs, sizeof(rn6752m_bt601_720p_25pfs)/2);
						else
							rn6752_write_reg(rn6752m_bt656_720p_25pfs, sizeof(rn6752m_bt656_720p_25pfs)/2);
					}
					break;
				case RN6752_MODE_720P_NTSC:
					if(dvr_rn6752->id == RN675X_ID_RN6752) {
						if(dvr_rn6752->itu601in == 0)
							rn6752_write_reg(rn6752_tiu656_720p_ntsc, sizeof(rn6752_tiu656_720p_ntsc)/2);
					} else if(dvr_rn6752->id == RN675X_ID_RN6752M) {
						if(dvr_rn6752->itu601in == 1)
							rn6752_write_reg(rn6752m_bt601_720p_30pfs, sizeof(rn6752m_bt601_720p_30pfs)/2);
						else
							rn6752_write_reg(rn6752m_bt656_720p_30pfs, sizeof(rn6752m_bt656_720p_30pfs)/2);
					}
					break;
				case RN6752_MODE_1080P_25FPS:
					if(dvr_rn6752->id == RN675X_ID_RN6752) {
					} else if(dvr_rn6752->id == RN675X_ID_RN6752M) {
						if(dvr_rn6752->itu601in == 1)
							rn6752_write_reg(rn6752m_bt601_1080p_25pfs, sizeof(rn6752m_bt601_1080p_25pfs)/2);
						else
							rn6752_write_reg(rn6752m_bt656_1080p_25pfs, sizeof(rn6752m_bt656_1080p_25pfs)/2);
					}
					break;
				case RN6752_MODE_1080P_30FPS:
					if(dvr_rn6752->id == RN675X_ID_RN6752) {
					} else if(dvr_rn6752->id == RN675X_ID_RN6752M) {
						if(dvr_rn6752->itu601in == 1)
							rn6752_write_reg(rn6752m_bt601_1080p_30pfs, sizeof(rn6752m_bt601_1080p_30pfs)/2);
						else
							rn6752_write_reg(rn6752m_bt656_1080p_30pfs, sizeof(rn6752m_bt656_1080p_30pfs)/2);
					}
					break;
				default:
					break;
			}
		} else {	//obtain mode by dynamic
			
		}
	}
	match = of_match_device(&rn6752_of_match[1], &client->dev);
	if (match) {
		memset(&pdata, 0, sizeof(struct ark_private_data));
		pdata.detect_signal = rn6752_detect_signal;
		pdata.get_progressive = rn6752_get_progressive;
		pdata.select_channel = rn6752_select_channel;
		pdata.display_effect = rn6752_set_display_effect;
#ifdef RN6752_LOW_POWER
		pdata.enter_carback_cb = rn6752_enter_carback_cb;
		pdata.exit_carback_cb = rn6752_exit_carback_cb;
#endif
		pdata.support_max_resolution = TYPE_720P;
		pdata.ic_type = IC_TYPE_RN6752;
		g_itu656in_priv = &pdata;
		g_itu656in_client = client;

		dvr_rn6752->eq_queue = create_singlethread_workqueue("rn6752_eq_queue");
		if(dvr_rn6752->eq_queue)
		{
			INIT_WORK(&dvr_rn6752->eq_work, rn6752_eq_work);
		}

		queue_work(dvr_rn6752->eq_queue, &dvr_rn6752->eq_work);

		if (dvr_rn6752->gpio_irq != -1) {
			gpio_direction_input(dvr_rn6752->gpio_irq);
			//gpio_set_debounce(dvr_rn6752->gpio_irq, 20);
			ret = devm_request_irq(&client->dev, gpio_to_irq(dvr_rn6752->gpio_irq), rn6752_intr_handler, IRQF_TRIGGER_FALLING, "rn6752", (void *)dvr_rn6752);
			if (ret)
			{
				printk(KERN_ERR "***ERR: %s: request irq error\n", __FUNCTION__);
				goto err_request_irq;
			}
		}

#ifdef RN6752_USE_TIMER
		dvr_rn6752->timer_start = false;
		timer_setup(&dvr_rn6752->timer, rn6752_timeout_timer, 0);
		mod_timer(&dvr_rn6752->timer, jiffies +  msecs_to_jiffies(100));
#endif
	}

	ret = sysfs_create_group(&client->dev.kobj, &rn6752_sysfs);
	if (ret) {
		printk(KERN_ERR "***ERR: sysfs_create_group failed\n");
		goto err_sysfs_create_group;
	}
	i2c_set_clientdata(client, dvr_rn6752);

	printk("%s:init done\n", __FUNCTION__);

	return 0;

err_sysfs_create_group:
#ifdef RN6752_USE_TIMER
	del_timer(&dvr_rn6752->timer);
#endif
err_request_irq:
	if(dvr_rn6752->eq_queue)
		destroy_workqueue(dvr_rn6752->eq_queue);
	g_dvr_rn6752 = NULL;
	g_itu656in_priv = NULL;
	g_itu656in_client = NULL;
	dvr_rn6752->gpio_reset = -1;
	dvr_rn6752->gpio_irq = -1;
	dvr_rn6752->gpio_pdn = -1;	
    return ret;	
}

static int dvr_rn6752_remove(struct i2c_client *client)
{
	struct dvr_rn6752 *dvr_rn6752 = i2c_get_clientdata(client);

	if (dvr_rn6752)
	{
        sysfs_remove_group(&client->dev.kobj, &rn6752_sysfs);
#ifdef RN6752_USE_TIMER
		del_timer(&dvr_rn6752->timer);
#endif
		dvr_rn6752->gpio_reset = -1;
		dvr_rn6752->gpio_irq = -1;
		dvr_rn6752->gpio_pdn = -1;	
		
		if(dvr_rn6752->eq_queue)
			destroy_workqueue(dvr_rn6752->eq_queue);
		//mutex_destroy(&dvr_rn6752->mutex_lock);
		i2c_set_clientdata(client, NULL);	
	}

	g_itu656in_priv = NULL;
	g_itu656in_client = NULL;
	g_dvr_rn6752 = NULL;

	return 0;
}

static struct i2c_driver dvr_rn6752_driver = {
	.driver = {
		.name = "rn6752",
		.owner = THIS_MODULE,
		.of_match_table = of_match_ptr(rn6752_of_match),
	},
	.probe = dvr_rn6752_probe,
	.remove = dvr_rn6752_remove,
};

static int __init dvr_rn6752_init(void)
{
	return i2c_add_driver(&dvr_rn6752_driver);
}

static void __exit dvr_rn6752_exit(void)
{
	i2c_del_driver(&dvr_rn6752_driver);
}


module_init(dvr_rn6752_init);
module_exit(dvr_rn6752_exit);

MODULE_AUTHOR("Arkmicro");
MODULE_DESCRIPTION("RN6752 Driver");
MODULE_LICENSE("GPL");
