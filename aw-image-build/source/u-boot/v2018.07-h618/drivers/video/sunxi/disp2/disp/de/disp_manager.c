/*
 * drivers/video/sunxi/disp2/disp/de/disp_manager.c
 *
 * Copyright (c) 2007-2019 Allwinnertech Co., Ltd.
 * Author: zhengxiaobin <zhengxiaobin@allwinnertech.com>
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */
#include "disp_manager.h"

struct disp_manager_private_data {
	bool applied;
	bool enabled;
	bool color_range_modified;
	struct disp_manager_data *cfg;

	s32 (*shadow_protect)(u32 disp, bool protect);

	u32 reg_base;
	u32 irq_no;
	struct clk *clk;
	struct clk *clk_parent;
	struct clk *extra_clk;
#if defined(CONFIG_INDEPENDENT_DE)
	struct clk *dpss_clk;
#endif
	bool sync;
	bool err;
	unsigned int err_cnt;
	unsigned int force_sync_cnt;
};

__attribute__((unused)) static spinlock_t mgr_data_lock;
__attribute__((unused)) static struct mutex mgr_mlock;


static struct disp_manager *mgrs = NULL;
static struct disp_manager_private_data *mgr_private;
static struct disp_manager_data *mgr_cfgs;

static struct disp_layer_config_data* lyr_cfgs;

/*
 * layer unit
 */
struct disp_layer_private_data {
	struct disp_layer_config_data *cfg;
	s32 (*shadow_protect)(u32 sel, bool protect);
};

static struct disp_layer *lyrs = NULL;
static struct disp_layer_private_data *lyr_private;
struct disp_layer* disp_get_layer(u32 disp, u32 chn, u32 layer_id)
{
	u32 num_screens, max_num_layers = 0;
	struct disp_layer *lyr = lyrs;
	int i;

	num_screens = bsp_disp_feat_get_num_screens();
	if ((disp >= num_screens)) {
		DE_WRN("disp %d is out of range %d\n",
			disp, num_screens);
		return NULL;
	}

	for (i=0; i<num_screens; i++) {
		max_num_layers += bsp_disp_feat_get_num_layers(i);
	}

	for (i=0; i<max_num_layers; i++) {
		if ((lyr->disp == disp) && (lyr->chn == chn) && (lyr->id == layer_id)) {
			DE_INF("%d,%d,%d, name=%s\n", disp, chn, layer_id, lyr->name);
			return lyr;
		}
		lyr++;
	}

	DE_WRN("%s (%d,%d,%d) fail\n", __func__, disp, chn, layer_id);
	return NULL;

}

struct disp_layer* disp_get_layer_1(u32 disp, u32 layer_id)
{
	u32 num_screens, num_layers;
	u32 i,k;
	u32 layer_index = 0, start_index = 0;

	num_screens = bsp_disp_feat_get_num_screens();
	if ((disp >= num_screens)) {
		DE_WRN("disp %d is out of range %d\n",
			disp, num_screens);
		return NULL;
	}

	for (i=0; i<disp; i++)
		start_index += bsp_disp_feat_get_num_layers(i);

	layer_id += start_index;

	for (i=0; i<num_screens; i++) {
			num_layers = bsp_disp_feat_get_num_layers(i);
			for (k=0; k<num_layers; k++) {
				if (layer_index == layer_id) {
					DE_INF("disp%d layer%d: %d,%d,%d\n", disp, layer_id, lyrs[layer_index].disp, lyrs[layer_index].chn, lyrs[layer_index].id);
					return &lyrs[layer_index];
				}
				layer_index ++;
		}
	}

	DE_WRN("%s fail\n", __func__);
	return NULL;
}

static struct disp_layer_private_data *disp_lyr_get_priv(struct disp_layer *lyr)
{
	if (NULL == lyr) {
		DE_WRN("NULL hdl!\n");
		return NULL;
	}

	return (struct disp_layer_private_data *)lyr->data;
}

/** __disp_config_transfer2inner - transfer disp_layer_config to inner one
 */
s32 __disp_config_transfer2inner(struct disp_layer_config_inner *config_inner,
				      struct disp_layer_config *config)
{
	config_inner->enable = config->enable;
	config_inner->channel = config->channel;
	config_inner->layer_id = config->layer_id;

	if (0 == config->enable) {
		memset(&(config->info), 0, sizeof(config->info));
	}
	/* layer info */
	config_inner->info.mode = config->info.mode;
	config_inner->info.zorder = config->info.zorder;
	config_inner->info.alpha_mode = config->info.alpha_mode;
	config_inner->info.alpha_value = config->info.alpha_value;
	memcpy(&config_inner->info.screen_win,
	       &config->info.screen_win,
	       sizeof(struct disp_rect));
	config_inner->info.b_trd_out = config->info.b_trd_out;
	config_inner->info.out_trd_mode = config->info.out_trd_mode;
	config_inner->info.id = config->info.id;
	/* fb info */
	memcpy(config_inner->info.fb.addr,
	       config->info.fb.addr,
	       sizeof(long long) * 3);
	memcpy(config_inner->info.fb.size,
	       config->info.fb.size,
	       sizeof(struct disp_rectsz) * 3);
	memcpy(config_inner->info.fb.align,
	    config->info.fb.align, sizeof(int) * 3);
	config_inner->info.fb.format = config->info.fb.format;
	config_inner->info.fb.color_space = config->info.fb.color_space;
	memcpy(config_inner->info.fb.trd_right_addr,
	       config->info.fb.trd_right_addr,
	       sizeof(long long) * 3);
	config_inner->info.fb.pre_multiply = config->info.fb.pre_multiply;
	memcpy(&config_inner->info.fb.crop,
	       &config->info.fb.crop,
	       sizeof(struct disp_rect64));
	config_inner->info.fb.flags = config->info.fb.flags;
	config_inner->info.fb.scan = config->info.fb.scan;
	config_inner->info.fb.eotf = DISP_EOTF_UNDEF;
	config_inner->info.fb.fbd_en = 0;
	config_inner->info.fb.metadata_buf = 0;
	config_inner->info.fb.metadata_size = 0;
	config_inner->info.fb.metadata_flag = 0;
	config_inner->info.atw.used = 0;

	if (config_inner->info.mode == LAYER_MODE_COLOR)
		config_inner->info.color = config->info.color;

	return 0;
}

/** __disp_config2_transfer2inner -transfer disp_layer_config2 to inner one
 */
s32 __disp_config2_transfer2inner(struct disp_layer_config_inner *config_inner,
				      struct disp_layer_config2 *config2)
{
	config_inner->enable = config2->enable;
	config_inner->channel = config2->channel;
	config_inner->layer_id = config2->layer_id;

	if (0 == config2->enable) {
		memset(&(config2->info), 0, sizeof(config2->info));
	}

	/* layer info */
	config_inner->info.mode = config2->info.mode;
	config_inner->info.zorder = config2->info.zorder;
	config_inner->info.alpha_mode = config2->info.alpha_mode;
	config_inner->info.alpha_value = config2->info.alpha_value;
	memcpy(&config_inner->info.screen_win,
	       &config2->info.screen_win,
	       sizeof(struct disp_rect));
	config_inner->info.b_trd_out = config2->info.b_trd_out;
	config_inner->info.out_trd_mode = config2->info.out_trd_mode;
	/* fb info */
	memcpy(config_inner->info.fb.addr,
	       config2->info.fb.addr,
	       sizeof(long long) * 3);
	memcpy(config_inner->info.fb.size,
	       config2->info.fb.size,
	       sizeof(struct disp_rectsz) * 3);
	memcpy(config_inner->info.fb.align,
	    config2->info.fb.align, sizeof(int) * 3);
	config_inner->info.fb.format = config2->info.fb.format;
	config_inner->info.fb.color_space = config2->info.fb.color_space;
	memcpy(config_inner->info.fb.trd_right_addr,
	       config2->info.fb.trd_right_addr,
	       sizeof(long long) * 3);
	config_inner->info.fb.pre_multiply = config2->info.fb.pre_multiply;
	memcpy(&config_inner->info.fb.crop,
	       &config2->info.fb.crop,
	       sizeof(struct disp_rect64));
	config_inner->info.fb.flags = config2->info.fb.flags;
	config_inner->info.fb.scan = config2->info.fb.scan;
	/* hdr related */
	config_inner->info.fb.eotf = config2->info.fb.eotf;
	config_inner->info.fb.fbd_en = config2->info.fb.fbd_en;
	config_inner->info.fb.metadata_buf = config2->info.fb.metadata_buf;
	config_inner->info.fb.metadata_size = config2->info.fb.metadata_size;
	config_inner->info.fb.metadata_flag =
	    config2->info.fb.metadata_flag;

	config_inner->info.id = config2->info.id;
	/* atw related */
	config_inner->info.atw.used = config2->info.atw.used;
	config_inner->info.atw.mode = config2->info.atw.mode;
	config_inner->info.atw.b_row = config2->info.atw.b_row;
	config_inner->info.atw.b_col = config2->info.atw.b_col;
	config_inner->info.atw.cof_addr = config2->info.atw.cof_addr;
	if (config_inner->info.mode == LAYER_MODE_COLOR)
		config_inner->info.color = config2->info.color;

#if defined(DE_VERSION_V33X) || defined(DE_VERSION_V35X)
	config_inner->info.transform = config2->info.transform;
	memcpy(&config_inner->info.snr, &config2->info.snr,
	       sizeof(struct disp_snr_info));

#endif

	return 0;
}

/** __disp_inner_transfer2config - transfer inner to disp_layer_config
 */
s32 __disp_inner_transfer2config(struct disp_layer_config *config,
				 struct disp_layer_config_inner *config_inner)
{
	config->enable = config_inner->enable;
	config->channel = config_inner->channel;
	config->layer_id = config_inner->layer_id;
	/* layer info */
	config->info.mode = config_inner->info.mode;
	config->info.zorder = config_inner->info.zorder;
	config->info.alpha_mode = config_inner->info.alpha_mode;
	config->info.alpha_value = config_inner->info.alpha_value;
	memcpy(&config->info.screen_win,
	       &config_inner->info.screen_win,
	       sizeof(struct disp_rect));
	config->info.b_trd_out = config_inner->info.b_trd_out;
	config->info.out_trd_mode = config_inner->info.out_trd_mode;
	config->info.id = config_inner->info.id;
	/* fb info */
	memcpy(config->info.fb.addr,
	       config_inner->info.fb.addr,
	       sizeof(long long) * 3);
	memcpy(config->info.fb.size,
	       config_inner->info.fb.size,
	       sizeof(struct disp_rectsz) * 3);
	memcpy(config->info.fb.align,
	    config_inner->info.fb.align, sizeof(int) * 3);
	config->info.fb.format = config_inner->info.fb.format;
	config->info.fb.color_space = config_inner->info.fb.color_space;
	memcpy(config->info.fb.trd_right_addr,
	       config_inner->info.fb.trd_right_addr,
	       sizeof(long long) * 3);
	config->info.fb.pre_multiply = config_inner->info.fb.pre_multiply;
	memcpy(&config->info.fb.crop,
	       &config_inner->info.fb.crop,
	       sizeof(struct disp_rect64));
	config->info.fb.flags = config_inner->info.fb.flags;
	config->info.fb.scan = config_inner->info.fb.scan;

	if (config->info.mode == LAYER_MODE_COLOR)
		config->info.color = config_inner->info.color;

	return 0;
}

/** __disp_inner_transfer2config2 - transfer inner to disp_layer_config2
 */
s32 __disp_inner_transfer2config2(struct disp_layer_config2 *config2,
				  struct disp_layer_config_inner *config_inner)
{
	config2->enable = config_inner->enable;
	config2->channel = config_inner->channel;
	config2->layer_id = config_inner->layer_id;
	/* layer info */
	config2->info.mode = config_inner->info.mode;
	config2->info.zorder = config_inner->info.zorder;
	config2->info.alpha_mode = config_inner->info.alpha_mode;
	config2->info.alpha_value = config_inner->info.alpha_value;
	memcpy(&config2->info.screen_win,
	       &config_inner->info.screen_win,
	       sizeof(struct disp_rect));
	config2->info.b_trd_out = config_inner->info.b_trd_out;
	config2->info.out_trd_mode = config_inner->info.out_trd_mode;
	/* fb info */
	memcpy(config2->info.fb.addr,
	       config_inner->info.fb.addr,
	       sizeof(long long) * 3);
	memcpy(config2->info.fb.size,
	       config_inner->info.fb.size,
	       sizeof(struct disp_rectsz) * 3);
	memcpy(config2->info.fb.align,
	    config_inner->info.fb.align, sizeof(int) * 3);
	config2->info.fb.format = config_inner->info.fb.format;
	config2->info.fb.color_space = config_inner->info.fb.color_space;
	memcpy(config2->info.fb.trd_right_addr,
	       config_inner->info.fb.trd_right_addr,
	       sizeof(long long) * 3);
	config2->info.fb.pre_multiply = config_inner->info.fb.pre_multiply;
	memcpy(&config2->info.fb.crop,
	       &config_inner->info.fb.crop,
	       sizeof(struct disp_rect64));
	config2->info.fb.flags = config_inner->info.fb.flags;
	config2->info.fb.scan = config_inner->info.fb.scan;
	/* hdr related */
	config2->info.fb.eotf = config_inner->info.fb.eotf;
	config2->info.fb.fbd_en = config_inner->info.fb.fbd_en;
	config2->info.fb.metadata_buf = config_inner->info.fb.metadata_buf;
	config2->info.fb.metadata_size = config_inner->info.fb.metadata_size;
	config2->info.fb.metadata_flag =
	    config_inner->info.fb.metadata_flag;

	config2->info.id = config_inner->info.id;
	/* atw related */
	config2->info.atw.used = config_inner->info.atw.used;
	config2->info.atw.mode = config_inner->info.atw.mode;
	config2->info.atw.b_row = config_inner->info.atw.b_row;
	config2->info.atw.b_col = config_inner->info.atw.b_col;
	config2->info.atw.cof_addr = config_inner->info.atw.cof_addr;
	if (config2->info.mode == LAYER_MODE_COLOR)
		config2->info.color = config_inner->info.color;

#if defined(DE_VERSION_V33X) || defined(DE_VERSION_V35X)
	config2->info.transform = config_inner->info.transform;
	memcpy(&config2->info.snr, &config_inner->info.snr,
	       sizeof(struct disp_snr_info));
#endif

	return 0;
}

static s32 disp_lyr_set_manager(struct disp_layer *lyr, struct disp_manager *mgr)
{
	unsigned long flags;
	struct disp_layer_private_data *lyrp = disp_lyr_get_priv(lyr);

	if ((NULL == lyr) || (NULL == lyrp) || (NULL == mgr)) {
		DE_WRN("NULL hdl!\n");
		return -1;
	}

	spin_lock_irqsave(&mgr_data_lock, flags);
	lyr->manager = mgr;
	list_add_tail(&lyr->list, &mgr->lyr_list);
	spin_unlock_irqrestore(&mgr_data_lock, flags);

	return DIS_SUCCESS;
}

static s32 disp_lyr_unset_manager(struct disp_layer *lyr)
{
	unsigned long flags;
	struct disp_layer_private_data *lyrp = disp_lyr_get_priv(lyr);

	if ((NULL == lyr) || (NULL == lyrp)) {
		DE_WRN("NULL hdl!\n");
		return -1;
	}

	spin_lock_irqsave(&mgr_data_lock, flags);
	lyr->manager = NULL;
	list_del(&lyr->list);
	spin_unlock_irqrestore(&mgr_data_lock, flags);

	return DIS_SUCCESS;
}

static s32 disp_lyr_check(struct disp_layer *lyr, struct disp_layer_config *config)
{
	struct disp_layer_private_data *lyrp = disp_lyr_get_priv(lyr);

	if ((NULL == lyr) || (NULL == lyrp)) {
		DE_WRN("NULL hdl!\n");
		return -1;
	}

	return DIS_SUCCESS;
}

static s32 disp_lyr_check2(struct disp_layer *lyr,
			   struct disp_layer_config2 *config)
{
	struct disp_layer_private_data *lyrp = disp_lyr_get_priv(lyr);

	if ((NULL == lyr) || (NULL == lyrp)) {
		DE_WRN("NULL hdl!\n");
		return -1;
	}

	return DIS_SUCCESS;
}

static s32 disp_lyr_save_and_dirty_check(struct disp_layer *lyr,
					 struct disp_layer_config *config)
{
	unsigned long flags;
	struct disp_layer_private_data *lyrp = disp_lyr_get_priv(lyr);

	if ((NULL == lyr) || (NULL == lyrp)) {
		DE_WRN("NULL hdl!\n");
		return -1;
	}

	spin_lock_irqsave(&mgr_data_lock, flags);
	if (lyrp->cfg) {
		struct disp_layer_config_inner *pre_config = &lyrp->cfg->config;
		if ((pre_config->enable != config->enable) ||
			(pre_config->info.fb.addr[0] != config->info.fb.addr[0]) ||
			(pre_config->info.fb.format != config->info.fb.format) ||
			(pre_config->info.fb.flags != config->info.fb.flags))
			lyrp->cfg->flag |= LAYER_ATTR_DIRTY;
		if ((pre_config->info.fb.size[0].width != config->info.fb.size[0].width) ||
			(pre_config->info.fb.size[0].height != config->info.fb.size[0].height) ||
			(pre_config->info.fb.crop.width != config->info.fb.crop.width) ||
			(pre_config->info.fb.crop.height != config->info.fb.crop.height) ||
			(pre_config->info.screen_win.width != config->info.screen_win.width) ||
			(pre_config->info.screen_win.height != config->info.screen_win.height)) {
			lyrp->cfg->flag |= LAYER_SIZE_DIRTY;
		}
		lyrp->cfg->flag = LAYER_ALL_DIRTY;
		__disp_config_transfer2inner(&lyrp->cfg->config, config);
	} else {
		DE_INF("cfg is NULL\n");
	}
	spin_unlock_irqrestore(&mgr_data_lock, flags);

	return DIS_SUCCESS;
}

static s32 disp_lyr_save_and_dirty_check2(struct disp_layer *lyr,
					 struct disp_layer_config2 *config)
{
	unsigned long flags;
	struct disp_layer_private_data *lyrp = disp_lyr_get_priv(lyr);
	if ((NULL == lyr) || (NULL == lyrp)) {
		DE_WRN("NULL hdl!\n");
		return -1;
	}

	spin_lock_irqsave(&mgr_data_lock, flags);
	if (lyrp->cfg) {
		struct disp_layer_config_inner *pre_cfg = &lyrp->cfg->config;
		struct disp_layer_info_inner *pre_info = &pre_cfg->info;
		struct disp_layer_info2 *info = &config->info;
		struct disp_fb_info_inner *pre_fb = &pre_info->fb;
		struct disp_fb_info2 *fb = &info->fb;

		if ((pre_cfg->enable != config->enable) ||
		    (pre_fb->addr[0] != fb->addr[0]) ||
		    (pre_fb->format != fb->format) ||
		    (pre_fb->flags != fb->flags))
			lyrp->cfg->flag |= LAYER_ATTR_DIRTY;
		if ((pre_fb->size[0].width != fb->size[0].width) ||
		    (pre_fb->size[0].height != info->fb.size[0].height) ||
		    (pre_fb->crop.width != info->fb.crop.width) ||
		    (pre_fb->crop.height != info->fb.crop.height) ||
		    (pre_info->screen_win.width != info->screen_win.width) ||
		    (pre_info->screen_win.height != info->screen_win.height))
			lyrp->cfg->flag |= LAYER_SIZE_DIRTY;

		lyrp->cfg->flag = LAYER_ALL_DIRTY;
		__disp_config2_transfer2inner(&lyrp->cfg->config,
						      config);
	} else {
		DE_INF("cfg is NULL\n");
	}
	spin_unlock_irqrestore(&mgr_data_lock, flags);

	return DIS_SUCCESS;
}

static s32 disp_lyr_is_dirty(struct disp_layer *lyr)
{
	struct disp_layer_private_data *lyrp = disp_lyr_get_priv(lyr);

	if ((NULL == lyr) || (NULL == lyrp)) {
		DE_WRN("NULL hdl!\n");
		return -1;
	}

	if (lyrp->cfg) {
		return (lyrp->cfg->flag & LAYER_ALL_DIRTY);
	}

	return 0;
}

static s32 disp_lyr_dirty_clear(struct disp_layer *lyr)
{
	struct disp_layer_private_data *lyrp = disp_lyr_get_priv(lyr);

	if ((NULL == lyr) || (NULL == lyrp)) {
		DE_WRN("NULL hdl!\n");
		return -1;
	}

	lyrp->cfg->flag = 0;

	return 0;
}

static s32 disp_lyr_get_config2(struct disp_layer *lyr,
				struct disp_layer_config2 *config)
{
	unsigned long flags;
	struct disp_layer_private_data *lyrp = disp_lyr_get_priv(lyr);

	if ((NULL == lyr) || (NULL == lyrp)) {
		DE_WRN("NULL hdl!\n");
		return -1;
	}

	spin_lock_irqsave(&mgr_data_lock, flags);
	if (lyrp->cfg)
		__disp_inner_transfer2config2(config, &lyrp->cfg->config);
	spin_unlock_irqrestore(&mgr_data_lock, flags);

	return DIS_SUCCESS;
}

static s32 disp_lyr_get_config(struct disp_layer *lyr,
			       struct disp_layer_config *config)
{
	unsigned long flags;
	struct disp_layer_private_data *lyrp = disp_lyr_get_priv(lyr);

	if ((NULL == lyr) || (NULL == lyrp)) {
		DE_WRN("NULL hdl!\n");
		return -1;
	}

	spin_lock_irqsave(&mgr_data_lock, flags);
	if (lyrp->cfg)
		__disp_inner_transfer2config(config, &lyrp->cfg->config);
	spin_unlock_irqrestore(&mgr_data_lock, flags);

	return DIS_SUCCESS;
}

static s32 disp_lyr_apply(struct disp_layer *lyr)
{
	struct disp_layer_private_data *lyrp = disp_lyr_get_priv(lyr);

	if ((NULL == lyr) || (NULL == lyrp)) {
		DE_WRN("NULL hdl!\n");
		return -1;
	}

	return DIS_SUCCESS;
}

static s32 disp_lyr_force_apply(struct disp_layer *lyr)
{
	unsigned long flags;
	struct disp_layer_private_data *lyrp = disp_lyr_get_priv(lyr);

	if ((NULL == lyr) || (NULL == lyrp)) {
		DE_WRN("NULL hdl!\n");
		return -1;
	}

	spin_lock_irqsave(&mgr_data_lock, flags);
	lyrp->cfg->flag |= LAYER_ALL_DIRTY;
	spin_unlock_irqrestore(&mgr_data_lock, flags);
	disp_lyr_apply(lyr);

	return DIS_SUCCESS;
}

static s32 disp_lyr_dump(struct disp_layer *lyr, char *buf)
{
	unsigned long flags;
	struct disp_layer_config_data data;
	struct disp_layer_private_data *lyrp = disp_lyr_get_priv(lyr);
	u32 count = 0;

	if ((NULL == lyr) || (NULL == lyrp)) {
		DE_WRN("NULL hdl!\n");
		return -1;
	}

	spin_lock_irqsave(&mgr_data_lock, flags);
	memcpy(&data, lyrp->cfg, sizeof(struct disp_layer_config_data));
	spin_unlock_irqrestore(&mgr_data_lock, flags);

	count += sprintf(buf + count, " %5s ", (data.config.info.mode == LAYER_MODE_BUFFER)? "BUF":"COLOR");
	count += sprintf(buf + count, " %8s ", (data.config.enable==1)?"enable":"disable");
	count += sprintf(buf + count, "ch[%1d] ", data.config.channel);
	count += sprintf(buf + count, "lyr[%1d] ", data.config.layer_id);
	count += sprintf(buf + count, "z[%1d] ", data.config.info.zorder);
	count += sprintf(buf + count, "prem[%1s] ", (data.config.info.fb.pre_multiply)? "Y":"N");
	count += sprintf(buf + count, "a[%5s %3d] ", (data.config.info.alpha_mode)? "globl":"pixel", data.config.info.alpha_value);
	count += sprintf(buf + count, "fmt[%3d] ", data.config.info.fb.format);
	count += sprintf(buf + count, "fb[%4d,%4d;%4d,%4d;%4d,%4d] ", data.config.info.fb.size[0].width, data.config.info.fb.size[0].height,
		data.config.info.fb.size[1].width, data.config.info.fb.size[1].height, data.config.info.fb.size[2].width, data.config.info.fb.size[2].height);
	count += sprintf(buf + count, "crop[%4d,%4d,%4d,%4d] ", (unsigned int)(data.config.info.fb.crop.x>>32), (unsigned int)(data.config.info.fb.crop.y>>32),
		(unsigned int)(data.config.info.fb.crop.width>>32), (unsigned int)(data.config.info.fb.crop.height>>32));
	count += sprintf(buf + count, "frame[%4d,%4d,%4d,%4d] ", data.config.info.screen_win.x, data.config.info.screen_win.y, data.config.info.screen_win.width, data.config.info.screen_win.height);
	count += sprintf(buf + count, "addr[%8llx,%8llx,%8llx] ", data.config.info.fb.addr[0], data.config.info.fb.addr[1], data.config.info.fb.addr[2]);
	count += sprintf(buf + count, "flags[0x%8x] trd[%1d,%1d]\n", data.config.info.fb.flags, data.config.info.b_trd_out, data.config.info.out_trd_mode);

	return count;
}

static s32 disp_init_lyr(disp_bsp_init_para * para)
{
	u32 num_screens = 0, num_channels = 0, num_layers = 0;
	u32 max_num_layers = 0;
	u32 disp, chn, layer_id, layer_index = 0;

	DE_INF("disp_init_lyr\n");

	num_screens = bsp_disp_feat_get_num_screens();
	for (disp=0; disp<num_screens; disp++) {
		max_num_layers += bsp_disp_feat_get_num_layers(disp);
	}

	lyrs = (struct disp_layer *)kmalloc(sizeof(struct disp_layer) * max_num_layers, GFP_KERNEL | __GFP_ZERO);
	if (NULL == lyrs) {
		DE_WRN("malloc memory fail! size=0x%x\n", (unsigned int)sizeof(struct disp_layer) * max_num_layers);
		return DIS_FAIL;
	}

	lyr_private = (struct disp_layer_private_data *)kmalloc(sizeof(struct disp_layer_private_data)\
		* max_num_layers, GFP_KERNEL | __GFP_ZERO);
	if (NULL == lyr_private) {
		DE_WRN("malloc memory fail! size=0x%x\n", (unsigned int)sizeof(struct disp_layer_private_data) * max_num_layers);
		return DIS_FAIL;
	}

	lyr_cfgs = (struct disp_layer_config_data *)kmalloc(sizeof(struct disp_layer_config_data)\
		* max_num_layers, GFP_KERNEL | __GFP_ZERO);
	if (NULL == lyr_cfgs) {
		DE_WRN("malloc memory fail! size=0x%x\n", (unsigned int)sizeof(struct disp_layer_private_data) * max_num_layers);
		return DIS_FAIL;
	}

	for (disp=0; disp<num_screens; disp++) {

		num_channels = bsp_disp_feat_get_num_channels(disp);
		for (chn=0; chn<num_channels; chn++) {
			num_layers = bsp_disp_feat_get_num_layers_by_chn(disp, chn);
			for (layer_id=0; layer_id<num_layers; layer_id++,layer_index ++) {
				struct disp_layer *lyr = &lyrs[layer_index];
				struct disp_layer_config_data *lyr_cfg = &lyr_cfgs[layer_index];
				struct disp_layer_private_data *lyrp = &lyr_private[layer_index];

				lyrp->shadow_protect = para->shadow_protect;
				lyrp->cfg = lyr_cfg;

				lyr_cfg->ops.vmap = disp_vmap;
				lyr_cfg->ops.vunmap = disp_vunmap;
				sprintf(lyr->name, "mgr%d chn%d lyr%d", disp, chn, layer_id);
				lyr->disp = disp;
				lyr->chn = chn;
				lyr->id = layer_id;
				lyr->data = (void*)lyrp;

				lyr->set_manager = disp_lyr_set_manager;
				lyr->unset_manager = disp_lyr_unset_manager;
				lyr->apply = disp_lyr_apply;
				lyr->force_apply = disp_lyr_force_apply;
				lyr->check = disp_lyr_check;
				lyr->check2 = disp_lyr_check2;
				lyr->save_and_dirty_check =
				    disp_lyr_save_and_dirty_check;
				lyr->save_and_dirty_check2 =
				    disp_lyr_save_and_dirty_check2;
				lyr->get_config = disp_lyr_get_config;
				lyr->get_config2 = disp_lyr_get_config2;
				lyr->dump = disp_lyr_dump;
				lyr->is_dirty = disp_lyr_is_dirty;
				lyr->dirty_clear = disp_lyr_dirty_clear;
				//lyr->init = disp_lyr_init;
				//lyr->exit = disp_lyr_exit;

				//lyr->init(lyr);
			}
		}
	}

	return 0;
}

struct disp_manager* disp_get_layer_manager(u32 disp)
{
	u32 num_screens;

	num_screens = bsp_disp_feat_get_num_screens();
	if (disp >= num_screens) {
		DE_WRN("disp %d out of range\n", disp);
		return NULL;
	}

	return &mgrs[disp];
}

static struct disp_manager_private_data *disp_mgr_get_priv(struct disp_manager *mgr)
{
	if (NULL == mgr) {
		DE_WRN("NULL hdl!\n");
		return NULL;
	}

	return &mgr_private[mgr->disp];
}

static struct disp_layer_config_data * disp_mgr_get_layer_cfg_head(struct disp_manager *mgr)
{
	int layer_index = 0, disp;
	int num_screens = bsp_disp_feat_get_num_screens();

	for (disp=0; disp<num_screens && disp<mgr->disp; disp++) {
		layer_index += bsp_disp_feat_get_num_layers(disp);
	}
	return &lyr_cfgs[layer_index];
}

static s32 disp_mgr_shadow_protect(struct disp_manager *mgr, bool protect)
{
	struct disp_manager_private_data *mgrp = disp_mgr_get_priv(mgr);

	if ((NULL == mgr) || (NULL == mgrp)) {
		DE_WRN("NULL hdl!\n");
		return -1;
	}

	if (mgrp->shadow_protect)
		return mgrp->shadow_protect(mgr->disp, protect);

	return -1;
}

#if 0
extern void sync_event_proc(u32 disp);
#if defined(__LINUX_PLAT__)
s32 disp_mgr_event_proc(int irq, void *parg)
#else
static s32 disp_mgr_event_proc(void *parg)
#endif
{
	u32 disp = (u32)parg;

	if (disp_al_manager_query_irq(disp)) {
		/* FIXME, curline & start_delay */
			sync_event_proc(disp);
	}

	return DISP_IRQ_RETURN;
}
#endif

static s32 disp_mgr_clk_init(struct disp_manager *mgr)
{
	struct disp_manager_private_data *mgrp = disp_mgr_get_priv(mgr);

	if ((NULL == mgr) || (NULL == mgrp)) {
		DE_WRN("NULL hdl!\n");
		return -1;
	}

	mgrp->clk_parent = clk_get_parent(mgrp->clk);
	mgrp->cfg->config.de_freq = clk_get_rate(mgrp->clk);

	return 0;
}

static s32 disp_mgr_clk_exit(struct disp_manager *mgr)
{
	struct disp_manager_private_data *mgrp = disp_mgr_get_priv(mgr);

	if ((NULL == mgr) || (NULL == mgrp)) {
		DE_WRN("NULL hdl!\n");
		return -1;
	}

	if (mgrp->clk_parent)
		clk_put(mgrp->clk_parent);

	return 0;
}

static s32 disp_mgr_clk_enable(struct disp_manager *mgr)
{
	struct disp_manager_private_data *mgrp = disp_mgr_get_priv(mgr);
	int ret = 0;
	unsigned long de_freq = 0;

	if ((NULL == mgr) || (NULL == mgrp)) {
		DE_WRN("NULL hdl!\n");
		return -1;
	}

	if (mgr->get_clk_rate && mgrp->clk) {
		DE_INF("set DE rate to %u\n", mgr->get_clk_rate(mgr));
		de_freq = mgr->get_clk_rate(mgr);
		clk_set_rate(mgrp->clk, de_freq);
		if (de_freq != clk_get_rate(mgrp->clk)) {
			if (mgrp->clk_parent)
				clk_set_rate(mgrp->clk_parent, de_freq);
			clk_set_rate(mgrp->clk, de_freq);
			if (de_freq != clk_get_rate(mgrp->clk)) {
				DE_WRN("Set DE clk fail\n");
				return -1;
			}
		}
	}

	DE_INF("mgr %d clk enable\n", mgr->disp);
	ret = clk_prepare_enable(mgrp->clk);

	if (0 != ret)
		DE_WRN("fail enable mgr's clock!\n");

	if (mgrp->extra_clk) {
		ret = clk_prepare_enable(mgrp->extra_clk);
		if (0 != ret)
			DE_WRN("fail enable mgr's extra_clk!\n");
	}
#if defined(CONFIG_INDEPENDENT_DE)
	if (mgrp->dpss_clk) {
		ret = clk_prepare_enable(mgrp->dpss_clk);
		if (ret != 0)
			DE_WRN("fail enable mgr's dpss_clk!\n");
	}
#endif

	return ret;
}

s32 disp_mgr_clk_disable(struct disp_manager *mgr)
{
	struct disp_manager_private_data *mgrp = disp_mgr_get_priv(mgr);

	if ((NULL == mgr) || (NULL == mgrp)) {
		DE_WRN("NULL hdl!\n");
		return -1;
	}

	if (mgrp->extra_clk)
		clk_disable(mgrp->extra_clk);

	//clk_disable(mgrp->clk);

#if defined(CONFIG_INDEPENDENT_DE)
	if (mgrp->dpss_clk)
		clk_disable(mgrp->dpss_clk);
#endif
	return 0;
}

/* Return: unit(hz) */
static s32 disp_mgr_get_clk_rate(struct disp_manager *mgr)
{
	struct disp_manager_private_data *mgrp = disp_mgr_get_priv(mgr);

	if ((NULL == mgr) || (NULL == mgrp)) {
		DE_WRN("NULL hdl!\n");
		return 0;
	}

#if defined(CONFIG_MACH_SUN8IW16)
	if (mgr->device && mgr->device->type != DISP_OUTPUT_TYPE_HDMI)
		mgrp->cfg->config.de_freq = 216000000;
	else
		mgrp->cfg->config.de_freq = 432000000;
#endif

	return mgrp->cfg->config.de_freq;
}

static s32 disp_mgr_init(struct disp_manager *mgr)
{
	struct disp_manager_private_data *mgrp = disp_mgr_get_priv(mgr);

	if ((NULL == mgr) || (NULL == mgrp)) {
		DE_WRN("NULL hdl!\n");
		return -1;
	}

	disp_mgr_clk_init(mgr);
	return 0;
}

static s32 disp_mgr_exit(struct disp_manager *mgr)
{
	struct disp_manager_private_data *mgrp = disp_mgr_get_priv(mgr);

	if ((NULL == mgr) || (NULL == mgrp)) {
		DE_WRN("NULL hdl!\n");
		return -1;
	}

	//FIXME, disable manager
	disp_mgr_clk_exit(mgr);

	return 0;
}

static s32 disp_mgr_set_back_color(struct disp_manager *mgr, struct disp_color *back_color)
{
	unsigned long flags;
	struct disp_manager_private_data *mgrp = disp_mgr_get_priv(mgr);

	if ((NULL == mgr) || (NULL == mgrp)) {
		DE_WRN("NULL hdl!\n");
		return -1;
	}

	spin_lock_irqsave(&mgr_data_lock, flags);
	memcpy(&mgrp->cfg->config.back_color, back_color, sizeof(struct disp_color));
	mgrp->cfg->flag |= MANAGER_BACK_COLOR_DIRTY;
	spin_unlock_irqrestore(&mgr_data_lock, flags);

	mgr->apply(mgr);

	return DIS_SUCCESS;
}

static s32 disp_mgr_get_back_color(struct disp_manager *mgr, struct disp_color *back_color)
{
	unsigned long flags;
	struct disp_manager_private_data *mgrp = disp_mgr_get_priv(mgr);

	if ((NULL == mgr) || (NULL == mgrp)) {
		DE_WRN("NULL hdl!\n");
		return -1;
	}

  spin_lock_irqsave(&mgr_data_lock, flags);
	memcpy(back_color, &mgrp->cfg->config.back_color, sizeof(struct disp_color));
	spin_unlock_irqrestore(&mgr_data_lock, flags);

	return DIS_SUCCESS;
}

static s32 disp_mgr_set_color_key(struct disp_manager *mgr, struct disp_colorkey *ck)
{
	unsigned long flags;
	struct disp_manager_private_data *mgrp = disp_mgr_get_priv(mgr);

	if ((NULL == mgr) || (NULL == mgrp)) {
		DE_WRN("NULL hdl!\n");
		return -1;
	}

	spin_lock_irqsave(&mgr_data_lock, flags);
	memcpy(&mgrp->cfg->config.ck, ck, sizeof(struct disp_colorkey));
	mgrp->cfg->flag |= MANAGER_CK_DIRTY;
	spin_unlock_irqrestore(&mgr_data_lock, flags);

	mgr->apply(mgr);

	return DIS_SUCCESS;
}

static s32 disp_mgr_get_color_key(struct disp_manager *mgr, struct disp_colorkey *ck)
{
	unsigned long flags;
	struct disp_manager_private_data *mgrp = disp_mgr_get_priv(mgr);

	if ((NULL == mgr) || (NULL == mgrp)) {
		DE_WRN("NULL hdl!\n");
		return -1;
	}

	spin_lock_irqsave(&mgr_data_lock, flags);
	memcpy(ck, &mgrp->cfg->config.ck, sizeof(struct disp_colorkey));
	spin_unlock_irqrestore(&mgr_data_lock, flags);

	return DIS_SUCCESS;
}

static s32 disp_mgr_set_output_color_range(struct disp_manager *mgr, u32 color_range)
{
	unsigned long flags;
	struct disp_manager_private_data *mgrp = disp_mgr_get_priv(mgr);

	if ((NULL == mgr) || (NULL == mgrp)) {
		DE_WRN("NULL hdl!\n");
		return -1;
	}

	spin_lock_irqsave(&mgr_data_lock, flags);
	mgrp->cfg->config.color_range = color_range;
	mgrp->cfg->flag |= MANAGER_COLOR_RANGE_DIRTY;
	mgrp->color_range_modified = true;
	spin_unlock_irqrestore(&mgr_data_lock, flags);

	mgr->apply(mgr);

	return DIS_SUCCESS;
}

static s32 disp_mgr_get_output_color_range(struct disp_manager *mgr)
{
	struct disp_manager_private_data *mgrp = disp_mgr_get_priv(mgr);

	if ((NULL == mgr) || (NULL == mgrp)) {
		DE_WRN("NULL hdl!\n");
		return 0;
	}

	return mgrp->cfg->config.color_range;
}

static s32 disp_mgr_update_color_space(struct disp_manager *mgr)
{
	unsigned long flags;
	struct disp_manager_private_data *mgrp = disp_mgr_get_priv(mgr);
	unsigned int cs = 0, color_range = 0;

	if ((NULL == mgr) || (NULL == mgrp)) {
		DE_WRN("NULL hdl!\n");
		return -1;
	}

	if (mgr->device) {
		if (mgr->device->get_input_csc)
			cs = mgr->device->get_input_csc(mgr->device);
		if (mgr->device && mgr->device->get_input_color_range)
			color_range = mgr->device->get_input_color_range(mgr->device);
	}

	spin_lock_irqsave(&mgr_data_lock, flags);
	mgrp->cfg->config.cs = cs;
	if (!mgrp->color_range_modified)
		mgrp->cfg->config.color_range = color_range;
	mgrp->cfg->flag |= MANAGER_COLOR_SPACE_DIRTY;
	spin_unlock_irqrestore(&mgr_data_lock, flags);

	mgr->apply(mgr);

	return DIS_SUCCESS;
}

static s32 disp_mgr_set_layer_config(struct disp_manager *mgr, struct disp_layer_config *config, unsigned int layer_num)
{
	struct disp_manager_private_data *mgrp = disp_mgr_get_priv(mgr);
	unsigned int num_layers = 0, layer_index = 0;
	struct disp_layer *lyr = NULL;

	if ((NULL == mgr) || (NULL == mgrp)) {
		DE_WRN("NULL hdl!\n");
		return -1;
	}
	DE_INF("mgr%d, config %d layers\n", mgr->disp, layer_num);

	num_layers = bsp_disp_feat_get_num_layers(mgr->disp);
	if ((NULL == config) || (layer_num == 0) || (layer_num > num_layers)) {
		DE_WRN("NULL hdl!\n");
		return -1;
	}
	DE_INF("layer:ch%d, layer%d, format=%d, size=<%d,%d>, crop=<%lld,%lld,%lld,%lld>,frame=<%d,%d>, en=%d addr[0x%llx,0x%llx,0x%llx> alpha=<%d,%d>\n",
		config->channel, config->layer_id,  config->info.fb.format,
		config->info.fb.size[0].width, config->info.fb.size[0].height,
		config->info.fb.crop.x>>32, config->info.fb.crop.y>>32, config->info.fb.crop.width>>32, config->info.fb.crop.height>>32,
		config->info.screen_win.width, config->info.screen_win.height,
		config->enable, config->info.fb.addr[0], config->info.fb.addr[1], config->info.fb.addr[2], config->info.alpha_mode, config->info.alpha_value);

	mutex_lock(&mgr_mlock);
	for (layer_index=0; layer_index < layer_num; layer_index++) {
		struct disp_layer *lyr = NULL;

		lyr = disp_get_layer(mgr->disp, config->channel, config->layer_id);
		if (lyr != NULL)
			lyr->save_and_dirty_check(lyr, config);
		else
			DE_WRN("get layer(%d,%d,%d) fail\n", mgr->disp,
			       config->channel, config->layer_id);

		config++;
	}

	if (mgr->apply)
		mgr->apply(mgr);

	list_for_each_entry(lyr, &mgr->lyr_list, list) {
		lyr->dirty_clear(lyr);
	}
	mutex_unlock(&mgr_mlock);

	return DIS_SUCCESS;
}

static s32 disp_mgr_get_layer_config(struct disp_manager *mgr, struct disp_layer_config *config, unsigned int layer_num)
{
	struct disp_manager_private_data *mgrp = disp_mgr_get_priv(mgr);
	struct disp_layer *lyr;
	unsigned int num_layers = 0, layer_index = 0;

	if ((NULL == mgr) || (NULL == mgrp)) {
		DE_WRN("NULL hdl!\n");
		return -1;
	}

	num_layers = bsp_disp_feat_get_num_layers(mgr->disp);
	if ((NULL == config) || (layer_num == 0) || (layer_num > num_layers)) {
		DE_WRN("NULL hdl!\n");
		return -1;
	}
	for (layer_index=0; layer_index < layer_num; layer_index++) {
		lyr = disp_get_layer(mgr->disp, config->channel, config->layer_id);
		if (lyr != NULL)
			lyr->get_config(lyr, config);
		else
			DE_WRN("get layer(%d,%d,%d) fail\n", mgr->disp,
			       config->channel, config->layer_id);

		config++;
	}

	return DIS_SUCCESS;
}

static s32
disp_mgr_set_layer_config2(struct disp_manager *mgr,
			  struct disp_layer_config2 *config,
			  unsigned int layer_num)
{
	struct disp_manager_private_data *mgrp = disp_mgr_get_priv(mgr);
	unsigned int num_layers = 0, layer_index = 0;
	struct disp_layer *lyr = NULL;

	if ((mgr == NULL) || (mgrp == NULL)) {
		DE_WRN("NULL hdl!\n");
		goto err;
	}
	DE_INF("mgr%d, config %d layers\n", mgr->disp, layer_num);

	num_layers = bsp_disp_feat_get_num_layers(mgr->disp);
	if ((config == NULL) || (layer_num == 0) || (layer_num > num_layers)) {
		DE_WRN("NULL hdl!\n");
		goto err;
	}

	mutex_lock(&mgr_mlock);
	for (layer_index = 0; layer_index < layer_num; layer_index++) {
		struct disp_layer *lyr = NULL;

		lyr = disp_get_layer(mgr->disp, config->channel,
				     config->layer_id);
		if (lyr != NULL)
			lyr->save_and_dirty_check2(lyr, config);
		else
			DE_WRN("get layer(%d,%d,%d) fail\n", mgr->disp,
			       config->channel, config->layer_id);

		config++;
	}

	if (mgr->apply)
		mgr->apply(mgr);

	list_for_each_entry(lyr, &mgr->lyr_list, list) {
		lyr->dirty_clear(lyr);
	}
	mutex_unlock(&mgr_mlock);

	return DIS_SUCCESS;

err:
	return -1;
}

static s32
disp_mgr_get_layer_config2(struct disp_manager *mgr,
			  struct disp_layer_config2 *config,
			  unsigned int layer_num)
{
	struct disp_manager_private_data *mgrp = disp_mgr_get_priv(mgr);
	struct disp_layer *lyr;
	unsigned int num_layers = 0, layer_index = 0;

	if ((mgr == NULL) || (mgrp == NULL)) {
		DE_WRN("NULL hdl!\n");
		goto err;
	}

	num_layers = bsp_disp_feat_get_num_layers(mgr->disp);
	if ((config == NULL) || (layer_num == 0) || (layer_num > num_layers)) {
		DE_WRN("NULL hdl!\n");
		goto err;
	}
	for (layer_index = 0; layer_index < layer_num; layer_index++) {
		lyr = disp_get_layer(mgr->disp, config->channel,
				     config->layer_id);
		if (lyr != NULL)
			lyr->get_config2(lyr, config);
		else
			DE_WRN("get layer(%d,%d,%d) fail\n", mgr->disp,
			       config->channel, config->layer_id);
		config++;
	}

	return DIS_SUCCESS;

err:
	return -1;
}

static s32 disp_mgr_sync(struct disp_manager *mgr)
{
	unsigned long flags;
	struct disp_manager_private_data *mgrp = NULL;
	struct disp_enhance *enhance = NULL;
	struct disp_smbl *smbl = NULL;
	struct disp_capture *cptr = NULL;

	if (NULL == mgr) {
		DE_WRN("NULL hdl!\n");
		return -1;
	}

	mgrp = disp_mgr_get_priv(mgr);
	if (NULL == mgrp) {
		DE_WRN("get mgr %d's priv fail!!\n", mgr->disp);
		return -1;
	}
	enhance = mgr->enhance;
	smbl = mgr->smbl;
	cptr = mgr->cptr;

	spin_lock_irqsave(&mgr_data_lock, flags);
	if (!mgrp->enabled) {
		spin_unlock_irqrestore(&mgr_data_lock, flags);
		return -1;
	}
	spin_unlock_irqrestore(&mgr_data_lock, flags);

	//mgr->update_regs(mgr);
	disp_al_manager_sync(mgr->disp);
	mgr->update_regs(mgr);
	if (mgr->device && mgr->device->is_in_safe_period) {
		if (!mgr->device->is_in_safe_period(mgr->device)) {
			mgrp->err = true;
			mgrp->err_cnt++;
		} else {
			mgrp->err = false;
		}
	}
	spin_lock_irqsave(&mgr_data_lock, flags);
	mgrp->applied = false;
	spin_unlock_irqrestore(&mgr_data_lock, flags);

	/* enhance */
	if (enhance && enhance->sync)
		enhance->sync(enhance);

	/* smbl */
	if (smbl && smbl->sync)
		smbl->sync(smbl);

	/* capture */
	if (cptr && cptr->sync)
		cptr->sync(cptr);

	return DIS_SUCCESS;
}

static s32 disp_mgr_tasklet(struct disp_manager *mgr)
{
	struct disp_manager_private_data *mgrp = NULL;
	struct disp_enhance *enhance = NULL;
	struct disp_smbl *smbl = NULL;
	struct disp_capture *cptr = NULL;

	if (NULL == mgr) {
		DE_WRN("NULL hdl!\n");
		return -1;
	}
	mgrp = disp_mgr_get_priv(mgr);
	if (NULL == mgrp) {
		DE_WRN("get mgr %d's priv fail!!\n", mgr->disp);
		return -1;
	}
	enhance = mgr->enhance;
	smbl = mgr->smbl;
	cptr = mgr->cptr;

	if (!mgrp->enabled)
		return -1;

	/* enhance */
	if (enhance && enhance->tasklet)
		enhance->tasklet(enhance);

	/* smbl */
	if (smbl && smbl->tasklet)
		smbl->tasklet(smbl);

	/* capture */
	if (cptr && cptr->tasklet)
		cptr->tasklet(cptr);

	return DIS_SUCCESS;
}

static s32 disp_mgr_update_regs(struct disp_manager *mgr)
{
	unsigned long flags;
	struct disp_manager_private_data *mgrp = disp_mgr_get_priv(mgr);

	if ((NULL == mgr) || (NULL == mgrp)) {
		DE_WRN("NULL hdl!\n");
		return -1;
	}
	//__inf("disp_mgr_update_regs, mgr%d\n", mgr->disp);

#if 0
	spin_lock_irqsave(&mgr_data_lock, flags);
	if (!mgrp->enabled) {
		spin_unlock_irqrestore(&mgr_data_lock, flags);
	   return -1;
	}
	spin_unlock_irqrestore(&mgr_data_lock, flags);
#endif

	//disp_mgr_shadow_protect(mgr, true);
	//FIXME update_regs, other module may need to sync while manager don't
	//if (true == mgrp->applied)
		disp_al_manager_update_regs(mgr->disp);
	//disp_mgr_shadow_protect(mgr, false);

	spin_lock_irqsave(&mgr_data_lock, flags);
	mgrp->applied = false;
	spin_unlock_irqrestore(&mgr_data_lock, flags);

	return DIS_SUCCESS;
}

static s32 disp_mgr_apply(struct disp_manager *mgr)
{
	unsigned long flags;
	struct disp_manager_private_data *mgrp = disp_mgr_get_priv(mgr);
	struct disp_manager_data data;
	bool mgr_dirty = false;
	bool lyr_drity = false;
	struct disp_layer *lyr = NULL;

	if ((NULL == mgr) || (NULL == mgrp)) {
		DE_WRN("NULL hdl!\n");
		return -1;
	}
	DE_INF("mgr %d apply\n", mgr->disp);

  spin_lock_irqsave(&mgr_data_lock, flags);
	if ((mgrp->enabled) && (mgrp->cfg->flag & MANAGER_ALL_DIRTY))
	{
		mgr_dirty = true;
		memcpy(&data, mgrp->cfg, sizeof(struct disp_manager_data));
		mgrp->cfg->flag &= ~MANAGER_ALL_DIRTY;
	}

	list_for_each_entry(lyr, &mgr->lyr_list, list)
	{
		if (lyr->is_dirty && lyr->is_dirty(lyr)) {
			lyr_drity = true;
			break;
		}
	}

	mgrp->applied = true;
	spin_unlock_irqrestore(&mgr_data_lock, flags);

	disp_mgr_shadow_protect(mgr, true);
	if (mgr_dirty)
		disp_al_manager_apply(mgr->disp, &data);

	if (lyr_drity && mgrp->enabled) {
		u32 num_layers = bsp_disp_feat_get_num_layers(mgr->disp);
		struct disp_layer_config_data *lyr_cfg = disp_mgr_get_layer_cfg_head(mgr);

		disp_al_layer_apply(mgr->disp, lyr_cfg, num_layers);
	}
	disp_mgr_shadow_protect(mgr, false);

	spin_lock_irqsave(&mgr_data_lock, flags);
	mgrp->applied = true;
	spin_unlock_irqrestore(&mgr_data_lock, flags);

	return DIS_SUCCESS;
}

static s32 disp_mgr_force_apply(struct disp_manager *mgr)
{
	unsigned long flags;
	struct disp_manager_private_data *mgrp = disp_mgr_get_priv(mgr);
	struct disp_layer* lyr = NULL;
	struct disp_enhance *enhance = NULL;
	struct disp_smbl *smbl = NULL;

	if ((NULL == mgr) || (NULL == mgrp)) {
		DE_WRN("NULL hdl!\n");
		return -1;
	}

	enhance = mgr->enhance;
	smbl = mgr->smbl;

	spin_lock_irqsave(&mgr_data_lock, flags);
	mgrp->cfg->flag |= MANAGER_ALL_DIRTY;
	spin_unlock_irqrestore(&mgr_data_lock, flags);
	list_for_each_entry(lyr, &mgr->lyr_list, list) {
		lyr->force_apply(lyr);
	}

	disp_mgr_apply(mgr);
	disp_mgr_sync(mgr);

	/* enhance */
	if (enhance && enhance->force_apply)
		enhance->force_apply(enhance);

	/* smbl */
	if (smbl && smbl->force_apply)
		smbl->force_apply(smbl);

	return 0;
}

static s32 disp_mgr_enable(struct disp_manager *mgr)
{
	unsigned long flags;
	struct disp_manager_private_data *mgrp = disp_mgr_get_priv(mgr);
	unsigned int width = 0, height = 0;
	unsigned int color_range = DISP_COLOR_RANGE_0_255;
	int ret;
	struct disp_device_config dev_config;

	if ((NULL == mgr) || (NULL == mgrp)) {
		DE_WRN("NULL hdl!\n");
		return -1;
	}
	DE_INF("mgr %d enable\n", mgr->disp);

	//disp_sys_register_irq(mgrp->irq_no,0,disp_mgr_event_proc,(void*)mgr->disp,0,0);
	//disp_sys_enable_irq(mgrp->irq_no);

	dev_config.bits = DISP_DATA_8BITS;
	dev_config.eotf = DISP_EOTF_GAMMA22;
	dev_config.cs = DISP_BT601_F;

	ret = disp_mgr_clk_enable(mgr);
	if (0 != ret)
		return ret;

	disp_al_manager_init(mgr->disp);

	if (mgr->device) {
		disp_al_device_set_de_id(mgr->device->hwdev_index, mgr->disp);
		disp_al_device_set_de_use_rcq(mgr->device->hwdev_index,
			disp_feat_is_using_rcq(mgr->disp));
		disp_al_device_set_output_type(mgr->device->hwdev_index,
			mgr->device->type);
		if (mgr->device->get_resolution)
			mgr->device->get_resolution(mgr->device, &width, &height);
		if (mgr->device->get_static_config)
			mgr->device->get_static_config(mgr->device,
						       &dev_config);
		if (mgr->device && mgr->device->get_input_color_range)
			color_range = mgr->device->get_input_color_range(mgr->device);
		mgrp->cfg->config.disp_device = mgr->device->disp;
		mgrp->cfg->config.hwdev_index = mgr->device->hwdev_index;
		if (mgr->device && mgr->device->is_interlace)
			mgrp->cfg->config.interlace = mgr->device->is_interlace(mgr->device);
		else
			mgrp->cfg->config.interlace = 0;
		if (mgr->device && mgr->device->get_fps)
			mgrp->cfg->config.device_fps =
			    mgr->device->get_fps(mgr->device);
		else
			mgrp->cfg->config.device_fps = 60;
	}

	DE_INF("output res: %d x %d, range=%d, interlace=%d\n",
		width, height, color_range, mgrp->cfg->config.interlace);

	spin_lock_irqsave(&mgr_data_lock, flags);
	mgrp->enabled = 1;
	mgrp->cfg->config.enable = 1;
	mgrp->cfg->flag |= MANAGER_ENABLE_DIRTY;

	mgrp->cfg->config.size.width = width;
	mgrp->cfg->config.size.height = height;
	mgrp->cfg->config.cs = dev_config.format;
	mgrp->cfg->config.color_space = dev_config.cs;
	mgrp->cfg->config.eotf = dev_config.eotf;
	mgrp->cfg->config.data_bits = dev_config.bits;
	if (!mgrp->color_range_modified)
		mgrp->cfg->config.color_range = color_range;
	mgrp->cfg->flag |= MANAGER_SIZE_DIRTY;
	mgrp->cfg->flag |= MANAGER_COLOR_SPACE_DIRTY;
	spin_unlock_irqrestore(&mgr_data_lock, flags);

	disp_mgr_force_apply(mgr);

	if (mgr->enhance && mgr->enhance->enable)
		mgr->enhance->enable(mgr->enhance);

	return 0;
}

static s32 disp_mgr_sw_enable(struct disp_manager *mgr)
{
	unsigned long flags;
	struct disp_manager_private_data *mgrp = disp_mgr_get_priv(mgr);
	unsigned int width = 0, height = 0;
	unsigned int cs = 0, color_range = DISP_COLOR_RANGE_0_255;

	if ((NULL == mgr) || (NULL == mgrp)) {
		DE_WRN("NULL hdl!\n");
		return -1;
	}
	DE_INF("mgr %d enable\n", mgr->disp);

	if (mgr->device) {
		if (mgr->device->get_resolution)
			mgr->device->get_resolution(mgr->device, &width, &height);
		if (mgr->device->get_input_csc)
			cs = mgr->device->get_input_csc(mgr->device);
		if (mgr->device && mgr->device->get_input_color_range)
			color_range = mgr->device->get_input_color_range(mgr->device);
		mgrp->cfg->config.disp_device = mgr->device->disp;
		mgrp->cfg->config.hwdev_index = mgr->device->hwdev_index;
		if (mgr->device && mgr->device->is_interlace)
			mgrp->cfg->config.interlace = mgr->device->is_interlace(mgr->device);
		else
			mgrp->cfg->config.interlace = 0;
	}
	DE_INF("output res: %d x %d, cs=%d, range=%d, interlace=%d\n",
		width, height, cs, color_range,mgrp->cfg->config.interlace);

	spin_lock_irqsave(&mgr_data_lock, flags);
	mgrp->enabled = 1;
	mgrp->cfg->config.enable = 1;
	mgrp->cfg->flag |= MANAGER_ENABLE_DIRTY;

	mgrp->cfg->config.size.width = width;
	mgrp->cfg->config.size.height = height;
	mgrp->cfg->config.cs = cs;
	mgrp->cfg->config.color_range = color_range;
	mgrp->cfg->flag |= MANAGER_SIZE_DIRTY;
	spin_unlock_irqrestore(&mgr_data_lock, flags);

	disp_mgr_force_apply(mgr);

	if (mgr->enhance && mgr->enhance->enable)
		mgr->enhance->enable(mgr->enhance);

	return 0;
}


static s32 disp_mgr_disable(struct disp_manager *mgr)
{
	unsigned long flags;
	struct disp_manager_private_data *mgrp = disp_mgr_get_priv(mgr);

	if ((NULL == mgr) || (NULL == mgrp)) {
		DE_WRN("NULL hdl!\n");
		return -1;
	}

	DE_INF("mgr %d disable\n", mgr->disp);

	if (mgr->enhance && mgr->enhance->disable)
		mgr->enhance->disable(mgr->enhance);

	spin_lock_irqsave(&mgr_data_lock, flags);
	mgrp->enabled = 0;
	mgrp->cfg->flag |= MANAGER_ENABLE_DIRTY;
	spin_unlock_irqrestore(&mgr_data_lock, flags);
	disp_mgr_force_apply(mgr);
	disp_delay_ms(5);

	disp_al_manager_exit(mgr->disp);
	disp_mgr_clk_disable(mgr);

	//disp_sys_disable_irq(mgrp->irq_no);
	//disp_sys_unregister_irq(mgrp->irq_no, disp_mgr_event_proc,(void*)mgr->disp);

	return 0;
}

static s32 disp_mgr_is_enabled(struct disp_manager *mgr)
{
	struct disp_manager_private_data *mgrp = disp_mgr_get_priv(mgr);

	if ((NULL == mgr) || (NULL == mgrp)) {
		DE_WRN("NULL hdl!\n");
		return -1;
	}

	return mgrp->enabled;

}

static s32 disp_mgr_dump(struct disp_manager *mgr, char *buf)
{

	struct disp_manager_private_data *mgrp = disp_mgr_get_priv(mgr);
	unsigned int count = 0;
	char const *fmt_name[] = {"rgb", "yuv444", "yuv422", "yuv420"};
	char const *bits_name[] = {
		"8bits",
		"10bits",
		"12bits",
		"16bits"
	};
	bool direct_show = false;

	if ((mgr == NULL) || (mgrp == NULL)) {
		DE_WRN("NULL hdl!\n");
		return -1;
	}

	if ((NULL == mgr) || (mgrp == NULL)) {
		DE_WRN("NULL hdl!\n");
		return -1;
	}

//	direct_show = disp_al_get_direct_show_state(mgr->disp);

	count += sprintf(
	    buf + count,
	    "mgr%d: %dx%d fmt[%s] cs[0x%x] range[%s] eotf[0x%x] bits[%s] err[%u] force_sync[%u] %s direct_show[%s] iommu[%d]\n",
	    mgr->disp, mgrp->cfg->config.size.width, mgrp->cfg->config.size.height,
	    (mgrp->cfg->config.cs < 4) ? fmt_name[mgrp->cfg->config.cs]
				       : "undef",
	    mgrp->cfg->config.color_space,
	    (mgrp->cfg->config.color_range == DISP_COLOR_RANGE_0_255) ? "full"
								      : "limit",
	    mgrp->cfg->config.eotf,
	    (mgrp->cfg->config.data_bits < 4) ?
		bits_name[mgrp->cfg->config.data_bits] : "undef",
		mgrp->err_cnt,
		 mgrp->force_sync_cnt,
		 (mgrp->cfg->config.blank) ? "blank" : "unblank",
	    (direct_show) ? "true" : "false", 0);


	return count;

}

static s32 disp_mgr_blank(struct disp_manager *mgr, bool blank)
{
	struct disp_manager_private_data *mgrp = disp_mgr_get_priv(mgr);
	struct disp_layer* lyr = NULL;

	if ((NULL == mgr) || (NULL == mgrp)) {
		DE_WRN("NULL hdl!\n");
		return -1;
	}

	mutex_lock(&mgr_mlock);
	list_for_each_entry(lyr, &mgr->lyr_list, list) {
		lyr->force_apply(lyr);
	}
	mgrp->cfg->config.blank = blank;
	mgrp->cfg->flag |= MANAGER_BLANK_DIRTY;
	mutex_unlock(&mgr_mlock);

	mgr->apply(mgr);

	return 0;
}

s32 disp_mgr_set_ksc_para(struct disp_manager *mgr,
		    struct disp_ksc_info *pinfo)
{
	unsigned long flags;
	struct disp_manager_private_data *mgrp = disp_mgr_get_priv(mgr);

	spin_lock_irqsave(&mgr_data_lock, flags);
	memcpy(&mgrp->cfg->config.ksc, pinfo, sizeof(struct disp_colorkey));
	mgrp->cfg->flag |= MANAGER_KSC_DIRTY;
	spin_unlock_irqrestore(&mgr_data_lock, flags);

	return mgr->apply(mgr);
}

s32 disp_init_mgr(disp_bsp_init_para * para)
{
	u32 num_screens;
	u32 disp;
	struct disp_manager *mgr;
	struct disp_manager_private_data *mgrp;

	DE_INF("%s\n", __func__);

	spin_lock_init(&mgr_data_lock);
	mutex_init(&mgr_mlock);
	num_screens = bsp_disp_feat_get_num_screens();
	mgrs = (struct disp_manager *)kmalloc(sizeof(struct disp_manager) * num_screens, GFP_KERNEL | __GFP_ZERO);
	if (NULL == mgrs) {
		DE_WRN("malloc memory fail!\n");
		return DIS_FAIL;
	}
	mgr_private = (struct disp_manager_private_data *)kmalloc(sizeof(struct disp_manager_private_data)\
	* num_screens, GFP_KERNEL | __GFP_ZERO);
	if (NULL == mgr_private) {
		DE_WRN("malloc memory fail! size=0x%x x %d\n", (unsigned int)sizeof(struct disp_manager_private_data), num_screens);
		return DIS_FAIL;
	}
	mgr_cfgs = (struct disp_manager_data *)kmalloc(sizeof(struct disp_manager_data)\
		* num_screens, GFP_KERNEL | __GFP_ZERO);
	if (NULL == mgr_private) {
		DE_WRN("malloc memory fail! size=0x%x x %d\n",
			(unsigned int)sizeof(struct disp_manager_private_data), num_screens);
		return DIS_FAIL;
	}

	for (disp=0; disp<num_screens; disp++) {
		mgr = &mgrs[disp];
		mgrp = &mgr_private[disp];

		DE_INF("mgr %d, 0x%p\n", disp, mgr);

		sprintf(mgr->name, "mgr%d", disp);
		mgr->disp = disp;
		mgrp->cfg = &mgr_cfgs[disp];
		mgrp->irq_no = para->irq_no[DISP_MOD_DE];
		mgrp->shadow_protect = para->shadow_protect;
		mgrp->clk = para->mclk[DISP_MOD_DE];
#if defined(CONFIG_INDEPENDENT_DE)
		if (disp == 0) {
			mgrp->dpss_clk = para->mclk[DISP_MOD_DPSS0];
		} else {
			mgrp->clk = para->mclk[DISP_MOD_DE1];
			mgrp->dpss_clk = para->mclk[DISP_MOD_DPSS1];
		}
#endif
#if defined(HAVE_DEVICE_COMMON_MODULE)
		mgrp->extra_clk = para->mclk[DISP_MOD_DEVICE];
#endif

		mgr->enable = disp_mgr_enable;
		mgr->sw_enable = disp_mgr_sw_enable;
		mgr->disable = disp_mgr_disable;
		mgr->is_enabled = disp_mgr_is_enabled;
		mgr->set_color_key = disp_mgr_set_color_key;
		mgr->get_color_key = disp_mgr_get_color_key;
		mgr->set_back_color = disp_mgr_set_back_color;
		mgr->get_back_color = disp_mgr_get_back_color;
		mgr->set_layer_config = disp_mgr_set_layer_config;
		mgr->get_layer_config = disp_mgr_get_layer_config;
		mgr->set_layer_config2 = disp_mgr_set_layer_config2;
		mgr->get_layer_config2 = disp_mgr_get_layer_config2;
		mgr->set_output_color_range = disp_mgr_set_output_color_range;
		mgr->get_output_color_range = disp_mgr_get_output_color_range;
		mgr->update_color_space = disp_mgr_update_color_space;
		mgr->set_ksc_para = disp_mgr_set_ksc_para;
		mgr->dump = disp_mgr_dump;
		mgr->blank = disp_mgr_blank;
		mgr->get_clk_rate = disp_mgr_get_clk_rate;

		mgr->init = disp_mgr_init;
		mgr->exit = disp_mgr_exit;

		mgr->apply = disp_mgr_apply;
		mgr->update_regs = disp_mgr_update_regs;
		mgr->force_apply = disp_mgr_force_apply;
		mgr->sync = disp_mgr_sync;
		mgr->tasklet = disp_mgr_tasklet;

		INIT_LIST_HEAD(&mgr->lyr_list);

		mgr->init(mgr);
	}

	disp_init_lyr(para);

	return 0;
}
