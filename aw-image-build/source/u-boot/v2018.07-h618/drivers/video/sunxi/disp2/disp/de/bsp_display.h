/*
 * drivers/video/sunxi/disp2/disp/de/bsp_display.h
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
#ifndef __BSP_DISPLAY_H__
#define __BSP_DISPLAY_H__

#include "disp_lcd.h"
#include "disp_private.h"

struct sunxi_disp_source_ops {
	int (*sunxi_lcd_delay_ms)(unsigned int ms);
	int (*sunxi_lcd_delay_us)(unsigned int us);
	int (*sunxi_lcd_tcon_enable)(unsigned int scree_id);
	int (*sunxi_lcd_tcon_disable)(unsigned int scree_id);
	int (*sunxi_lcd_cpu_write)(u32 sel, u32 index, u32 data);
	int (*sunxi_lcd_cpu_write_index)(unsigned int scree_id, unsigned int index);
	int (*sunxi_lcd_cpu_write_data)(unsigned int scree_id, unsigned int data);
	int (*sunxi_lcd_cpu_set_auto_mode)(unsigned int scree_id);
	int (*sunxi_lcd_dsi_dcs_write)(unsigned int scree_id, unsigned char command, unsigned char *para, unsigned int para_num);
	int (*sunxi_lcd_dsi_gen_write)(unsigned int scree_id, unsigned char command, unsigned char *para, unsigned int para_num);
	int (*sunxi_lcd_dsi_clk_enable)(u32 screen_id, u32 en);
	s32 (*sunxi_lcd_dsi_gen_short_read)(u32 sel, u8 *para_p, u8 para_num,
					    u8 *result);
	s32 (*sunxi_lcd_dsi_dcs_read)(u32 sel, u8 cmd, u8 *result, u32 *num_p);
	s32 (*sunxi_lcd_dsi_set_max_ret_size)(u32 sel, u32 size);
	int (*sunxi_lcd_dsi_mode_switch)(unsigned int scree_id,
					 u32 cmd_en, u32 lp_en);
	int (*sunxi_lcd_backlight_enable)(unsigned int screen_id);
	int (*sunxi_lcd_backlight_disable)(unsigned int screen_id);
	int (*sunxi_lcd_pwm_enable)(unsigned int screen_id);
	int (*sunxi_lcd_pwm_disable)(unsigned int screen_id);
	int (*sunxi_lcd_power_enable)(unsigned int screen_id, unsigned int pwr_id);
	int (*sunxi_lcd_power_disable)(unsigned int screen_id, unsigned int pwr_id);
	int (*sunxi_lcd_set_panel_funs)(char *drv_name, disp_lcd_panel_fun *lcd_cfg);
	int (*sunxi_lcd_pin_cfg)(unsigned int screen_id, unsigned int bon);
	int (*sunxi_lcd_gpio_set_value)(unsigned int screen_id, unsigned int io_index, u32 value);
	/**** add for lcd read gpio level id by lxm 20220310 ***/
	int (*sunxi_lcd_gpio_get_value)(unsigned int screen_id, unsigned int io_index);
	int (*sunxi_lcd_gpio_set_direction)(unsigned int screen_id, unsigned int io_index, u32 direction);
	int (*sunxi_lcd_switch_compat_panel)(unsigned int disp, unsigned int index);
};

struct sunxi_disp_edp_ops {
	int (*sunxi_edp_delay_ms)(unsigned int ms);
	int (*sunxi_edp_delay_us)(unsigned int us);
	int (*sunxi_edp_backlight_enable)(unsigned int screen_id);
	int (*sunxi_edp_backlight_disable)(unsigned int screen_id);
	int (*sunxi_edp_pwm_enable)(unsigned int screen_id);
	int (*sunxi_edp_pwm_disable)(unsigned int screen_id);
	int (*sunxi_edp_power_enable)(unsigned int screen_id,
								  unsigned int pwr_id);
	int (*sunxi_edp_power_disable)(unsigned int screen_id,
								   unsigned int pwr_id);
	int (*sunxi_edp_set_panel_funs)(char *drv_name, disp_lcd_panel_fun *edp_cfg);
	int (*sunxi_edp_pin_cfg)(unsigned int screen_id, unsigned int bon);
	int (*sunxi_edp_gpio_set_value)(unsigned int screen_id,
									unsigned int io_index, u32 value);
	int (*sunxi_edp_gpio_set_direction)(unsigned int screen_id,
					     unsigned int io_index,
					     u32 direction);
};

s32 bsp_disp_init(disp_bsp_init_para *para);
s32 bsp_disp_exit(u32 mode);
s32 bsp_disp_open(void);
s32 bsp_disp_close(void);
s32 bsp_disp_feat_get_num_screens(void);
s32 bsp_disp_feat_get_num_channels(u32 disp);
s32 bsp_disp_feat_get_num_layers(u32 screen_id);
s32 bsp_disp_feat_get_num_layers_by_chn(u32 disp, u32 chn);
s32 bsp_disp_feat_is_supported_output_types(u32 screen_id, u32 output_type);
u32 bsp_disp_get_screen_physical_width(u32 disp);
u32 bsp_disp_get_screen_physical_height(u32 disp);
s32 bsp_disp_get_screen_width(u32 disp);
s32 bsp_disp_get_screen_height(u32 disp);
s32 bsp_disp_get_screen_width_from_output_type(u32 disp, u32 output_type, u32 output_mode);
s32 bsp_disp_get_screen_height_from_output_type(u32 disp, u32 output_type, u32 output_mode);
s32 bsp_disp_get_lcd_registered(u32 disp);
s32 bsp_disp_get_hdmi_registered(void);
s32 bsp_disp_get_output_type(u32 disp);
s32 bsp_disp_device_switch(int disp, enum disp_output_type output_type, enum disp_output_type mode);
s32 bsp_disp_device_set_config(int disp, struct disp_device_config *config);
s32 bsp_disp_set_edp_func(struct disp_tv_func *func);
#ifdef CONFIG_EINK_PANEL_USED
s32 bsp_disp_eink_update(struct disp_eink_manager *manager, struct eink_8bpp_image *cimage);
#endif
s32 bsp_disp_set_hdmi_func(struct disp_device_func *func);
s32 bsp_disp_hdmi_check_support_mode(u32 disp, enum disp_output_type mode);
s32 bsp_disp_hdmi_get_support_mode(u32 disp, u32 init_mode);
s32 bsp_disp_hdmi_get_work_mode(u32 disp);
s32 bsp_disp_hdmi_set_detect(bool hpd);
s32 bsp_disp_hdmi_get_hpd_status(u32 disp);
s32 bsp_disp_tv_get_hpd_status(u32 disp);
s32 bsp_disp_sync_with_hw(disp_bsp_init_para *para);
s32 bsp_disp_get_fps(u32 disp);
s32 bsp_disp_get_health_info(u32 disp, disp_health_info *info);
s32 bsp_disp_tv_register(struct disp_tv_func *func);
s32 bsp_disp_tv_set_hpd(u32 state);

//lcd
s32 bsp_disp_lcd_set_panel_funs(char *name, disp_lcd_panel_fun *lcd_cfg);
s32 bsp_disp_lcd_backlight_enable(u32 disp);
s32 bsp_disp_lcd_backlight_disable(u32 disp);
s32 bsp_disp_lcd_pwm_enable(u32 disp);
s32 bsp_disp_lcd_pwm_disable(u32 disp);
s32 bsp_disp_lcd_power_enable(u32 disp, u32 power_id);
s32 bsp_disp_lcd_power_disable(u32 disp, u32 power_id);
s32 bsp_disp_lcd_set_bright(u32 disp, u32 bright);
s32 bsp_disp_lcd_get_bright(u32 disp);
s32 bsp_disp_lcd_tcon_enable(u32 disp);
s32 bsp_disp_lcd_tcon_disable(u32 disp);
s32 bsp_disp_lcd_pin_cfg(u32 disp, u32 en);
s32 bsp_disp_lcd_gpio_set_value(u32 disp, u32 io_index, u32 value);
/**** add for lcd read gpio level id by lxm 20220310 ***/
s32 bsp_disp_lcd_gpio_get_value(u32 disp, u32 io_index);
s32 bsp_disp_lcd_gpio_set_direction(u32 disp, unsigned int io_index, u32 direction);
disp_lcd_flow *bsp_disp_lcd_get_open_flow(u32 disp);
disp_lcd_flow *bsp_disp_lcd_get_close_flow(u32 disp);
s32 bsp_disp_get_panel_info(u32 disp, disp_panel_para *info);
s32 bsp_disp_lcd_switch_compat_panel(u32 disp, u32 index);
disp_lcd_flow *bsp_disp_lcd_get_open_flow(u32 disp);
disp_lcd_flow *bsp_disp_lcd_get_close_flow(u32 disp);

/* edp */
s32 bsp_disp_edp_set_panel_funs(char *name, disp_lcd_panel_fun *lcd_cfg);
s32 bsp_disp_edp_backlight_enable(u32 disp);
s32 bsp_disp_edp_backlight_disable(u32 disp);
s32 bsp_disp_edp_pwm_enable(u32 disp);
s32 bsp_disp_edp_pwm_disable(u32 disp);
s32 bsp_disp_edp_power_enable(u32 disp, u32 power_id);
s32 bsp_disp_edp_power_disable(u32 disp, u32 power_id);
s32 bsp_disp_edp_set_bright(u32 disp, u32 bright);
s32 bsp_disp_edp_get_bright(u32 disp);
s32 bsp_disp_edp_pin_cfg(u32 disp, u32 en);
s32 bsp_disp_edp_gpio_set_value(u32 disp, u32 io_index, u32 value);
s32 bsp_disp_edp_gpio_set_direction(u32 disp, unsigned int io_index,
				    u32 direction);

s32 bsp_disp_vsync_event_enable(u32 disp, bool enable);
s32 bsp_disp_shadow_protect(u32 disp, bool protect);

s32 disp_delay_ms(u32 ms);
s32 disp_delay_us(u32 us);

s32 bsp_disp_tv_suspend(void);//for test tv suspend
s32 bsp_disp_tv_resume(void);
s32   dsi_dcs_wr(u32 sel, u8 cmd, u8 *para_p, u32 para_num);
s32   dsi_gen_wr(u32 sel, u8 cmd, u8 *para_p, u32 para_num);
s32   dsi_clk_enable(u32 sel, u32 en);
s32 bsp_disp_lcd_dsi_clk_enable(u32 disp, u32 en);
s32 bsp_disp_lcd_dsi_dcs_wr(u32 disp, u8 command, u8 *para, u32 para_num);
s32 bsp_disp_lcd_dsi_gen_wr(u32 disp, u8 command, u8 *para, u32 para_num);
s32 bsp_disp_lcd_dsi_mode_switch(u32 screen_id, u32 cmd_en, u32 lp_en);
s32 bsp_disp_lcd_dsi_dcs_read(u32 sel, u8 cmd, u8 *result, u32 *num_p);
s32 bsp_disp_lcd_set_max_ret_size(u32 sel, u32 size);
s32 bsp_disp_lcd_dsi_gen_short_read(u32 sel, u8 *para_p, u8 para_num,
				    u8 *result);

struct disp_manager *disp_get_layer_manager(u32 disp);

int bsp_disp_get_fb_info(unsigned int disp, struct disp_layer_info *info);
int bsp_disp_get_display_size(u32 disp, unsigned int *width, unsigned int *height);

#ifdef CONFIG_DEVFREQ_DRAM_FREQ_IN_VSYNC
/* dramfreq interface */
s32 bsp_disp_get_vb_time(void);
s32 bsp_disp_get_next_vb_time(void);
s32 bsp_disp_is_in_vb(void);
#endif

#endif

