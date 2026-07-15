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
 *      ark_display_fb.c
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
#include <linux/version.h>
#include <linux/kernel.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <asm/uaccess.h>
#include <linux/interrupt.h>
#include <linux/wait.h>
#include <linux/fb.h>
#include <linux/matroxfb.h>

#include <linux/mm.h>
#include <linux/vmalloc.h>
#include <linux/dma-mapping.h>

#include <linux/videodev2.h>
#include <media/videobuf-core.h>
#include <media/v4l2-common.h>

#include <linux/ark/ringpair.h>
#include <linux/ark/ark_display_io.h>
#include <mach/hardware.h>

#include "ark_display.h"
#include "ark_display_priv.h"
#include "ark_lcd_regs.h"
#include "ark_sys_regs.h"
#include "ark_display_fb.h"

static struct ark_disp_context *fb_disp_context;

/* macros for testing fb_var_screeninfo attributes */
#define is_attribute_mode(var) (((var)->bits_per_pixel == 4) && \
    ((var)->nonstd != 0))
#define is_yuv(var) ((((var)->bits_per_pixel == 16) || \
    ((var)->bits_per_pixel == 8)) && ((var)->nonstd != 0))
#define is_window_interlaced(var) (((var)->vmode & FB_VMODE_INTERLACED) \
    == FB_VMODE_INTERLACED)

/* macros for testing fb_videomode attributes */
#define is_display_interlaced(mode) (((mode)->vmode & FB_VMODE_INTERLACED) \
	== FB_VMODE_INTERLACED)


static int ark_disp_fb_is_osd_window(const struct fb_info *info)
{
    const struct ark_disp_obj_info *disp = info->par;

    if (disp->layer_info.type == ARKDISP_LAYER_OSD)
        return 1;
    else
        return 0;
}

static int ark_disp_fb_pixfmt_is_rgb(const struct fb_info *info)
{
    const struct ark_disp_obj_info *disp = info->par;

    switch (disp->layer_info.pixfmt)
    {
    case ARKDISP_PIXFMT_RGBI555:
    case ARKDISP_PIXFMT_RGB565:
    case ARKDISP_PIXFMT_RGBA888:
    case ARKDISP_PIXFMT_RGB888:
    case ARKDISP_PIXFMT_RGBA1555:
    case ARKDISP_PIXFMT_RGBA1888:
    case ARKDISP_PIXFMT_RGBA4888:
    case ARKDISP_PIXFMT_RGB666:
    case ARKDISP_PIXFMT_RGBA1666:
        return 1;
        break;
    default:
        break;
    }
    return 0;
}

/*
 * Construct an fb_var_screeninfo structure from an fb_videomode structure
 * describing the display and a davinci_layer_config structure describing a window.
 * The following members of var not modified:
 *	var->xoffset
 *	var->yoffset
 *	var->xres_virtual
 *	var->yres_virtual
 * The following members of var are loaded with values derived from mode:
 *	var->pixclock
 *	var->left_margin
 *	var->hsync_len
 *	var->vsync_len
 *	var->right_margin
 *	var->upper_margin
 *	var->lower_margin
 *	var->sync
 *	var->vmode (all bits except bit 0: FB_VMODE_INTERLACED)
 * The following members of var are loaded with values derived from lconfig:
 *	var->xres
 *	var->yres
 *	var->bits_per_pixel
 *	var->red
 *	var->green
 *	var->blue
 *	var->transp
 *	var->nonstd
 *	var->grayscale
 *	var->activate
 *	var->height
 *	var->width
 *	var->accel_flags
 *	var->rotate
 *	var->vmode (only bit 0: FB_VMODE_INTERLACED)
 *
 * If the display resolution (xres and yres) specified in mode matches the
 * window resolution specified in lconfig, then the display timing info returned
 * in var is valid and var->pixclock will be the value derived from mode.
 * If the display resolution does not match the window resolution, then
 * var->pixclock will be set to 0 to indicate that the display timing info
 * returned in var is not valid.
 *
 */


/*
 * this macro converts a 16-bit color passed to fb_setcolreg to the width
 * supported by the pixel format.
 */
#define CNVT2HW(val, width) ((((val)<<(width))+0x7FFF-(val))>>16)

/* This function sets a single color register
 *
 * Arguments:
 *   regno  : color register number
 *   red    : red color index
 *   green  : green color index
 *   blue   : blue color index
 *   transp : alpha index
 *   info   : pointer to a fb_info object that represents a single frame buffer
 *
 * Return:
 *   return negative errno on error, or zero on success.
 */
static int
ark_disp_fb_setcolreg(
    unsigned int regno,
    unsigned int red, unsigned int green, unsigned int blue,
    unsigned int transp, struct fb_info *info
    )
{
    unsigned r, g, b, t;

    ARKDISP_DBGPRTK(
        "%s %d: regno(%d) red(0x%x), green(0x%x), blue(0x%x), transp(0x%x)\n",
        __FUNCTION__, __LINE__, regno, red, green, blue, transp);

    if (!ark_disp_fb_is_osd_window(info))
        return -EINVAL;

    if (regno >= 256)	/* no. of hw registers */
        return -EINVAL;

    /*
     * an RGB color palette isn't applicable to a window with a YUV pixel
     * format or to a window in attribute mode.
     */
    if (is_yuv(&info->var) || is_attribute_mode(&info->var))
       return -EINVAL;

    /* Truecolor:
     *    var->{color}.offset contains start of bitfield
     *    var->{color}.length contains length of bitfield
     *    pseudo_palette is programmed to (red << red.offset) |
     *                                    (green << green.offset) |
     *                                    (blue << blue.offset) |
     *                                    (transp << transp.offset)
     *    color depth = SUM(var->{color}.length})
     *
     *  The color depth is used by fbcon for choosing the logo and also
     *  for color palette transformation if color depth < 4
     */

    switch (info->fix.visual) {
    case FB_VISUAL_TRUECOLOR:
        r = CNVT2HW(red, info->var.red.length);
        g = CNVT2HW(green, info->var.green.length);
        b = CNVT2HW(blue, info->var.blue.length);
        t = CNVT2HW(transp, info->var.transp.length);
        break;
    case FB_VISUAL_PSEUDOCOLOR:
    default:
        r = CNVT2HW(red, 8);
        g = CNVT2HW(green, 8);
        b = CNVT2HW(blue, 8);
        t = 0;
        break;
    }

    /* truecolor has hardware independent palette */
    if (info->fix.visual == FB_VISUAL_TRUECOLOR) {
        u32 v;

        if (regno >= 16)
           return -EINVAL;

        v = (r << info->var.red.offset) |
            (g << info->var.green.offset) |
            (b << info->var.blue.offset) |
            (t << info->var.transp.offset);

        switch (info->var.bits_per_pixel) {
        case 16:
            ((u16 *) (info->pseudo_palette))[regno] = v;
            break;
        case 24:
        case 32:
            ((u32 *) (info->pseudo_palette))[regno] = v;
            break;
        }
        return 0;
    }

    /* This is the point were you need to fill up the contents of
     * info->pseudo_palette. This structure is used _only_ by fbcon, thus
     * it only contains 16 entries to match the number of colors supported
     * by the console. The pseudo_palette is used only if the visual is
     * in directcolor or truecolor mode.  With other visuals, the
     * pseudo_palette is not used. (This might change in the future.)
     *
     * The contents of the pseudo_palette is in raw pixel format. Thus, each
     * entry can be written directly to the framebuffer without any conversion.
     * The pseudo_palette is (void *).  However, if using the generic
     * drawing functions (cfb_imageblit, cfb_fillrect), the pseudo_palette
     * must be casted to (u32 *) _regardless_ of the bits per pixel. If the
     * driver is using its own drawing functions, then it can use whatever
     * size it wants.
     */
//chitest
//	spin_lock_irqsave(&display_obj->fb_info.lock, flags);
//
//	spin_unlock_irqrestore(&display_obj->fb_info.lock, flags);

    return 0;
}


/*  NOTE: with truecolor mode (used in this driver), no color palettes
 *  are supported. Here a pseudo palette is created which we store the
 *  value in pseudo_palette in struct fb_info. For pseudocolor mode we
 *  have a limited color palette. To deal with this we can program what
 *  color is displayed for a particular pixel value. DirectColor is
 *  similar in that we can program each color field. If we have a static
 *  colormap we don't need to implement this function.
 */

/* This function pans (or wrap, depending on the 'vmode' field) the display
 * using the 'xoffset' and 'yoffset' fields of the 'var' structure
 *
 * Arguments:
 *   var  : frame buffer variable screen structure
 *   info : pointer to a fb_info object that represents a single frame buffer
 *
 * Return:
 *   return negative errno on error, or zero on success.
 */
static int
ark_disp_fb_pan_display(struct fb_var_screeninfo *var, struct fb_info *info)
{
    struct ark_disp_context *context = fb_disp_context;
    struct ark_disp_obj_info *disp = info->par;
    unsigned int addr;

    ARKDISP_DBGPRTK("%s %d: enter\n", __FUNCTION__, __LINE__);

    if (context == NULL) {
        ARKDISP_DBGPRTK("%s %d: null context err\n", __FUNCTION__, __LINE__);
        return -EFAULT;
    }

    if (var->xoffset > info->var.xres_virtual - info->var.xres)
        return -EINVAL;
    if (var->yoffset > info->var.yres_virtual - info->var.yres)
        return -EINVAL;

    /* xoffset must be a multiple of xpanstep */
    if (info->fix.xpanstep) {
        if (var->xoffset % info->fix.xpanstep)
            return -EINVAL;
    }

    /* xoffset must be a multiple of xpanstep */
    if (info->fix.ypanstep) {
        if (var->yoffset % info->fix.ypanstep)
            return -EINVAL;
    }

#if (0)
    /*
    * the followings have been checked by fb_pan_display() in fbmem.c
    * before calling this function
    */
    if (var->xoffset) {
        /* not supported */
        return -EINVAL;
    }

    if (!info->fix.ypanstep || var->yoffset % info->fix.ypanstep) {
        ARKDISP_DBGPRTK(
            "%s %d: zero ypanstep or yoffset is not a multiple of ypanstep\n",
            __FUNCTION__, __LINE__
            );
        return -EINVAL;
    }

    if (var->yoffset >= info->var.yres_virtual) {
        ARKDISP_DBGPRTK("%s %d: out of range\n", __FUNCTION__, __LINE__);
        return -EINVAL;
    }
#endif
    
    if (!ark_disp_fb_is_osd_window(info)) {
        ARKDISP_DBGPRTK("%s %d: fb is video window\n",
            __FUNCTION__, __LINE__);
        return -EINVAL;
    }

    if (!ark_disp_fb_pixfmt_is_rgb(info)) {
        ARKDISP_DBGPRTK("%s %d: fb is non-rgb plane\n",
            __FUNCTION__, __LINE__);
        return -EINVAL;
    }

    spin_lock(&context->lock);

    info->var.yoffset = var->yoffset;

    addr = info->fix.smem_start + info->fix.line_length * info->var.yoffset;

    /* configure hardware */
    ark_disp_set_next_buf_start_addr(disp->id, addr);

    spin_unlock(&context->lock);

    ARKDISP_DBGPRTK("%s %d: addr=0x%08x\n", __FUNCTION__, __LINE__, addr);

    return 0;
}

#if (0)
/* This function blanks the display (NOT a required function)
 * using the 'xoffset' and 'yoffset' fields of the 'var' structure
 *
 * Arguments:
 *   blank_mode: the blank mode we want
 *   info      : pointer to a fb_info object that represents a single frame buffer
 *
 * Return:
 *   return non-zero for any modes that are unimplemented, or zero on success.
 */
static int ark_disp_fb_blank(int blank_mode, struct fb_info *info)
{
    ARKDISP_DBGPRTK("%s %d: blank_mode(0x%x)\n",
        __FUNCTION__, __LINE__, blank_mode);

    switch (blank_mode) {
    case FB_BLANK_UNBLANK:
        /* Screen: On, HSync: On, VSync: On */
        /* control CRT monitor power management */
        break;
    case FB_BLANK_HSYNC_SUSPEND:
        /* Screen: Off, HSync: Off, VSync: On */
        /* control CRT monitor power management */
        break;
    case FB_BLANK_VSYNC_SUSPEND:
        /* Screen: Off, HSync: On, VSync: Off */
        /* control CRT monitor power management */
        break;
    case FB_BLANK_POWERDOWN:
        /* Screen: Off, HSync: Off, VSync: Off */
        /* control CRT monitor power management */
        break;
    }

    return 0;
}

/*
 * fb_blank method
 *
 * Blank the screen if blank_mode != 0, else unblank.
 */
int ark_disp_fb_blank(int blank_mode, struct fb_info *info)
{
    struct ark_disp_obj_info *disp = info->par;
    int retval = 0;

//chitest check if this window is the only active window on this screen

    if (!blank_mode) {
        retval = info->fbops->fb_check_var(&info->var, info);
        if (retval)
            return retval;
        retval = info->fbops->fb_set_par(info);
    } else {
        ark_display_disp_disable_layer(display_obj->layer_info.out_dev);
    }

    return retval;
}
#endif


/*
 * Determine if the window configuration specified by var will fit in a
 * framebuffer of size fb_size.
 * Returns 1 if the window will fit in the framebuffer, or 0 otherwise.
 */
static int ark_disp_fb_window_will_fit_framebuffer(
    const struct fb_var_screeninfo *var,
    unsigned fb_size
    )
{
    unsigned line_length;

    line_length = (var->bits_per_pixel * var->xres_virtual + 7) / 8;
    /* line length must be a multiple of the cache line size (32) */
    line_length = ((line_length + 31) / 32) * 32;

    if (var->yres_virtual * line_length <= fb_size)
        return 1;
    else
        return 0;
}

/*
 *  NOTE: Setting the video mode has been split into two parts.
 *  First part, xxxfb_check_var, must not write anything
 *  to hardware, it should only verify and adjust var.
 *  This means it doesn't alter par but it does use hardware
 *  data from it to check this var.
 */

/* This function checks variable screen info
 *
 * Arguments:
 *   var  : pointer to variable screen information
 *   info : pointer to a fb_info object that represents a single frame buffer
 *
 * Return:
 *   return negative errno on error, or zero on success.
 */
static int
ark_disp_fb_check_var(struct fb_var_screeninfo *var, struct fb_info *info)
{
    struct ark_disp_obj_info *disp = info->par;

    /*
     * xres, yres, xres_virtual, or yres_virtual equal to zero is treated as
     * a special case.  It indicates that the window should be disabled.  If
     * the window is a video window, it will also be released.
     */
    if (var->xres == 0 || var->yres == 0 || var->xres_virtual == 0
        || var->yres_virtual == 0) {
        var->xres = 0;
        var->yres = 0;
        var->xres_virtual = 0;
        var->yres_virtual = 0;
        return 0;
    }

    switch (var->bits_per_pixel) {
    case 1:
    case 2:
    case 4:
    case 8:
    case 16:
    case 24:
    case 32:
        break;
    default:
        return -EINVAL;
    }

    if (var->xres_virtual < var->xres || var->yres_virtual < var->yres)
        return -EINVAL;
    if (var->xoffset > var->xres_virtual - var->xres)
        return -EINVAL;
    if (var->yoffset > var->yres_virtual - var->yres)
        return -EINVAL;
    if (disp->layer_info.src_xres != var->xres ||
        disp->layer_info.src_yres != var->yres)
        return -EINVAL;

    /* offset must be a multiple of panstep */
//    if (var->xoffset & ~(fix.xpanstep - 1))
    if (info->fix.xpanstep && var->xoffset % info->fix.xpanstep)
        return -EINVAL;

    if (info->fix.ypanstep && var->yoffset % info->fix.ypanstep)
        return -EINVAL;

    /* check if we have enough video memory to support this mode */
    if (!ark_disp_fb_window_will_fit_framebuffer(var, info->fix.smem_len))
        return -EINVAL;

    /*
     * reject this var if the osd would have to modify the window
     * geometry to make it work.
     */
    if (disp->layer_info.src_xres != var->xres ||
        disp->layer_info.src_yres != var->yres)
        return -EINVAL;

    /*
     * At this point we have accepted the var, so now we convert our layer
     * configuration struct back to the var in order to make all of the
     * pixel format and geometry values consistent.  The var timing values
     * will be unmodified, as we have no way to verify them.
     */

    return 0;
}

static int ark_disp_fb_set_par(struct fb_info *info)
{
    struct ark_disp_obj_info *disp = info->par;
    struct ark_disp_layer_info *layer;
    struct ark_disp_buf_info *buf;
    unsigned int rgb_order, pixfmt;
    struct ark_disp_layer_cfg_arg layer_arg;
    int error;


    ARKDISP_DBGPRTK("%s %d:enter\n", __FUNCTION__, __LINE__);

    layer = &disp->layer_info;
    buf   = &disp->buf_info;

    /* NOTE: info->var contains the updated var info */

    /*
     * xres, yres, xres_virtual, or yres_virtual equal to zero is treated as
     * a special case.  It indicates that the window should be disabled.  If
     * the window is a video window, it will also be released.
     * Note that we disable the window, but we do not set the
     * win->disable_window flag.  This allows the window to be re-enabled
     * simply by using the FBIOPUT_VSCREENINFO ioctl to set a valid
     * configuration.
     */
    if (info->var.xres == 0 || info->var.yres == 0 ||
        info->var.xres_virtual == 0 || info->var.yres_virtual == 0) {
        spin_lock(&fb_disp_context->lock);
        ark_disp_set_layer_en(disp->id, 0);
        spin_unlock(&fb_disp_context->lock);
        return 0;
    }

    if (info->var.grayscale != 0 || info->var.nonstd != 0) {
        return -EINVAL;
    }

    if (info->var.xres * info->var.yres * buf->buf_num > buf->buf_pool_size) {
        return -EINVAL;
    }

    if (info->var.bits_per_pixel == 32) {
//       if (info->var.transp.offset == 0 &&
//           info->var.transp.length == 8 &&
//           info->var.transp.msb_right == 0 &&
//           info->var.red.offset == 8 &&
//           info->var.red.length == 8 &&
//            info->var.red.msb_right == 0 &&
//            info->var.green.offset == 16 &&
//            info->var.green.length == 8 &&
//            info->var.green.msb_right == 0 &&
//            info->var.blue.offset == 24 &&
//            info->var.blue.length == 8 &&
//            info->var.blue.msb_right == 0) {
//            ARKDISP_DBGPRTK("%s %d: BGRA8888\n", __FUNCTION__, __LINE__);
//            rgb_order = ARKDISP_RGB_ORDER_BGR;
//            pixfmt = ARKDISP_PIXFMT_RGBA888;
//        } else if (
	if (info->var.transp.offset == 24 &&
            info->var.transp.length == 8 &&
            info->var.transp.msb_right == 0 &&
            info->var.red.offset == 16 &&
            info->var.red.length == 8 &&
            info->var.red.msb_right == 0 &&
            info->var.green.offset == 8 &&
            info->var.green.length == 8 &&
            info->var.green.msb_right == 0 &&
            info->var.blue.offset == 0 &&
            info->var.blue.length == 8 &&
            info->var.blue.msb_right == 0) {
            ARKDISP_DBGPRTK("%s %d: RGBA8888\n", __FUNCTION__, __LINE__);
            rgb_order = ARKDISP_RGB_ORDER_RGB;
            pixfmt = ARKDISP_PIXFMT_RGBA888;
        } else {
            return -EINVAL;
        }
    } else if (info->var.bits_per_pixel == 16) {
        if (info->var.transp.length == 0 &&
            info->var.red.offset == 0 &&
            info->var.red.length == 5 &&
            info->var.red.msb_right == 0 &&
            info->var.green.offset == 5 &&
            info->var.green.length == 6 &&
            info->var.green.msb_right == 0 &&
            info->var.blue.offset == 11 &&
            info->var.blue.length == 5 &&
            info->var.blue.msb_right == 0) {
            ARKDISP_DBGPRTK("%s %d: BGR565\n", __FUNCTION__, __LINE__);
            rgb_order = ARKDISP_RGB_ORDER_BGR;
            pixfmt = ARKDISP_PIXFMT_RGB565;
        } else if (
            info->var.transp.length == 0 &&
            info->var.red.offset == 11 &&
            info->var.red.length == 5 &&
            info->var.red.msb_right == 0 &&
            info->var.green.offset == 5 &&
            info->var.green.length == 6 &&
            info->var.green.msb_right == 0 &&
            info->var.blue.offset == 0 &&
            info->var.blue.length == 5 &&
            info->var.blue.msb_right == 0) {
            ARKDISP_DBGPRTK("%s %d: RGB565\n", __FUNCTION__, __LINE__);
            rgb_order = ARKDISP_RGB_ORDER_RGB;
            pixfmt = ARKDISP_PIXFMT_RGB565;
        } else {
            return -EINVAL;
        }
    } else if (info->var.bits_per_pixel == 24) {
        if (info->var.transp.length == 0 &&
            info->var.red.offset == 0 &&
            info->var.red.length == 8 &&
            info->var.red.msb_right == 0 &&
            info->var.green.offset == 8 &&
            info->var.green.length == 8 &&
            info->var.green.msb_right == 0 &&
            info->var.blue.offset == 16 &&
            info->var.blue.length == 8 &&
            info->var.blue.msb_right == 0)
        {
            ARKDISP_DBGPRTK("%s %d: BGR888\n", __FUNCTION__, __LINE__);
            rgb_order = ARKDISP_RGB_ORDER_BGR;
            pixfmt = ARKDISP_PIXFMT_RGB888;
        } else if (
            info->var.transp.length == 0 &&
            info->var.red.offset == 16 &&
            info->var.red.length == 8 &&
            info->var.red.msb_right == 0 &&
            info->var.green.offset == 8 &&
            info->var.green.length == 8 &&
            info->var.green.msb_right == 0 &&
            info->var.blue.offset == 0 &&
            info->var.blue.length == 8 &&
            info->var.blue.msb_right == 0) {
            ARKDISP_DBGPRTK("%s %d: RGB888\n", __FUNCTION__, __LINE__);
            rgb_order = ARKDISP_RGB_ORDER_RGB;
            pixfmt = ARKDISP_PIXFMT_RGB888;
        } else {
            return -EINVAL;
        }
    } else {
        return -EINVAL;
    }

    /* change format first */
    layer_arg.layer_id = disp->id;
    if (ark_disp_get_layer_cfg(&layer_arg) != 0) {
        ARKDISP_DBGPRTK("%s %d: get layer cfg err\n", __FUNCTION__, __LINE__);
        return -EINVAL;
    }

    if (layer_arg.pixfmt != pixfmt ||
        layer_arg.rgb_order != rgb_order ||
        layer_arg.src_xres != info->var.xres ||
        layer_arg.src_yres != info->var.yres
        ) {
        layer_arg.enable = 1; // always enable layer for valid settings
        layer_arg.pixfmt = pixfmt;
        layer_arg.rgb_order = rgb_order;
        layer_arg.src_xres = info->var.xres;
        layer_arg.src_yres = info->var.yres;

        spin_lock(&disp->lock);
        error = ark_disp_try_layer_cfg(&layer_arg);
        spin_unlock(&disp->lock);
        if (error) {
            ARKDISP_DBGPRTK("%s %d: invalid para\n", __FUNCTION__, __LINE__);
            return -EINVAL;
        }

        spin_lock(&disp->lock);
        error = ark_disp_set_layer_cfg(&layer_arg);
        spin_unlock(&disp->lock);

        if (error) {
            ARKDISP_DBGPRTK("%s %d: get layer cfg err\n",
                __FUNCTION__, __LINE__);
            return -EINVAL;
        }
    }

    return 0;
}

/*
 * FBIO_SETATTRIBUTE handler
 *
 * This ioctl is deprecated. The user can write the attribute values directly
 * to the OSD framebuffer.
 *
 * Set a uniform attribute value over a rectangular area on the attribute
 * window. The attribute value (0 to 15) is passed through the fb_fillrect's
 * color parameter. r->dx and r->width must both be even.  If not, they are
 * rounded down.
 */

/* This function handles frame buffer ioctl
 *
 * Arguments:
 *   info : pointer to a fb_info object that represents a single frame buffer
 *   cmd  : frame buffer ioctl command
 *   arg  : input argument
 *
 * Return:
 *   return negative errno on error, or zero on success.
 */
static int ark_disp_fb_ioctl(struct fb_info *info, unsigned int cmd,
    unsigned long arg)
{
	struct ark_disp_obj_info *disp = info->par;

    ARKDISP_DBGPRTK("%s %d: enter\n", __FUNCTION__, __LINE__);

    switch (cmd) {
    case FBIO_WAITFORVSYNC:
	case ARKFB_WAITFORVSYNC:
        //ARKDISP_DBGPRTK("%s %d:FBIO_WAITFORVSYNC\n", __FUNCTION__, __LINE__);

        /* this ioctl accepts an integer argument to specify a
         * display; since we can get the display number directly from
         * info, so we will simply ignore the argument.
         */

        /* wait for the next vsync interrupt */
        return ark_disp_wait_frame_sync_interrupt(disp);
	case ARKFB_UPDATE_WINDOW:
		{
			struct arkfb_update_window input_arg;
			
			copy_from_user(&input_arg, (void*)arg, sizeof(input_arg));
			return ark_fb_update_window(disp, &input_arg);
		}
	case ARKFB_SET_COLOR_KEY:
		{
			struct arkfb_color_key input_arg;
			
			copy_from_user(&input_arg, (void*)arg, sizeof(input_arg));
			return ark_fb_set_color_key(disp, &input_arg);
	    }
	case ARKFB_SET_BLEND:
		{
			struct arkfb_blend input_arg;
			
			copy_from_user(&input_arg, (void*)arg, sizeof(input_arg));
			return ark_fb_set_blend(disp, &input_arg);			
		}
	case ARKFB_SET_WINDOW_ADDR:
		{
			struct arkfb_window_addr input_arg;
			
			copy_from_user(&input_arg, (void*)arg, sizeof(input_arg));
			return ark_fb_set_window_addr(disp, &input_arg);			
		}
	case ARKFB_SHOW_WINDOW:
		{
			ark_fb_show_window(disp);
		}
		break;
	case ARKFB_HIDE_WINDOW:
		{
			ark_fb_hide_window(disp);
		}
		break;
    default:
        ARKDISP_DBGPRTK("%s %d: unknown ioctl %08x\n",
            __FUNCTION__, __LINE__, cmd);
        return -EINVAL;
    }
    return 0;
}

static struct fb_ops ark_disp_fp_ops = {
    .owner          = THIS_MODULE,
    .fb_read        = fb_sys_read,
    .fb_write       = fb_sys_write,
    //.fb_fillrect    = sys_fillrect,
    .fb_fillrect    = cfb_fillrect,
    // function draws a rectangle on the screen (use generic one here)
    /*	Meaning of struct fb_fillrect
     *
     *	@dx: The x and y corrdinates of the upper left hand corner of the
     *	@dy: area we want to draw to.
     *	@width: How wide the rectangle is we want to draw.
     *	@height: How tall the rectangle is we want to draw.
     *	@color:	The color to fill in the rectangle with.
     *	@rop: The raster operation. We can draw the rectangle with a COPY
     *	      of XOR which provides erasing effect.
     */

    //.fb_copyarea    = ec_dcu_fb_copy_area,//sys_copyarea,
    //.fb_copyarea    = sys_copyarea,
    .fb_copyarea    = cfb_copyarea,
    // function copies one area of the screen to another area (use generic one here)
    /*  Meaning of struct fb_copyarea
     *
     *  @dx: The x and y coordinates of the upper left hand corner of the
     *  @dy: destination area on the screen.
     *  @width: How wide the rectangle is we want to copy.
     *  @height: How tall the rectangle is we want to copy.
     *  @sx: The x and y coordinates of the upper left hand corner of the
     *  @sy: source area on the screen.
     */

    //.fb_imageblit   = sys_imageblit,
    .fb_imageblit   = cfb_imageblit,
    // function copies a image from system memory to the screen (use generic one here)
    /*  Meaning of struct fb_image *image
     *
     *  @dx: The x and y coordinates of the upper left hand corner of the
     *  @dy: destination area to place the image on the screen.
     *  @width: How wide the image is we want to copy.
     *  @height: How tall the image is we want to copy.
     *  @fg_color: For mono bitmap images this is color data for
     *  @bg_color: the foreground and background of the image to
     *	           write directly to the framebuffer.
     *  @depth:	How many bits represent a single pixel for this image.
     *  @data: The actual data used to construct the image on the display.
     *  @cmap: The colormap used for color images.
     */

    .fb_setcolreg   = ark_disp_fb_setcolreg,
    .fb_pan_display = ark_disp_fb_pan_display,
    .fb_check_var   = ark_disp_fb_check_var,
    .fb_set_par     = ark_disp_fb_set_par,
    .fb_ioctl       = ark_disp_fb_ioctl,
    .fb_sync        = NULL,  // no need
    .fb_cursor      = NULL,  // not supported
    .fb_rotate	    = NULL,  // not supported
//  .fb_blank       = ec_dcu_fb_blank,
//  .fb_set_cmap	= ec_dcu_fb_set_cmap, // no need, as ec_dcu_fb_setcolreg is implemented
};


int ark_disp_sync_other_cfg_to_fb_info(int layer_id,
    struct ark_disp_context *context)
{
    struct ark_disp_layer_info *layer;
    struct ark_disp_buf_info *buf;
    struct ark_disp_fb_info  *fbd;

    if (context)
        fb_disp_context = context;

    if (!fb_disp_context) {
        ARKDISP_DBGPRTK("%s %d: null context ptr\n", __FUNCTION__, __LINE__);
        return -EINVAL;
    }

    layer = &fb_disp_context->disp_obj[layer_id].layer_info;
    buf   = &fb_disp_context->disp_obj[layer_id].buf_info;
    fbd   = &fb_disp_context->disp_obj[layer_id].fbd_info;
        
    fbd->fb_info.fix.smem_start = buf->cur_buf_addr_phys; 
                                    // physical address of frame buffer
    fbd->fb_info.fix.smem_len =
        layer->src_xres * layer->src_yres * layer->bits_per_pixel *
        buf->buf_num / 8;
    fbd->fb_info.fix.ypanstep = layer->src_yres;
                            // unit (step) for y-direction panning
    // for implementation of display pan using ark_disp_fb_pan_display
    fbd->fb_info.fix.ywrapstep = 0; // no hw ywrap
    fbd->fb_info.fix.line_length = layer->line_length; //in bytes

//chitest arm linux does not support ioremap memory operaitons
#if 0
    iounmap(fbd->fb_info.screen_base);
    fbd->fb_info.screen_base
        = ioremap(fbd->fb_info.fix.smem_start, fbd->fb_info.fix.smem_len);
#else
    ARKDISP_DBGPRTK("%s %d: smem_start=%p cur_buf_addr_phys=%p cur_buf_addr_virt=%p\n",
        __FUNCTION__, __LINE__, fbd->fb_info.fix.smem_start,
        buf->cur_buf_addr_phys, buf->cur_buf_addr_virt);
    fbd->fb_info.screen_base = buf->cur_buf_addr_virt;
#endif     
                                      // memory mapped frame buffer
    if (fbd->fb_info.var.xres != layer->src_xres ||
        fbd->fb_info.var.yres != layer->src_yres ||
        fbd->fb_info.var.bits_per_pixel != layer->bits_per_pixel) {
        fbd->fb_info.var.xoffset = 0; // reset offset when fmem size changes
        fbd->fb_info.var.yoffset = 0;
    }

    fbd->fb_info.var.xres = layer->src_xres;
    fbd->fb_info.var.yres = layer->src_yres;
    fbd->fb_info.var.xres_virtual = layer->src_xres;
    fbd->fb_info.var.yres_virtual = layer->src_yres * buf->buf_num;

    fbd->fb_info.var.bits_per_pixel = layer->bits_per_pixel;
    fbd->fb_info.var.grayscale = 0; // use colors

    fbd->fb_info.var.nonstd = 0;
    switch (layer->pixfmt)
    {
    case ARKDISP_PIXFMT_RGBI555:
        fbd->fb_info.var.transp.offset    = 0;
        fbd->fb_info.var.transp.length    = 0;
        fbd->fb_info.var.transp.msb_right = 0;
        fbd->fb_info.var.red.offset       = 0;
        fbd->fb_info.var.red.length       = 5;
        fbd->fb_info.var.red.msb_right    = 0;
        fbd->fb_info.var.green.offset     = 5;
        fbd->fb_info.var.green.length     = 5;
        fbd->fb_info.var.green.msb_right  = 0;
        fbd->fb_info.var.blue.offset      = 10;
        fbd->fb_info.var.blue.length      = 5;
        fbd->fb_info.var.blue.msb_right   = 0;
        break;
    case ARKDISP_PIXFMT_RGB565:
        switch (layer->rgb_order)
        {
        case ARKDISP_RGB_ORDER_RGB:
            fbd->fb_info.var.transp.offset    = 0;
            fbd->fb_info.var.transp.length    = 0;
            fbd->fb_info.var.transp.msb_right = 0;
            fbd->fb_info.var.red.offset       = 11;
            fbd->fb_info.var.red.length       = 5;
            fbd->fb_info.var.red.msb_right    = 0;
            fbd->fb_info.var.green.offset     = 5;
            fbd->fb_info.var.green.length     = 6;
            fbd->fb_info.var.green.msb_right  = 0;
            fbd->fb_info.var.blue.offset      = 0;
            fbd->fb_info.var.blue.length      = 5;
            fbd->fb_info.var.blue.msb_right   = 0;
            break;
        case ARKDISP_RGB_ORDER_RBG:
            fbd->fb_info.var.transp.offset    = 0;
            fbd->fb_info.var.transp.length    = 0;
            fbd->fb_info.var.transp.msb_right = 0;
            fbd->fb_info.var.red.offset       = 11;
            fbd->fb_info.var.red.length       = 5;
            fbd->fb_info.var.red.msb_right    = 0;
            fbd->fb_info.var.green.offset     = 0;
            fbd->fb_info.var.green.length     = 6;
            fbd->fb_info.var.green.msb_right  = 0;
            fbd->fb_info.var.blue.offset      = 6;
            fbd->fb_info.var.blue.length      = 5;
            fbd->fb_info.var.blue.msb_right   = 0;
            break;
        case ARKDISP_RGB_ORDER_GRB:
            fbd->fb_info.var.transp.offset    = 0;
            fbd->fb_info.var.transp.length    = 0;
            fbd->fb_info.var.transp.msb_right = 0;
            fbd->fb_info.var.red.offset       = 5;
            fbd->fb_info.var.red.length       = 5;
            fbd->fb_info.var.red.msb_right    = 0;
            fbd->fb_info.var.green.offset     = 10;
            fbd->fb_info.var.green.length     = 6;
            fbd->fb_info.var.green.msb_right  = 0;
            fbd->fb_info.var.blue.offset      = 0;
            fbd->fb_info.var.blue.length      = 5;
            fbd->fb_info.var.blue.msb_right   = 0;
            break;
        case ARKDISP_RGB_ORDER_GBR:
            fbd->fb_info.var.transp.offset    = 0;
            fbd->fb_info.var.transp.length    = 0;
            fbd->fb_info.var.transp.msb_right = 0;
            fbd->fb_info.var.red.offset       = 0;
            fbd->fb_info.var.red.length       = 5;
            fbd->fb_info.var.red.msb_right    = 0;
            fbd->fb_info.var.green.offset     = 10;
            fbd->fb_info.var.green.length     = 6;
            fbd->fb_info.var.green.msb_right  = 0;
            fbd->fb_info.var.blue.offset      = 5;
            fbd->fb_info.var.blue.length      = 5;
            fbd->fb_info.var.blue.msb_right   = 0;
            break;
        case ARKDISP_RGB_ORDER_BRG:
            fbd->fb_info.var.transp.offset    = 0;
            fbd->fb_info.var.transp.length    = 0;
            fbd->fb_info.var.transp.msb_right = 0;
            fbd->fb_info.var.red.offset       = 6;
            fbd->fb_info.var.red.length       = 5;
            fbd->fb_info.var.red.msb_right    = 0;
            fbd->fb_info.var.green.offset     = 0;
            fbd->fb_info.var.green.length     = 6;
            fbd->fb_info.var.green.msb_right  = 0;
            fbd->fb_info.var.blue.offset      = 11;
            fbd->fb_info.var.blue.length      = 5;
            fbd->fb_info.var.blue.msb_right   = 0;
            break;
        case ARKDISP_RGB_ORDER_BGR:
        default:
            fbd->fb_info.var.transp.offset    = 0;
            fbd->fb_info.var.transp.length    = 0;
            fbd->fb_info.var.transp.msb_right = 0;
            fbd->fb_info.var.red.offset       = 0;
            fbd->fb_info.var.red.length       = 5;
            fbd->fb_info.var.red.msb_right    = 0;
            fbd->fb_info.var.green.offset     = 5;
            fbd->fb_info.var.green.length     = 6;
            fbd->fb_info.var.green.msb_right  = 0;
            fbd->fb_info.var.blue.offset      = 11;
            fbd->fb_info.var.blue.length      = 5;
            fbd->fb_info.var.blue.msb_right   = 0;
            break;
        }
        break;
    case ARKDISP_PIXFMT_RGBA888:
        switch (layer->rgb_order)
        {
        case ARKDISP_RGB_ORDER_RGB:
            fbd->fb_info.var.transp.offset    = 24;
            fbd->fb_info.var.transp.length    = 8;
            fbd->fb_info.var.transp.msb_right = 0;
            fbd->fb_info.var.red.offset       = 16;
            fbd->fb_info.var.red.length       = 8;
            fbd->fb_info.var.red.msb_right    = 0;
            fbd->fb_info.var.green.offset     = 8;
            fbd->fb_info.var.green.length     = 8;
            fbd->fb_info.var.green.msb_right  = 0;
            fbd->fb_info.var.blue.offset      = 0;
            fbd->fb_info.var.blue.length      = 8;
            fbd->fb_info.var.blue.msb_right   = 0;
            break;
        case ARKDISP_RGB_ORDER_RBG:
            fbd->fb_info.var.transp.offset    = 24;
            fbd->fb_info.var.transp.length    = 8;
            fbd->fb_info.var.transp.msb_right = 0;
            fbd->fb_info.var.red.offset       = 16;
            fbd->fb_info.var.red.length       = 8;
            fbd->fb_info.var.red.msb_right    = 0;
            fbd->fb_info.var.green.offset     = 0;
            fbd->fb_info.var.green.length     = 8;
            fbd->fb_info.var.green.msb_right  = 0;
            fbd->fb_info.var.blue.offset      = 8;
            fbd->fb_info.var.blue.length      = 8;
            fbd->fb_info.var.blue.msb_right   = 0;
            break;
        case ARKDISP_RGB_ORDER_GRB:
            fbd->fb_info.var.transp.offset    = 24;
            fbd->fb_info.var.transp.length    = 8;
            fbd->fb_info.var.transp.msb_right = 0;
            fbd->fb_info.var.red.offset       = 8;
            fbd->fb_info.var.red.length       = 8;
            fbd->fb_info.var.red.msb_right    = 0;
            fbd->fb_info.var.green.offset     = 16;
            fbd->fb_info.var.green.length     = 8;
            fbd->fb_info.var.green.msb_right  = 0;
            fbd->fb_info.var.blue.offset      = 0;
            fbd->fb_info.var.blue.length      = 8;
            fbd->fb_info.var.blue.msb_right   = 0;
            break;
        case ARKDISP_RGB_ORDER_GBR:
            fbd->fb_info.var.transp.offset    = 24;
            fbd->fb_info.var.transp.length    = 8;
            fbd->fb_info.var.transp.msb_right = 0;
            fbd->fb_info.var.red.offset       = 0;
            fbd->fb_info.var.red.length       = 8;
            fbd->fb_info.var.red.msb_right    = 0;
            fbd->fb_info.var.green.offset     = 16;
            fbd->fb_info.var.green.length     = 8;
            fbd->fb_info.var.green.msb_right  = 0;
            fbd->fb_info.var.blue.offset      = 8;
            fbd->fb_info.var.blue.length      = 8;
            fbd->fb_info.var.blue.msb_right   = 0;
            break;
        case ARKDISP_RGB_ORDER_BRG:
            fbd->fb_info.var.transp.offset    = 24;
            fbd->fb_info.var.transp.length    = 8;
            fbd->fb_info.var.transp.msb_right = 0;
            fbd->fb_info.var.red.offset       = 8;
            fbd->fb_info.var.red.length       = 8;
            fbd->fb_info.var.red.msb_right    = 0;
            fbd->fb_info.var.green.offset     = 0;
            fbd->fb_info.var.green.length     = 8;
            fbd->fb_info.var.green.msb_right  = 0;
            fbd->fb_info.var.blue.offset      = 16;
            fbd->fb_info.var.blue.length      = 8;
            fbd->fb_info.var.blue.msb_right   = 0;
            break;
        case ARKDISP_RGB_ORDER_BGR:
        default:
            fbd->fb_info.var.transp.offset    = 24;
            fbd->fb_info.var.transp.length    = 8;
            fbd->fb_info.var.transp.msb_right = 0;
            fbd->fb_info.var.red.offset       = 0;
            fbd->fb_info.var.red.length       = 8;
            fbd->fb_info.var.red.msb_right    = 0;
            fbd->fb_info.var.green.offset     = 8;
            fbd->fb_info.var.green.length     = 8;
            fbd->fb_info.var.green.msb_right  = 0;
            fbd->fb_info.var.blue.offset      = 16;
            fbd->fb_info.var.blue.length      = 8;
            fbd->fb_info.var.blue.msb_right   = 0;
            break;
        }
        break;
    case ARKDISP_PIXFMT_RGB888:
        switch (layer->rgb_order)
        {
        case ARKDISP_RGB_ORDER_RGB:
            fbd->fb_info.var.transp.offset    = 0;
            fbd->fb_info.var.transp.length    = 0;
            fbd->fb_info.var.transp.msb_right = 0;
            fbd->fb_info.var.red.offset       = 16;
            fbd->fb_info.var.red.length       = 8;
            fbd->fb_info.var.red.msb_right    = 0;
            fbd->fb_info.var.green.offset     = 8;
            fbd->fb_info.var.green.length     = 8;
            fbd->fb_info.var.green.msb_right  = 0;
            fbd->fb_info.var.blue.offset      = 0;
            fbd->fb_info.var.blue.length      = 8;
            fbd->fb_info.var.blue.msb_right   = 0;
            break;
        case ARKDISP_RGB_ORDER_RBG:
            fbd->fb_info.var.transp.offset    = 0;
            fbd->fb_info.var.transp.length    = 0;
            fbd->fb_info.var.transp.msb_right = 0;
            fbd->fb_info.var.red.offset       = 16;
            fbd->fb_info.var.red.length       = 8;
            fbd->fb_info.var.red.msb_right    = 0;
            fbd->fb_info.var.green.offset     = 0;
            fbd->fb_info.var.green.length     = 8;
            fbd->fb_info.var.green.msb_right  = 0;
            fbd->fb_info.var.blue.offset      = 8;
            fbd->fb_info.var.blue.length      = 8;
            fbd->fb_info.var.blue.msb_right   = 0;
            break;
        case ARKDISP_RGB_ORDER_GRB:
            fbd->fb_info.var.transp.offset    = 0;
            fbd->fb_info.var.transp.length    = 0;
            fbd->fb_info.var.transp.msb_right = 0;
            fbd->fb_info.var.red.offset       = 8;
            fbd->fb_info.var.red.length       = 8;
            fbd->fb_info.var.red.msb_right    = 0;
            fbd->fb_info.var.green.offset     = 16;
            fbd->fb_info.var.green.length     = 8;
            fbd->fb_info.var.green.msb_right  = 0;
            fbd->fb_info.var.blue.offset      = 0;
            fbd->fb_info.var.blue.length      = 8;
            fbd->fb_info.var.blue.msb_right   = 0;
            break;
        case ARKDISP_RGB_ORDER_GBR:
            fbd->fb_info.var.transp.offset    = 0;
            fbd->fb_info.var.transp.length    = 0;
            fbd->fb_info.var.transp.msb_right = 0;
            fbd->fb_info.var.red.offset       = 0;
            fbd->fb_info.var.red.length       = 8;
            fbd->fb_info.var.red.msb_right    = 0;
            fbd->fb_info.var.green.offset     = 16;
            fbd->fb_info.var.green.length     = 8;
            fbd->fb_info.var.green.msb_right  = 0;
            fbd->fb_info.var.blue.offset      = 8;
            fbd->fb_info.var.blue.length      = 8;
            fbd->fb_info.var.blue.msb_right   = 0;
            break;
        case ARKDISP_RGB_ORDER_BRG:
            fbd->fb_info.var.transp.offset    = 0;
            fbd->fb_info.var.transp.length    = 0;
            fbd->fb_info.var.transp.msb_right = 0;
            fbd->fb_info.var.red.offset       = 8;
            fbd->fb_info.var.red.length       = 8;
            fbd->fb_info.var.red.msb_right    = 0;
            fbd->fb_info.var.green.offset     = 0;
            fbd->fb_info.var.green.length     = 8;
            fbd->fb_info.var.green.msb_right  = 0;
            fbd->fb_info.var.blue.offset      = 16;
            fbd->fb_info.var.blue.length      = 8;
            fbd->fb_info.var.blue.msb_right   = 0;
            break;
        case ARKDISP_RGB_ORDER_BGR:
        default:
            fbd->fb_info.var.transp.offset    = 0;
            fbd->fb_info.var.transp.length    = 0;
            fbd->fb_info.var.transp.msb_right = 0;
            fbd->fb_info.var.red.offset       = 0;
            fbd->fb_info.var.red.length       = 8;
            fbd->fb_info.var.red.msb_right    = 0;
            fbd->fb_info.var.green.offset     = 8;
            fbd->fb_info.var.green.length     = 8;
            fbd->fb_info.var.green.msb_right  = 0;
            fbd->fb_info.var.blue.offset      = 16;
            fbd->fb_info.var.blue.length      = 8;
            fbd->fb_info.var.blue.msb_right   = 0;
            break;
        }
        break;
    case ARKDISP_PIXFMT_RGBA1555:
        fbd->fb_info.var.transp.offset    = 0;
        fbd->fb_info.var.transp.length    = 1;
        fbd->fb_info.var.transp.msb_right = 0;
        fbd->fb_info.var.red.offset       = 1;
        fbd->fb_info.var.red.length       = 5;
        fbd->fb_info.var.red.msb_right    = 0;
        fbd->fb_info.var.green.offset     = 6;
        fbd->fb_info.var.green.length     = 5;
        fbd->fb_info.var.green.msb_right  = 0;
        fbd->fb_info.var.blue.offset      = 11;
        fbd->fb_info.var.blue.length      = 5;
        fbd->fb_info.var.blue.msb_right   = 0;
        break;
    case ARKDISP_PIXFMT_RGBA1888:
        fbd->fb_info.var.transp.offset    = 0;
        fbd->fb_info.var.transp.length    = 1;
        fbd->fb_info.var.transp.msb_right = 0;
        fbd->fb_info.var.red.offset       = 1;
        fbd->fb_info.var.red.length       = 8;
        fbd->fb_info.var.red.msb_right    = 0;
        fbd->fb_info.var.green.offset     = 9;
        fbd->fb_info.var.green.length     = 8;
        fbd->fb_info.var.green.msb_right  = 0;
        fbd->fb_info.var.blue.offset      = 17;
        fbd->fb_info.var.blue.length      = 8;
        fbd->fb_info.var.blue.msb_right   = 0;
        break;
    case ARKDISP_PIXFMT_RGBA4888:
        fbd->fb_info.var.transp.offset    = 0;
        fbd->fb_info.var.transp.length    = 4;
        fbd->fb_info.var.transp.msb_right = 0;
        fbd->fb_info.var.red.offset       = 4;
        fbd->fb_info.var.red.length       = 8;
        fbd->fb_info.var.red.msb_right    = 0;
        fbd->fb_info.var.green.offset     = 12;
        fbd->fb_info.var.green.length     = 8;
        fbd->fb_info.var.green.msb_right  = 0;
        fbd->fb_info.var.blue.offset      = 20;
        fbd->fb_info.var.blue.length      = 8;
        fbd->fb_info.var.blue.msb_right   = 0;
        break;
    case ARKDISP_PIXFMT_RGB666:
        fbd->fb_info.var.transp.offset    = 0;
        fbd->fb_info.var.transp.length    = 0;
        fbd->fb_info.var.transp.msb_right = 0;
        fbd->fb_info.var.red.offset       = 0;
        fbd->fb_info.var.red.length       = 6;
        fbd->fb_info.var.red.msb_right    = 0;
        fbd->fb_info.var.green.offset     = 6;
        fbd->fb_info.var.green.length     = 6;
        fbd->fb_info.var.green.msb_right  = 0;
        fbd->fb_info.var.blue.offset      = 12;
        fbd->fb_info.var.blue.length      = 6;
        fbd->fb_info.var.blue.msb_right   = 0;
        break;
    case ARKDISP_PIXFMT_RGBA1666:
        fbd->fb_info.var.transp.offset    = 0;
        fbd->fb_info.var.transp.length    = 1;
        fbd->fb_info.var.transp.msb_right = 0;
        fbd->fb_info.var.red.offset       = 1;
        fbd->fb_info.var.red.length       = 6;
        fbd->fb_info.var.red.msb_right    = 0;
        fbd->fb_info.var.green.offset     = 7;
        fbd->fb_info.var.green.length     = 6;
        fbd->fb_info.var.green.msb_right  = 0;
        fbd->fb_info.var.blue.offset      = 13;
        fbd->fb_info.var.blue.length      = 6;
        fbd->fb_info.var.blue.msb_right   = 0;
        break;
    default:
        fbd->fb_info.var.nonstd = 1; // not a std pixel format
        break;        
    }

    fbd->fb_info.var.height = layer->yres; //-1;//0;
    fbd->fb_info.var.width  = layer->xres; //-1;//0;

    return 0;
}

/* This function initialilzes frame buffer control data structures
 *
 * Arguments:
 *   context : pointer to the driver control context
 *
 * Return:
 *   return 0 if success, else return -1
 */
int ark_disp_fb_init(struct ark_disp_context *context)
{
    struct ark_disp_fb_info  *fbd;
    struct ark_disp_buf_info *buf;
    struct ark_disp_layer_info *layer;
    char id[16];
    int i;

    ARKDISP_DBGPRTK("%s %d: enter\n", __FUNCTION__, __LINE__);

    fb_disp_context = context;

    for (i = 0; i < ARK_DISP_FB_DEV_NUM; i++) {
        fbd = &context->disp_obj[i].fbd_info;
        buf = &context->disp_obj[i].buf_info;
        layer = &context->disp_obj[i].layer_info;

        ARKDISP_DBGPRTK("%s %d: i=%d\n", __FUNCTION__, __LINE__, i);

        fbd->fb_info.node  = -1;
        fbd->fb_info.flags = FBINFO_DEFAULT | FBINFO_HWACCEL_NONE;

        /* set fix screen info */
        sprintf(id, "arkfb%d", i);
        strcpy(fbd->fb_info.fix.id, id);
        fbd->fb_info.fix.smem_start = buf->cur_buf_addr_phys; 
                                        // physical address of frame buffer
        fbd->fb_info.fix.smem_len = buf->buf_size * buf->buf_num;

        fbd->fb_info.fix.type = FB_TYPE_PACKED_PIXELS;
                       // for direct mapping from video memory to pixel value

        fbd->fb_info.fix.type_aux = 0;

        fbd->fb_info.fix.visual = FB_VISUAL_TRUECOLOR;
        // truecolor has hardware independent palette
        fbd->fb_info.fix.xpanstep = 0; // no hw panning
        if (buf->buf_num)
            fbd->fb_info.fix.ypanstep = layer->src_yres;
        else
            fbd->fb_info.fix.ypanstep = 0;
                                // unit (step) for y-direction panning
        // for implementation of display pan using ark_disp_fb_pan_display
        fbd->fb_info.fix.ywrapstep = 0; // no hw ywrap
        fbd->fb_info.fix.line_length = layer->line_length; //in bytes
        fbd->fb_info.fix.mmio_start = LCD_BASE;
                          // for other func to do mapped io (physical address)
        // e.g. ioremap(fix.mmio_start, fix.mmio_len)
        fbd->fb_info.fix.mmio_len = 0x800;
        fbd->fb_info.fix.accel = FB_ACCEL_NONE;

        ARKDISP_DBGPRTK("%s %d: screen_base=%p\n", __FUNCTION__, __LINE__, fbd->fb_info.screen_base);
        ARKDISP_DBGPRTK("%s %d: smem_start=%p smem_len=%d\n",
            __FUNCTION__, __LINE__, fbd->fb_info.fix.smem_start, fbd->fb_info.fix.smem_len);

//chitest arm linux does not support memory ioremap operations
#if 0
        fbd->fb_info.screen_base
            = ioremap(fbd->fb_info.fix.smem_start, fbd->fb_info.fix.smem_len);
#else
         ARKDISP_DBGPRTK("%s %d: smem_start=%p cur_buf_addr_phys=%p cur_buf_addr_virt=%p\n",
            __FUNCTION__, __LINE__, fbd->fb_info.fix.smem_start,
            buf->cur_buf_addr_phys, buf->cur_buf_addr_virt);
         fbd->fb_info.screen_base = buf->cur_buf_addr_virt;
#endif
                                          // memory mapped frame buffer
        ARKDISP_DBGPRTK("%s %d: screen_base=%p\n", __FUNCTION__, __LINE__, fbd->fb_info.screen_base);

        /* NOTE: some functions like cfb_copyarea directly use this info to
         * access the frame buffer
         */
        fbd->fb_info.screen_size = 0; 
                    // 0: the system will use smem_len instead

        fbd->fb_info.fbops = &ark_disp_fp_ops;

        fbd->fb_info.pseudo_palette = fbd->pseudo_palette;
        // this data structure will be set by ec_dcu_fb_setcolreg & 
        // fb_set_cmap, it will be called by xxx_imageblit, xxx_fillrect

        /* provide the associated dislay object info */
        fbd->fb_info.par = (void *)&context->disp_obj[i];

        /* set variable screen info */
        fbd->fb_info.var.xres = layer->src_xres;
        fbd->fb_info.var.yres = layer->src_yres;
        fbd->fb_info.var.xres_virtual = layer->src_xres;
        fbd->fb_info.var.yres_virtual = layer->src_yres * buf->buf_num;
        fbd->fb_info.var.xoffset = 0;
        fbd->fb_info.var.yoffset = 0;
        fbd->fb_info.var.bits_per_pixel = layer->bits_per_pixel;
        fbd->fb_info.var.grayscale = 0; // use colors

        fbd->fb_info.var.transp.offset    = 24; // RGBa888 in 32bpp
        fbd->fb_info.var.transp.length    = 8;
        fbd->fb_info.var.transp.msb_right = 0;
        fbd->fb_info.var.red.offset       = 16;
        fbd->fb_info.var.red.length       = 8;
        fbd->fb_info.var.red.msb_right    = 0;
        fbd->fb_info.var.green.offset     = 8;
        fbd->fb_info.var.green.length     = 8;
        fbd->fb_info.var.green.msb_right  = 0;
        fbd->fb_info.var.blue.offset      = 0;
        fbd->fb_info.var.blue.length      = 8;
        fbd->fb_info.var.blue.msb_right   = 0;

        fbd->fb_info.var.nonstd = 0; // std pixel format
        fbd->fb_info.var.activate = FB_ACTIVATE_NOW;
        fbd->fb_info.var.height = layer->yres; //-1;//0;
        fbd->fb_info.var.width  = layer->xres; //-1;//0;
        fbd->fb_info.var.accel_flags = 0;
        fbd->fb_info.var.vmode = FB_VMODE_NONINTERLACED;

        /* This has to been done !!! */
        if (fb_alloc_cmap(&fbd->fb_info.cmap, 256, 0) < 0) {
            ARKDISP_DBGPRTK("%s %d: fb_alloc_cmap err\n",
                __FUNCTION__, __LINE__);

            while (i >= 0)
                iounmap(context->disp_obj[i--].fbd_info.fb_info.screen_base);

            return -EINVAL;
        }

        if (register_framebuffer(&fbd->fb_info) < 0) {
            ARKDISP_DBGPRTK("%s %d: register_framebuffer err\n",
                __FUNCTION__, __LINE__);

            iounmap(fbd->fb_info.screen_base);

            while (i >= 0) {
                iounmap(context->disp_obj[i].fbd_info.fb_info.screen_base);
                fb_dealloc_cmap(&context->disp_obj[i].fbd_info.fb_info.cmap);
                i--;
            }
            return -EINVAL;
        }

        ARKDISP_DBGPRTK("%s %d: fb%d: %s init done\n",
            __FUNCTION__, __LINE__, fbd->fb_info.node, fbd->fb_info.fix.id);
    }
    return 0;
}



