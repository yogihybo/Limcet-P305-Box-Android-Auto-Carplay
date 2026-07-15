/*
 * Copyright(c) 2012 Hong Kong Applied Science and Technology
 * Research Institute Company Limited (ASTRI)
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 * MA 02110-1301, USA.
 * 
 * Name:
 *      ark_pwm.c
 *
 * Description:
 *
 * 
 * Author:
 *      Chan Man Chi
 *
 * Remarks:
 *
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/platform_device.h>
#include <linux/err.h>
#include <linux/clk.h>
#include <linux/io.h>
#include <linux/pwm.h>

#include <linux/version.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <asm/uaccess.h>
#include <linux/proc_fs.h>


/* uncomment the line below to enable PROC FS interface */
#define ARK_PWM_USE_PROC_DIR_ENTRY

/* uncomment the line below to use default device major number */
//#define ARK_PWM_USE_DEV_MAJOR_DEFAULT


/* This driver is written to work for kernel 2.6.16 or above */

MODULE_AUTHOR("Chan Man Chi");
MODULE_DESCRIPTION("ArkMicro PWM Driver");
MODULE_LICENSE("GPL");

#define ARK_PWM_DEV_MAJOR   248
#define ARK_PWM_DEV_MINOR   0
#define ARK_PWM_DEV_NAME    "dev"

/*************************************************************************
 * Driver information definitions
 *************************************************************************/
#define ARK_PWM_MAJOR_RELEASE              0
#define ARK_PWM_MINOR_RELEASE              0
#define ARK_PWM_BUILD                      1
#define ARK_PWM_VERSION_CODE ((ARK_PWM_MAJOR_RELEASE << 16) | \
    (ARK_PWM_MINOR_RELEASE << 8) | ARK_PWM_BUILD)


/*************************************************************************
 * Debug print functions
 *************************************************************************/
#ifdef ARK_PWM_DBG
#define ARKPWM_DBGPRTK(...) printk(KERN_ALERT __VA_ARGS__)
#else
#define ARKPWM_DBGPRTK(...)
#endif

/*************************************************************************
 * Register definitions
 *************************************************************************/
#define ARK_PWM_REG_BASE    PWM_BASE
#define ARK_PWM_REG_SIZE    0x10

static unsigned int ark_pwm_mmio_base;

#define rPWM_ENA(x)  (*(volatile unsigned int *)(ark_pwm_mmio_base + 0x10 * (x) + 0x00))
#define rPWM_DUTY(x) (*(volatile unsigned int *)(ark_pwm_mmio_base + 0x10 * (x) + 0x04))
#define rPWM_CNTR(x) (*(volatile unsigned int *)(ark_pwm_mmio_base + 0x10 * (x) + 0x08))


#define ARK_PWM_PCLK_MHZ    24

struct pwm_device {
    unsigned int    pwm_id;
    unsigned int    assigned;
    const char      *label;
#ifndef ARK_PWM_USE_HW_EMULATION
    void __iomem    *mmio_base;
#else
    unsigned char   mmio_base[ARK_PWM_REG_SIZE] __attribute__
        ((aligned(ARK_PWM_REG_SIZE)));
#endif
    unsigned int mmio_start;
    unsigned int mmio_end;
    
    struct list_head        node;
    struct platform_device  *pdev;
	const char *driver_name;
	const char *name;
	const char *proc_dev_name;
    int major;
    int minor_start;
    int minor_num;
    int num;
	struct cdev cdev;
    struct device *dev;
};

static char *ark_pwm_proc_dev_name[] = {
    "pwm0",
    "pwm1",
    "pwm2",
    "pwm3"
};

static DEFINE_MUTEX(pwm_lock);
static LIST_HEAD(pwm_list);


/*
 * pwm_config - change a PWM device configuration
 *
 * duty_ns:   The duty cycle of the PWM, in nano-seconds
 * period_ns: The period of the PWM, in nano-seconds
 */
int pwm_config(struct pwm_device *pwm, int duty_ns, int period_ns)
{
    unsigned int pwm_id = pwm->pwm_id;

    ARKPWM_DBGPRTK("%s %d: pwm_id=%d duty_ns=%d period_ns=%d\n",
        __FUNCTION__, __LINE__, pwm_id, duty_ns, period_ns);

	if (!pwm || duty_ns < 0 || period_ns <= 0 || duty_ns > period_ns) {
         ARKPWM_DBGPRTK("%s %d: invalid input\n", __FUNCTION__, __LINE__);
        return -EINVAL;
    }

    /* NOTE: given that PCLK = 24MHz,
     * the minimum duty_ns   = 1/24 * 10^3 = 41.67ns = 42ns
     * the minimum period_ns = minimum duty_ns = 42ns
     */
	if (duty_ns < 1000 / ARK_PWM_PCLK_MHZ) {
         ARKPWM_DBGPRTK("%s %d: invalid input duty_ns\n",
             __FUNCTION__, __LINE__);
        return -EINVAL;
    }

    /* set pwm duty register */
    rPWM_DUTY(pwm_id) = duty_ns * ARK_PWM_PCLK_MHZ / 1000; 

    /* set pwm counter register */
    rPWM_CNTR(pwm_id) = period_ns * ARK_PWM_PCLK_MHZ / 1000;

    return 0;
}
EXPORT_SYMBOL(pwm_config);

/*
 * pwm_enable - start a PWM output toggling
 */
int pwm_enable(struct pwm_device *pwm)
{
    unsigned int pwm_id = pwm->pwm_id;

    ARKPWM_DBGPRTK("%s %d: pwm_id = %d\n", __FUNCTION__, __LINE__, pwm_id);

    rPWM_ENA(pwm_id) = 1;

    return 0;
}
EXPORT_SYMBOL(pwm_enable);

/*
 * pwm_disable - stop a PWM output toggling
 */
void pwm_disable(struct pwm_device *pwm)
{
    unsigned int pwm_id = pwm->pwm_id;

    ARKPWM_DBGPRTK("%s %d: pwm_id = %d\n", __FUNCTION__, __LINE__, pwm_id);

    rPWM_ENA(pwm_id) = 0;
}
EXPORT_SYMBOL(pwm_disable);

/*
 * pwm_request - request a PWM device
 */
struct pwm_device *pwm_request(int pwm_id, const char *label)
{
    struct pwm_device *pwm;
    int found = 0;

    ARKPWM_DBGPRTK("%s %d: pwm_id=%d label=%s\n",
        __FUNCTION__, __LINE__, pwm_id, label);

    mutex_lock(&pwm_lock);

    list_for_each_entry(pwm, &pwm_list, node) {
        if (pwm->pwm_id == pwm_id) {
            found = 1;
            break;
        }
    }

    if (found) {
        if (!pwm->assigned) {
            pwm->assigned = 1;
            pwm->label = label;

            ARKPWM_DBGPRTK("%s %d: request done\n",
                __FUNCTION__, __LINE__);
        } else {
            ARKPWM_DBGPRTK("%s %d: requested pwm not available\n",
                __FUNCTION__, __LINE__);

            pwm = ERR_PTR(-EBUSY);
        }
    } else {
        ARKPWM_DBGPRTK("%s %d: requested pwm not found\n",
            __FUNCTION__, __LINE__);

        pwm = ERR_PTR(-ENOENT);
    }

    mutex_unlock(&pwm_lock);
    return pwm;
}
EXPORT_SYMBOL(pwm_request);

/*
 * pwm_free - free a PWM device
 */
void pwm_free(struct pwm_device *pwm)
{
    mutex_lock(&pwm_lock);

    if (pwm->assigned) {
        pwm->assigned = 0;
        pwm->label = NULL;
    } else {
        ARKPWM_DBGPRTK("%s %d: PWM device already freed\n",
            __FUNCTION__, __LINE__);
    }

    mutex_unlock(&pwm_lock);
}
EXPORT_SYMBOL(pwm_free);

static inline void __add_pwm(struct pwm_device *pwm)
{
    mutex_lock(&pwm_lock);
    list_add_tail(&pwm->node, &pwm_list);
    mutex_unlock(&pwm_lock);
}

struct platform_device *ark_pwm_platform_device = NULL;

#ifdef ARK_PWM_USE_PROC_DIR_ENTRY
/* ===========================================================================
 * Description:
 *		Procfs read and write function
 *
 * Arguments:
 *
 * Return value:
 *
 * Remarks:
 *
 * ===========================================================================
 */
static int ark_pwm_read_proc(char *buf, char **start, off_t offset,
    int count, int *eof, void *data)
{
    struct pwm_device *pwm = (struct pwm_device *)data;
    int len = 0;
    int enable, duty_ns, period_ns;

//    ARKPWM_DBGPRTK("%s %d: enter\n", __FUNCTION__, __LINE__);

    len += sprintf(buf+len,
        "\nUsage: [Operation code] [arg 1] ...\n"
        "\t Read Register   - 0  offset[hex]\n"
        "\t Write Register  - 1  offset[hex] data[hex]\n"
        "\t Config pwm      - 2  duty_ns[dec] period_ns[dec]\n"
        "\t Set pwm_enable  - 3  enable[0/1]\n"
        "\n"
        );

    enable = rPWM_ENA(pwm->pwm_id);

    duty_ns = rPWM_DUTY(pwm->pwm_id) * 1000 / ARK_PWM_PCLK_MHZ;

    period_ns = rPWM_CNTR(pwm->pwm_id) * 1000 / ARK_PWM_PCLK_MHZ;

    len += sprintf(buf+len,
        "\n Current configuration:\n"
        "\t pwm_id=%d\n"
        "\t pwm_enable=%d\n"
        "\t duty_ns=%d\n"
        "\t period_ns=%d\n"
        "\t mmio_base  = 0x%08x\n"
        "\t mmio_start = 0x%08x\n"
        "\t mmio_end   = 0x%08x\n"
        "\n",
        pwm->pwm_id,
        enable, duty_ns, period_ns,
        (unsigned int)pwm->mmio_base, pwm->mmio_start, pwm->mmio_end
        );

    return len;
}

static int ark_pwm_write_proc(struct file *file, const char *buffer,
    unsigned long count, void *data)
{
    struct pwm_device *pwm = (struct pwm_device *)data;
    ssize_t result;
    int op_code;

//    ARKPWM_DBGPRTK("%s %d: enter\n", __FUNCTION__, __LINE__);

    if (count == 0) {
        ARKPWM_DBGPRTK("%s %d: count=0\n", __FUNCTION__, __LINE__);
        return 0;
    }

    result = sscanf(buffer, "%d", &op_code);

    switch (op_code) {
    case 0:	/* read register */
        {
            unsigned int offset, val32;

            sscanf(buffer, "%*d %x", &offset);

            if (offset % 4) {
                printk(KERN_ALERT
                    "invalid reg offset 0x%02x (should be multiples of 4)\n",
                    offset);
                break;
            }

            if (offset >= pwm->mmio_end - pwm->mmio_start) {
                printk(KERN_ALERT
                    "invalid reg offset 0x%02x (out of range)\n",
                    offset);
                break;
            }

            val32 = *((volatile u32 *)(pwm->mmio_base + (offset)));

            printk(KERN_ALERT
                "succeed to read "
                "reg = 0x%02x, val = 0x%02x\n",
                offset,
                val32
                );
        }
        break;
    case 1:	/* write register */
        {
            unsigned int offset, val32;

            sscanf(buffer, "%*d %x %x", &offset, &val32);

            if (offset % 4) {
                printk(KERN_ALERT
                    "invalid reg offset 0x%02x (should be multiples of 4)\n",
                    offset);
                break;
            }

            if (offset >= pwm->mmio_end - pwm->mmio_start) {
                printk(KERN_ALERT
                    "invalid reg offset 0x%02x (out of range)\n",
                    offset);
                break;
            }

            *((volatile u32 *)(pwm->mmio_base + (offset))) = val32;

            printk(KERN_ALERT
                "succeed to write "
                "reg = 0x%02x, val = 0x%02x\n",
                offset, val32
                );
        }
        break;
    case 2: /* Config pwm      - 2  duty_ns[dec] period_ns[dec] */
        {
            unsigned int duty_ns, period_ns;
            int error;

            sscanf(buffer, "%*d %d %d", &duty_ns, &period_ns);

            printk(KERN_ALERT
                "input: duty_ns=%d period_ns=%d\n", duty_ns, period_ns);

            error = pwm_config(pwm, duty_ns, period_ns);
            if (error)
                printk(KERN_ALERT "config pwm failed\n");
            else
                printk(KERN_ALERT "config pwm success\n");
        }
        break;
    case 3: /* Set pwm_enable  - 3  enable[0/1] */
        {
            int enable;

            sscanf(buffer, "%*d %x", &enable);

            printk(KERN_ALERT "input: enable=%d\n", enable);

            if (enable)
                pwm_enable(pwm);
            else
                pwm_disable(pwm);
        }
        break;
    default:
        printk(KERN_ALERT
            "\n Usage: [Operation code] [arg 1] ...\n"
            "\t Read Register   - 0  offset[hex]\n"
            "\t Write Register  - 1  offset[hex] data[hex]\n"
            "\t Config pwm      - 2  duty_ns[dec] period_ns[dec]\n"
            "\t Set pwm_enable  - 3  enable[0/1]\n"
            "\n"
            );
        break;
    }

    return count;
}
#endif //#ifndef ARK_PWM_USE_PROC_DIR_ENTRY

/* This is the first function being called when opening the device
 * driver. It identifies the driver-specific data structure of the device
 * being opened and stores the start pointer of the data structure in the 
 * private_data field of the file structure, i.e. filp->private_data, 
 * for easier access in the future.
 *
 * Arguments:
 *   inode : kernel data structure that represents the device file of the
 *           driver on disk
 *   filp  : a pointer to the file structure that is created by the kernel
 *           to represent the file opened for the device driver
 *
 * Return:
 *   0
 */
static int ark_pwm_open(struct inode *inode, struct file *filp)
{
    struct pwm_device *dev;

    ARKPWM_DBGPRTK("%s %d: enter\n", __FUNCTION__, __LINE__);

    dev = container_of(inode->i_cdev, struct pwm_device, cdev);
    filp->private_data = dev;

    return 0;
}

/* This function is invoked when the associated file structure is being
 * released.
 *
 * Arguments:
 *   inode : kernel data structure that represents the device file of the
 *           driver on disk
 *   filp  : a pointer to the file structure that is created by the kernel
 *           to represent the file opened for the device driver
 *
 * Return:
 *   0
 */
static int ark_pwm_release(struct inode *inode, struct file *filp)
{
    struct pwm_device *dev;

    ARKPWM_DBGPRTK("%s %d: enter\n", __FUNCTION__, __LINE__);

    dev = container_of(inode->i_cdev, struct pwm_device, cdev);

    /* NOTE: intend not to do anything here */

	return(0);
}


/* This function is invoked to handle ioctl system calls and implement
 * the requested ioctl command. It validates the ioctl command and performs
 * the requested device control function. If the command number does not
 * match a valid operation, it returns -ENOTTY. Otherwise, it returns 0
 *
 * Arguments:
 *   inode : kernel data structure that represents the device file of the
 *           driver on disk
 *   filp  : a pointer to the file structure that is created by the kernel
 *           to represent the file opened for the device driver
 *
 * Return:
 *   0
 */
static long ark_pwm_ioctl(struct inode *inode, struct file *file,
    unsigned int cmd, unsigned long arg)
{
//    struct pwm_device *pwm =
//        (struct pwm_device *)file->private_data;

    ARKPWM_DBGPRTK("%s %d: enter\n", __FUNCTION__, __LINE__);

    switch (cmd) {
    default:
        ARKPWM_DBGPRTK("%s %d: undefined cmd (0x%2X)\n",
            __FUNCTION__, __LINE__, cmd);
        return -EINVAL;
    }

    return 0;
}

static struct file_operations ark_pwm_fops = {
    .owner          = THIS_MODULE,
    .open           = ark_pwm_open,
    .unlocked_ioctl = ark_pwm_ioctl,
    .release        = ark_pwm_release,
};

/* This function is invoked when the device module is loaded into the
 * kernel. It allocates system resources for constructing driver control
 * data structures and initializes them accordingly.
 */
static struct
pwm_device *__pwm_probe(
    struct platform_device *pdev,
    unsigned int pwm_id,
    struct pwm_device *parent_pwm
    )
{
    struct pwm_device *pwm;
	struct proc_dir_entry *proc_file;
    struct resource *r;
	dev_t dev;
    int error = 0;

    printk(KERN_ERR "%s %d:enter\n", __FUNCTION__, __LINE__);

    pwm = kzalloc(sizeof(struct pwm_device), GFP_KERNEL);
    if (pwm == NULL) {
        printk(KERN_ERR "%s %d: failed to allocate memory\n",
            __FUNCTION__, __LINE__);
        return ERR_PTR(-ENOMEM);
    }

    pwm->assigned = 0;
    pwm->pwm_id = pwm_id;
    pwm->pdev = pdev;

    pwm->driver_name = "ark_pwm_drv";
    pwm->name = "ark_pwm";
#ifdef ARK_PWM_USE_DEV_MAJOR_DEFAULT
    pwm->major = ARK_PWM_DEV_MAJOR;
#else
    pwm->major = 0; /* if 0, let system choose */
#endif
    pwm->minor_start = pwm_id;
    pwm->minor_num = 1; /* one dev only */
    pwm->num = 1;

    /* register char device */
    if (!pwm->major) {
        error = alloc_chrdev_region(
                    &dev,
                    pwm->minor_start,
                    pwm->num,
                    pwm->name
                    );
        if (!error) {
            pwm->major = MAJOR(dev);
            pwm->minor_start = MINOR(dev);
            printk(KERN_ERR "%s %d: allocate device major=%d minor=%d\n",
                __FUNCTION__, __LINE__,
                pwm->major, pwm->minor_start);
        }
    } else {
        dev = MKDEV(pwm->major, pwm->minor_start);
        printk(KERN_ERR "%s %d: dev %d\n", __FUNCTION__, __LINE__, dev);
        error = register_chrdev_region(dev, pwm->num,
            (char *)pwm->name);
    }

    if (error < 0) {
        printk(KERN_ERR "%s %d: register driver error\n",
            __FUNCTION__, __LINE__);
        goto err_driver_register;
    }

    /* associate the file operations */
    cdev_init(&pwm->cdev, &ark_pwm_fops);
    pwm->cdev.owner    = THIS_MODULE; //driver->owner;
    pwm->cdev.ops      = &ark_pwm_fops;
    error = cdev_add(&pwm->cdev, dev, pwm->num);
    if (error) {
        printk(KERN_ERR "%s %d: cdev add error\n", __FUNCTION__, __LINE__);
        goto err_cdev_add;
    }
    printk(KERN_ERR "%s %d: cdev made, name: %s, major: %d, minor: %d \n",
        __FUNCTION__, __LINE__, pwm->name, pwm->major, pwm->minor_start);

#ifdef ARK_PWM_USE_PROC_DIR_ENTRY
    pwm->proc_dev_name = ark_pwm_proc_dev_name[pwm_id];
    /* create procfs entry */
    proc_file = create_proc_entry(
                    pwm->proc_dev_name,
                    0644,
                    NULL
                    );
    proc_file->data = (void *)pwm;
    proc_file->read_proc  = ark_pwm_read_proc;
    proc_file->write_proc = ark_pwm_write_proc;
#endif // ARK_PSCL_PROC_DIR_ENTRY_ENABLE

    printk(KERN_ERR "%s %d\n", __FUNCTION__, __LINE__);

    if (parent_pwm != NULL) {
        /* registers for the second PWM has offset of 0x10 */
#ifndef ARK_PWM_USE_HW_EMULATION
        pwm->mmio_base  = parent_pwm->mmio_base + 0x10;
#endif
        pwm->mmio_start = parent_pwm->mmio_start;
        pwm->mmio_end   = parent_pwm->mmio_end;

        printk(KERN_ERR "%s %d: ark_pwm_mmio_base=%08x mmio_base=%08x\n",
            __FUNCTION__, __LINE__, ark_pwm_mmio_base,
            (unsigned int)pwm->mmio_base);

        pwm->dev = &pdev->dev;

        __add_pwm(pwm);

        platform_set_drvdata(pdev, pwm);

        return pwm;
    }

#ifdef ARK_PWM_USE_HW_EMULATION
    /* remap register space */
	r = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (r == NULL)
        printk(KERN_ERR "%s %d: no memory resource defined\n",
            __FUNCTION__, __LINE__);
	else
        printk(KERN_ERR "%s %d: mmio_start=0x%08x mmio_end=0x%08x\n",
            __FUNCTION__, __LINE__, r->start, r->end);

    pwm->mmio_start = r->start;
	pwm->mmio_end   = r->end;
#else
	r = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (r == NULL) {
        printk(KERN_ERR
            "%s %d: no memory resource defined\n",
            __FUNCTION__, __LINE__
            );
		error = -ENODEV;
		goto err_mem_res_req;
	}

	r = request_mem_region(r->start, r->end - r->start + 1, pdev->name);
	if (r == NULL) {
        printk(KERN_ERR
            "%s %d: failed to request memory resource, start=%08x end=%08x\n",
            __FUNCTION__, __LINE__, r->start, r->end
            );
		error = -EBUSY;
		goto err_mem_res_req;
	}

	pwm->mmio_base = ioremap(r->start, r->end - r->start + 1);
	if (pwm->mmio_base == NULL) {
        printk(KERN_ERR
            "%s %d: failed to ioremap registers, start=%08x end=%08x\n",
            __FUNCTION__, __LINE__, r->start, r->end
            );
		error = -ENODEV;
		goto err_ioremap;
	}
    printk(KERN_ERR "%s %d: ioremap regs done, start=%08x end=%08x\n",
        __FUNCTION__, __LINE__, r->start, r->end);

    pwm->mmio_start = r->start;
	pwm->mmio_end   = r->end;
#endif
    ark_pwm_mmio_base = (unsigned int)pwm->mmio_base;

    printk(KERN_ERR "%s %d: ark_pwm_mmio_base=%08x mmio_base=%08x\n",
        __FUNCTION__, __LINE__, ark_pwm_mmio_base,
        (unsigned int)pwm->mmio_base);

    pwm->dev = &pdev->dev;

	__add_pwm(pwm);

	platform_set_drvdata(pdev, pwm);
	return pwm;

#ifndef ARK_PWM_USE_HW_EMULATION
err_ioremap:
    release_mem_region(
        pwm->mmio_start,
        pwm->mmio_end - pwm->mmio_start + 1);
err_mem_res_req:
#endif

#ifndef ARK_PWM_PROC_DIR_ENTRY_ENABLE
	remove_proc_entry(pwm->proc_dev_name, NULL);
#endif
	cdev_del(&pwm->cdev);
err_cdev_add:
	unregister_chrdev_region(dev, pwm->num);
err_driver_register:
	kfree(pwm);
	return ERR_PTR(error);
}

static int __devinit ark_pwm_probe(struct platform_device *pdev)
{
	struct pwm_device *pwm;

    printk(KERN_ERR "%s %d:enter\n", __FUNCTION__, __LINE__);

    ark_pwm_platform_device = pdev;

    /* assign pwm channel 0 as primary pmw output */
	pwm = __pwm_probe(pdev, 0, NULL);
	if (IS_ERR(pwm))
		return PTR_ERR(pwm);

#if (1)
    /* assign pwm channel 1 as secondary pmw output */
	pwm = __pwm_probe(pdev, 1, pwm);
	if (IS_ERR(pwm))
		return PTR_ERR(pwm);

	pwm = __pwm_probe(pdev, 2, pwm);
	if (IS_ERR(pwm))
		return PTR_ERR(pwm);

	pwm = __pwm_probe(pdev, 3, pwm);
	if (IS_ERR(pwm))
		return PTR_ERR(pwm);	
#endif

    return 0;
}

/* This function is invoked when the device module is removed from the
 * kernel. It releases all resources to the system.
 */
static int __devexit ark_pwm_remove(struct platform_device *pdev)
{
    struct pwm_device *pwm;
    dev_t dev;

    printk(KERN_ERR "%s %d:enter\n", __FUNCTION__, __LINE__);

    pwm = platform_get_drvdata(pdev);
    if (pwm == NULL)
        return -ENODEV;

    dev = MKDEV(pwm->major, pwm->minor_start);

    mutex_lock(&pwm_lock);
    list_del(&pwm->node);
    mutex_unlock(&pwm_lock);

#ifndef ARK_PWM_USE_HW_EMULATION
    iounmap(pwm->mmio_base);
	release_mem_region(
        pwm->mmio_start,
        pwm->mmio_end - pwm->mmio_start + 1
        );
#endif

#ifdef ARK_PWM_USE_PROC_DIR_ENTRY
	/* remove procfs entry */
	remove_proc_entry(pwm->proc_dev_name, NULL);
#endif
	cdev_del(&pwm->cdev);

	unregister_chrdev_region(dev, pwm->num);

    kfree(pwm);
    return 0;
}

static struct platform_driver ark_pwm_driver = {
    .driver     = {
        .name   = "ark-pwm",
    },
    .probe      = ark_pwm_probe,
    .remove     = __devexit_p(ark_pwm_remove),
};

static int __init ark_pwm_init(void)
{
    int ret = 0;

    printk(KERN_ERR "%s %d:enter\n", __FUNCTION__, __LINE__);
    ret = platform_driver_register(&ark_pwm_driver);
    if (ret) {
        printk(KERN_ERR "%s %d: failed to register ark_lcd_driver\n",
            __FUNCTION__, __LINE__);
    }

    return ret;
}
postcore_initcall(ark_pwm_init);

static void __exit ark_pwm_exit(void)
{
    printk(KERN_ERR "%s %d:enter\n", __FUNCTION__, __LINE__);
    platform_driver_unregister(&ark_pwm_driver);
}
postcore_exitcall(ark_pwm_exit);

EXPORT_SYMBOL(ark_pwm_platform_device);
