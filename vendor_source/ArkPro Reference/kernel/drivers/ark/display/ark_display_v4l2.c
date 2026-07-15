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
 *      ark_display_v4l2.c
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
#include <asm/atomic.h>
#include <linux/interrupt.h>
#include <linux/wait.h>
#include <linux/fb.h>
#include <linux/matroxfb.h>

#include <linux/mm.h>
#include <linux/vmalloc.h>
#include <linux/dma-mapping.h>

/* header files for V4L2 Linux kernel interface */
#include <media/v4l2-dev.h>
#include <linux/videodev2.h>
#include <media/videobuf-core.h>
#include <media/v4l2-common.h>
#include <media/v4l2-ioctl.h>
#include <media/videobuf-dma-contig.h>
#include <mach/va_map.h>
#include <linux/ark/ringpair.h>
#include <linux/ark/ark_display_io.h>
#include <mach/hardware.h>

#include "ark_display.h"
#include "ark_display_priv.h"
#include "ark_lcd_regs.h"
#include "ark_sys_regs.h"


/* uncomment the line below for new v4l2 API */
#define ARK_DISP_NEW_V4L2_API


static struct v4l2_queryctrl ark_disp_qctrl[] = {
    {
        .id            = V4L2_CID_BRIGHTNESS,
        .name          = "Brightness",
        .minimum       = 0,
        .maximum       = 0xFF, // 16-bit
        .default_value = ARK_DISP_VDE_BRIGHTNESS_DEFAULT,
        .type          = V4L2_CTRL_TYPE_INTEGER,
    },
    {
        .id            = V4L2_CID_CONTRAST,
        .name          = "Contrast",
        .minimum       = 0,
        .maximum       = 0xFF, // 16-bit
        .default_value = ARK_DISP_VDE_CONTRAST_DEFAULT,
        .type          = V4L2_CTRL_TYPE_INTEGER,
    },
    {
        .id            = V4L2_CID_SATURATION,
        .name          = "Saturation",
        .minimum       = 0,
        .maximum       = 0xFF, // 16-bit
        .default_value = ARK_DISP_VDE_SATURATION_DEFAULT,
        .type          = V4L2_CTRL_TYPE_INTEGER,
    },
    {
        .id            = V4L2_CID_HUE,
        .name          = "Hue",
        .minimum       = 0,
        .maximum       = 0xFF, // 16-bit
        .default_value = ARK_DISP_VDE_HUE_DEFAULT,
        .type          = V4L2_CTRL_TYPE_INTEGER,
    },

    /* HINT: the disabled controls are only here to satisfy requirements of
     * some applications
     */
    {
        .id             = V4L2_CID_BLACK_LEVEL,
        .name           = "Black level",
        .flags          = V4L2_CTRL_FLAG_DISABLED,
    },
    {
        .id             = V4L2_CID_AUTO_WHITE_BALANCE,
        .name           = "Auto white balance",
        .flags          = V4L2_CTRL_FLAG_DISABLED,
    },
    {
        .id             = V4L2_CID_DO_WHITE_BALANCE,
        .name           = "Do white balance",
        .flags          = V4L2_CTRL_FLAG_DISABLED,
    },
    {
        .id             = V4L2_CID_RED_BALANCE,
        .name           = "Red balance",
        .flags          = V4L2_CTRL_FLAG_DISABLED,
    },
    {
        .id             = V4L2_CID_BLUE_BALANCE,
        .name           = "Blue balance",
        .flags          = V4L2_CTRL_FLAG_DISABLED,
    },
    {
        .id             = V4L2_CID_GAMMA,
        .name           = "Gamma",
        .flags          = V4L2_CTRL_FLAG_DISABLED,
    },
    {
        .id             = V4L2_CID_WHITENESS,
        .name           = "Whiteness",
        .flags          = V4L2_CTRL_FLAG_DISABLED,
    },
    {
        .id             = V4L2_CID_EXPOSURE,
        .name           = "Exposure",
        .flags          = V4L2_CTRL_FLAG_DISABLED,
    },
    {
        .id             = V4L2_CID_AUTOGAIN,
        .name           = "Autogain",
        .flags          = V4L2_CTRL_FLAG_DISABLED,
    },
    {
        .id             = V4L2_CID_GAIN,
        .name           = "Gain",
        .flags          = V4L2_CTRL_FLAG_DISABLED,
    },
    {
        .id             = V4L2_CID_HFLIP,
        .name           = "Horizontal flip",
        .flags          = V4L2_CTRL_FLAG_DISABLED,
    },        
    {
        .id             = V4L2_CID_VFLIP,
        .name           = "Vertical flip",
        .flags          = V4L2_CTRL_FLAG_DISABLED,
    },
    {
        .id             = V4L2_CID_HUE_AUTO,
        .name           = "Hue auto",
        .flags          = V4L2_CTRL_FLAG_DISABLED,
    },
    {
        .id             = V4L2_CID_WHITE_BALANCE_TEMPERATURE,
        .name           = "White balance temperature",
        .flags          = V4L2_CTRL_FLAG_DISABLED,
    },
    {
        .id             = V4L2_CID_SHARPNESS,
        .name           = "Sharpness",
        .flags          = V4L2_CTRL_FLAG_DISABLED,
    },
    {
        .id             = V4L2_CID_BACKLIGHT_COMPENSATION,
        .name           = "Backlight compensation",
        .flags          = V4L2_CTRL_FLAG_DISABLED,
    },
    {
        .id             = V4L2_CID_CHROMA_AGC,
        .name           = "Chroma AGC",
        .flags          = V4L2_CTRL_FLAG_DISABLED,
    },
    {
        .id             = V4L2_CID_COLOR_KILLER,
        .name           = "Color killer",
        .flags          = V4L2_CTRL_FLAG_DISABLED,
    },
};

static struct ark_disp_context *v4l2_disp_context;


#define ARK_DISP_PIXELASPECT_NTSC 		{11, 10}
#define ARK_DISP_PIXELASPECT_PAL  		{54, 59}
#define ARK_DISP_PIXELASPECT_SP    	    {1, 1}

/* settings for commonly used video formats */
#define ARK_DISP_WIN_NTSC     {0, 0, 720, 480}
#define ARK_DISP_WIN_PAL      {0, 0, 720, 576}
#define ARK_DISP_WIN_640_480  {0, 0, 640, 480}
#define ARK_DISP_WIN_640_400  {0, 0, 640, 400}
#define ARK_DISP_WIN_640_350  {0, 0, 640, 350}
#define ARK_DISP_WIN_720P     {0, 0, 1280, 720}
#define ARK_DISP_WIN_1080I    {0, 0, 1920, 1080}

/* uncomment the line below to always set cropcap equal to screen size */
//#define ARKDISP_SET_CROPCAP_INFO_EQUAL_TO_SCREEN_SIZE

#ifdef ARKDISP_SET_CROPCAP_INFO_EQUAL_TO_SCREEN_SIZE
static struct v4l2_fract ntsc_aspect = ARK_DISP_PIXELASPECT_NTSC;
static struct v4l2_fract pal_aspect  = ARK_DISP_PIXELASPECT_PAL;
static struct v4l2_fract sp_aspect   = ARK_DISP_PIXELASPECT_SP;

static struct v4l2_rect ntsc_bounds     = ARK_DISP_WIN_NTSC;
static struct v4l2_rect pal_bounds      = ARK_DISP_WIN_PAL;
static struct v4l2_rect vga_bounds      = ARK_DISP_WIN_640_480;
static struct v4l2_rect hd_720p_bounds  = ARK_DISP_WIN_720P;
static struct v4l2_rect hd_1080i_bounds = ARK_DISP_WIN_1080I;
#endif


struct videobuf_dma_contig_memory {
	u32 magic;
	void *vaddr;
	dma_addr_t dma_handle;
	unsigned long size;
};

#define MAGIC_DC_MEM 0x0733ac61
#define MAGIC_CHECK(is, should)						    \
	if (unlikely((is) != (should)))	{				    \
		pr_err("magic mismatch: %x expected %x\n", (is), (should)); \
		BUG();							    \
	}

dma_addr_t ark_disp_videobuf_to_dma_contig(struct videobuf_buffer *buf)
{
    struct videobuf_dma_contig_memory *mem = buf->priv;

    BUG_ON(!mem);
    MAGIC_CHECK(mem->magic, MAGIC_DC_MEM);

    return mem->dma_handle;
}

void ark_disp_videobuf_dma_contig_free(struct videobuf_queue *q,
			      struct videobuf_buffer *buf)
{
    struct videobuf_dma_contig_memory *mem = buf->priv;
#ifdef ARK_DISP_ALLOC_VIDEO_BUFFERS_IN_ADVANCE
    struct ark_disp_v4l2_fh  *fh = q->priv_data;
    struct ark_disp_obj_info *disp = fh->disp_obj;
    struct ark_disp_vid_buf_ctrl *vbc = NULL;
    struct list_head *pos, *n;
#endif
	/* mmapped memory can't be freed here, otherwise mmapped region
	 * would be released, while still needed. In this case, the memory
	 * release should happen inside videobuf_vm_close().
	 * So, it should free memory only if the memory were allocated for
	 * read() operation.
	 */
	if ((buf->memory != V4L2_MEMORY_USERPTR) || !buf->baddr)
		return;

	if (!mem)
		return;

	MAGIC_CHECK(mem->magic, MAGIC_DC_MEM);

#ifndef ARK_DISP_ALLOC_VIDEO_BUFFERS_IN_ADVANCE
	dma_free_coherent(q->dev, mem->size, mem->vaddr, mem->dma_handle);
#else
    spin_lock(&disp->vid_info.lock);
    if (!list_empty(&disp->vid_info.vbuf_list)) {
        /* examine the head buf from done_list */
        list_for_each_safe(pos, n, &disp->vid_info.vbuf_list) {
            vbc = list_entry(pos, struct ark_disp_vid_buf_ctrl, list);
            if ((void *)vbc->buf_addr_virt == mem->vaddr) {
                /* matched buffer found, remove it from vbuf_list */
                list_del(pos);
                disp->vid_info.vbuf_list_entry_count--;
                break;
            }
            vbc = NULL;
        }
    }
    spin_unlock(&disp->vid_info.lock);

    if (vbc == NULL) {
        if (disp->vid_info.vbuf_list_entry_count != 0) {
            // should not happen
            ARKDISP_DBGPRTK("%s %d: SHOULD NOT HAPPEN\n",
                __FUNCTION__, __LINE__);
        }
    } else {
        /* enqueue the buf to free_list */
        spin_lock(&v4l2_disp_context->lock);
        list_add_tail(pos, &v4l2_disp_context->free_vbuf_list);
        v4l2_disp_context->free_vbuf_list_entry_count++;
        spin_unlock(&v4l2_disp_context->lock);
    }
#endif

	mem->vaddr = NULL;
}

static void ark_disp_videobuf_vm_open(struct vm_area_struct *vma)
{
    struct videobuf_mapping *map = vma->vm_private_data;

    ARKDISP_DBGPRTK("%s %d: vm_open %p [count=%u,vma=%08lx-%08lx]\n",
        __FUNCTION__, __LINE__,
        map, map->count, vma->vm_start, vma->vm_end);
    
    map->count++;
}

static void ark_disp_videobuf_vm_close(struct vm_area_struct *vma)
{
    struct videobuf_mapping *map = vma->vm_private_data;
    struct videobuf_queue *q = map->q;
    int i;
#ifdef ARK_DISP_ALLOC_VIDEO_BUFFERS_IN_ADVANCE
    struct ark_disp_v4l2_fh  *fh = q->priv_data;
    struct ark_disp_obj_info *disp = fh->disp_obj;
    struct ark_disp_vid_buf_ctrl *vbc = NULL;
    struct list_head *pos = NULL, *n;
#endif
    ARKDISP_DBGPRTK("%s %d: vm_close %p [count=%u,vma=%08lx-%08lx]\n",
        __FUNCTION__, __LINE__,
        map, map->count, vma->vm_start, vma->vm_end);

    map->count--;
    if (0 == map->count) {
        struct videobuf_dma_contig_memory *mem;

        ARKDISP_DBGPRTK("%s %d: munmap %p q=%p\n",
            __FUNCTION__, __LINE__, map, q);

        mutex_lock(&q->vb_lock);

        /* We need first to cancel streams, before unmapping */
        if (q->streaming)
            videobuf_queue_cancel(q);

        for (i = 0; i < VIDEO_MAX_FRAME; i++) {
            if (NULL == q->bufs[i])
                continue;

            if (q->bufs[i]->map != map)
                continue;

            mem = q->bufs[i]->priv;
            if (mem) {
                /* This callback is called only if kernel has
                 * allocated memory and this memory is mmapped.
                 * In this case, memory should be freed,
                 * in order to do memory unmap.
                 */

                MAGIC_CHECK(mem->magic, MAGIC_DC_MEM);

                /* vfree is not atomic - can't be
                 * called with IRQ's disabled
                 */
                ARKDISP_DBGPRTK(
                    "%s %d: buf[%d] freeing %p vbuf_list_entry_count=%d\n",
                    __FUNCTION__, __LINE__, i, mem->vaddr,
                    disp->vid_info.vbuf_list_entry_count
                    );

#ifndef ARK_DISP_ALLOC_VIDEO_BUFFERS_IN_ADVANCE
                dma_free_coherent(
                    q->dev, mem->size,
                    mem->vaddr, mem->dma_handle);
#else
                /* return vbuf to vbuf_free_list */
                spin_lock(&disp->vid_info.lock);
                if (!list_empty(&disp->vid_info.vbuf_list)) {
                    /* examine the head buf from done_list */
                    list_for_each_safe(pos, n, &disp->vid_info.vbuf_list) {
                        vbc = list_entry(pos, struct ark_disp_vid_buf_ctrl, list);
                        if ((void *)vbc->buf_addr_virt == mem->vaddr) {
                            /* matched buffer found, remove it from vbuf_list */
                            list_del(pos);
                            disp->vid_info.vbuf_list_entry_count--;
                            break;
                        }
                        vbc = NULL;
                    }
                }
                spin_unlock(&disp->vid_info.lock);

                if (vbc == NULL) {
                    if (disp->vid_info.vbuf_list_entry_count != 0) {
                        // should not happen
                        ARKDISP_DBGPRTK("%s %d: SHOULD NOT HAPPEN\n",
                            __FUNCTION__, __LINE__);
                    }
                } else {
                    /* enqueue the buf to free_list */
                    spin_lock(&v4l2_disp_context->lock);
                    list_add_tail(pos, &v4l2_disp_context->free_vbuf_list);
                    v4l2_disp_context->free_vbuf_list_entry_count++;
                    spin_unlock(&v4l2_disp_context->lock);
                }
#endif
                mem->vaddr = NULL;
            }

            q->bufs[i]->map   = NULL;
            q->bufs[i]->baddr = 0;
        }

        kfree(map);
        mutex_unlock(&q->vb_lock);
    }
}

static struct vm_operations_struct ark_disp_videobuf_vm_ops = {
    .open  = ark_disp_videobuf_vm_open,
    .close = ark_disp_videobuf_vm_close,
};

static struct videobuf_buffer *ark_disp_videobuf_alloc_vb(size_t size)
{
    struct videobuf_dma_contig_memory *mem;
    struct videobuf_buffer *vb;

    vb = kzalloc(size + sizeof(*mem), GFP_KERNEL);
    if (vb) {
        mem = vb->priv = ((char *)vb) + size;
        mem->magic = MAGIC_DC_MEM;
    }

    return vb;
}

static void *ark_disp_videobuf_to_vmalloc(struct videobuf_buffer *buf)
{
    struct videobuf_dma_contig_memory *mem = buf->priv;

    BUG_ON(!mem);
    MAGIC_CHECK(mem->magic, MAGIC_DC_MEM);

    return mem->vaddr;
}

static int ark_disp_videobuf_iolock(
    struct videobuf_queue *q,
    struct videobuf_buffer *vb,
    struct v4l2_framebuffer *fbuf)
{
    struct videobuf_dma_contig_memory *mem = vb->priv;	
#ifdef ARK_DISP_ALLOC_VIDEO_BUFFERS_IN_ADVANCE
    struct ark_disp_v4l2_fh  *fh = q->priv_data;
    struct ark_disp_obj_info *disp = fh->disp_obj;
    struct ark_disp_vid_buf_ctrl *vbc;
    struct list_head *head;
#endif
    BUG_ON(!mem);
    MAGIC_CHECK(mem->magic, MAGIC_DC_MEM);

    switch (vb->memory) {
    case V4L2_MEMORY_MMAP:
        ARKDISP_DBGPRTK("%s %d: MMAP\n", __FUNCTION__, __LINE__);

        /* All handling should be done by __videobuf_mmap_mapper() */
        if (!mem->vaddr) {
            ARKDISP_DBGPRTK("%s %d: mem not mmapped/allocated\n",
                __FUNCTION__, __LINE__);
            return -EINVAL;
        }
        break;
    case V4L2_MEMORY_USERPTR:
        ARKDISP_DBGPRTK("%s %d: USERPTR baddr=0x%08lx\n",
            __FUNCTION__, __LINE__, vb->baddr);

        if (vb->baddr) {
            /* memory allocated, no need to alloc memory */
//            break;
        }

        mem->size = PAGE_ALIGN(vb->size);

#ifndef ARK_DISP_ALLOC_VIDEO_BUFFERS_IN_ADVANCE
        mem->vaddr = dma_alloc_coherent(q->dev, mem->size, &mem->dma_handle,
                        GFP_KERNEL);
        if (!mem->vaddr) {
            ARKDISP_DBGPRTK("%s %d: dma_alloc_coherent size %ld failed\n",
                __FUNCTION__, __LINE__, mem->size);
            return -ENOMEM;
        }
        ARKDISP_DBGPRTK(
            "%s %d: dma_alloc_coherent data is at addr %p (size %ld)\n",
            __FUNCTION__, __LINE__, mem->vaddr, mem->size
            );
#else
        if (list_empty(&v4l2_disp_context->free_vbuf_list))
            return -ENOMEM;

        /* get a video buffer from free_vbuf_list */
        spin_lock(&disp->vid_info.lock);
        head = v4l2_disp_context->free_vbuf_list.next;
        list_del(head);
        v4l2_disp_context->free_vbuf_list_entry_count--;
        spin_unlock(&disp->vid_info.lock);

        vbc = list_entry(head, struct ark_disp_vid_buf_ctrl, list);

        vbc->buf_size = mem->size;
        vbc->in_xres  = vb->width;
        vbc->in_yres  = vb->height;

        if (q->streaming) {
            if (vbc->buf_addr_virt % 32) {
                ARKDISP_DBGPRTK("%s %d: offset is not aligned to 32 bytes\n",
                    __FUNCTION__, __LINE__);
                return -EINVAL;
            }
        }
        mem->vaddr      = (void *)vbc->buf_addr_virt;
        mem->dma_handle = vbc->buf_addr_phys;

        /* queue vbc to vbuf_list */
        spin_lock(&disp->vid_info.lock);
        list_add_tail(head, &disp->vid_info.vbuf_list);
        disp->vid_info.vbuf_list_entry_count++;
        spin_unlock(&disp->vid_info.lock);
#endif
        break;
    case V4L2_MEMORY_OVERLAY:
    default:
        ARKDISP_DBGPRTK("%s %d: OVERLAY/unknown\n", __FUNCTION__, __LINE__);
        return -EINVAL;
    }

    return 0;
}

static int ark_disp_videobuf_sync(struct videobuf_queue *q,
    struct videobuf_buffer *buf)
{
    struct videobuf_dma_contig_memory *mem = buf->priv;

    BUG_ON(!mem);
    MAGIC_CHECK(mem->magic, MAGIC_DC_MEM);

    dma_sync_single_for_cpu(q->dev, mem->dma_handle, mem->size,
        DMA_FROM_DEVICE);
    return 0;
}

#ifndef ARK_DISP_NEW_V4L2_API
static int ark_disp_videobuf_copy_to_user(
    struct videobuf_queue *q,
    char __user *data, size_t count,
    int nonblocking)
{
    struct videobuf_dma_contig_memory *mem = q->read_buf->priv;
    void *vaddr;

    BUG_ON(!mem);
    MAGIC_CHECK(mem->magic, MAGIC_DC_MEM);
    BUG_ON(!mem->vaddr);

    /* copy to userspace */
    if (count > q->read_buf->size - q->read_off)
        count = q->read_buf->size - q->read_off;

    vaddr = mem->vaddr;

    if (copy_to_user(data, vaddr + q->read_off, count))
        return -EFAULT;

    return count;
}

static int ark_disp_videobuf_copy_stream(
    struct videobuf_queue *q,
    char __user *data, size_t count, size_t pos,
    int vbihack, int nonblocking)
{
    unsigned int  *fc;
    struct videobuf_dma_contig_memory *mem = q->read_buf->priv;

    BUG_ON(!mem);
    MAGIC_CHECK(mem->magic, MAGIC_DC_MEM);

    if (vbihack) {
        /* dirty, undocumented hack -- pass the frame counter
         * within the last four bytes of each vbi data block.
         * We need that one to maintain backward compatibility
         * to all vbi decoding software out there ...
         */
        fc = (unsigned int *)mem->vaddr;
        fc += (q->read_buf->size >> 2) - 1;
        *fc = q->read_buf->field_count >> 1;
        dev_dbg(q->dev, "vbihack: %d\n", *fc);
    }

    /* copy stuff using the common method */
    count = ark_disp_videobuf_copy_to_user(q, data, count, nonblocking);

    if ((count == -EFAULT) && (pos == 0))
        return -EFAULT;

    return count;
}

static int ark_disp_videobuf_mmap_free(struct videobuf_queue *q)
{
    unsigned int i;

    ARKDISP_DBGPRTK("%s %d: enter\n", __FUNCTION__, __LINE__);
    for (i = 0; i < VIDEO_MAX_FRAME; i++) {
        if (q->bufs[i] && q->bufs[i]->map)
        return -EBUSY;
    }

    return 0;
}

#endif
static int ark_disp_videobuf_mmap_mapper(struct videobuf_queue *q,
    struct videobuf_buffer *buf, struct vm_area_struct *vma)
{
    struct videobuf_dma_contig_memory *mem;
    struct videobuf_mapping *map;
    unsigned int first;
    int retval;
    unsigned long size, offset = vma->vm_pgoff << PAGE_SHIFT;	
#ifdef ARK_DISP_ALLOC_VIDEO_BUFFERS_IN_ADVANCE
    struct ark_disp_v4l2_fh  *fh = q->priv_data;
    struct ark_disp_obj_info *disp = fh->disp_obj;
    struct ark_disp_vid_buf_ctrl *vbc;
    struct list_head *head;
#endif

    ARKDISP_DBGPRTK("%s %d: vm_pgoff=%ld(0x%lx) offset=%ld(0x%lx)\n",
        __FUNCTION__, __LINE__,
        (unsigned long)vma->vm_pgoff, (unsigned long)vma->vm_pgoff,
        offset, offset
        );

    if (!(vma->vm_flags & VM_WRITE) || !(vma->vm_flags & VM_SHARED))
        return -EINVAL;

    /* look for first buffer to map */
    for (first = 0; first < VIDEO_MAX_FRAME; first++) {
        if (!q->bufs[first])
            continue;

        if (V4L2_MEMORY_MMAP != q->bufs[first]->memory)
            continue;

        if (q->bufs[first]->boff == offset)
            break;
    }
    if (VIDEO_MAX_FRAME == first) {
        ARKDISP_DBGPRTK("%s %d: invalid user space offset [offset=0x%lx]\n",
            __FUNCTION__, __LINE__, offset);
        return -EINVAL;
    }

    ARKDISP_DBGPRTK("%s %d: first=%d\n", __FUNCTION__, __LINE__, first);

    /* create mapping + update buffer list */
    map = kzalloc(sizeof(struct videobuf_mapping), GFP_KERNEL);
    if (!map)
        return -ENOMEM;

    q->bufs[first]->map = map;
#ifndef ARK_DISP_NEW_V4L2_API
    map->start = vma->vm_start;
    map->end = vma->vm_end;
#endif
    map->q = q;

    q->bufs[first]->baddr = vma->vm_start;

    mem = q->bufs[first]->priv;
    BUG_ON(!mem);
    MAGIC_CHECK(mem->magic, MAGIC_DC_MEM);
    mem->size = PAGE_ALIGN(q->bufs[first]->bsize);

#ifndef ARK_DISP_ALLOC_VIDEO_BUFFERS_IN_ADVANCE
    mem->vaddr = dma_alloc_coherent(q->dev, mem->size,
                    &mem->dma_handle, GFP_KERNEL);
    if (!mem->vaddr) {
        ARKDISP_DBGPRTK("%s %d: dma_alloc_coherent size %ld failed\n",
            __FUNCTION__, __LINE__, mem->size);
        goto error;
    }
    ARKDISP_DBGPRTK(
        "%s %d: dma_alloc_coherent data is at addr %p (size %ld)\n",
        __FUNCTION__, __LINE__, mem->vaddr, mem->size
        );
#else
    if (list_empty(&v4l2_disp_context->free_vbuf_list))
        goto error;

    /* get a video buffer from free_vbuf_list */
    spin_lock(&disp->vid_info.lock);
    head = v4l2_disp_context->free_vbuf_list.next;
    list_del(head);
    v4l2_disp_context->free_vbuf_list_entry_count--;
    spin_unlock(&disp->vid_info.lock);

    vbc = list_entry(head, struct ark_disp_vid_buf_ctrl, list);

    vbc->buf_size = mem->size;
    vbc->in_xres  = q->bufs[first]->width;
    vbc->in_yres  = q->bufs[first]->height;

    if (q->streaming) {
        if (vbc->buf_addr_virt % 32) {
            ARKDISP_DBGPRTK("%s %d: offset is not aligned to 32 bytes\n",
                __FUNCTION__, __LINE__);
            goto error;
        }
    }
    mem->vaddr      = (void *)vbc->buf_addr_virt;
    mem->dma_handle = vbc->buf_addr_phys;

    /* queue vbc to vbuf_list */
    spin_lock(&disp->vid_info.lock);
    list_add_tail(head, &disp->vid_info.vbuf_list);
    disp->vid_info.vbuf_list_entry_count++;
    spin_unlock(&disp->vid_info.lock);
#endif

    ARKDISP_DBGPRTK(
        "%s %d: mem->size=%ld mem->vaddr=0x%08x mem->dma_handle=0x%08x\n",
        __FUNCTION__, __LINE__,
        mem->size, (unsigned int)mem->vaddr, mem->dma_handle
        );

    /* Try to remap memory */

    size = vma->vm_end - vma->vm_start;
    size = (size < mem->size) ? size : mem->size;

    vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
    retval = remap_pfn_range(vma, vma->vm_start,
    mem->dma_handle >> PAGE_SHIFT,
    size, vma->vm_page_prot);
    if (retval) {
        ARKDISP_DBGPRTK("%s %d: mmap: remap failed with error %d.",
            __FUNCTION__, __LINE__, retval);
#ifndef ARK_DISP_ALLOC_VIDEO_BUFFERS_IN_ADVANCE
        dma_free_coherent(q->dev, mem->size,
            mem->vaddr, mem->dma_handle);
#else
        /* remove the buf from vbuf_list */
        spin_lock(&disp->vid_info.lock);
        list_del(head);
        disp->vid_info.vbuf_list_entry_count--;
        spin_unlock(&disp->vid_info.lock);

        /* enqueue the buf to free_vbuf_list */
        spin_lock(&v4l2_disp_context->lock);
        list_add_tail(head, &v4l2_disp_context->free_vbuf_list);
        v4l2_disp_context->free_vbuf_list_entry_count++;
        spin_unlock(&v4l2_disp_context->lock);
#endif
        goto error;
    }

    vma->vm_ops          = &ark_disp_videobuf_vm_ops;
    vma->vm_flags       |= VM_DONTEXPAND;
    vma->vm_private_data = map;

    ARKDISP_DBGPRTK(
        "%s %d: mmap %p: q=%p %08lx-%08lx (%lx) pgoff %08lx buf %d\n",
        __FUNCTION__, __LINE__,
        map, q, vma->vm_start, vma->vm_end,
        (long int) q->bufs[first]->bsize,
        vma->vm_pgoff, first
        );

    ark_disp_videobuf_vm_open(vma);

    return 0;

error:
    kfree(map);
    return -ENOMEM;
}

static struct videobuf_qtype_ops ark_disp_qops = {
    .magic        = MAGIC_QTYPE_OPS,

    .alloc_vb     = ark_disp_videobuf_alloc_vb,
    .iolock       = ark_disp_videobuf_iolock,
    .sync         = ark_disp_videobuf_sync,
    .mmap_mapper  = ark_disp_videobuf_mmap_mapper,
#ifndef ARK_DISP_NEW_V4L2_API
    .mmap_free    = ark_disp_videobuf_mmap_free,
    .video_copy_to_user = ark_disp_videobuf_copy_to_user,
    .copy_stream  = ark_disp_videobuf_copy_stream,
    .vmalloc      = ark_disp_videobuf_to_vmalloc,
#else
    .vaddr        = ark_disp_videobuf_to_vmalloc,
#endif
};


void ark_disp_videobuf_queue_dma_contig_init(
    struct videobuf_queue *q,
    struct videobuf_queue_ops *ops,
    struct device *dev,
    spinlock_t *irqlock,
    enum v4l2_buf_type type,
    enum v4l2_field field,
    unsigned int msize,
    void *priv)
{
    videobuf_queue_core_init(q, ops, dev, irqlock, type, field, msize,
        priv, &ark_disp_qops, NULL);
}

/*
 * This function sets up v4l2 buffers (e.g. bsize)
 */
static int ark_disp_v4l2_buffer_setup(struct videobuf_queue *q,
    unsigned int *count, unsigned int *size)
{
    struct ark_disp_v4l2_fh  *fh = q->priv_data;
    struct ark_disp_obj_info *disp = fh->disp_obj;
    int buf_size;

    ARKDISP_DBGPRTK("%s %d: enter\n", __FUNCTION__, __LINE__);

//	if (V4L2_MEMORY_MMAP != disp->vid_info.memory)
//		return 0;

    *size    = disp->vid_info.pix_fmt.sizeimage;
    buf_size = disp->buf_info.buf_size;

    if (*size > buf_size)
        *size = buf_size;

    /* store number of buffers allocated in numbuffer member */
    if (*count < disp->buf_info.buf_num)
        *count = disp->buf_info.buf_num;

    ARKDISP_DBGPRTK("%s %d: size=%d count=%d\n", __FUNCTION__, __LINE__,
        *size, *count);
    return 0;
}

/*
 * This is the callback function called from videobuf_qbuf() function
 * the buffer is prepared and user space virtual address is converted into
 * physical address
 */
static int ark_disp_v4l2_buffer_prepare(struct videobuf_queue *q,
    struct videobuf_buffer *vb, enum v4l2_field field)
{
    struct ark_disp_v4l2_fh  *fh = q->priv_data;
    struct ark_disp_obj_info *disp = fh->disp_obj;
	struct videobuf_dma_contig_memory *mem = vb->priv;
    unsigned int addr;
    int ret = 0;

    ARKDISP_DBGPRTK("%s %d: vbuf_list_entry_count=%d\n",
        __FUNCTION__, __LINE__, disp->vid_info.vbuf_list_entry_count);

    ARKDISP_DBGPRTK("%s %d: free_vbuf_list_entry_count=%d\n",
        __FUNCTION__, __LINE__, v4l2_disp_context->free_vbuf_list_entry_count);

    /* if buffer is not initialized, initialize it */
    if (VIDEOBUF_NEEDS_INIT == vb->state) {
        vb->width  = disp->layer_info.src_xres;
        vb->height = disp->layer_info.src_yres;
        vb->size   = disp->vid_info.pix_fmt.sizeimage;
        vb->field  = field;

        ret = videobuf_iolock(q, vb, NULL);
        if (ret < 0) {
            ARKDISP_DBGPRTK("%s %d: failed to map user addr\n",
                __FUNCTION__, __LINE__);
            goto error;
        }

        addr = videobuf_to_dma_contig(vb);

        ARKDISP_DBGPRTK("%s %d: addr=0x%08x\n", __FUNCTION__, __LINE__, addr);

        /* Make sure user addresses are aligned to 32 bytes */
#ifndef ARK_DISP_ALLOC_VIDEO_BUFFERS_IN_ADVANCE
        if (!ALIGN(addr, 32)) {
            ARKDISP_DBGPRTK("%s %d: 32-bit align error\n",
                __FUNCTION__, __LINE__);
            goto error;
        }
#endif
    }

    if (V4L2_MEMORY_USERPTR == vb->memory) {
        /* copy userptr to mem->vaddr */
        ARKDISP_DBGPRTK("%s %d: vaddr=0x%08x userptr=0x%08lx size=%ld\n",
            __FUNCTION__, __LINE__, (unsigned int)mem->vaddr, vb->baddr,
            vb->size);

        ARKDISP_DBGPRTK("%s %d: dma_handle=0x%08x\n",
            __FUNCTION__, __LINE__, (unsigned int)mem->dma_handle);

        ret = copy_from_user(
                mem->vaddr, //dst
                (void __user *)vb->baddr, //src
                vb->size //size in bytes
                );
        if (ret != 0) {
            ARKDISP_DBGPRTK("%s %d: copy_from_user error\n",
                __FUNCTION__, __LINE__);
            goto error;
        }
    }

    vb->state = VIDEOBUF_PREPARED;
    return 0;

error:
    ARKDISP_DBGPRTK("%s %d: err\n", __FUNCTION__, __LINE__);
    return -EINVAL;
}

/*
 * This function adds the buffer to DMA queue
 */
static void ark_disp_v4l2_buffer_queue(struct videobuf_queue *q,
    struct videobuf_buffer *vb)
{
    struct ark_disp_v4l2_fh  *fh = q->priv_data;
    struct ark_disp_obj_info *disp = fh->disp_obj;

    ARKDISP_DBGPRTK("%s %d: output_queue_entry_count=%d\n",
        __FUNCTION__, __LINE__, disp->vid_info.output_queue_entry_count);

    ARKDISP_DBGPRTK("%s %d: free_vbuf_list_entry_count=%d\n",
        __FUNCTION__, __LINE__, v4l2_disp_context->free_vbuf_list_entry_count);

    /* add the buffer to the DMA queue */
    list_add_tail(&vb->queue, &disp->vid_info.output_queue);
    disp->vid_info.output_queue_entry_count++;

    /* update statistics */
    disp->vid_info.out_buf_cnt++;

    /* change state of the buffer */
    vb->state = VIDEOBUF_QUEUED;
}

/*
 * This function is called from the videobuf layer to free memory allocated
 * to the buffers
 */
static void ark_disp_v4l2_buffer_release(struct videobuf_queue *q,
    struct videobuf_buffer *vb)
{
    struct ark_disp_v4l2_fh  *fh = q->priv_data;
    struct ark_disp_obj_info *disp = fh->disp_obj;

    ark_disp_flush_wait_list(disp);

    ARKDISP_DBGPRTK("%s %d: vbuf_list_entry_count=%d\n",
        __FUNCTION__, __LINE__,
        disp->vid_info.vbuf_list_entry_count);

    ARKDISP_DBGPRTK("%s %d: free_vbuf_list_entry_count=%d\n",
        __FUNCTION__, __LINE__,
        v4l2_disp_context->free_vbuf_list_entry_count);

    ark_disp_videobuf_dma_contig_free(q, vb);

    vb->state = VIDEOBUF_NEEDS_INIT;
}

static struct videobuf_queue_ops ark_disp_video_qops = {
    .buf_setup   = ark_disp_v4l2_buffer_setup,
    .buf_prepare = ark_disp_v4l2_buffer_prepare,
    .buf_queue   = ark_disp_v4l2_buffer_queue,
    .buf_release = ark_disp_v4l2_buffer_release,
};

static struct v4l2_capability ark_disp_videocap = {
    .driver   = "ark_display",
    .card     = "Display",
    .bus_info = "Platform",
    .version  = ARK_DISP_VERSION_CODE,
    .capabilities = V4L2_CAP_VIDEO_OUTPUT | V4L2_CAP_STREAMING
};

/* NOTE_1:
 * Video output devices can take digitized images from computer memory,
 * such as those captured from a capture device, and convert them into a
 * video signal. A video signal is a standard analog signal such as a PAL
 * or NTSC signal, or a digital video signal such as an ITU-R601, or
 * DV signal. A video output device can also output video to a graphics
 * display frame buffer or overlay device.
 *
 * An output device can be thought of a reverse capture device, or 
 * as a codec device where the output goes out onto a wire or to 
 * a frame buffer instead of to a memory buffer. 
 */
/* NOTE_2:
 * Video streaming devices can be ...
 */

static int ark_disp_v4l2_queryctrl(struct file *file, void *priv,
    struct v4l2_queryctrl *qc)
{
    int i;

    for (i = 0; i < ARRAY_SIZE(ark_disp_qctrl); i++) {
        if (qc->id && qc->id == ark_disp_qctrl[i].id) {
            memcpy(qc, &(ark_disp_qctrl[i]), sizeof(*qc));
            return 0;
        }
    }
    return -EINVAL;
}

static int ark_disp_v4l2_g_ctrl(struct file *file, void *priv,
    struct v4l2_control *ctrl)
{
    struct ark_disp_v4l2_fh  *fh = file->private_data;
    struct ark_disp_obj_info *disp = fh->disp_obj;
    struct ark_disp_vde_cfg_arg arg;

    ARKDISP_DBGPRTK("%s %d: id=%d\n", __FUNCTION__, __LINE__, disp->id);

    /* safety check */
    if (v4l2_disp_context == NULL)
        return -EIO;

    arg.layer_id = disp->id;
    ark_disp_get_vde_cfg(&arg);

    switch (ctrl->id) {
    case V4L2_CID_BRIGHTNESS:
        ctrl->value = arg.brightness;
        return 0;
    case V4L2_CID_CONTRAST:
        ctrl->value = arg.contrast;
        return 0;
    case V4L2_CID_SATURATION:
        ctrl->value = arg.saturation;
        return 0;
    case V4L2_CID_HUE:
        ctrl->value = arg.hue;
        return 0;
    }
    return -EINVAL;
}

static int ark_disp_v4l2_s_ctrl(struct file *file, void *priv,
    struct v4l2_control *ctrl)
{
    struct ark_disp_v4l2_fh  *fh = file->private_data;
    struct ark_disp_obj_info *disp = fh->disp_obj;
    struct ark_disp_vde_cfg_arg arg;

    ARKDISP_DBGPRTK("%s %d: id=%d\n", __FUNCTION__, __LINE__, disp->id);

    /* safety check */
    if (v4l2_disp_context == NULL)
        return -EIO;

    arg.layer_id = disp->id;
    ark_disp_get_vde_cfg(&arg);

    switch (ctrl->id) {
    case V4L2_CID_BRIGHTNESS:
        arg.brightness = ctrl->value;
        break;
    case V4L2_CID_CONTRAST:
        arg.contrast = ctrl->value;
        break;
    case V4L2_CID_SATURATION:
        arg.saturation = ctrl->value;
        break;
    case V4L2_CID_HUE:
        arg.hue = ctrl->value;
        break;
    default:
        return -EINVAL;
    }

    spin_lock(&disp->lock);
    ark_disp_set_vde_cfg(&arg);
    spin_unlock(&disp->lock);

    return 0;
}

static int ark_disp_v4l2_querycap(struct file *file, void  *priv,
    struct v4l2_capability *cap)
{
//    struct ark_disp_v4l2_fh  *fh = file->private_data;
//    struct ark_disp_obj_info *disp = fh->disp_obj;

//    ARKDISP_DBGPRTK("%s %d: id=%d\n", __FUNCTION__, __LINE__, disp->id);

    memset(cap, 0, sizeof(*cap));
    *cap = ark_disp_videocap;

    return 0;
}

static int ark_disp_v4l2_g_fmt(struct file *file, void *priv,
    struct v4l2_format *fmt)
{
    struct ark_disp_v4l2_fh  *fh = file->private_data;
    struct ark_disp_obj_info *disp = fh->disp_obj;
    int ret = 0;

    ARKDISP_DBGPRTK("%s %d: id=%d\n", __FUNCTION__, __LINE__, disp->id);

    if (V4L2_BUF_TYPE_VIDEO_OUTPUT == fmt->type) {
        /* buffer type is video output */
        struct v4l2_pix_format *pixfmt = &fmt->fmt.pix;

        /* fill in the information about format */
        ret = mutex_lock_interruptible(&disp->vid_info.mutex_lock);
        if (!ret) {
            *pixfmt = disp->vid_info.pix_fmt;
            mutex_unlock(&disp->vid_info.mutex_lock);
        }
    } else {
        ARKDISP_DBGPRTK("%s %d: invalid type\n", __FUNCTION__, __LINE__);
        ret = -EINVAL;
    }
    return ret;
}

static int ark_disp_v4l2_enum_fmt(struct file *file, void  *priv,
    struct v4l2_fmtdesc *fmt)
{
    int ret = 0;
    unsigned int index = 0;

    ARKDISP_DBGPRTK("%s %d: id=%d\n", __FUNCTION__, __LINE__, disp->id);

    if (fmt->index > 0) {
        ARKDISP_DBGPRTK("%s %d: invalid fmt index (%d)\n",
            __FUNCTION__, __LINE__, fmt->index);
        return -EINVAL;
    }

    /* fill in the information about format */
    index = fmt->index;
    memset(fmt, 0, sizeof(*fmt));
    fmt->index = index;
    fmt->type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    if (index == 0) {
        strcpy(fmt->description, "YUV 4:2:2 - UYVY");
        fmt->pixelformat = V4L2_PIX_FMT_UYVY;
    } else if (index == 1) {
        strcpy(fmt->description, "Y/CbCr 4:2:0");
        fmt->pixelformat = V4L2_PIX_FMT_NV12;
    }
    return ret;
}



/* This function examines the input format pixfmt to see if it can be 
 * supported by the display hardware; in the course of the checking, it would 
 * modify the input pixfmt settings to fulfill the hardware requirements; 
 * in the end, if there is no big problem (just minor changes made), 0 will 
 * be returned; otherwise, a negative number will be returned; 
 */
int ark_disp_v4l2_try_format(struct ark_disp_obj_info *disp, 
    struct v4l2_pix_format *pixfmt)
{
    if (pixfmt->width > 1920 || pixfmt->height > 1080)
        return -EINVAL;

    switch (pixfmt->pixelformat)
    {
    case V4L2_PIX_FMT_RGB565:
    case V4L2_PIX_FMT_BGR24:
    case V4L2_PIX_FMT_RGB24:
    case V4L2_PIX_FMT_BGR32:
    case V4L2_PIX_FMT_RGB32:
    case V4L2_PIX_FMT_YVU420: // 12bpp
    case V4L2_PIX_FMT_YUYV: // 16bpp 
    case V4L2_PIX_FMT_YVYU: // 16bpp
    case V4L2_PIX_FMT_YUV422P: // 16bpp YUV422 planar
    case V4L2_PIX_FMT_YUV420: // 12bpp
    /* two planes - one Y, one cb + cr interleaved */
    case V4L2_PIX_FMT_NV12: // 12bpp Y/CbCr 4:2:0
    case V4L2_PIX_FMT_NV21: // 12bpp Y/CrCb 4:2:0
    case V4L2_PIX_FMT_NV16: // 16bpp Y/CbCr 4:2:2
    case V4L2_PIX_FMT_NV61: // 16bpp Y/CrCb 4:2:2
    /* two non contiguous planes - one Y, one cb + cr interleaved */
//    case V4L2_PIX_FMT_NV12M: // 12bpp Y/CbCr 4:2:0
//    case V4L2_PIX_FMT_NV21M: // 12bpp Y/CrCb 4:2:0
    /* three non contiguous planes - one Y, one cb, one cr */
//    case V4L2_PIX_FMT_YUV420M: // 12bpp YUV 4:2:0 planar
//    case V4L2_PIX_FMT_YVU420M: // 12bpp YVU 4:2:0 planar
        break;
    default:
        return -EINVAL;
    }

    switch (pixfmt->field)
    {
    case V4L2_FIELD_NONE: // this device has no field
    case V4L2_FIELD_INTERLACED: // both fields interlaced
//    case V4L2_FIELD_INTERLACEDD_TB: // both fields interlaced, top field
                                    // first and the top field is transmitted
                                    // first
//    case V4L2_FIELD_INTERLACEDD_BT: // both fields interlaced, top field
                                    // first and the bottom field is
                                    // transmitted first
        break;
    case V4L2_FIELD_ANY: // driver can choose from none, top, bottom,
                         // interlaced, depending on whatever it thinks is
                         // approximage ...
    case V4L2_FIELD_TOP: // top field only
    case V4L2_FIELD_BOTTOM: // bottom field only
    case V4L2_FIELD_SEQ_TB: // both fields sequential into one buffer, in
                            // top-bottom order
    case V4L2_FIELD_SEQ_BT: // both fields sequential into one buffer, in
                            // bottom-top order
    case V4L2_FIELD_ALTERNATE: // both fields alternating into separate
                               // buffers
    default:
        return -EINVAL;
    }

//    if (pixfmt->bytesperline/)
//    if (pixfmt->sizeimage)
//    if (pixfmt->colorspace)
    return 0;
}

/* NOTE: Scaling requires a source and a target. On a video capture or 
 * overlay device the source is the video signal, and the cropping ioctls
 * determine the area actually sampled. The target are images read by the
 * application or overlaid onto the graphics screen. Their size (and position
 * for an overlay) is negotiated with the VIDIOC_G_FMT and VIDIOC_S_FMT
 * ioctls.
 *
 * On a video output device the source are the images passed in by the
 * application, and their size is again negotiated with the VIDIOC_G/S_FMT
 * ioctls, or may be encoded in a compressed video stream. The target is the
 * video signal, and the cropping ioctls determine the area where the images
 * are inserted.
 *
 * In short, VIDIOC_G/S_FMT ioctls are used to get/set output layer size,
 * VIDIOC_G/S_CROP ioctls are used to get/set crop layer size and 
 * VIDIOC_CROPCAP ioctl returns source image size
 */

static int ark_disp_v4l2_s_fmt(struct file *file, void *priv,
    struct v4l2_format *fmt)
{
    struct ark_disp_v4l2_fh  *fh = file->private_data;
    struct ark_disp_obj_info *disp = fh->disp_obj;
    int ret = 0;
    struct ark_disp_layer_cfg_arg layer_arg;
    int bitsperpixel;

    ARKDISP_DBGPRTK("%s %d: id=%d\n", __FUNCTION__, __LINE__, disp->id);

    if (disp->vid_info.started) {
        /* streaming is started, return error */
        ARKDISP_DBGPRTK("%s %d: streaming already started\n",
            __FUNCTION__, __LINE__);
        return -EBUSY;
    }

    if (V4L2_BUF_TYPE_VIDEO_OUTPUT == fmt->type) {
        struct v4l2_pix_format *pixfmt = &fmt->fmt.pix;
        /* check for valid field format */
        ret = ark_disp_v4l2_try_format(disp, pixfmt);
        if (ret) {
            ARKDISP_DBGPRTK("%s %d: invalid pixfmt\n",
                __FUNCTION__, __LINE__);
            return ret;
        }
        /* NOTE: ark_display_try_format() examines the input format pixfmt
         * to see if it can be supported by the display hardware; in the
         * course of the checking, it would modify the input pixfmt settings
         * to fulfill the hardware requirements; in the end, if there is no
         * big problem (just minor changes made), 0 will be returned;
         * otherwise, a negative number will be returned; 
         */
        ret = mutex_lock_interruptible(&disp->vid_info.mutex_lock);
        if (ret) {
            ARKDISP_DBGPRTK("%s %d: set lock failed\n",
                __FUNCTION__, __LINE__);
            return ret;
        }

        /* get current layer configuration */
        layer_arg.layer_id = disp->id;
        ark_disp_get_layer_cfg(&layer_arg);

        /* update the local copy of current layer configuration */
        switch (pixfmt->pixelformat)
        {
        case V4L2_PIX_FMT_RGB565:
            layer_arg.pixfmt     = ARKDISP_PIXFMT_RGB565;
            layer_arg.rgb_order  = ARKDISP_RGB_ORDER_RGB;
            layer_arg.yuyv_order = 0;
            bitsperpixel = 16;
            break;
        case V4L2_PIX_FMT_BGR24:
            layer_arg.pixfmt     = ARKDISP_PIXFMT_RGB888;
            layer_arg.rgb_order  = ARKDISP_RGB_ORDER_BGR;
            layer_arg.yuyv_order = 0;
            bitsperpixel = 24;
            break;
        case V4L2_PIX_FMT_RGB24:
            layer_arg.pixfmt     = ARKDISP_PIXFMT_RGB888;
            layer_arg.rgb_order  = ARKDISP_RGB_ORDER_RGB;
            layer_arg.yuyv_order = 0;
            bitsperpixel = 24;
            break;
        case V4L2_PIX_FMT_BGR32:
            layer_arg.pixfmt     = ARKDISP_PIXFMT_RGBA888;
            layer_arg.rgb_order  = ARKDISP_RGB_ORDER_BGR;
            layer_arg.yuyv_order = 0;
            bitsperpixel = 32;
            break;
        case V4L2_PIX_FMT_RGB32:
            layer_arg.pixfmt     = ARKDISP_PIXFMT_RGBA888;
            layer_arg.rgb_order  = ARKDISP_RGB_ORDER_RGB;
            layer_arg.yuyv_order = 0;
            bitsperpixel = 32;
            break;
        case V4L2_PIX_FMT_YVU420: // 12bpp
            layer_arg.pixfmt     = ARKDISP_PIXFMT_YUV420;
            layer_arg.rgb_order  = 0;
            layer_arg.yuyv_order = ARKDISP_YUYV_ORDER_YVYU;
            bitsperpixel = 12;
            break;
        case V4L2_PIX_FMT_YUYV: // 16bpp 
            layer_arg.pixfmt     = ARKDISP_PIXFMT_YUYV;
            layer_arg.rgb_order  = 0;
            layer_arg.yuyv_order = ARKDISP_YUYV_ORDER_YUYV;
            bitsperpixel = 16;
            break;
        case V4L2_PIX_FMT_YVYU: // 16bpp
            layer_arg.pixfmt     = ARKDISP_PIXFMT_YUYV;
            layer_arg.rgb_order  = 0;
            layer_arg.yuyv_order = ARKDISP_YUYV_ORDER_YVYU;
            bitsperpixel = 16;
            break;
        case V4L2_PIX_FMT_YUV422P: // 16bpp YUV422 planar
            layer_arg.pixfmt     = ARKDISP_PIXFMT_YUV422;
            layer_arg.rgb_order  = 0;
            layer_arg.yuyv_order = ARKDISP_YUYV_ORDER_YUYV;
            bitsperpixel = 12;
            break;
        case V4L2_PIX_FMT_YUV420: // 12bpp
            layer_arg.pixfmt     = ARKDISP_PIXFMT_YUV420;
            layer_arg.rgb_order  = 0;
            layer_arg.yuyv_order = ARKDISP_YUYV_ORDER_YUYV;
            bitsperpixel = 12;
            break;
        /* two planes - one Y, one cb + cr interleaved */
        case V4L2_PIX_FMT_NV12: // 12bpp Y/CbCr 4:2:0
            layer_arg.pixfmt     = ARKDISP_PIXFMT_Y_UV420;
            layer_arg.rgb_order  = 0;
            layer_arg.yuyv_order = ARKDISP_YUYV_ORDER_YUYV;
            bitsperpixel = 12;
            break;
        case V4L2_PIX_FMT_NV21: // 12bpp Y/CrCb 4:2:0
            layer_arg.pixfmt     = ARKDISP_PIXFMT_Y_UV420;
            layer_arg.rgb_order  = 0;
            layer_arg.yuyv_order = ARKDISP_YUYV_ORDER_YVYU;
            bitsperpixel = 12;
            break;
        case V4L2_PIX_FMT_NV16: // 16bpp Y/CbCr 4:2:2
            layer_arg.pixfmt     = ARKDISP_PIXFMT_Y_UV422;
            layer_arg.rgb_order  = 0;
            layer_arg.yuyv_order = ARKDISP_YUYV_ORDER_YUYV;
            bitsperpixel = 16;
            break;
        case V4L2_PIX_FMT_NV61: // 16bpp Y/CrCb 4:2:2
            layer_arg.pixfmt     = ARKDISP_PIXFMT_Y_UV420;
            layer_arg.rgb_order  = 0;
            layer_arg.yuyv_order = ARKDISP_YUYV_ORDER_YVYU;
            bitsperpixel = 16;
            break;
        /* two non contiguous planes - one Y, one cb + cr interleaved */
//        case V4L2_PIX_FMT_NV12M: // 12bpp Y/CbCr 4:2:0
//            layer_arg.pixfmt     = ARKDISP_PIXFMT_Y_UV420;
//            layer_arg.rgb_order  = 0;
//            layer_arg.yuyv_order = ARKDISP_YUYV_ORDER_YUYV;
//            bitsperpixel = 12;
//            break;
//        case V4L2_PIX_FMT_NV21M: // 12bpp Y/CrCb 4:2:0
//            layer_arg.pixfmt     = ARKDISP_PIXFMT_Y_UV420;
//            layer_arg.rgb_order  = 0;
//            layer_arg.yuyv_order = ARKDISP_YUYV_ORDER_YVYU;
//            bitsperpixel = 12;
//            break;
        /* three non contiguous planes - one Y, one cb, one cr */
//        case V4L2_PIX_FMT_YUV420M: // 12bpp YUV 4:2:0 planar
//            layer_arg.pixfmt     = ARKDISP_PIXFMT_YUV420;
//            layer_arg.rgb_order  = 0;
//            layer_arg.yuyv_order = ARKDISP_YUYV_ORDER_YUYV;
//            bitsperpixel = 12;
//            break;
//        case V4L2_PIX_FMT_YVU420M: // 12bpp YVU 4:2:0 planar
//            layer_arg.pixfmt     = ARKDISP_PIXFMT_YUV420;
//            layer_arg.rgb_order  = 0;
//            layer_arg.yuyv_order = ARKDISP_YUYV_ORDER_YVYU;
//            bitsperpixel = 12;
//            break;
        default:
            mutex_unlock(&disp->vid_info.mutex_lock);
            return -EINVAL;
        }

        switch (pixfmt->field)
        {
        case V4L2_FIELD_NONE: // this device has no field
            layer_arg.interlaced = 0;
            break;
        case V4L2_FIELD_INTERLACED: // both fields interlaced
//        case V4L2_FIELD_INTERLACEDD_TB: // both fields interlaced, top field
                                        // first and the top field is transmitted
                                        // first
//        case V4L2_FIELD_INTERLACEDD_BT: // both fields interlaced, top field
                                        // first and the bottom field is
                                        // transmitted first
            layer_arg.interlaced = 1;
            break;
        default:
            mutex_unlock(&disp->vid_info.mutex_lock);
            return -EINVAL;
        }

        layer_arg.xres    = pixfmt->width;
        layer_arg.yres    = pixfmt->height;
//        layer_arg.line_length = pixfmt->bytesperline;
        /* NOTE: line_length = src_xres * (bit_per_pixel - bit_per_pixel%8)/8
         * (pixel data are closely packed)
         */

        if (ark_disp_try_layer_cfg(&layer_arg)) {
            mutex_unlock(&disp->vid_info.mutex_lock);
            ARKDISP_DBGPRTK("%s %d: invalid s_fmt para\n",
                __FUNCTION__, __LINE__);
            return -EINVAL;
        }

        /* set the format */
        if (ark_disp_set_layer_cfg(&layer_arg)) {
            mutex_unlock(&disp->vid_info.mutex_lock);
            ARKDISP_DBGPRTK("%s %d: set config failed\n",
                __FUNCTION__, __LINE__);
            return -EINVAL;
        }

        mutex_unlock(&disp->vid_info.mutex_lock);
    } else {
        ARKDISP_DBGPRTK("%s %d: invalid type\n", __FUNCTION__, __LINE__);
        return -EINVAL;
    }
    return ret;
}

static int ark_disp_v4l2_try_fmt(struct file *file, void *priv,
    struct v4l2_format *fmt)
{
    struct ark_disp_v4l2_fh  *fh = file->private_data;
    struct ark_disp_obj_info *disp = fh->disp_obj;
    int ret = 0;

    ARKDISP_DBGPRTK("%s %d: id=%d\n", __FUNCTION__, __LINE__, disp->id);

    if (V4L2_BUF_TYPE_VIDEO_OUTPUT == fmt->type) {
        struct v4l2_pix_format *pixfmt = &fmt->fmt.pix;
        /* check for valid field format */
        ret = ark_disp_v4l2_try_format(disp, pixfmt);
    } else {
        ARKDISP_DBGPRTK("%s %d: invalid type\n", __FUNCTION__, __LINE__);
        ret = -EINVAL;
    }
    return ret;
}

static int ark_disp_v4l2_reqbufs(struct file *file, void *priv,
    struct v4l2_requestbuffers *req_buf)
{
    struct ark_disp_v4l2_fh  *fh = file->private_data;
    struct ark_disp_obj_info *disp = fh->disp_obj;
    int ret = 0;

    ARKDISP_DBGPRTK("%s %d: id=%d2yy count=%d type=%d memory=%d\n",
        __FUNCTION__, __LINE__, disp->id,
        req_buf->count, req_buf->type, req_buf->memory);

    if (req_buf->type != V4L2_BUF_TYPE_VIDEO_OUTPUT) {
        ARKDISP_DBGPRTK("%s %d: not V4L2_BUF_TYPE_VIDEO_OUTPUT\n",
            __FUNCTION__, __LINE__);
        return -EINVAL;
    }
    /* if io users of the layer is not zero, return error */
    if (disp->vid_info.io_usrs) {
        ARKDISP_DBGPRTK("%s %d: not IO user\n", __FUNCTION__, __LINE__);
        return -EBUSY;
    }
    ret = mutex_lock_interruptible(&disp->vid_info.mutex_lock);
    if (ret) {
        ARKDISP_DBGPRTK("%s %d: set lock failed\n", __FUNCTION__, __LINE__);
        return ret;
    }

    /* initialize videobuf queue as per the buffer type */
    ark_disp_videobuf_queue_dma_contig_init(
        &disp->vid_info.buf_queue,
        &ark_disp_video_qops,
        v4l2_disp_context->dev,
        &disp->vid_info.lock,
        V4L2_BUF_TYPE_VIDEO_OUTPUT,
        disp->vid_info.pix_fmt.field,
        sizeof(struct videobuf_buffer),
        fh
        );

    /* set io allowed member of file handle to TRUE */
    fh->io_allowed = 1;

    /* increment io usrs member of layer object to 1 */
    disp->vid_info.io_usrs = 1;

    /* store type of memory requested in layer object */
    disp->vid_info.memory = req_buf->memory;

    /* dequeue dma queue */
    if (!list_empty(&disp->vid_info.output_queue)) {
        // should not happen
        ARKDISP_DBGPRTK("%s %d: SHOULD NOT HAPPEN\n", __FUNCTION__, __LINE__);
    }

    /* allocate buffers */
    ret = videobuf_reqbufs(&disp->vid_info.buf_queue, req_buf);
    mutex_unlock(&disp->vid_info.mutex_lock);

    return ret;
}

static int ark_disp_v4l2_querybuf(struct file *file, void *priv,
    struct v4l2_buffer *buf)
{
    struct ark_disp_v4l2_fh  *fh = file->private_data;
    struct ark_disp_obj_info *disp = fh->disp_obj;
    int ret = 0;

    ARKDISP_DBGPRTK("%s %d: id=%d\n", __FUNCTION__, __LINE__, disp->id);

    /* call videobuf_querybuf to get information */
    ret = videobuf_querybuf(&disp->vid_info.buf_queue, buf);

    return ret;
}

static int ark_disp_v4l2_qbuf(struct file *file, void *priv,
    struct v4l2_buffer *p)
{
    struct ark_disp_v4l2_fh  *fh = file->private_data;
    struct ark_disp_obj_info *disp = fh->disp_obj;

    ARKDISP_DBGPRTK("%s %d: id=%d\n", __FUNCTION__, __LINE__, disp->id);

    /* if this file handle is not allowed to do IO, return error */
    if (!fh->io_allowed) {
        ARKDISP_DBGPRTK("%s %d: no io_allowed\n", __FUNCTION__, __LINE__);
        return -EACCES;
    }

    return videobuf_qbuf(&disp->vid_info.buf_queue, p);
}

static int ark_disp_v4l2_dqbuf(struct file *file, void *priv,
    struct v4l2_buffer *buf)
{
    struct ark_disp_v4l2_fh  *fh = file->private_data;
    struct ark_disp_obj_info *disp = fh->disp_obj;
    int ret = 0;

    ARKDISP_DBGPRTK("%s %d: id=%d\n", __FUNCTION__, __LINE__, disp->id);

    /* if this file handle is not allowed to do IO, return error */
    if (!fh->io_allowed) {
        ARKDISP_DBGPRTK("%s %d: no io_allowed\n", __FUNCTION__, __LINE__);
        return -EACCES;
    }
    if (file->f_flags & O_NONBLOCK) {
        /* call videobuf_dqbuf for non blocking mode */
        ARKDISP_DBGPRTK("%s %d: non-blocking\n", __FUNCTION__, __LINE__);
        ret = videobuf_dqbuf(&disp->vid_info.buf_queue, buf, 1);
    } else {
        /* call videobuf_dqbuf for blocking mode */
        ARKDISP_DBGPRTK("%s %d: blocking\n", __FUNCTION__, __LINE__);
        ret = videobuf_dqbuf(&disp->vid_info.buf_queue, buf, 0);
    }

    ARKDISP_DBGPRTK("%s %d: buf.length=%d\n", __FUNCTION__, __LINE__,
        buf->length);

    return ret;
}

static int ark_disp_v4l2_streamon(struct file *file, void *priv,
    enum v4l2_buf_type buf_type)
{
    struct ark_disp_v4l2_fh  *fh = file->private_data;
    struct ark_disp_obj_info *disp = fh->disp_obj;
    int ret = 0;

    ARKDISP_DBGPRTK("%s %d: id=%d\n", __FUNCTION__, __LINE__, disp->id);

    /* if file handle is not allowed IO, return error */
    if (!fh->io_allowed) {
        ARKDISP_DBGPRTK("%s %d: no io_allowed\n", __FUNCTION__, __LINE__);
        return -EACCES;
    }

    /* if streaming is already started, return error */
    if (disp->vid_info.started) {
        ARKDISP_DBGPRTK("%s %d: already streaming\n", __FUNCTION__, __LINE__);
        return -EBUSY;
    }

    /* call videobuf_streamon to start streaming in videobuf */
    ret = videobuf_streamon(&disp->vid_info.buf_queue);
    if (ret) {
        ARKDISP_DBGPRTK("%s %d: err in videobuf_streamon\n",
            __FUNCTION__, __LINE__);
        return ret;
    }

    ret = mutex_lock_interruptible(&disp->vid_info.mutex_lock);
    if (ret)
        return ret;

    /* if buffer queue is empty, return error */
    if (list_empty(&disp->vid_info.output_queue)) {
        ARKDISP_DBGPRTK("%s %d: buf queue empty\n", __FUNCTION__, __LINE__);
        mutex_unlock(&disp->vid_info.mutex_lock);
        return -EIO;
    }

//    /* get the next frame from the buffer queue */
//    disp->vid_info.cur_frm = list_entry(disp->vid_info.dma_queue.next,
//                                struct videobuf_buffer, queue);    
//    disp->vid_info.next_frm = disp->vid_info.cur_frm;
//
//    /* remove buffer from the buffer queue */
//    list_del(&disp->vid_info.cur_frm->queue);
//
//    /* mark state of the current frame to active */
//    disp->vid_info.cur_frm->state = VIDEOBUF_ACTIVE;

    /* initialize field_id and started member */
    disp->vid_info.field_id = 0;

    /* put the current frame to disp list */
    ark_disp_set_video_stream_on(disp->id);

    disp->vid_info.started = 1;

    ARKDISP_DBGPRTK("%s %d: started streaming on layer id=%d ret=%d\n",
        __FUNCTION__, __LINE__, disp->id, ret);

    mutex_unlock(&disp->vid_info.mutex_lock);

    return ret;
}

static int ark_disp_v4l2_streamoff(struct file *file, void *priv,
    enum v4l2_buf_type buf_type)
{
    struct ark_disp_v4l2_fh  *fh = file->private_data;
    struct ark_disp_obj_info *disp = fh->disp_obj;
    int ret = 0;

    ARKDISP_DBGPRTK("%s %d: id=%d\n", __FUNCTION__, __LINE__, disp->id);

    if (!fh->io_allowed) {
        ARKDISP_DBGPRTK("%s %d: no io_allowed\n", __FUNCTION__, __LINE__);
        return -EACCES;
    }

    /* If streaming is not started, return error */
    if (!disp->vid_info.started) {
        ARKDISP_DBGPRTK("%s %d: streaming not started\n",
            __FUNCTION__, __LINE__);
        return -EINVAL;
    }

    ret = mutex_lock_interruptible(&disp->vid_info.mutex_lock);
    if (ret)
        return ret;

    ark_disp_set_layer_en(disp->id, 0);

    disp->vid_info.started = 0;

    mutex_unlock(&disp->vid_info.mutex_lock);
    ret = videobuf_streamoff(&disp->vid_info.buf_queue);

    return ret;
}

static int ark_disp_v4l2_cropcap(struct file *file, void *priv,
    struct v4l2_cropcap *cropcap)
{
    struct ark_disp_v4l2_fh  *fh = file->private_data;
    struct ark_disp_obj_info *disp = fh->disp_obj;
#ifdef ARKDISP_SET_CROPCAP_INFO_EQUAL_TO_SCREEN_SIZE
    struct ark_disp_tvenc_cfg_arg tvenc_arg;
#endif
    int ret = 0;

//    ARKDISP_DBGPRTK("%s %d: id=%d\n", __FUNCTION__, __LINE__, disp->id);

#ifdef ARKDISP_SET_CROPCAP_INFO_EQUAL_TO_SCREEN_SIZE
    if (ark_disp_get_tvenc_cfg(&tvenc_arg)) {
        ARKDISP_DBGPRTK("%s %d: get tvenc cfg err\n",
            __FUNCTION__, __LINE__);
        return -EINVAL;
    }

    ret = mutex_lock_interruptible(&disp->vid_info.mutex_lock);
    if (ret)
        return ret;

    cropcap->type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    if (tvenc_arg.out_mode == ARKDISP_TVENC_OUT_CVBS_NTSC) {
        cropcap->bounds = cropcap->defrect = ntsc_bounds;
        cropcap->pixelaspect = ntsc_aspect;
    } else if (tvenc_arg.out_mode == ARKDISP_TVENC_OUT_CVBS_PAL) {
        cropcap->bounds = cropcap->defrect = pal_bounds;
        cropcap->pixelaspect = pal_aspect;
    } else if (tvenc_arg.out_mode == ARKDISP_TVENC_OUT_VGA_640x480HZ60) {
        cropcap->bounds = cropcap->defrect = vga_bounds;
        cropcap->pixelaspect = sp_aspect;
    } else if (tvenc_arg.out_mode == ARKDISP_TVENC_OUT_YPBPR_P720HZ60) {
        cropcap->bounds = cropcap->defrect = hd_720p_bounds;
        cropcap->pixelaspect = sp_aspect;
    } else if (tvenc_arg.out_mode == ARKDISP_TVENC_OUT_YPBPR_I1080HZ60) {
        cropcap->bounds = cropcap->defrect = hd_1080i_bounds;
        cropcap->pixelaspect = sp_aspect;
    } else {
        ARKDISP_DBGPRTK("%s %d: unknown encoder display mode\n",
            __FUNCTION__, __LINE__);
        ret = -EINVAL;
    }

    mutex_unlock(&disp->vid_info.mutex_lock);
#else
    ret = mutex_lock_interruptible(&disp->vid_info.mutex_lock);
    if (ret)
        return ret;

    cropcap->type = V4L2_BUF_TYPE_VIDEO_OUTPUT;

    spin_lock(&disp->lock);
    cropcap->bounds.left = 0;
    cropcap->bounds.top  = 0;
    cropcap->bounds.width  = disp->layer_info.src_xres;
    cropcap->bounds.height = disp->layer_info.src_yres;
    spin_unlock(&disp->lock);

    cropcap->defrect = cropcap->bounds;

	cropcap->pixelaspect.numerator   = 1;
	cropcap->pixelaspect.denominator = 1;

    mutex_unlock(&disp->vid_info.mutex_lock);
#endif

//    ARKDISP_DBGPRTK("%s %d: end\n", __FUNCTION__, __LINE__);

    return ret;
}

static int ark_disp_v4l2_g_crop(struct file *file, void *priv,
    struct v4l2_crop *crop)
{
    struct ark_disp_v4l2_fh  *fh = file->private_data;
    struct ark_disp_obj_info *disp = fh->disp_obj;
    int ret = 0;

    ARKDISP_DBGPRTK("%s %d: id=%d\n", __FUNCTION__, __LINE__, disp->id);

    if (crop->type == V4L2_BUF_TYPE_VIDEO_OUTPUT) {
        struct v4l2_rect *rect = &crop->c;

        ret = mutex_lock_interruptible(&disp->vid_info.mutex_lock);
        if (ret)
            return ret;

        rect->top    = disp->layer_info.crop_ypos;
        rect->left   = disp->layer_info.crop_xpos;
        rect->width  = disp->layer_info.crop_xres;
        rect->height = disp->layer_info.crop_yres;
        mutex_unlock(&disp->vid_info.mutex_lock);
    } else {
        ARKDISP_DBGPRTK("%s %d: invalid buf type\n", __FUNCTION__, __LINE__);
        ret = -EINVAL;
    }
    return ret;
}

static int ark_disp_v4l2_s_crop(struct file *file, void *priv,
    struct v4l2_crop *crop)
{
    struct ark_disp_v4l2_fh  *fh = file->private_data;
    struct ark_disp_obj_info *disp = fh->disp_obj;
    struct ark_disp_layer_cfg_arg layer_arg;
    int ret = 0;

    ARKDISP_DBGPRTK("%s %d: id=%d\n", __FUNCTION__, __LINE__, disp->id);

    if (crop->type == V4L2_BUF_TYPE_VIDEO_OUTPUT) {
        struct v4l2_rect *rect = &crop->c;

        if (rect->top < 0 || rect->left < 0) {
            ARKDISP_DBGPRTK("%s %d: -ve value for top/left\n",
                __FUNCTION__, __LINE__);
            return -EINVAL;
        }

        layer_arg.layer_id = disp->id;

        ret = mutex_lock_interruptible(&disp->vid_info.mutex_lock);
        if (ret)
            return ret;
        ark_disp_get_layer_cfg(&layer_arg);

        layer_arg.crop_xpos = rect->left;
        layer_arg.crop_ypos = rect->top;
        layer_arg.crop_xres = rect->width;
        layer_arg.crop_yres = rect->height;

        if (ark_disp_try_layer_cfg(&layer_arg)) {
            mutex_unlock(&disp->vid_info.mutex_lock);
            ARKDISP_DBGPRTK("%s %d: invalid s_crop para\n",
                __FUNCTION__, __LINE__);
            return -EINVAL;
        }

        if (ark_disp_set_layer_cfg(&layer_arg)) {
            mutex_unlock(&disp->vid_info.mutex_lock);
            ARKDISP_DBGPRTK("%s %d: error in set crop para\n",
                __FUNCTION__, __LINE__);
            return -EINVAL;
        }
        mutex_unlock(&disp->vid_info.mutex_lock);
    } else {
            ARKDISP_DBGPRTK("%s %d: invalid buf type\n",
                __FUNCTION__, __LINE__);
        return -EINVAL;
    }
    return ret;
}

static int ark_disp_v4l2_g_priority(struct file *file, void *priv,
    enum v4l2_priority *p)
{
    struct ark_disp_v4l2_fh  *fh = file->private_data;
    struct ark_disp_obj_info *disp = fh->disp_obj;

    *p = v4l2_prio_max(&disp->vid_info.prio);

    return 0;
}

static int ark_disp_v4l2_s_priority(struct file *file, void *priv,
    enum v4l2_priority p)
{
    struct ark_disp_v4l2_fh  *fh = file->private_data;
    struct ark_disp_obj_info *disp = fh->disp_obj;
    int ret;

    ret = v4l2_prio_change(&disp->vid_info.prio, &fh->prio, p);

    return ret;
}

static long ark_disp_v4l2_param_handler(struct file *file, void *priv, bool valid_prio, int cmd,
    void *param)
{
    struct ark_disp_v4l2_fh  *fh = file->private_data;
    struct ark_disp_obj_info *disp = fh->disp_obj;
    int ret = 0;

    ret = mutex_lock_interruptible(&disp->vid_info.mutex_lock);
    if (ret)
        return ret;

    switch (cmd) {
        case VIDIOC_S_COFST:
            disp->buf_info.cbcr_offset = *((unsigned long *)param);
            break;
        default:
            ret = -EINVAL;
            break;
    }

    mutex_unlock(&disp->vid_info.mutex_lock);
    return ret;
}

/*
 * This function creates object of file handle structure and stores it in
 * priv_data member of filepointer
 */
static int ark_disp_v4l2_fops_open(struct file *filep)
{
    int minor = iminor(filep->f_path.dentry->d_inode);
    int i = 0;
    struct ark_disp_obj_info *disp;
    struct ark_disp_v4l2_fh *fh = NULL;

    ARKDISP_DBGPRTK("%s %d: minor=%d\n", __FUNCTION__, __LINE__, minor);

    /* Check for valid minor number */
    for (i = 0; i < ARK_DISP_V4L2_DEV_NUM; i++) {
        /* Get the pointer to the layer object */
        disp = &v4l2_disp_context->disp_obj[i];
//        ARKDISP_DBGPRTK("%s %d: video_dev[%d]->minor=%d\n",
//            __FUNCTION__, __LINE__, i, disp->vid_info.video_dev->minor);
        if (minor == disp->vid_info.video_dev->minor) {
            break;
        }
    }

    /* If not found, return error no device */
    if (i == ARK_DISP_V4L2_DEV_NUM) {
        ARKDISP_DBGPRTK("%s %d: device not found\n", __FUNCTION__, __LINE__);
        return -ENODEV;
    }

//    ARKDISP_DBGPRTK("%s %d: id=%d\n", __FUNCTION__, __LINE__, disp->id);

    /* Allocate memory for the file handle object */
    fh = kmalloc(sizeof(struct ark_disp_v4l2_fh), GFP_KERNEL);
    if (!fh) {
        ARKDISP_DBGPRTK(
            "%s %d: allocate memory err for file handle object\n",
            __FUNCTION__, __LINE__);
        return -ENOMEM;
    }

    /* store pointer to fh in priv_data member of filep */
    filep->private_data = fh;
    fh->disp_obj = disp;

    /* increment layer usrs counter */
    atomic_inc((atomic_t *)&disp->vid_info.usrs);

    /* set io_allowed member to false */
    fh->io_allowed = 0;

    fh->initialized = 0;

    /* initialize priority of this instance to default priority */
    fh->prio = V4L2_PRIORITY_UNSET;
    v4l2_prio_open(&disp->vid_info.prio, &fh->prio);

    ARKDISP_DBGPRTK("%s %d: open done id=%d\n",
        __FUNCTION__, __LINE__, disp->id);
    return 0;
}
/* NOTE: Open is called when a file descriptor is opened on the device file.
 * Using the input filepointer, the driver can figure out which minor the 
 * device is. Then, using this minor number, the driver can
 * access the proper device and retrieve it's private data,which usually
 * contains information specific to that device, and that needs to be passed
 * around frequently. The private data, which is like a file handler, is
 * assigned to the priv_data variable of the struct *file pointer which
 * entered the open function. V4L2 devices can be opened more than once
 * simultaneously, either by the same application, or by more than one
 * application. After open, all the other method calls access the private
 * data of the device via the struct *file pointer which comes in to all the
 * functions after open. The return value is returned to the application.
 */


/*
 * This function deletes buffer queue, frees the buffers and the display
 * file handle
 */
static int ark_disp_v4l2_fops_release(struct file *filep)
{
    struct ark_disp_v4l2_fh  *fh = filep->private_data;
    struct ark_disp_obj_info *disp = fh->disp_obj;
    int ret = 0;

    ARKDISP_DBGPRTK("%s %d: enter\n", __FUNCTION__, __LINE__);

    if ((disp->vid_info.usrs != 1) && fh->io_allowed) {
        /* this is doing IO and other layer are not closed, quit */
        ARKDISP_DBGPRTK("%s %d: error, usrs=%d io_allowed=%d\n",
            __FUNCTION__, __LINE__, disp->vid_info.usrs, fh->io_allowed
            );
        return -EAGAIN;
    }

    /* get the lock on layer object */
    ret = mutex_lock_interruptible(&disp->vid_info.mutex_lock);
    if (ret) {
        ARKDISP_DBGPRTK("%s %d: get lock failed\n", __FUNCTION__, __LINE__);
        return ret;
    }

    if (fh->io_allowed) {
        /* this instance is doing IO, reset io_usrs member of layer object */
        ARKDISP_DBGPRTK("%s %d: reset io_urs\n", __FUNCTION__, __LINE__);

        disp->vid_info.io_usrs = 0;
        ark_disp_set_layer_en(disp->id, 0);
        disp->vid_info.started = 0;

        /* free buffers allocated */
        videobuf_queue_cancel(&disp->vid_info.buf_queue);
        videobuf_mmap_free(&disp->vid_info.buf_queue);
    }

    /* decrement layer usrs counter */
    atomic_dec((atomic_t *)&disp->vid_info.usrs);

    if (!disp->vid_info.usrs) {
        /* this file handle has initialized encoder device, reset it */
        ARKDISP_DBGPRTK("%s %d: disable output\n", __FUNCTION__, __LINE__);

        ark_disp_set_layer_en(disp->id, 0);
    }

    /* close the priority */
#ifndef ARK_DISP_NEW_V4L2_API
    v4l2_prio_close(&disp->vid_info.prio, &fh->prio);
#else
    v4l2_prio_close(&disp->vid_info.prio, fh->prio);
#endif
    filep->private_data = NULL;

    /* free memory allocated to file handle object */
    if (!fh)
        kfree(fh);

    /* unlock mutex on layer object */
    mutex_unlock(&disp->vid_info.mutex_lock);

    ARKDISP_DBGPRTK("%s %d: release done\n", __FUNCTION__, __LINE__);
    return 0;
}

/* This function is used to map kernel space buffers into user spaces
 */
static int ark_disp_v4l2_fops_mmap(struct file *filep,
    struct vm_area_struct *vma)
{
    /* get the layer object and file handle object */
    struct ark_disp_v4l2_fh  *fh = filep->private_data;
    struct ark_disp_obj_info *disp = fh->disp_obj;
    int err = 0;

	ARKDISP_DBGPRTK("%s %d: vma start=0x%08lx, size=%ld\n",
         __FUNCTION__, __LINE__,
		(unsigned long)vma->vm_start,
		(unsigned long)vma->vm_end-(unsigned long)vma->vm_start
        );

    err = ark_disp_videobuf_mmap_mapper(&disp->vid_info.buf_queue, NULL, vma);

	ARKDISP_DBGPRTK("%s %d: vma start=0x%08lx, size=%ld, err=%d\n",
         __FUNCTION__, __LINE__,
		(unsigned long)vma->vm_start,
		(unsigned long)vma->vm_end-(unsigned long)vma->vm_start,
		err
        );

    return err;
}

/* NOTE: Called when the application calls mmap(). The V4L2 specification 
 * defines a general-purpose protocol for
 * using mmap() for buffer mapping that all V4L2 devices need to follow.
 * Return 0 for success or a negative error code. The return value is returned
 * to the application. Return -EINVAL for an invalid parameter, or -ENODEV if
 * the buffer could not be allocated. 
 */

/* This function is used for select/poll system call 
 */
static unsigned int ark_disp_v4l2_fops_poll(struct file *filep,
    poll_table *wait)
{
    struct ark_disp_v4l2_fh  *fh = filep->private_data;
    struct ark_disp_obj_info *disp = fh->disp_obj;
    int err = 0;

//    ARKDISP_DBGPRTK("%s %d: enter\n", __FUNCTION__, __LINE__);

    if (disp->vid_info.started)
        err = videobuf_poll_stream(filep, &disp->vid_info.buf_queue, wait);

    ARKDISP_DBGPRTK("%s %d: err=%d\n", __FUNCTION__, __LINE__, err);
    return err;
}

static const struct v4l2_file_operations ark_disp_v4l2_fops = {
    .owner          = THIS_MODULE,
    .open           = ark_disp_v4l2_fops_open,
    .release        = ark_disp_v4l2_fops_release,
//    .read           = ark_disp_v4l2_fops_read,
//    .write          = ark_disp_v4l2_fops_write,
    .poll           = ark_disp_v4l2_fops_poll,
    .ioctl          = video_ioctl2, /* V4L2 ioctl handler */
//    .compat_ioctl   = v4l_compat_ioctl32,
    .mmap           = ark_disp_v4l2_fops_mmap,
//    .llseek         = no_llseek
};


/* NOTE: It is necessary to mention that the video_ioctl2 standard function
 * is used as a handler for the ioctl call. This function performs the initial
 * processing of an ioctl call, analyzing the command's code and calling a 
 * corresponding handler using pointers to functions specified in the 
 * v4l2_ioctl_ops structure. You can specify your own function for processing
 * ioctl, but in that case, you will have to implement code for command
 * analysis independently. Developers of V4L2 recommend using video_ioctl2, in
 * order to decrease the probabilitiesof errors in driver code.
 */

static const struct v4l2_ioctl_ops ark_disp_ioctl_ops = {
	.vidioc_queryctrl        = ark_disp_v4l2_queryctrl,
	.vidioc_g_ctrl           = ark_disp_v4l2_g_ctrl,
	.vidioc_s_ctrl           = ark_disp_v4l2_s_ctrl,
    .vidioc_querycap         = ark_disp_v4l2_querycap,
    .vidioc_g_fmt_vid_out    = ark_disp_v4l2_g_fmt,
    .vidioc_enum_fmt_vid_out = ark_disp_v4l2_enum_fmt,
    .vidioc_s_fmt_vid_out    = ark_disp_v4l2_s_fmt,
    .vidioc_try_fmt_vid_out  = ark_disp_v4l2_try_fmt,
    .vidioc_reqbufs          = ark_disp_v4l2_reqbufs,
    .vidioc_querybuf         = ark_disp_v4l2_querybuf,
    .vidioc_qbuf             = ark_disp_v4l2_qbuf,
    .vidioc_dqbuf            = ark_disp_v4l2_dqbuf,
    .vidioc_streamon         = ark_disp_v4l2_streamon,
    .vidioc_streamoff        = ark_disp_v4l2_streamoff,
    .vidioc_cropcap          = ark_disp_v4l2_cropcap,
    .vidioc_g_crop           = ark_disp_v4l2_g_crop,
    .vidioc_s_crop           = ark_disp_v4l2_s_crop,
    .vidioc_g_priority       = ark_disp_v4l2_g_priority,
    .vidioc_s_priority       = ark_disp_v4l2_s_priority,
    .vidioc_default          = ark_disp_v4l2_param_handler,
#ifdef CONFIG_VIDEO_V4L1_COMPAT
//    .vidiocgmbuf             = vidiocgmbuf,
#endif
};

/* NOTE: The v4l2_ioctl_ops structure stores pointers to handlers of V4L2 
 * commands transmitted by means of an ioctl system call. This structure is
 * declared in media/v4l2-ioctl.h> file.
 */

/* NOTE: It is important for drivers to handle VIDIOC_QUERYCTRL and to return
 * -EINVAL if the id field is out of range. If your driver does not have any
 * private controls, you need to return -EINVAL if the id field is
 * V4L2_CID_PRIVATE_BASE. It is not necessary to fill in the v4l2_queryctrl
 * category or group fields for pre-defined control IDs; videodev will do it.
 * Videodev fills them in before the driver is called, so the driver can
 * change the default values if desired. 
 */

static struct video_device ark_disp_template = {
    .name  = "ark_disp_v4l2",          // canonical name for this device
    .fops  = &ark_disp_v4l2_fops,
    .ioctl_ops = &ark_disp_ioctl_ops,
    .minor  = -1,                    // device's minor number 
    .release = video_device_release, // standard func, just release memory
                                     // allocated for the video_device
                                     // structure

    /* video standard vars */
    .tvnorms = V4L2_STD_525_60,      // supported tv norms
    .current_norm = V4L2_STD_NTSC_M, // current tvnorm
};

int ark_disp_convert_layer_cfg_to_v4l2_info(int layer_id,
    struct ark_disp_context *context)
{
    struct ark_disp_obj_info   *disp;
    struct ark_disp_layer_info *layer;
    struct ark_disp_v4l2_info  *vid;

    if (context)
        v4l2_disp_context = context;

    disp = &v4l2_disp_context->disp_obj[layer_id];
    layer = &v4l2_disp_context->disp_obj[layer_id].layer_info;
    vid = &v4l2_disp_context->disp_obj[layer_id].vid_info;

    vid->pix_fmt.width  = layer->xres;
    vid->pix_fmt.height = layer->yres;

    vid->pix_fmt.bytesperline = layer->line_length;

    switch (layer->pixfmt)
    {
    case ARKDISP_PIXFMT_YUV422:
        vid->pix_fmt.pixelformat = V4L2_PIX_FMT_YUV422P;
        break;
    case ARKDISP_PIXFMT_YUV420:
        /* three non contiguous planes - one Y, one cb, one cr */
//        if (layer->yuyv_order == ARKDISP_YUYV_ORDER_YUYV)
//            vid->pix_fmt.pixelformat = V4L2_PIX_FMT_YUV420M;
//        else
//            vid->pix_fmt.pixelformat = V4L2_PIX_FMT_YVU420M;
return -EINVAL;
        break;
    case ARKDISP_PIXFMT_YUYV:
        if (layer->yuyv_order == ARKDISP_YUYV_ORDER_YUYV)
            vid->pix_fmt.pixelformat = V4L2_PIX_FMT_YUYV;
        else if (layer->yuyv_order == ARKDISP_YUYV_ORDER_YVYU)
            vid->pix_fmt.pixelformat = V4L2_PIX_FMT_YVYU;
        else if (layer->yuyv_order == ARKDISP_YUYV_ORDER_UYVY)
            vid->pix_fmt.pixelformat = V4L2_PIX_FMT_UYVY;
        else
            vid->pix_fmt.pixelformat = V4L2_PIX_FMT_VYUY;
        break;
//    case ARKDISP_PIXFMT_YUV:
//        if (layer->yuyv_order == ARKDISP_YUYV_ORDER_YUYV)
//            vid->pix_fmt.pixelformat = V4L2_PIX_FMT_NV24;
//        else
//            vid->pix_fmt.pixelformat = V4L2_PIX_FMT_NV42;
//        break;
    case ARKDISP_PIXFMT_RGB565:
        if (layer->rgb_order == ARKDISP_RGB_ORDER_RGB)
            vid->pix_fmt.pixelformat = V4L2_PIX_FMT_RGB565;
        else
            vid->pix_fmt.pixelformat = V4L2_PIX_FMT_RGB565;
        break;
    case ARKDISP_PIXFMT_RGBA888:
        if (layer->rgb_order == ARKDISP_RGB_ORDER_RGB)
            vid->pix_fmt.pixelformat = V4L2_PIX_FMT_RGB32;
        else
            vid->pix_fmt.pixelformat = V4L2_PIX_FMT_BGR32;
        break;
    case ARKDISP_PIXFMT_RGB888:
        if (layer->rgb_order == ARKDISP_RGB_ORDER_RGB)
            vid->pix_fmt.pixelformat = V4L2_PIX_FMT_RGB24;
        else
            vid->pix_fmt.pixelformat = V4L2_PIX_FMT_BGR24;
        break;
    case ARKDISP_PIXFMT_Y_UV422:
        /* two planes - one Y, one cb + cr interleaved */
        if (layer->yuyv_order == ARKDISP_YUYV_ORDER_YUYV)
            vid->pix_fmt.pixelformat = V4L2_PIX_FMT_NV16;//16bpp Y/CbCr 4:2:2
        else
            vid->pix_fmt.pixelformat = V4L2_PIX_FMT_NV61;//16bpp Y/CrCb 4:2:2
        break;
    case ARKDISP_PIXFMT_Y_UV420:
        /* two planes - one Y, one cb + cr interleaved */
        if (layer->yuyv_order == ARKDISP_YUYV_ORDER_YUYV)
            vid->pix_fmt.pixelformat = V4L2_PIX_FMT_NV12;//12bpp Y/CbCr 4:2:0
        else
            vid->pix_fmt.pixelformat = V4L2_PIX_FMT_NV21;//12bpp Y/CrCb 4:2:0
        break;
    case ARKDISP_PIXFMT_RGBI555:
    case ARKDISP_PIXFMT_RGBA1666:
    case ARKDISP_PIXFMT_RGBA4888:
    case ARKDISP_PIXFMT_RGB666:
    case ARKDISP_PIXFMT_RGBA1888:
    case ARKDISP_PIXFMT_RGBA1555:
    default:
        return -EINVAL;
    }

    if (layer->interlaced) {
        vid->pix_fmt.field = V4L2_FIELD_INTERLACED;// both fields interlaced
//        vid->pix_fmt.field = V4L2_FIELD_INTERLACEDD_TB; 
                                    // both fields interlaced, top field
                                    // first and the top field is transmitted
                                    // first
//        vid->pix_fmt.field = V4L2_FIELD_INTERLACEDD_BT; 
                                    // both fields interlaced, top field
                                    // first and the bottom field is
                                    // transmitted first
    } else 
        vid->pix_fmt.field = V4L2_FIELD_NONE;

    disp->vid_info.pix_fmt.sizeimage =
        layer->bits_per_pixel * layer->src_xres * layer->src_yres / 8;

//    disp->vid_info.pix_fmt.colorspace = V4L2_COLORSPACE_SMPTE170M; //NTSC/PAL
    disp->vid_info.pix_fmt.colorspace = V4L2_COLORSPACE_REC709; //HDTV, modern device
    return 0;
}

int ark_disp_v4l2_init(struct ark_disp_context *context)
{
    struct video_device *vfd;
    struct ark_disp_v4l2_info *vid;
    int i, j;
    int ret = 0;

    ARKDISP_DBGPRTK("%s %d: enter\n", __FUNCTION__, __LINE__);

    v4l2_disp_context = context;

    for (i = 0; i < ARK_DISP_LAYER_NUM; i++) {
        vfd = video_device_alloc();
        if (!vfd) {
            /* allocation failed */
            ARKDISP_DBGPRTK("%s %d: alloc failed i=%d\n",
                __FUNCTION__, __LINE__, i);
            ret = -ENOMEM;
            goto init_err;
        }

        context->disp_obj[i].vid_info.video_dev = vfd;

        *vfd = ark_disp_template;
        snprintf(vfd->name, sizeof(vfd->name), "ark_disp%d", i);

        //        ret = video_register_device(vfd, VFL_TYPE_GRABBER, -1);
        if (ARK_DISP_LAYER_IS_OSD(i)) {
            ret = video_register_device(vfd, VFL_TYPE_GRABBER, 2 + i);
            if (ret < 0) {
                /* registration failed */
                ARKDISP_DBGPRTK("%s %d: register failed i=%d\n",
                    __FUNCTION__, __LINE__, i);
                goto init_err;
            }
            /* NOTE: the nr parameter (the last para) sets the number of the 
             * registered device: 0 for /dev/video0, 1 for /dev/video1, ...  
             * specifying -1 for this parameter will assign the first
             * available number to the device
             */
            /* in this case, devices for osd1, osd2,  osd3 are registered as
             * /dev/video10, /dev/video11, /dev/video12
             */
            vfd->minor = 2 + i;
        } else {
            ret = video_register_device(vfd, VFL_TYPE_GRABBER,
                    ARK_DISP_GET_VIDEO_ID_FROM_LAYER_ID(i));
            if (ret < 0) {
                /* registration failed */
                ARKDISP_DBGPRTK("%s %d: register failed i=%d\n",
                    __FUNCTION__, __LINE__, i);
                goto init_err;
            }
            /* in this case, devices for video, video2 are registered as
             * /dev/video0, /dev/video1
             */
            vfd->minor = ARK_DISP_GET_VIDEO_ID_FROM_LAYER_ID(i);
        }

        ARKDISP_DBGPRTK("%s %d: vfd[%d].minor=%d\n",
            __FUNCTION__, __LINE__, i, vfd->minor);

        video_set_drvdata(vfd, &context->disp_obj[i]);

        vid = &context->disp_obj[i].vid_info;

        INIT_LIST_HEAD(&vid->output_queue);
        vid->output_queue_entry_count = 0;

        mutex_init(&vid->mutex_lock);
        spin_lock_init(&vid->lock);

        vid->usrs    = 0;
        vid->io_usrs = 0;
        vid->started = 0;

        INIT_LIST_HEAD(&vid->vbuf_list);
        vid->vbuf_list_entry_count = 0;

        ark_disp_convert_layer_cfg_to_v4l2_info(i, context);

        ARKDISP_DBGPRTK("%s %d: init done i=%d\n", __FUNCTION__, __LINE__, i);
    }

    return 0;

init_err:
    /* allocation failed */
    for (j = 0; j < i; j++)
        video_device_release(context->disp_obj[j].vid_info.video_dev);

    return ret;
}

int ark_disp_v4l2_release(struct ark_disp_context *context)
{
    int i;

    for (i = 0; i < ARK_DISP_V4L2_DEV_NUM; i++) {
        if (context->disp_obj[i].vid_info.video_dev) {
            /* remove the video device from the system */
            video_unregister_device(
                context->disp_obj[i].vid_info.video_dev);

            /* release memory occupied by the structure */
            video_device_release(context->disp_obj[i].vid_info.video_dev);

            context->disp_obj[i].vid_info.video_dev = NULL;
        }
    }

    v4l2_disp_context = NULL;
    return 0;
}

