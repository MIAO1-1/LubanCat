/* Allwinner SoCs display driver.
 *
 * Copyright (C) 2016 Allwinner.
 *
 * This file is licensed under the terms of the GNU General Public
 * License version 2.  This program is licensed "as is" without any
 * warranty of any kind, whether express or implied.
 */

#include "dev_disp.h"
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/version.h>
#include <linux/fb.h>
#include <linux/memblock.h>
#if defined(SUPPORT_EDP)
#include "de/disp_edp.h"
#endif /*endif defined(SUPPORT_EDP) */
#include "fb_g2d_rot.h"
#include <linux/decompress/unlzma.h>
#include <linux/dma-buf.h>
#include <linux/dma-mapping.h>

#ifndef dma_mmap_writecombine
#define dma_mmap_writecombine dma_mmap_wc
#endif

#if defined(CONFIG_SUNXI_DISP2_FB_ROTATION_SUPPORT) ||                         \
    defined(CONFIG_SUNXI_DISP2_FB_HW_ROTATION_SUPPORT)
#define SUPPORT_ROTATE
#endif

#define VSYNC_NUM 4
#define SUNXI_FB_MAX 1
struct fb_info_t {
	struct device *dev;

	bool fb_enable[SUNXI_FB_MAX];
	enum disp_fb_mode fb_mode[SUNXI_FB_MAX];
	u32 layer_hdl[SUNXI_FB_MAX][2];	/* channel, layer_id */
	struct fb_info *fbinfo[SUNXI_FB_MAX];
	struct disp_fb_create_info fb_para[SUNXI_FB_MAX];
	u32 pseudo_palette[SUNXI_FB_MAX][16];
	wait_queue_head_t wait[3];
	unsigned long wait_count[3];
	struct fb_g2d_rot_t *fb_rot[FB_MAX];
	int mem_cache_flag[SUNXI_FB_MAX];

	int blank[3];
	struct disp_ion_mem *mem[SUNXI_FB_MAX];
	struct disp_layer_config config[SUNXI_FB_MAX];
};

static struct fb_info_t g_fbi;
static phys_addr_t bootlogo_addr;
static int bootlogo_sz;

#define FBHANDTOID(handle)  ((handle) - 100)
#define FBIDTOHAND(ID)  ((ID) + 100)

static struct __fb_addr_para g_fb_addr;

s32 sunxi_get_fb_addr_para(struct __fb_addr_para *fb_addr_para)
{
	if (fb_addr_para) {
		fb_addr_para->fb_paddr = g_fb_addr.fb_paddr;
		fb_addr_para->fb_size = g_fb_addr.fb_size;
		return 0;
	}

	return -1;
}
EXPORT_SYMBOL(sunxi_get_fb_addr_para);

#define sys_put_wvalue(addr, data) writel(data, (void __iomem *)addr)

s32 fb_draw_gray_pictures(char *base, u32 width, u32 height,
			  struct fb_var_screeninfo *var)
{
	u32 time = 0;

	for (time = 0; time < 18; time++) {
		u32 i = 0, j = 0;

		for (i = 0; i < height; i++) {
			for (j = 0; j < width; j++) {
				char *addr = base + (i * width + j) * 4;
				u32 value =
				    (0xff << 24) | ((time * 15) << 16) |
				    ((time * 15) << 8) | (time * 15);

				sys_put_wvalue((void *)addr, value);
			}
		}
	}
	return 0;
}

static int fb_map_video_memory(struct fb_info *info)
{
#if defined(CONFIG_ION)
	g_fbi.mem[info->node] =
	    disp_ion_malloc(info->fix.smem_len, (u32 *)(&info->fix.smem_start));
	if (g_fbi.mem[info->node])
		info->screen_base = (char __iomem *)g_fbi.mem[info->node]->vaddr;
#else
	info->screen_base =
	    (char __iomem *)disp_malloc(info->fix.smem_len,
					(u32 *) (&info->fix.smem_start));
#endif
	if (info->screen_base) {
		__inf("%s(reserve),va=0x%p, pa=0x%p size:0x%x\n", __func__,
		      (void *)info->screen_base,
		      (void *)info->fix.smem_start,
		      (unsigned int)info->fix.smem_len);
		memset((void *__force)info->screen_base, 0x0,
		       info->fix.smem_len);

		g_fb_addr.fb_paddr = (uintptr_t) info->fix.smem_start;
		g_fb_addr.fb_size = info->fix.smem_len;

		return 0;
	}

	__wrn("disp_malloc fail!\n");
	return -ENOMEM;

	return 0;
}

static inline void fb_unmap_video_memory(struct fb_info *info)
{
	if (!info->screen_base) {
		__wrn("%s: screen_base is null\n", __func__);
		return;
	}
	__inf("%s: screen_base=0x%p, smem=0x%p, len=0x%x\n", __func__,
	      (void *)info->screen_base,
	      (void *)info->fix.smem_start, info->fix.smem_len);
#if defined(CONFIG_ION)
	disp_ion_free((void *__force)info->screen_base,
		  (void *)info->fix.smem_start, info->fix.smem_len);
#else
	disp_free((void *__force)info->screen_base,
		  (void *)info->fix.smem_start, info->fix.smem_len);
#endif
	info->screen_base = 0;
	info->fix.smem_start = 0;
	g_fb_addr.fb_paddr = 0;
	g_fb_addr.fb_size = 0;
}

static void *Fb_map_kernel(unsigned long phys_addr, unsigned long size)
{
	int npages = PAGE_ALIGN(size) / PAGE_SIZE;
	struct page **pages = vmalloc(sizeof(struct page *) * npages);
	struct page **tmp = pages;
	struct page *cur_page = phys_to_page(phys_addr);
	pgprot_t pgprot;
	void *vaddr = NULL;
	int i;

	if (!pages)
		return NULL;

	for (i = 0; i < npages; i++)
		*(tmp++) = cur_page++;

	pgprot = pgprot_noncached(PAGE_KERNEL);
	vaddr = vmap(pages, npages, VM_MAP, pgprot);

	vfree(pages);
	return vaddr;
}

static void *Fb_map_kernel_cache(unsigned long phys_addr, unsigned long size)
{
	int npages = PAGE_ALIGN(size) / PAGE_SIZE;
	struct page **pages = vmalloc(sizeof(struct page *) * npages);
	struct page **tmp = pages;
	struct page *cur_page = phys_to_page(phys_addr);
	pgprot_t pgprot;
	void *vaddr = NULL;
	int i;

	if (!pages)
		return NULL;

	for (i = 0; i < npages; i++)
		*(tmp++) = cur_page++;

	pgprot = PAGE_KERNEL;
	vaddr = vmap(pages, npages, VM_MAP, pgprot);

	vfree(pages);
	return vaddr;
}

static void Fb_unmap_kernel(void *vaddr)
{
	vunmap(vaddr);
}

static s32 disp_fb_to_var(enum disp_pixel_format format,
			  struct fb_var_screeninfo *var)
{
	switch (format) {
	case DISP_FORMAT_ARGB_8888:
		var->bits_per_pixel = 32;
		var->transp.length = 8;
		var->red.length = 8;
		var->green.length = 8;
		var->blue.length = 8;
		var->blue.offset = 0;
		var->green.offset = var->blue.offset + var->blue.length;
		var->red.offset = var->green.offset + var->green.length;
		var->transp.offset = var->red.offset + var->red.length;
		break;
	case DISP_FORMAT_ABGR_8888:
		var->bits_per_pixel = 32;
		var->transp.length = 8;
		var->red.length = 8;
		var->green.length = 8;
		var->blue.length = 8;
		var->red.offset = 0;
		var->green.offset = var->red.offset + var->red.length;
		var->blue.offset = var->green.offset + var->green.length;
		var->transp.offset = var->blue.offset + var->blue.length;
		break;
	case DISP_FORMAT_RGBA_8888:
		var->bits_per_pixel = 32;
		var->transp.length = 8;
		var->red.length = 8;
		var->green.length = 8;
		var->blue.length = 8;
		var->transp.offset = 0;
		var->blue.offset = var->transp.offset + var->transp.length;
		var->green.offset = var->blue.offset + var->blue.length;
		var->red.offset = var->green.offset + var->green.length;
		break;
	case DISP_FORMAT_BGRA_8888:
		var->bits_per_pixel = 32;
		var->transp.length = 8;
		var->red.length = 8;
		var->green.length = 8;
		var->blue.length = 8;
		var->transp.offset = 0;
		var->red.offset = var->transp.offset + var->transp.length;
		var->green.offset = var->red.offset + var->red.length;
		var->blue.offset = var->green.offset + var->green.length;
		break;
	case DISP_FORMAT_RGB_888:
		var->bits_per_pixel = 24;
		var->red.length = 8;
		var->green.length = 8;
		var->blue.length = 8;
		var->blue.offset = 0;
		var->green.offset = var->blue.offset + var->blue.length;
		var->red.offset = var->green.offset + var->green.length;

		break;
	case DISP_FORMAT_BGR_888:
		var->bits_per_pixel = 24;
		var->transp.length = 0;
		var->red.length = 8;
		var->green.length = 8;
		var->blue.length = 8;
		var->red.offset = 0;
		var->green.offset = var->red.offset + var->red.length;
		var->blue.offset = var->green.offset + var->green.length;

		break;
	case DISP_FORMAT_RGB_565:
		var->bits_per_pixel = 16;
		var->transp.length = 0;
		var->red.length = 5;
		var->green.length = 6;
		var->blue.length = 5;
		var->blue.offset = 0;
		var->green.offset = var->blue.offset + var->blue.length;
		var->red.offset = var->green.offset + var->green.length;

		break;
	case DISP_FORMAT_BGR_565:
		var->bits_per_pixel = 16;
		var->transp.length = 0;
		var->red.length = 5;
		var->green.length = 6;
		var->blue.length = 5;
		var->red.offset = 0;
		var->green.offset = var->red.offset + var->red.length;
		var->blue.offset = var->green.offset + var->green.length;

		break;
	case DISP_FORMAT_ARGB_4444:
		var->bits_per_pixel = 16;
		var->transp.length = 4;
		var->red.length = 4;
		var->green.length = 4;
		var->blue.length = 4;
		var->blue.offset = 0;
		var->green.offset = var->blue.offset + var->blue.length;
		var->red.offset = var->green.offset + var->green.length;
		var->transp.offset = var->red.offset + var->red.length;

		break;
	case DISP_FORMAT_ABGR_4444:
		var->bits_per_pixel = 16;
		var->transp.length = 4;
		var->red.length = 4;
		var->green.length = 4;
		var->blue.length = 4;
		var->red.offset = 0;
		var->green.offset = var->red.offset + var->red.length;
		var->blue.offset = var->green.offset + var->green.length;
		var->transp.offset = var->blue.offset + var->blue.length;

		break;
	case DISP_FORMAT_RGBA_4444:
		var->bits_per_pixel = 16;
		var->transp.length = 4;
		var->red.length = 4;
		var->green.length = 5;
		var->blue.length = 4;
		var->transp.offset = 0;
		var->blue.offset = var->transp.offset + var->transp.length;
		var->green.offset = var->blue.offset + var->blue.length;
		var->red.offset = var->green.offset + var->green.length;

		break;
	case DISP_FORMAT_BGRA_4444:
		var->bits_per_pixel = 16;
		var->transp.length = 4;
		var->red.length = 4;
		var->green.length = 4;
		var->blue.length = 4;
		var->transp.offset = 0;
		var->red.offset = var->transp.offset + var->transp.length;
		var->green.offset = var->red.offset + var->red.length;
		var->blue.offset = var->green.offset + var->green.length;

		break;
	case DISP_FORMAT_ARGB_1555:
		var->bits_per_pixel = 16;
		var->transp.length = 1;
		var->red.length = 5;
		var->green.length = 5;
		var->blue.length = 5;
		var->blue.offset = 0;
		var->green.offset = var->blue.offset + var->blue.length;
		var->red.offset = var->green.offset + var->green.length;
		var->transp.offset = var->red.offset + var->red.length;

		break;
	case DISP_FORMAT_ABGR_1555:
		var->bits_per_pixel = 16;
		var->transp.length = 1;
		var->red.length = 5;
		var->green.length = 5;
		var->blue.length = 5;
		var->red.offset = 0;
		var->green.offset = var->red.offset + var->red.length;
		var->blue.offset = var->green.offset + var->green.length;
		var->transp.offset = var->blue.offset + var->blue.length;

		break;
	case DISP_FORMAT_RGBA_5551:
		var->bits_per_pixel = 16;
		var->transp.length = 1;
		var->red.length = 5;
		var->green.length = 5;
		var->blue.length = 5;
		var->transp.offset = 0;
		var->blue.offset = var->transp.offset + var->transp.length;
		var->green.offset = var->blue.offset + var->blue.length;
		var->red.offset = var->green.offset + var->green.length;

		break;
	case DISP_FORMAT_BGRA_5551:
		var->bits_per_pixel = 16;
		var->transp.length = 1;
		var->red.length = 5;
		var->green.length = 5;
		var->blue.length = 5;
		var->transp.offset = 0;
		var->red.offset = var->transp.offset + var->transp.length;
		var->green.offset = var->red.offset + var->red.length;
		var->blue.offset = var->green.offset + var->green.length;

		break;
	default:
		__wrn("[FB]not support format %d\n", format);
	}

	__inf
	    ("fmt%d para: %dbpp, a(%d,%d),r(%d,%d),g(%d,%d),b(%d,%d)\n",
	     (int)format, (int)var->bits_per_pixel, (int)var->transp.offset,
	     (int)var->transp.length, (int)var->red.offset,
	     (int)var->red.length, (int)var->green.offset,
	     (int)var->green.length, (int)var->blue.offset,
	     (int)var->blue.length);

	return 0;
}

static s32 var_to_disp_fb(struct disp_fb_info *fb,
			  struct fb_var_screeninfo *var,
			  struct fb_fix_screeninfo *fix)
{
	if (var->nonstd == 0) {
		/* argb */
		switch (var->bits_per_pixel) {
		case 32:
			if (var->red.offset == 16 && var->green.offset == 8
			    && var->blue.offset == 0)
				fb->format = DISP_FORMAT_ARGB_8888;
			else if (var->blue.offset == 24
				 && var->green.offset == 16
				 && var->red.offset == 8)
				fb->format = DISP_FORMAT_BGRA_8888;
			else if (var->blue.offset == 16
				 && var->green.offset == 8
				 && var->red.offset == 0)
				fb->format = DISP_FORMAT_ABGR_8888;
			else if (var->red.offset == 24
				 && var->green.offset == 16
				 && var->blue.offset == 8)
				fb->format = DISP_FORMAT_RGBA_8888;
			else
				__wrn
				    ("invalid fmt,off<a:%d,r:%d,g:%d,b:%d>\n",
				     var->transp.offset, var->red.offset,
				     var->green.offset, var->blue.offset);

			break;
		case 24:
			if ((var->red.offset == 16)
			 && (var->green.offset == 8)
			 && (var->blue.offset == 0)) {	/* rgb */
				fb->format = DISP_FORMAT_RGB_888;
			} else if ((var->blue.offset == 16)
			       && (var->green.offset == 8)
			       && (var->red.offset == 0)) {	/* bgr */
				fb->format = DISP_FORMAT_BGR_888;
			} else {
				__wrn
				    ("invalid fmt,off<a:%d,r:%d,g:%d,b:%d>\n",
				     var->transp.offset, var->red.offset,
				     var->green.offset, var->blue.offset);
			}

			break;
		case 16:
			if (var->red.offset == 11 && var->green.offset == 5
			    && var->blue.offset == 0) {
				fb->format = DISP_FORMAT_RGB_565;
			} else if (var->blue.offset == 11
				   && var->green.offset == 5
				   && var->red.offset == 0) {
				fb->format = DISP_FORMAT_BGR_565;
			} else if (var->transp.offset == 12
				   && var->red.offset == 8
				   && var->green.offset == 4
				   && var->blue.offset == 0) {
				fb->format = DISP_FORMAT_ARGB_4444;
			} else if (var->transp.offset == 12
				   && var->blue.offset == 8
				   && var->green.offset == 4
				   && var->red.offset == 0) {
				fb->format = DISP_FORMAT_ABGR_4444;
			} else if (var->red.offset == 12
				   && var->green.offset == 8
				   && var->blue.offset == 4
				   && var->transp.offset == 0) {
				fb->format = DISP_FORMAT_RGBA_4444;
			} else if (var->blue.offset == 12
				   && var->green.offset == 8
				   && var->red.offset == 4
				   && var->transp.offset == 0) {
				fb->format = DISP_FORMAT_BGRA_4444;
			} else if (var->transp.offset == 15
				   && var->red.offset == 10
				   && var->green.offset == 5
				   && var->blue.offset == 0) {
				fb->format = DISP_FORMAT_ARGB_1555;
			} else if (var->transp.offset == 15
				   && var->blue.offset == 10
				   && var->green.offset == 5
				   && var->red.offset == 0) {
				fb->format = DISP_FORMAT_ABGR_1555;
			} else if (var->red.offset == 11
				   && var->green.offset == 6
				   && var->blue.offset == 1
				   && var->transp.offset == 0) {
				fb->format = DISP_FORMAT_RGBA_5551;
			} else if (var->blue.offset == 11
				   && var->green.offset == 6
				   && var->red.offset == 1
				   && var->transp.offset == 0) {
				fb->format = DISP_FORMAT_BGRA_5551;
			} else {
				__wrn
				    ("invalid fmt,off<a:%d,r:%d,g:%d,b:%d>\n",
				     var->transp.offset, var->red.offset,
				     var->green.offset, var->blue.offset);
			}

			break;

		default:
			__wrn("invalid bits_per_pixel :%d\n",
			      var->bits_per_pixel);
			return -EINVAL;
		}
	}
	__inf
	    ("format%d,para:%dbpp,a(%d,%d),r(%d,%d),g(%d,%d),b(%d,%d)\n",
	     (int)fb->format, (int)var->bits_per_pixel, (int)var->transp.offset,
	     (int)var->transp.length, (int)var->red.offset,
	     (int)var->red.length, (int)var->green.offset,
	     (int)var->green.length, (int)var->blue.offset,
	     (int)var->blue.length);

	fb->size[0].width = var->xres_virtual;
	fb->size[1].width = var->xres_virtual;
	fb->size[2].width = var->xres_virtual;

	fix->line_length = (var->xres_virtual * var->bits_per_pixel) / 8;

	return 0;
}

static int sunxi_fb_open(struct fb_info *info, int user)
{
	u32 num_screens;
	u32 sel = 0;

	num_screens = bsp_disp_feat_get_num_screens();
	for (sel = 0; sel < num_screens; sel++) {
		if (sel == g_fbi.fb_mode[info->node]) {
			struct disp_manager *mgr = g_disp_drv.mgr[sel];

			if (mgr && mgr->device) {
				if (mgr->device->is_enabled) {
					if (!mgr->device->is_enabled(mgr->device)) {
						if (mgr->device->enable)
							mgr->device->enable(mgr->device);
						mgr->set_layer_config(mgr, &g_fbi.config[sel],
								      1);
						disp_delay_ms(20);
					}
				}
			}
		}
	}
	return 0;
}
static int sunxi_fb_release(struct fb_info *info, int user)
{
	return 0;
}

static int fb_wait_for_vsync(struct fb_info *info)
{
	unsigned long count;
	u32 sel = 0;
	int ret;
	int num_screens;

	num_screens = bsp_disp_feat_get_num_screens();

	for (sel = 0; sel < num_screens; sel++) {
		if (sel == g_fbi.fb_mode[info->node]) {
			struct disp_manager *mgr = g_disp_drv.mgr[sel];

			if (!mgr || !mgr->device
			    || (mgr->device->is_enabled == NULL))
				return 0;

			if (mgr->device->is_enabled(mgr->device) == 0)
				return 0;

			count = g_fbi.wait_count[sel];
			ret =
			    wait_event_interruptible_timeout(g_fbi.wait[sel],
							     count !=
							     g_fbi.
							     wait_count[sel],
							     msecs_to_jiffies
							     (50));
			if (ret == 0) {
				__inf("timeout\n");
				return -ETIMEDOUT;
			}
		}
	}

	return 0;
}

static int sunxi_fb_pan_display(struct fb_var_screeninfo *var,
				struct fb_info *info)
{
	u32 sel = 0;
	u32 num_screens;
#if defined(CONFIG_SUNXI_DISP2_FB_ROTATION_SUPPORT)
	s32 type_switch_flag = 0;
	u32 y_offset = var->yoffset;
#endif
	int need_wait_vsync = 1;

	num_screens = bsp_disp_feat_get_num_screens();

	for (sel = 0; sel < num_screens; sel++) {
		if (sel == g_fbi.fb_mode[info->node]) {
			u32 buffer_num = 1;
			s32 chan = g_fbi.layer_hdl[info->node][0];
			s32 layer_id = g_fbi.layer_hdl[info->node][1];
			struct disp_layer_config config;
			struct disp_manager *mgr = g_disp_drv.mgr[sel];
#if defined(CONFIG_SUNXI_DISP2_FB_ROTATION_SUPPORT)
			struct disp_rect dirty_rect;
#endif

			memset(&config, 0, sizeof(struct disp_layer_config));
			if (mgr && mgr->get_layer_config
			    && mgr->set_layer_config) {
				config.channel = chan;
				config.layer_id = layer_id;
				if (mgr->get_layer_config(mgr, &config, 1)
				    != 0) {
					__wrn
					    ("fb%d,get_lyr_cfg(%d,%d,%d)fail\n",
					     info->node, sel, chan, layer_id);
					return -1;
				}

#if defined(CONFIG_SUNXI_DISP2_FB_ROTATION_SUPPORT)
				if (mgr->rot_sw) {
					if (0 != var->reserved[2] &&
					    0 != var->reserved[3]) {
						dirty_rect.x = var->reserved[0];
						dirty_rect.y = var->reserved[1];
						dirty_rect.width =
						    var->reserved[2];
						dirty_rect.height =
						    var->reserved[3];
					} else {
						dirty_rect.x = 0;
						dirty_rect.y = 0;
						dirty_rect.width = 0;
						dirty_rect.height = 0;
						return 0;
					}

					if (mgr->rot_sw->checkout) {
						type_switch_flag =
						    mgr->rot_sw->checkout(
							mgr->rot_sw, &config);
						if (type_switch_flag == 1) {
							dirty_rect.x = 0;
							dirty_rect.y = 0;
							dirty_rect.width =
							    config.info.fb
								.size[0]
								.width;
							dirty_rect.height =
							    config.info.fb
								.size[0]
								.height;
						}
					}
				}
#endif
				config.info.fb.crop.x =
				    ((long long)var->xoffset) << 32;
				config.info.fb.crop.y =
				    ((unsigned long long)(var->yoffset)) << 32;
				config.info.fb.crop.width =
				    ((long long)var->xres) << 32;
				config.info.fb.crop.height =
				    ((long long)(var->yres / buffer_num)) << 32;

#if defined(CONFIG_SUNXI_DISP2_FB_ROTATION_SUPPORT)
				if (mgr->rot_sw && mgr->rot_sw->apply) {
					if (0 !=
					    mgr->rot_sw->apply(mgr->rot_sw,
							       &config,
							       dirty_rect)) {
						__wrn("not need to rot sw\n");
						return 0;
					}
					config.info.fb.crop.x =
						((unsigned long long)(y_offset)) << 32;
					need_wait_vsync = 0;
				}
#endif
#if defined(CONFIG_SUNXI_DISP2_FB_HW_ROTATION_SUPPORT)
				if (g_fbi.fb_rot[sel])
					g_fbi.fb_rot[sel]->apply(g_fbi.fb_rot[sel], &config);
#endif
				if (mgr->set_layer_config(mgr, &config, 1)
				    != 0) {
					__wrn
					    ("fb%d,set_lyr_cfg(%d,%d,%d)fail\n",
					     info->node, sel, chan, layer_id);
					return -1;
				} else {
					memcpy(&g_fbi.config[sel], &config, sizeof(struct disp_layer_config));
				}
			}
		}
	}

	if (need_wait_vsync == 1)
		fb_wait_for_vsync(info);

	return 0;
}

static int sunxi_fb_check_var(struct fb_var_screeninfo *var,
			      struct fb_info *info)
{
	struct disp_fb_info disp_fb;
	struct fb_fix_screeninfo fix;

	if (var_to_disp_fb(&disp_fb, var, &fix) != 0) {
		switch (var->bits_per_pixel) {
		case 8:
		case 4:
		case 2:
		case 1:
			disp_fb_to_var(DISP_FORMAT_ARGB_8888, var);
			break;

		case 19:
		case 18:
		case 16:
			disp_fb_to_var(DISP_FORMAT_RGB_565, var);
			break;

		case 32:
		case 28:
		case 25:
		case 24:
			disp_fb_to_var(DISP_FORMAT_ARGB_8888, var);
			break;

		default:
			return -EINVAL;
		}
	}

	return 0;
}

static int sunxi_fb_blank(int blank_mode, struct fb_info *info)
{
	u32 sel = 0;
	u32 num_screens;

	num_screens = bsp_disp_feat_get_num_screens();

	__inf("sunxi_fb_blank,mode:%d\n", blank_mode);

	for (sel = 0; sel < num_screens; sel++) {
		if (sel == g_fbi.fb_mode[info->node]) {
			s32 chan = g_fbi.layer_hdl[info->node][0];
			s32 layer_id = g_fbi.layer_hdl[info->node][1];
			struct disp_layer_config config;
			struct disp_manager *mgr = g_disp_drv.mgr[sel];

			memcpy(&config, &g_fbi.config[sel], sizeof(struct disp_layer_config));
			if (blank_mode == FB_BLANK_POWERDOWN) {
				if (mgr && mgr->set_layer_config) {
					config.enable = 0;
					mgr->set_layer_config(mgr, &config, 1);
				}
			} else {
				if (mgr && mgr->set_layer_config) {
					config.channel = chan;
					config.layer_id = layer_id;
					config.enable = 1;
					mgr->set_layer_config(mgr, &config, 1);
				}
			}
		}
	}
	return 0;
}

static int sunxi_fb_cursor(struct fb_info *info, struct fb_cursor *cursor)
{
	__inf("sunxi_fb_cursor\n");

	return 0;
}

static int sunxi_fb_mmap(struct fb_info *info, struct vm_area_struct *vma)
{
	unsigned int off = vma->vm_pgoff << PAGE_SHIFT;

	if (off < info->fix.smem_len) {
#if defined(CONFIG_ION)
		return g_fbi.mem[info->node]->p_item->dmabuf->ops->mmap(
		    g_fbi.mem[info->node]->p_item->dmabuf, vma);

#else /* CONFIG_ION */
		return dma_mmap_writecombine(g_fbi.dev, vma, info->screen_base,
				info->fix.smem_start,
				info->fix.smem_len);
#endif /* CONFIG_ION */
	}

	return -EINVAL;
}


void DRV_disp_int_process(u32 sel)
{
	g_fbi.wait_count[sel]++;
	wake_up_interruptible(&g_fbi.wait[sel]);
}

static inline u32 convert_bitfield(int val, struct fb_bitfield *bf)
{
	u32 mask = ((1 << bf->length) - 1) << bf->offset;

	return (val << bf->offset) & mask;
}

static int sunxi_fb_setcolreg(unsigned int regno, unsigned int red, unsigned int green,
			      unsigned int blue, unsigned int transp,
			      struct fb_info *info)
{
	u32 val;
	u32 ret = 0;

	switch (info->fix.visual) {
	case FB_VISUAL_PSEUDOCOLOR:
		ret = -EINVAL;
		break;
	case FB_VISUAL_TRUECOLOR:
		if (regno < 16) {
			val = convert_bitfield(transp, &info->var.transp) |
			    convert_bitfield(red, &info->var.red) |
			    convert_bitfield(green, &info->var.green) |
			    convert_bitfield(blue, &info->var.blue);
			__inf("regno=%2d,a=%2X,r=%2X,g=%2X,b=%2X,result=%08X\n",
			      regno, transp, red, green, blue,
			      val);
			((u32 *) info->pseudo_palette)[regno] = val;
		} else {
			ret = 0;
		}
		break;
	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}

static int sunxi_fb_setcmap(struct fb_cmap *cmap, struct fb_info *info)
{
	unsigned int j, r = 0;
	unsigned char hred, hgreen, hblue, htransp = 0xff;
	unsigned short *red, *green, *blue, *transp;

	__inf("Fb_setcmap, cmap start:%d len:%d, %dbpp\n", cmap->start,
	      cmap->len, info->var.bits_per_pixel);

	red = cmap->red;
	green = cmap->green;
	blue = cmap->blue;
	transp = cmap->transp;

	for (j = 0; j < cmap->len; j++) {
		hred = *red++;
		hgreen = *green++;
		hblue = *blue++;
		if (transp)
			htransp = (*transp++) & 0xff;
		else
			htransp = 0xff;

		r = sunxi_fb_setcolreg(cmap->start + j, hred, hgreen, hblue,
				       htransp, info);
		if (r)
			return r;
	}

	return 0;
}

struct fb_dmabuf_export {
	int fd;
	__u32 flags;
};

/*custom ioctl command here*/
#define FBIO_CACHE_SYNC         0x4630
#define FBIO_ENABLE_CACHE       0x4631
#define FBIO_GET_IONFD          0x4632
#define FBIO_GET_PHY_ADDR       0x4633
#define FBIOGET_DMABUF         _IOR('F', 0x21, struct fb_dmabuf_export)

#if !defined(CONFIG_ION)

struct sunxi_dmabuf_info {
	struct sg_table *s_sg_table;
	struct fb_info *info;
	struct kref ref;
	void *ker_addr;
};

static struct sunxi_dmabuf_info *sunxi_info[SUNXI_FB_MAX];

void sunxi_info_free(struct kref *kref)
{
	struct sunxi_dmabuf_info *s_info;
	struct fb_info *f_info;

	s_info = container_of(kref, struct sunxi_dmabuf_info, ref);
	f_info = s_info->info;

	lock_fb_info(f_info);

	sg_free_table(s_info->s_sg_table);
	Fb_unmap_kernel(s_info->ker_addr);
	kfree(s_info->s_sg_table);
	kfree(s_info);
	sunxi_info[f_info->node%SUNXI_FB_MAX] = NULL;

	unlock_fb_info(f_info);
}

static struct sg_table *sunxi_map_dma_buf(struct dma_buf_attachment *attachment,
					enum dma_data_direction direction)
{
	struct sunxi_dmabuf_info *s_info;

	s_info = (struct sunxi_dmabuf_info *)attachment->dmabuf->priv;

	kref_get(&s_info->ref);

	return s_info->s_sg_table;
}

static void sunxi_unmap_dma_buf(struct dma_buf_attachment *attachment,
						struct sg_table *sg_tab,
						enum dma_data_direction direction)
{
	struct sunxi_dmabuf_info *s_info;

	s_info = (struct sunxi_dmabuf_info *)attachment->dmabuf->priv;

	kref_put(&s_info->ref, sunxi_info_free);
}

static int sunxi_user_mmap(struct dma_buf *buff, struct vm_area_struct *vma)
{
	struct sunxi_dmabuf_info *s_info = (struct sunxi_dmabuf_info *)buff->priv;
	struct sg_table *table = s_info->s_sg_table;
	unsigned long addr = vma->vm_start;
	unsigned long offset = vma->vm_pgoff * PAGE_SIZE;
	struct scatterlist *sg;
	int i;
	int ret;

	vma->vm_page_prot = pgprot_writecombine(vma->vm_page_prot);
	for_each_sg(table->sgl, sg, table->nents, i) {
		struct page *page = sg_page(sg);
		unsigned long remainder = vma->vm_end - addr;
		unsigned long len = sg->length;

		if (offset >= sg->length) {
			offset -= sg->length;
			continue;
		} else if (offset) {
			page += offset / PAGE_SIZE;
			len = sg->length - offset;
			offset = 0;
		}
		len = min(len, remainder);
		ret = remap_pfn_range(vma, addr, page_to_pfn(page), len,
				      vma->vm_page_prot);
		if (ret)
			return ret;
		addr += len;
		if (addr >= vma->vm_end)
			return 0;
	}

	return 0;
}

static void sunxi_dma_buf_release(struct dma_buf *buff)
{
	struct sunxi_dmabuf_info *s_info;

	s_info = (struct sunxi_dmabuf_info *)buff->priv;

	kref_put(&s_info->ref, sunxi_info_free);
}

static int sunxi_dma_buf_attach(struct dma_buf *buff,
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 19, 0)
			struct device *dev,
#endif
			struct dma_buf_attachment *attachment)
{
	struct sunxi_dmabuf_info *s_info;

	s_info = (struct sunxi_dmabuf_info *)buff->priv;
	attachment->priv = (void *)s_info;

	kref_get(&s_info->ref);
	return 0;
}

static void sunxi_dma_buf_detach(struct dma_buf *buff, struct dma_buf_attachment *attachment)
{
	struct sunxi_dmabuf_info *s_info;

	s_info = (struct sunxi_dmabuf_info *)buff->priv;

	attachment->priv = NULL;

	kref_put(&s_info->ref, sunxi_info_free);
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 12, 0)
static void *sunxi_dma_buf_kmap(struct dma_buf *buff, unsigned long page_num)
{
	struct sunxi_dmabuf_info *s_info;

	s_info = (struct sunxi_dmabuf_info *)buff->priv;

	return s_info->ker_addr + page_num * PAGE_SIZE;
}

static void sunxi_dma_buf_kunmap(struct dma_buf *buff, unsigned long page_num,
			       void *ptr)
{

}
#endif

static struct dma_buf_ops sunxi_dma_buf_ops = {
	.attach = sunxi_dma_buf_attach,
	.detach = sunxi_dma_buf_detach,
	.map_dma_buf = sunxi_map_dma_buf,
	.unmap_dma_buf = sunxi_unmap_dma_buf,
	.mmap = sunxi_user_mmap,
	.release = sunxi_dma_buf_release,
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 12, 0)
	.kmap_atomic = sunxi_dma_buf_kmap,
	.kunmap_atomic = sunxi_dma_buf_kunmap,
	.kmap = sunxi_dma_buf_kmap,
	.kunmap = sunxi_dma_buf_kunmap,
#endif
};
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 12, 0)
static struct dma_buf *sunxi_share_dma_buf(struct fb_info *info)
{
	struct dma_buf *dmabuf = NULL;
#if !defined(CONFIG_ION)
	DEFINE_DMA_BUF_EXPORT_INFO(exp_info);
	struct sunxi_dmabuf_info *s_info;
#else
	struct ion_handle *handle;
#endif
	if (info->fix.smem_start == 0 || info->fix.smem_len == 0)
		return NULL;

#if defined(CONFIG_ION)
	handle = g_fbi.mem[info->node%SUNXI_FB_MAX]->handle;
	dmabuf = ion_share_dma_buf(g_disp_drv.ion_mgr.client, handle);
#else /* CONFIG_ION */
	s_info = sunxi_info[info->node%SUNXI_FB_MAX];
	if (s_info == NULL) {
		s_info = kzalloc(sizeof(struct sunxi_dmabuf_info), GFP_KERNEL);
		if (s_info == NULL)
			goto ret_err;
		s_info->s_sg_table = kzalloc(sizeof(struct sg_table), GFP_KERNEL);
		if (!s_info->s_sg_table) {
			kfree(s_info);
			goto ret_err;
		}
		if (sg_alloc_table(s_info->s_sg_table, 1, GFP_KERNEL)) {
			kfree(s_info->s_sg_table);
			kfree(s_info);
			goto ret_err;
		}
		sg_set_page(s_info->s_sg_table->sgl, pfn_to_page(PFN_DOWN(info->fix.smem_start)),
				info->fix.smem_len, info->fix.smem_start&PAGE_MASK);
		s_info->info = info;
		kref_init(&s_info->ref);
		s_info->ker_addr = (void *)((char *)Fb_map_kernel(info->fix.smem_start
						+ (info->fix.smem_start&PAGE_MASK), info->fix.smem_len));
		sunxi_info[info->node%SUNXI_FB_MAX] = s_info;
	} else {
		kref_get(&s_info->ref);
	}
	exp_info.ops = &sunxi_dma_buf_ops;
	exp_info.size = info->fix.smem_len;
	exp_info.flags = O_RDWR;
	exp_info.priv = s_info;

	dmabuf = dma_buf_export(&exp_info);
	if (IS_ERR_OR_NULL(dmabuf)) {
		kref_put(&s_info->ref, sunxi_info_free);
		return dmabuf;
	}
#endif /* CONFIG_ION */
	return dmabuf;
#if !defined(CONFIG_ION)
ret_err:
	__wrn("%s, alloc mem err...\n", __func__);

	return NULL;
#endif
}
#endif /* LINUX_VERSION_CODE < KERNEL_VERSION(4, 12, 0) */

static int sunxi_fb_ioctl(struct fb_info *info, unsigned int cmd,
			  unsigned long arg)
{
	long ret = 0;
	void __user *argp = (void __user *)arg;
	unsigned long karg[4];
#if defined(CONFIG_ION)
	void *get_phy_addr = NULL;
#endif
	switch (cmd) {
#if 0
	case FBIOGET_VBLANK:
		{
			struct fb_vblank vblank;
			struct disp_video_timings tt;
			u32 line = 0;
			u32 sel;

			sel =
			    (g_fbi.fb_mode[info->node] ==
			     FB_MODE_SCREEN1) ? 1 : 0;
			line = bsp_disp_get_cur_line(sel);
			bsp_disp_get_timming(sel, &tt);

			memset(&vblank, 0, sizeof(struct fb_vblank));
			vblank.flags |= FB_VBLANK_HAVE_VBLANK;
			vblank.flags |= FB_VBLANK_HAVE_VSYNC;
			if (line <= (tt.ver_total_time - tt.y_res))
				vblank.flags |= FB_VBLANK_VBLANKING;
			if ((line > tt.ver_front_porch)
			    && (line <
				(tt.ver_front_porch + tt.ver_sync_time))) {
				vblank.flags |= FB_VBLANK_VSYNCING;
			}

			if (copy_to_user
			    ((void __user *)arg, &vblank,
			     sizeof(struct fb_vblank)))
				ret = -EFAULT;

			break;
		}
#endif

	case FBIO_ENABLE_CACHE:
		{
			if (copy_from_user((void *)karg, argp,
					   sizeof(unsigned long)))
				return -EFAULT;
			g_fbi.mem_cache_flag[info->node] = (karg[0] == 1)?1:0;
			break;
		}

	case FBIO_WAITFORVSYNC:
		{
			/* ret = fb_wait_for_vsync(info); */
			break;
		}

	case FBIO_FREE:
		{
			int fb_id = 0;	/* fb0 */
			struct fb_info *fbinfo = g_fbi.fbinfo[fb_id];

			if ((!g_fbi.fb_enable[fb_id])
			    || (fbinfo != info)) {
				__wrn("%s, fb%d already release ? or fb_info mismatch: fbinfo=0x%p, info=0x%p\n",
				      __func__, fb_id, fbinfo, info);
				return -1;
			}

			__inf("### FBIO_FREE ###\n");

			fb_unmap_video_memory(fbinfo);
#if defined(CONFIG_SUNXI_DISP2_FB_HW_ROTATION_SUPPORT)
			if (g_fbi.fb_rot[fb_id])
				g_fbi.fb_rot[fb_id]->free(g_fbi.fb_rot[fb_id]);
#endif

			/* unbound fb0 from layer(1,0)  */
			g_fbi.layer_hdl[fb_id][0] = 0;
			g_fbi.layer_hdl[fb_id][1] = 0;
			g_fbi.fb_mode[fb_id] = 0;
			g_fbi.fb_enable[fb_id] = 0;
			break;
		}

	case FBIO_ALLOC:
		{
			int fb_id = 0;	/* fb0 */
			struct fb_info *fbinfo = g_fbi.fbinfo[fb_id];

			if (g_fbi.fb_enable[fb_id]
			    || (fbinfo != info)) {
				__wrn("%s, fb%d already enable ? or fb_info mismatch: fbinfo=0x%p, info=0x%p\n",
				      __func__, fb_id, fbinfo, info);
				return -1;
			}

			__inf("### FBIO_ALLOC ###\n");

			/* fb0 bound to layer(1,0)  */
			g_fbi.layer_hdl[fb_id][0] = 1;
			g_fbi.layer_hdl[fb_id][1] = 0;

			fb_map_video_memory(fbinfo);

			g_fbi.fb_mode[fb_id] = g_fbi.fb_para[fb_id].fb_mode;
			g_fbi.fb_enable[fb_id] = 1;
#if defined(CONFIG_SUNXI_DISP2_FB_HW_ROTATION_SUPPORT)
			g_fbi.fb_rot[fb_id] = fb_g2d_rot_create(
								info, fb_id, &g_fbi.config[fb_id]);
#endif
			break;
		}

	case FBIO_GET_IONFD:{
	if (argp) {
		if (copy_from_user((struct disp_ion_mem *)karg, argp,
				2 * sizeof(unsigned long)))
			return -EFAULT;
		}
#if defined(CONFIG_ION)
	if (argp == NULL) {
		ret = disp_get_ion_fd((struct disp_ion_mem *)g_fbi.mem[info->node]);
	} else {
		ret = disp_get_ion_fd((struct disp_ion_mem *)karg[0]);
	}
#endif
		break;
	}
	case FBIO_GET_PHY_ADDR:{
	if (argp) {
		if (copy_from_user((struct disp_ion_mem *)karg, argp,
				sizeof(dma_addr_t)))
			return -EFAULT;
		}
#if defined(CONFIG_ION)
	if (argp == NULL) {
		get_phy_addr = disp_get_phy_addr((struct disp_ion_mem *)g_fbi.mem[info->node]);
		return get_phy_addr == NULL ? -1 : 0;
	} else {
		get_phy_addr = disp_get_phy_addr((struct disp_ion_mem *)g_fbi.mem[info->node]);
		ret = get_phy_addr == NULL ? -1 : 0;
		get_phy_addr = get_phy_addr + karg[0];
		return ret;
	}
#endif
		break;
	}
	case FBIOGET_DMABUF:
	{
		struct dma_buf *dmabuf;
		struct fb_dmabuf_export k_ret = {-1, 0};

		ret = -1;
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 12, 0)
		dmabuf = sunxi_share_dma_buf(info);
#else
		dmabuf = g_fbi.mem[info->node%SUNXI_FB_MAX]->p_item->dmabuf;
#endif
		if (IS_ERR_OR_NULL(dmabuf))
			return PTR_ERR(dmabuf);

		k_ret.fd = dma_buf_fd(dmabuf, O_CLOEXEC);
		if (k_ret.fd < 0) {
			dma_buf_put(dmabuf);
			break;
		}

		if (copy_to_user(argp, &k_ret, sizeof(struct fb_dmabuf_export))) {
			__wrn("%s, copy to user err\n", __func__);
		}
		ret = 0;
		break;
	}
	default:
		break;
	}
	return ret;
}

static struct fb_ops dispfb_ops = {
	.owner = THIS_MODULE,
	.fb_open = sunxi_fb_open,
	.fb_release = sunxi_fb_release,
	.fb_pan_display = sunxi_fb_pan_display,
#if defined(CONFIG_COMPAT)
	.fb_compat_ioctl = sunxi_fb_ioctl,
#endif
	.fb_ioctl = sunxi_fb_ioctl,
	.fb_check_var = sunxi_fb_check_var,
	.fb_blank = sunxi_fb_blank,
	.fb_cursor = sunxi_fb_cursor,
	.fb_mmap = sunxi_fb_mmap,
#if defined(CONFIG_FB_CONSOLE_SUNXI)
	.fb_fillrect = cfb_fillrect,
	.fb_copyarea = cfb_copyarea,
	.fb_imageblit = cfb_imageblit,
#endif
	.fb_setcmap = sunxi_fb_setcmap,
	.fb_setcolreg = sunxi_fb_setcolreg,

};

static int Fb_copy_boot_fb(u32 sel, struct fb_info *info)
{
	enum {
		BOOT_FB_ADDR = 0,
		BOOT_FB_WIDTH,
		BOOT_FB_HEIGHT,
		BOOT_FB_BPP,
		BOOT_FB_STRIDE,
		BOOT_FB_CROP_L,
		BOOT_FB_CROP_T,
		BOOT_FB_CROP_R,
		BOOT_FB_CROP_B,
	};

	char *boot_fb_str = NULL;
	char *src_phy_addr = NULL;
	char *src_addr = NULL;
	char *src_addr_b = NULL;
	char *src_addr_e = NULL;
	int src_width = 0;
	int src_height = 0;
	int src_bpp = 0;
	int src_stride = 0;
	int src_cp_btyes = 0;
	int src_crop_l = 0;
	int src_crop_t = 0;
	int src_crop_r = 0;
	int src_crop_b = 0;

	char *dst_addr = NULL;
	int dst_width = 0;
	int dst_height = 0;
	int dst_bpp = 0;
	int dst_stride = 0;
	int ret;

	unsigned long map_offset;

	if (info == NULL) {
		__wrn("%s,%d: null pointer\n", __func__, __LINE__);
		return -1;
	}

	boot_fb_str = (char *)disp_boot_para_parse_str("boot_fb0");
	if (boot_fb_str != NULL) {
		int i = 0;
		char boot_fb[128] = { 0 };
		int len = strlen(boot_fb_str);

		if (sizeof(boot_fb) - 1 < len) {
			__wrn("need bigger array size[%d] for boot_fb\n", len);
			return -1;
		}
		memcpy((void *)boot_fb, (void *)boot_fb_str, len);
		boot_fb[len] = '\0';
		boot_fb_str = boot_fb;
		for (i = 0;; ++i) {
			char *p = strstr(boot_fb_str, ",");

			if (p != NULL)
				*p = '\0';
			if (i == BOOT_FB_ADDR) {
				ret = kstrtoul(boot_fb_str, 16,
				    (unsigned long *)&src_phy_addr);
				if (ret)
					pr_warn("parse src_phy_addr fail!\n");
			} else if (i == BOOT_FB_WIDTH) {
				ret = kstrtou32(boot_fb_str, 16, &src_width);
				if (ret)
					pr_warn("parse src_width fail!\n");
			} else if (i == BOOT_FB_HEIGHT) {
				ret = kstrtou32(boot_fb_str, 16, &src_height);
				if (ret)
					pr_warn("parse src_height fail!\n");
			} else if (i == BOOT_FB_BPP) {
				ret = kstrtou32(boot_fb_str, 16, &src_bpp);
				if (ret)
					pr_warn("parse src_bpp fail!\n");
			} else if (i == BOOT_FB_STRIDE) {
				ret = kstrtou32(boot_fb_str, 16, &src_stride);
				if (ret)
					pr_warn("parse src_stride fail!\n");
			} else if (i == BOOT_FB_CROP_L) {
				ret = kstrtou32(boot_fb_str, 16, &src_crop_l);
				if (ret)
					pr_warn("parse src_crop_l fail!\n");
			} else if (i == BOOT_FB_CROP_T) {
				ret = kstrtou32(boot_fb_str, 16, &src_crop_t);
				if (ret)
					pr_warn("parse src_crop_t fail!\n");
			} else if (i == BOOT_FB_CROP_R) {
				ret = kstrtou32(boot_fb_str, 16, &src_crop_r);
				if (ret)
					pr_warn("parse src_crop_r fail!\n");
			} else if (i == BOOT_FB_CROP_B) {
				ret = kstrtou32(boot_fb_str, 16, &src_crop_b);
				if (ret)
					pr_warn("parse src_crop_b fail!\n");
			} else {
				break;
			}

			if (p == NULL)
				break;
			boot_fb_str = p + 1;
		}
	} else {
		__wrn("no boot_fb0\n");
		return -1;
	}

	dst_addr = (char *)(info->screen_base);
	dst_width = info->var.xres;
	dst_height = info->var.yres;
	dst_bpp = info->var.bits_per_pixel;
	dst_stride = info->fix.line_length;

	if ((src_phy_addr == NULL)
	    || (src_width <= 0)
	    || (src_height <= 0)
	    || (src_stride <= 0)
	    || (src_bpp <= 0)
	    || (dst_addr == NULL)
	    || (dst_width <= 0)
	    || (dst_height <= 0)
	    || (dst_stride <= 0)
	    || (dst_bpp <= 0)
	    || (src_bpp != dst_bpp)) {
		__wrn
		    ("wrong para: src[phy_addr=%p,w=%d,h=%d,bpp=%d,stride=%d], dst[addr=%p,w=%d,h=%d,bpp=%d,stride=%d]\n",
		     src_phy_addr,
		     src_width, src_height, src_bpp, src_stride, dst_addr,
		     dst_width, dst_height, dst_bpp, dst_stride);
		return -1;
	}

	map_offset = (unsigned long)src_phy_addr + PAGE_SIZE
	    - PAGE_ALIGN((unsigned long)src_phy_addr + 1);
	src_addr = (char *)Fb_map_kernel_cache((unsigned long)src_phy_addr -
					       map_offset,
					       src_stride * src_height +
					       map_offset);
	if (src_addr == NULL) {
		__wrn("Fb_map_kernel_cache for src_addr failed\n");
		return -1;
	}

	src_addr_b = src_addr + map_offset;
	if ((src_crop_b > src_crop_t) &&
	    (src_height > src_crop_b - src_crop_t) &&
	    (src_crop_t >= 0) &&
	    (src_height >= src_crop_b)) {
		src_height = src_crop_b - src_crop_t;
		src_addr_b += (src_stride * src_crop_t);
	}
	if ((src_crop_r > src_crop_l)
	    && (src_width > src_crop_r - src_crop_l)
	    && (src_crop_l >= 0)
	    && (src_width >= src_crop_r)) {
		src_width = src_crop_r - src_crop_l;
		src_addr_b += (src_crop_l * src_bpp >> 3);
	}
	if (src_height < dst_height) {
		int dst_crop_t = (dst_height - src_height) >> 1;

		dst_addr += (dst_stride * dst_crop_t);
	} else if (src_height > dst_height) {
		__wrn("src_height(%d) > dst_height(%d),please cut the height\n",
		      src_height,
		      dst_height);
		Fb_unmap_kernel(src_addr);
		return -1;
	}
	if (src_width < dst_width) {
		int dst_crop_l = (dst_width - src_width) >> 1;

		dst_addr += (dst_crop_l * dst_bpp >> 3);
	} else if (src_width > dst_width) {
		__wrn("src_width(%d) > dst_width(%d),please cut the width!\n",
		      src_width,
		      dst_width);
		Fb_unmap_kernel(src_addr);
		return -1;
	}

	src_cp_btyes = src_width * src_bpp >> 3;
	src_addr_e = src_addr_b + src_stride * src_height;
	for (; src_addr_b != src_addr_e; src_addr_b += src_stride) {
		memcpy((void *)dst_addr, (void *)src_addr_b, src_cp_btyes);
		dst_addr += dst_stride;
	}

	Fb_unmap_kernel(src_addr);

	memblock_free((unsigned long)src_phy_addr, src_stride * src_height + 1024);
	return 0;
}

#if defined(SUPPORT_ROTATE)
static int rgb24_to_rgb32(const void *psrc, struct bmp_header *bmp_header,
			  struct fb_info *info, void *pdst, int zero_num)
{
	int srclinesize, dstlinesize, w, h;
	const unsigned char *psrcline = NULL, *psrcdot = NULL;
	unsigned char *pdstline = NULL, *pdstdot = NULL;
	int i = 0, j = 0;

	if (!psrc || !pdst || !bmp_header || !info) {
		__wrn("Invalid parameter\n");
		return -1;
	}

	w = (info->var.xres < bmp_header->width) ? info->var.xres
						 : bmp_header->width;
	h = (bmp_header->height & 0x80000000) ? (-bmp_header->
						 height) : (bmp_header->height);
	h = (info->var.yres < h) ? info->var.yres : h;

	srclinesize = w * 3 + zero_num;
	dstlinesize = info->var.xres * 4;
	psrcline = (const unsigned char *)psrc;
	pdstline = (unsigned char *)pdst;

	if (bmp_header->height & 0x80000000) {
		for (i = 0; i < h; ++i) {
			psrcdot = psrcline;
			pdstdot = pdstline;
			for (j = 0; j < w; ++j) {
				*pdstdot++ = psrcdot[j*3];
				*pdstdot++ = psrcdot[j*3+1];
				*pdstdot++ = psrcdot[j*3+2];
				*pdstdot++ = 0xff;
			}
			psrcline += srclinesize;
			pdstline += dstlinesize;
		}
	} else {
		for (i = h - 1; i >= 0; --i) {
			psrcdot = psrcline;
			pdstdot = pdstline;
			for (j = 0; j < w; ++j) {
				*pdstdot++ = psrcdot[j*3];
				*pdstdot++ = psrcdot[j*3+1];
				*pdstdot++ = psrcdot[j*3+2];
				*pdstdot++ = 0xff;
			}
			psrcline -= srclinesize;
			pdstline += dstlinesize;
		}
	}
	info->var.bits_per_pixel = 32;

	return 0;
}
#endif

#if defined(CONFIG_DECOMPRESS_LZMA)
int lzma_decode(uintptr_t paddr, struct fb_info *info)
{
	void *vaddr = NULL;
	long pos = 0;
	unsigned char *out = NULL;
	int ret = -1, i = 0;
	struct lzma_header lzma_head;
	struct bmp_header bmp_header;
	unsigned int x, y, bmp_bpix, fb_width, fb_height;
	unsigned int effective_width, effective_height;
	int zero_num = 0;
	struct sunxi_bmp_store bmp_info;
	void *screen_offset = NULL, *image_offset = NULL;
	char *tmp_buffer = NULL;
	char *bmp_data = NULL;

	if (!paddr || !info) {
		__wrn("Null pointer!\n");
		goto OUT;
	}

	vaddr = (void *)Fb_map_kernel(paddr, sizeof(struct lzma_header));
	if (vaddr == NULL) {
		__wrn("fb_map_kernel failed, paddr=0x%p,size=0x%x\n",
		      (void *)paddr, (unsigned int)sizeof(struct lzma_header));
		goto OUT;
	}

	memcpy(&lzma_head.signature[0], vaddr, sizeof(struct lzma_header));

	if ((lzma_head.signature[0] != 'L') ||
	    (lzma_head.signature[1] != 'Z') ||
	    (lzma_head.signature[2] != 'M') ||
	    (lzma_head.signature[3] != 'A')) {
		__wrn("this is not a LZMA file.\n");
		Fb_unmap_kernel(vaddr);
		goto OUT;
	}

	Fb_unmap_kernel(vaddr);

	out = kmalloc(lzma_head.original_file_size, GFP_KERNEL | __GFP_ZERO);
	if (!out) {
		__wrn("kmalloc outbuffer fail!\n");
		goto OUT;
	}

	vaddr = (void *)Fb_map_kernel(paddr, lzma_head.file_size +
						 sizeof(struct lzma_header));
	if (vaddr == NULL) {
		__wrn("fb_map_kernel failed, paddr=0x%p,size=0x%x\n",
		      (void *)paddr,
		      (unsigned int)(lzma_head.file_size +
				     sizeof(struct lzma_header)));
		goto FREE_OUT;
	}

	ret = unlzma((unsigned char *)(vaddr + sizeof(struct lzma_header)),
		     lzma_head.file_size, NULL, NULL, out, &pos, NULL);
	if (ret) {
		__wrn("unlzma fail:%d\n", ret);
		goto UMAPKERNEL;
	}

	memcpy(&bmp_header, out, sizeof(struct bmp_header));

	if ((bmp_header.signature[0] != 'B') ||
	    (bmp_header.signature[1] != 'M')) {
		__wrn("%s:this is not a bmp picture.\n", __func__);
		goto UMAPKERNEL;
	}

	bmp_bpix = bmp_header.bit_count / 8;

	if ((bmp_bpix != 3) && (bmp_bpix != 4)) {
		__wrn("%s:not support bmp format:%d.\n", __func__, bmp_bpix);
		goto UMAPKERNEL;
	}

	x = bmp_header.width;
	y = (bmp_header.height & 0x80000000) ? (-bmp_header.height)
					     : (bmp_header.height);
	if (bmp_bpix == 3)
		zero_num = (4 - ((3 * x) % 4)) & 3;
	fb_width = info->var.xres;
	fb_height = info->var.yres;
	bmp_info.x = x;
	bmp_info.y = y;
	bmp_info.bit = bmp_header.bit_count;
	bmp_info.buffer = (void *__force)(info->screen_base);

	if (bmp_bpix == 3)
		info->var.bits_per_pixel = 24;
	else if (bmp_bpix == 4)
		info->var.bits_per_pixel = 32;
	else
		info->var.bits_per_pixel = 32;

	tmp_buffer = (char *)bmp_info.buffer;
	screen_offset = (void *)bmp_info.buffer;
	bmp_data =
	    (char *)(out + bmp_header.data_offset + sizeof(struct lzma_header));
	image_offset = (void *)bmp_data;
	effective_width = (fb_width < x) ? fb_width : x;
	effective_height = (fb_height < y) ? fb_height : y;

	if (bmp_header.height & 0x80000000) {
#if defined(SUPPORT_ROTATE)
		if (info->var.bits_per_pixel == 24) {
			screen_offset =
			    (void *)((void *__force)info->screen_base +
				     (fb_width * (abs(fb_height - y) / 2) +
				      abs(fb_width - x) / 2) *
					 4);
			rgb24_to_rgb32(image_offset, &bmp_header, info,
				       screen_offset, zero_num);
		} else
#endif
		{
			screen_offset =
			    (void *)((void *__force)info->screen_base +
				     (fb_width * (abs(fb_height - y) / 2) +
				      abs(fb_width - x) / 2) *
					 (info->var.bits_per_pixel >> 3));
			for (i = 0; i < effective_height; i++) {
				memcpy((void *)screen_offset, image_offset,
				       effective_width *
					   (info->var.bits_per_pixel >> 3));
				screen_offset =
				    (void *)(screen_offset +
					     fb_width *
						 (info->var.bits_per_pixel >>
						  3));
				image_offset =
				    (void *)image_offset +
				    x * (info->var.bits_per_pixel >> 3);
			}
		}

	} else {

#if defined(SUPPORT_ROTATE)
		if (info->var.bits_per_pixel == 24) {
			screen_offset =
			    (void *)((void *__force)info->screen_base +
				     (fb_width * (abs(fb_height - y) / 2) +
				      abs(fb_width - x) / 2) *
					 4);

			image_offset =
			    (void *)bmp_data +
			    (effective_height - 1) * (x * 3 + zero_num);
			rgb24_to_rgb32(image_offset, &bmp_header, info,
				       screen_offset, zero_num);
		} else
#endif
		{
			screen_offset =
			    (void *)((void *__force)info->screen_base +
				     (fb_width * (abs(fb_height - y) / 2) +
				      abs(fb_width - x) / 2) *
					 (info->var.bits_per_pixel >> 3));

			image_offset = (void *)bmp_data +
				       (effective_height - 1) * x *
					   (info->var.bits_per_pixel >> 3);
			for (i = effective_height - 1; i >= 0; i--) {
				memcpy((void *)screen_offset, image_offset,
				       effective_width *
					   (info->var.bits_per_pixel >> 3));
				screen_offset =
				    (void *)(screen_offset +
					     fb_width *
						 (info->var.bits_per_pixel >>
						  3));
				image_offset =
				    (void *)bmp_data +
				    i * x * (info->var.bits_per_pixel >> 3);
			}
		}
	}

UMAPKERNEL:
	Fb_unmap_kernel(vaddr);
FREE_OUT:
	kfree(out);
OUT:
	return ret;
}
#endif

static int Fb_map_kernel_logo(u32 sel, struct fb_info *info)
{
	void *vaddr = NULL;
	uintptr_t paddr = 0;
	void *screen_offset = NULL, *image_offset = NULL;
	char *tmp_buffer = NULL;
	char *bmp_data = NULL;
	struct sunxi_bmp_store s_bmp_info;
	struct sunxi_bmp_store *bmp_info = &s_bmp_info;
	struct bmp_pad_header bmp_pad_header;
	struct bmp_header *bmp_header;
	int zero_num = 0;
	unsigned int x, y, bmp_bpix, fb_width, fb_height;
	unsigned int effective_width, effective_height;
	uintptr_t offset;
	int i = 0;
	struct disp_manager *mgr;

	mgr = g_disp_drv.mgr[sel];

	paddr = bootlogo_addr;
	if (paddr == 0) {
		__inf("Fb_map_kernel_logo failed!");
		return Fb_copy_boot_fb(sel, info);
	}

	/* parser bmp header */
	offset = paddr & ~PAGE_MASK;
	vaddr = (void *)Fb_map_kernel(paddr, sizeof(struct bmp_header));
	if (vaddr == NULL) {
		__wrn("fb_map_kernel failed, paddr=0x%p,size=0x%x\n",
		      (void *)paddr, (unsigned int)sizeof(struct bmp_header));
		return -1;
	}

	memcpy(&bmp_pad_header.signature[0], vaddr + offset,
	       sizeof(struct bmp_header));
	bmp_header = (struct bmp_header *) &bmp_pad_header.signature[0];
	if ((bmp_header->signature[0] != 'B')
	    || (bmp_header->signature[1] != 'M')) {
		Fb_unmap_kernel(vaddr);
#if defined(CONFIG_DECOMPRESS_LZMA)
		return lzma_decode(paddr, info);
#else
		__wrn("this is not a bmp picture.\n");
		return -1;

#endif
	}

	bmp_bpix = bmp_header->bit_count / 8;

	if ((bmp_bpix != 3) && (bmp_bpix != 4))
		return -1;

	x = bmp_header->width;
	y = (bmp_header->height & 0x80000000) ? (-bmp_header->
						 height) : (bmp_header->height);

	if (bmp_bpix == 3) {
		zero_num = (4 - ((3 * x) % 4)) & 3;
#ifndef SUPPORT_ROTATE
		/*uboot have removed zero for us*/
		zero_num = 0;
#endif
	}
	fb_width = info->var.xres;
	fb_height = info->var.yres;
	if ((paddr <= 0) || x <= 1 || y <= 1) {
		__wrn("kernel logo para error!\n");
		return -EINVAL;
	}

	bmp_info->x = x;
	bmp_info->y = y;
	bmp_info->bit = bmp_header->bit_count;
	bmp_info->buffer = (void *__force)(info->screen_base);

	if (bmp_bpix == 3)
		info->var.bits_per_pixel = 24;
	else if (bmp_bpix == 4)
		info->var.bits_per_pixel = 32;
	else
		info->var.bits_per_pixel = 32;

	Fb_unmap_kernel(vaddr);

	/* map the total bmp buffer */
	vaddr =
	    (void *)Fb_map_kernel(paddr,
				  (x * bmp_bpix + zero_num) * y + sizeof(struct bmp_header));
	if (vaddr == NULL) {
		__wrn("fb_map_kernel failed, paddr=0x%p,size=0x%x\n",
		      (void *)paddr,
		      (unsigned int)(x * y * bmp_bpix +
		       sizeof(struct bmp_header)));
		return -1;
	}

	tmp_buffer = (char *)bmp_info->buffer;
	screen_offset = (void *)bmp_info->buffer;
	bmp_data = (char *)(vaddr + bmp_header->data_offset);
	image_offset = (void *)bmp_data;
	effective_width = (fb_width < x) ? fb_width : x;
	effective_height = (fb_height < y) ? fb_height : y;

	if (bmp_header->height & 0x80000000) {
#if defined(SUPPORT_ROTATE)
		if (info->var.bits_per_pixel == 24) {
			screen_offset =
				(void *)((void *__force)info->screen_base +
					 (fb_width * (abs(fb_height - y) / 2)
					  + abs(fb_width - x) / 2)
					 * 4);
			rgb24_to_rgb32(image_offset, bmp_header, info,
				       screen_offset, zero_num);
		} else
#endif
		{
		screen_offset =
			(void *)((void *__force)info->screen_base +
				 (fb_width * (abs(fb_height - y) / 2)
				  + abs(fb_width - x) / 2)
				 * (info->var.bits_per_pixel >> 3));
			for (i = 0; i < effective_height; i++) {
				memcpy((void *)screen_offset, image_offset,
				       effective_width *
				       (info->var.bits_per_pixel >> 3));
				screen_offset =
					(void *)(screen_offset + fb_width *
						 (info->var.bits_per_pixel >> 3));
				image_offset =
					(void *)image_offset +
					x * (info->var.bits_per_pixel >> 3);
			}
		}

	} else {

#if defined(SUPPORT_ROTATE)
		if (info->var.bits_per_pixel == 24) {
			screen_offset =
				(void *)((void *__force)info->screen_base +
					 (fb_width * (abs(fb_height - y) / 2)
					  + abs(fb_width - x) / 2)
					 * 4);

			image_offset =
			    (void *)bmp_data +
			    (effective_height - 1) * (x * 3 + zero_num);
			rgb24_to_rgb32(image_offset, bmp_header, info,
				       screen_offset, zero_num);
		} else
#endif
		{
			screen_offset =
				(void *)((void *__force)info->screen_base +
					 (fb_width * (abs(fb_height - y) / 2)
					  + abs(fb_width - x) / 2)
					 * (info->var.bits_per_pixel >> 3));

			image_offset =
				(void *)bmp_data + (effective_height -
						    1) * x *
				(info->var.bits_per_pixel >> 3);
			for (i = effective_height - 1; i >= 0; i--) {
				memcpy((void *)screen_offset, image_offset,
				       effective_width *
				       (info->var.bits_per_pixel >> 3));
				screen_offset =
					(void *)(screen_offset +
						 fb_width *
						 (info->var.bits_per_pixel >> 3));
				image_offset =
					(void *)bmp_data +
					i * x * (info->var.bits_per_pixel >> 3);
			}
		}
	}

	Fb_unmap_kernel(vaddr);
	return 0;
}

static s32 display_fb_request(u32 fb_id, struct disp_fb_create_info *fb_para)
{
	struct fb_info *info = NULL;
	struct disp_layer_config config;
	u32 sel;
	u32 xres, yres;
	u32 num_screens;
	s32 ret = 0;

#if defined(CONFIG_ARCH_SUN8IW12) || defined(CONFIG_ARCH_SUN8IW16)\
    || defined(CONFIG_ARCH_SUN8IW19)
	/* fb bound to layer(2,0)  */
	g_fbi.layer_hdl[fb_id][0] = 2;
	g_fbi.layer_hdl[fb_id][1] = 0;
#else
	/* fb bound to layer(1,0)  */
	g_fbi.layer_hdl[fb_id][0] = 1;
	g_fbi.layer_hdl[fb_id][1] = 0;
#endif


	num_screens = bsp_disp_feat_get_num_screens();

	printk("%s,fb_id:%d\n", __func__, fb_id);

	if (g_fbi.fb_enable[fb_id]) {
		__wrn("%s, fb%d is already requested!\n", __func__, fb_id);
		return -1;
	}
	info = g_fbi.fbinfo[fb_id];

	xres = fb_para->width;
	yres = fb_para->height;
	if ((xres == 0) || (yres == 0) || (info->var.bits_per_pixel == 0)) {
		__wrn("invalid paras xres(%d), yres(%d) bpp(%d)\n", xres, yres,
		      info->var.bits_per_pixel);
		return -1;
	}

	info->var.xoffset = 0;
	info->var.yoffset = 0;
	info->var.xres = xres;
	info->var.yres = yres;
	info->var.xres_virtual = xres;
	info->fix.line_length =
	    (fb_para->width * info->var.bits_per_pixel) >> 3;
	info->fix.smem_len =
	    info->fix.line_length * fb_para->height * fb_para->buffer_num;
	if (info->fix.line_length != 0)
		info->var.yres_virtual =
		    info->fix.smem_len / info->fix.line_length;
	ret = fb_map_video_memory(info);
	if (ret)
		goto OUT;

	for (sel = 0; sel < num_screens; sel++) {
		if (sel == fb_para->fb_mode) {
			u32 y_offset = 0, src_width = xres, src_height = yres;
			struct disp_video_timings tt;
			struct disp_manager *mgr = NULL;

			mgr = g_disp_drv.mgr[sel];
			if (mgr && mgr->device && mgr->device->get_timings) {
				mgr->device->get_timings(mgr->device, &tt);
				if (tt.pixel_clk != 0)
					g_fbi.fbinfo[fb_id]->var.pixclock =
					    1000000000 / tt.pixel_clk;
				g_fbi.fbinfo[fb_id]->var.left_margin =
				    tt.hor_back_porch;
				g_fbi.fbinfo[fb_id]->var.right_margin =
				    tt.hor_front_porch;
				g_fbi.fbinfo[fb_id]->var.upper_margin =
				    tt.ver_back_porch;
				g_fbi.fbinfo[fb_id]->var.lower_margin =
				    tt.ver_front_porch;
				g_fbi.fbinfo[fb_id]->var.hsync_len =
				    tt.hor_sync_time;
				g_fbi.fbinfo[fb_id]->var.vsync_len =
				    tt.ver_sync_time;
			}
			info->var.width =
			    bsp_disp_get_screen_physical_width(sel);
			info->var.height =
			    bsp_disp_get_screen_physical_height(sel);

			memset(&config, 0, sizeof(struct disp_layer_config));

			config.channel = g_fbi.layer_hdl[fb_id][0];
			config.layer_id = g_fbi.layer_hdl[fb_id][1];
			config.enable = 1;
			Fb_map_kernel_logo(sel, info);
			if (g_disp_drv.para.boot_info.sync == 1) {
				if ((sel == g_disp_drv.para.boot_info.disp) &&
				    (g_disp_drv.para.boot_info.type !=
				     DISP_OUTPUT_TYPE_NONE)) {
					bsp_disp_get_display_size(sel,
					      &fb_para->output_width,
					      &fb_para->output_height);
				}
			}


#if defined(CONFIG_SUNXI_DISP2_FB_ROTATION_SUPPORT)
			if (mgr && mgr->rot_sw &&
			    (ROTATION_SW_90 == mgr->rot_sw->degree ||
			     ROTATION_SW_270 == mgr->rot_sw->degree)) {
				config.info.screen_win.height =
					(fb_para->output_width == 0)
					? src_width
					: fb_para->output_width;
				config.info.screen_win.width =
					(fb_para->output_height == 0)
					? src_width
					: fb_para->output_height;
			} else
#endif
			{
				config.info.screen_win.width =
					(0 ==
					 fb_para->output_width) ? src_width : fb_para->
					output_width;
				config.info.screen_win.height =
					(0 ==
					 fb_para->output_height) ? src_width : fb_para->
					output_height;
			}

			config.info.mode = LAYER_MODE_BUFFER;
			config.info.zorder = 16;
			config.info.alpha_mode = 1;
			config.info.alpha_value = 0xff;
			config.info.fb.crop.x = (0LL) << 32;
			config.info.fb.crop.y = ((long long)y_offset) << 32;
			config.info.fb.crop.width =
			    ((long long)src_width) << 32;
			config.info.fb.crop.height =
			    ((long long)src_height) << 32;
			config.info.screen_win.x = 0;
			config.info.screen_win.y = 0;
			var_to_disp_fb(&(config.info.fb), &(info->var),
				       &(info->fix));
			config.info.fb.addr[0] =
			    (unsigned long long)info->fix.smem_start;
			config.info.fb.addr[1] = 0;
			config.info.fb.addr[2] = 0;
			config.info.fb.flags = DISP_BF_NORMAL;
			config.info.fb.scan = DISP_SCAN_PROGRESSIVE;
			config.info.fb.size[0].width = fb_para->width;
			config.info.fb.size[0].height = fb_para->height;
			config.info.fb.size[1].width = fb_para->width;
			config.info.fb.size[1].height = fb_para->height;
			config.info.fb.size[2].width = fb_para->width;
			config.info.fb.size[2].height = fb_para->height;
			config.info.fb.color_space = DISP_BT601;

#if defined(CONFIG_SUNXI_DISP2_FB_ROTATION_SUPPORT)
			if (mgr && mgr->rot_sw && mgr->rot_sw->apply) {
				struct disp_rect dirty_rect;

				dirty_rect.x = config.info.fb.crop.x >> 32;
				dirty_rect.y = config.info.fb.crop.y >> 32;
				dirty_rect.width =
				    config.info.fb.crop.width >> 32;
				dirty_rect.height =
				    config.info.fb.crop.height >> 32;

				mgr->rot_sw->apply(mgr->rot_sw, &config,
						   dirty_rect);
			}
#endif
#if defined(CONFIG_SUNXI_DISP2_FB_HW_ROTATION_SUPPORT)
			g_fbi.fb_rot[fb_id] = fb_g2d_rot_create(info, fb_id, &config);
			if (g_fbi.fb_rot[fb_id])
				g_fbi.fb_rot[fb_id]->apply(g_fbi.fb_rot[fb_id], &config);
#endif

#if (!IS_ENABLED(CONFIG_EINK_PANEL_USED)) && (!IS_ENABLED(CONFIG_EINK200_SUNXI))
			if (mgr && mgr->set_layer_config)
				mgr->set_layer_config(mgr, &config, 1);
#endif
			memcpy(&g_fbi.config[fb_id], &config, sizeof(struct disp_layer_config));
		}
	}

	g_fbi.fb_enable[fb_id] = 1;
	g_fbi.fb_mode[fb_id] = fb_para->fb_mode;
	memcpy(&g_fbi.fb_para[fb_id], fb_para,
	       sizeof(struct disp_fb_create_info));
OUT:
	return ret;
}

static s32 display_fb_release(u32 fb_id)
{
	u32 num_screens;

	num_screens = bsp_disp_feat_get_num_screens();

	__inf("%s, fb_id:%d\n", __func__, fb_id);

	if (g_fbi.fb_enable[fb_id]) {
		u32 sel = 0;
		struct fb_info *info = g_fbi.fbinfo[fb_id];

		for (sel = 0; sel < num_screens; sel++) {
			if (sel == g_fbi.fb_mode[fb_id]) {
				struct disp_manager *mgr = NULL;
				struct disp_layer_config config;

				mgr = g_disp_drv.mgr[sel];
				memset(&config, 0,
				       sizeof(struct disp_layer_config));
				config.channel = g_fbi.layer_hdl[fb_id][0];
				config.layer_id = g_fbi.layer_hdl[fb_id][1];
				if (mgr && mgr->set_layer_config)
					mgr->set_layer_config(mgr, &config, 1);
			}
		}
		g_fbi.layer_hdl[fb_id][0] = 0;
		g_fbi.layer_hdl[fb_id][1] = 0;
		g_fbi.fb_mode[fb_id] = FB_MODE_SCREEN0;
		memset(&g_fbi.fb_para[fb_id], 0,
		       sizeof(struct disp_fb_create_info));
		g_fbi.fb_enable[fb_id] = 0;
#if defined(CONFIG_FB_CONSOLE_SUNXI)
		fb_dealloc_cmap(&info->cmap);
#endif
		fb_unmap_video_memory(info);
#if defined(CONFIG_SUNXI_DISP2_FB_HW_ROTATION_SUPPORT)
		if (g_fbi.fb_rot[fb_id])
			g_fbi.fb_rot[fb_id]->free(g_fbi.fb_rot[fb_id]);
#endif

		return 0;
	}

	__wrn("invalid paras fb_id:%d in %s\n", fb_id, __func__);
	return -1;
}

s32 Display_set_fb_timming(u32 sel)
{
	u8 fb_id = 0;

	for (fb_id = 0; fb_id < SUNXI_FB_MAX; fb_id++) {
		if (g_fbi.fb_enable[fb_id]) {
			if (sel == g_fbi.fb_mode[fb_id]) {
				struct disp_video_timings tt;
				struct disp_manager *mgr = g_disp_drv.mgr[sel];

				if (mgr && mgr->device
				    && mgr->device->get_timings) {
					mgr->device->get_timings(mgr->device,
								 &tt);
					if (tt.pixel_clk != 0)
						g_fbi.fbinfo[fb_id]->var.
						    pixclock =
						    1000000000 / tt.pixel_clk;
					g_fbi.fbinfo[fb_id]->var.left_margin =
					    tt.hor_back_porch;
					g_fbi.fbinfo[fb_id]->var.right_margin =
					    tt.hor_front_porch;
					g_fbi.fbinfo[fb_id]->var.upper_margin =
					    tt.ver_back_porch;
					g_fbi.fbinfo[fb_id]->var.lower_margin =
					    tt.ver_front_porch;
					g_fbi.fbinfo[fb_id]->var.hsync_len =
					    tt.hor_sync_time;
					g_fbi.fbinfo[fb_id]->var.vsync_len =
					    tt.ver_sync_time;
				}
			}
		}
	}

	return 0;
}

static s32 fb_parse_bootlogo_base(phys_addr_t *fb_base, int *fb_size)
{
	*fb_base = (phys_addr_t) disp_boot_para_parse("fb_base");

	return 0;
}

unsigned long fb_get_address_info(u32 fb_id, u32 phy_virt_flag)
{
	struct fb_info *info = NULL;
	unsigned long phy_addr = 0;
	unsigned long virt_addr = 0;

	if (fb_id >= SUNXI_FB_MAX)
		return 0;

	info = g_fbi.fbinfo[fb_id];
	phy_addr = info->fix.smem_start;
	virt_addr = (unsigned long)info->screen_base;

	if (phy_virt_flag == 0)
		/* get virtual address */
		return virt_addr;

	/* get phy address */
	return phy_addr;
}

s32 fb_init(struct platform_device *pdev)
{
	struct disp_fb_create_info fb_para;
	unsigned long i;
	u32 num_screens;
	s32 ret = 0;
#if IS_ENABLED(CONFIG_EINK200_SUNXI)
	s32 value = 0;
	char primary_key[20];

	sprintf(primary_key, "eink");
#endif
	/* struct sched_param param = { .sched_priority = MAX_RT_PRIO - 1 }; */

	g_fbi.dev = &pdev->dev;
	num_screens = bsp_disp_feat_get_num_screens();

	__inf("[DISP] %s\n", __func__);

	fb_parse_bootlogo_base(&bootlogo_addr, &bootlogo_sz);
	init_waitqueue_head(&g_fbi.wait[0]);
	init_waitqueue_head(&g_fbi.wait[1]);
	init_waitqueue_head(&g_fbi.wait[2]);
	disp_register_sync_finish_proc(DRV_disp_int_process);

	for (i = 0; i < SUNXI_FB_MAX; i++) {
		g_fbi.fbinfo[i] = framebuffer_alloc(0, g_fbi.dev);
		g_fbi.fbinfo[i]->fbops = &dispfb_ops;
		g_fbi.fbinfo[i]->flags = 0;
		g_fbi.fbinfo[i]->device = g_fbi.dev;
		g_fbi.fbinfo[i]->par = &g_fbi;
		g_fbi.fbinfo[i]->var.xoffset = 0;
		g_fbi.fbinfo[i]->var.yoffset = 0;
		g_fbi.fbinfo[i]->var.xres = 800;
		g_fbi.fbinfo[i]->var.yres = 480;
		g_fbi.fbinfo[i]->var.xres_virtual = 800;
		g_fbi.fbinfo[i]->var.yres_virtual = 480 * 2;
		g_fbi.fbinfo[i]->var.nonstd = 0;
		g_fbi.fbinfo[i]->var.bits_per_pixel = 32;
		g_fbi.fbinfo[i]->var.transp.length = 8;
		g_fbi.fbinfo[i]->var.red.length = 8;
		g_fbi.fbinfo[i]->var.green.length = 8;
		g_fbi.fbinfo[i]->var.blue.length = 8;
		g_fbi.fbinfo[i]->var.transp.offset = 24;
		g_fbi.fbinfo[i]->var.red.offset = 16;
		g_fbi.fbinfo[i]->var.green.offset = 8;
		g_fbi.fbinfo[i]->var.blue.offset = 0;
		g_fbi.fbinfo[i]->var.activate = FB_ACTIVATE_FORCE;
		g_fbi.fbinfo[i]->fix.type = FB_TYPE_PACKED_PIXELS;
		g_fbi.fbinfo[i]->fix.type_aux = 0;
		g_fbi.fbinfo[i]->fix.visual = FB_VISUAL_TRUECOLOR;
		g_fbi.fbinfo[i]->fix.xpanstep = 1;
		g_fbi.fbinfo[i]->fix.ypanstep = 1;
		g_fbi.fbinfo[i]->fix.ywrapstep = 0;
		g_fbi.fbinfo[i]->fix.accel = FB_ACCEL_NONE;
		g_fbi.fbinfo[i]->fix.line_length =
		    g_fbi.fbinfo[i]->var.xres_virtual * 4;
		g_fbi.fbinfo[i]->fix.smem_len =
		    g_fbi.fbinfo[i]->fix.line_length *
		    g_fbi.fbinfo[i]->var.yres_virtual * 2;
		g_fbi.fbinfo[i]->screen_base = NULL;
		g_fbi.fbinfo[i]->pseudo_palette = g_fbi.pseudo_palette[i];
		g_fbi.fbinfo[i]->fix.smem_start = 0x0;
		g_fbi.fbinfo[i]->fix.mmio_start = 0;
		g_fbi.fbinfo[i]->fix.mmio_len = 0;

		if (fb_alloc_cmap(&g_fbi.fbinfo[i]->cmap, 256, 1) < 0)
			return -ENOMEM;
	}

	if (g_disp_drv.disp_init.b_init) {
		struct disp_init_para *disp_init = &g_disp_drv.disp_init;

		for (i = 0; i < SUNXI_FB_MAX; i++) {
			u32 screen_id = g_disp_drv.disp_init.disp_mode;

			if (g_disp_drv.para.boot_info.sync)
				screen_id = g_disp_drv.para.boot_info.disp;

			disp_fb_to_var(disp_init->format[i],
				       &(g_fbi.fbinfo[i]->var));
			fb_para.buffer_num = g_disp_drv.disp_init.buffer_num[i];
			if ((disp_init->fb_width[i] == 0)
			    || (disp_init->fb_height[i] == 0)) {
				fb_para.width =
				    bsp_disp_get_screen_width_from_output_type
				    (screen_id,
				     disp_init->output_type[screen_id],
				     disp_init->output_mode[screen_id]);
				fb_para.height =
				    bsp_disp_get_screen_height_from_output_type
				    (screen_id,
				     disp_init->output_type[screen_id],
				     disp_init->output_mode[screen_id]);
			} else {
				fb_para.width = disp_init->fb_width[i];
				fb_para.height = disp_init->fb_height[i];
			}

			fb_para.output_width =
			    bsp_disp_get_screen_width_from_output_type
			    (screen_id,
			     disp_init->output_type[screen_id],
			     disp_init->output_mode[screen_id]);
			fb_para.output_height =
			    bsp_disp_get_screen_height_from_output_type
			    (screen_id,
			     disp_init->output_type[screen_id],
			     disp_init->output_mode[screen_id]);
			fb_para.fb_mode = screen_id;

#if (defined(SUPPORT_EINK) && defined(CONFIG_EINK_PANEL_USED))
			fb_para.output_width = fb_para.width;
			fb_para.output_height = fb_para.height;
#elif IS_ENABLED(CONFIG_EINK200_SUNXI)
			ret = disp_sys_script_get_item(primary_key, "eink_width", &value, 1);
			if (ret == 1)
				fb_para.output_width = value;
			ret = disp_sys_script_get_item(primary_key, "eink_height", &value, 1);
			if (ret == 1)
				fb_para.output_height = value;
			pr_err("%s:fb width = %d, height = %d\n", __func__,
					fb_para.output_width, fb_para.output_height);
			fb_para.width = fb_para.output_width;
			fb_para.height = fb_para.output_height;
#endif

			ret = display_fb_request(i, &fb_para);
			if (ret)
				break;
#if defined(CONFIG_DISP2_SUNXI_BOOT_COLORBAR)
			disp_draw_colorbar(screen_id, 0);
#endif
		}
		for (i = 0; i < SUNXI_FB_MAX; i++)
			register_framebuffer(g_fbi.fbinfo[i]);
	}

	return ret;
}

s32 fb_exit(void)
{
	unsigned int fb_id = 0;

	disp_unregister_sync_finish_proc(DRV_disp_int_process);
	for (fb_id = 0; fb_id < SUNXI_FB_MAX; fb_id++) {
		if (g_fbi.fbinfo[fb_id]) {
			fb_dealloc_cmap(&g_fbi.fbinfo[fb_id]->cmap);
			display_fb_release(fb_id);
			unregister_framebuffer(g_fbi.fbinfo[fb_id]);
			framebuffer_release(g_fbi.fbinfo[fb_id]);
			g_fbi.fbinfo[fb_id] = NULL;
		}
	}

	return 0;
}

