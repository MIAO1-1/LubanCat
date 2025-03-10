/*
 * Firmware I/O implementation for XRadio drivers
 *
 * Copyright (c) 2013
 * Xradio Technology Co., Ltd. <www.xradiotech.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/init.h>
#include <linux/vmalloc.h>
#include <linux/sched.h>
#include <linux/firmware.h>

#include "xradio.h"
#include "fwio.h"
#include "hwio.h"
#include "sbus.h"
#include "bh.h"

/* Macroses are local. */
#define APB_WRITE(reg, val) \
	do { \
		ret = xradio_apb_write_32(hw_priv, APB_ADDR(reg), (val)); \
		if (ret < 0) { \
			xradio_dbg(XRADIO_DBG_ERROR, \
				"%s: can't write %s at line %d.\n", \
				__func__, #reg, __LINE__); \
			goto error; \
		} \
	} while (0)
#define APB_READ(reg, val) \
	do { \
		ret = xradio_apb_read_32(hw_priv, APB_ADDR(reg), &(val)); \
		if (ret < 0) { \
			xradio_dbg(XRADIO_DBG_ERROR, \
				"%s: can't read %s at line %d.\n", \
				__func__, #reg, __LINE__); \
			goto error; \
		} \
	} while (0)
#define REG_WRITE(reg, val) \
	do { \
		ret = xradio_reg_write_32(hw_priv, (reg), (val)); \
		if (ret < 0) { \
			xradio_dbg(XRADIO_DBG_ERROR, \
				"%s: can't write %s at line %d.\n", \
				__func__, #reg, __LINE__); \
			goto error; \
		} \
	} while (0)
#define REG_READ(reg, val) \
	do { \
		ret = xradio_reg_read_32(hw_priv, (reg), &(val)); \
		if (ret < 0) { \
			xradio_dbg(XRADIO_DBG_ERROR, \
				"%s: can't read %s at line %d.\n", \
				__func__, #reg, __LINE__); \
			goto error; \
		} \
	} while (0)

static int xradio_get_hw_type(u32 config_reg_val, int *major_revision)
{
	int hw_type = -1;
	u32 hif_type = (config_reg_val >> 24) & 0x4;
	/*u32 hif_vers = (config_reg_val >> 31) & 0x1;*/

	/* Check if we have XRADIO */
	if (hif_type == 0x4) {
		*major_revision = 0x4;
		hw_type = HIF_HW_TYPE_XRADIO;
	} else {
		/*hw type unknown.*/
		*major_revision = 0x0;
	}
	return hw_type;
}

/*
 * This function is called to Parse the SDD file
 * to extract some informations
 */
static int xradio_parse_sdd(struct xradio_common *hw_priv, u32 *dpll)
{
	int ret = 0;
	const char *sdd_path = NULL;
	struct xradio_sdd *pElement = NULL;
	int parsedLength = 0;

	sta_printk(XRADIO_DBG_TRC, "%s\n", __func__);
	SYS_BUG(hw_priv->sdd != NULL);

	/* select and load sdd file depend on hardware version. */
	switch (hw_priv->hw_revision) {
	case XR829_HW_REV0:
		sdd_path = XR829_SDD_FILE;
#ifdef CONFIG_XRADIO_ETF
		if (etf_is_connect())
			sdd_path = etf_get_sddpath();
#endif
		break;
	default:
		xradio_dbg(XRADIO_DBG_ERROR,
			   "%s: unknown hardware version.\n", __func__);
		return ret;
	}

	ret = request_firmware(&hw_priv->sdd, sdd_path, hw_priv->pdev);
	if (unlikely(ret)) {
		xradio_dbg(XRADIO_DBG_ERROR, "%s: can't load sdd file %s.\n",
			   __func__, sdd_path);
		return ret;
	}

	/*parse SDD config.*/
	hw_priv->is_BT_Present = false;
	pElement = (struct xradio_sdd *)hw_priv->sdd->data;
	parsedLength += (FIELD_OFFSET(struct xradio_sdd, data) + \
			 pElement->length);
	pElement = FIND_NEXT_ELT(pElement);

	while (parsedLength < hw_priv->sdd->size) {
		switch (pElement->id) {
		case SDD_PTA_CFG_ELT_ID:
			hw_priv->conf_listen_interval =
			    (*((u16 *) pElement->data + 1) >> 7) & 0x1F;
			hw_priv->is_BT_Present = true;
			xradio_dbg(XRADIO_DBG_NIY,
				   "PTA element found.Listen Interval %d\n",
				   hw_priv->conf_listen_interval);
			break;
		case SDD_REFERENCE_FREQUENCY_ELT_ID:
			switch (*((uint16_t *) pElement->data)) {
			case 0x32C8:
				*dpll = 0x1D89D241;
				break;
			case 0x3E80:
				*dpll = 0x1E1;
				break;
			case 0x41A0:
				*dpll = 0x124931C1;
				break;
			case 0x4B00:
				*dpll = 0x191;
				break;
			case 0x5DC0:
				*dpll = 0x141;
				break;
			case 0x6590:
				*dpll = 0x0EC4F121;
				break;
			case 0x8340:
				*dpll = 0x92490E1;
				break;
			case 0x9600:
				*dpll = 0x100010C1;
				break;
			case 0x9C40:
				*dpll = 0xC1;
				break;
			case 0xBB80:
				*dpll = 0xA1;
				break;
			case 0xCB20:
				*dpll = 0x7627091;
				break;
			default:
				*dpll = DPLL_INIT_VAL_XRADIO;
				xradio_dbg(XRADIO_DBG_WARN,
					   "Unknown Reference clock frequency."
					   "Use default DPLL value=0x%08x.",
					    DPLL_INIT_VAL_XRADIO);
				break;
			}
			xradio_dbg(XRADIO_DBG_NIY,
					"Reference clock=%uKHz, DPLL value=0x%08x.\n",
					*((uint16_t *) pElement->data), *dpll);
		default:
			break;
		}
		parsedLength += (FIELD_OFFSET(struct xradio_sdd, data) + \
				 pElement->length);
		pElement = FIND_NEXT_ELT(pElement);
	}

	xradio_dbg(XRADIO_DBG_MSG, "sdd size=%zu parse len=%d.\n",
		   hw_priv->sdd->size, parsedLength);


	if (hw_priv->is_BT_Present == false) {
		hw_priv->conf_listen_interval = 0;
		xradio_dbg(XRADIO_DBG_NIY, "PTA element NOT found.\n");
	}
	return ret;
}

int xradio_update_dpllctrl(struct xradio_common *hw_priv, u32 dpll_update)
{
	int ret   = 0;
	u32 val32 = 0;
	u32 dpll_read = 0;
	int i = 0;

	xradio_ahb_read_32(hw_priv, PWRCTRL_WLAN_START_CFG, &val32);
	do {
		ret = xradio_ahb_read_32(hw_priv, PWRCTRL_WLAN_COMMON_CFG, &val32);
		/*Check DPLL, return success if sync finish*/
		if (val32 & PWRCTRL_COMMON_REG_DONE) {
			xradio_ahb_read_32(hw_priv, PWRCTRL_WLAN_DPLL_CTRL, &dpll_read);
			if (dpll_update == dpll_read) {
				xradio_dbg(XRADIO_DBG_NIY, "%s: dpll sync ok=0x%08x.\n",
					__func__, dpll_read);
				break;
			} else {
				xradio_dbg(XRADIO_DBG_ERROR, "%s: dpll is incorrect, " \
						"dpll_read=0x%08x, dpll_update=0x%08x.\n",
					__func__, dpll_read, dpll_update);
				/*dpll_ctrl need to be corrected it by follow procedure.*/
			}
		}

		/*correct dpll_ctrl if wlan is accessible.*/
		if ((val32 & PWRCTRL_COMMON_REG_ARBT) && /*wait for arbit end.*/
			!(val32 & PWRCTRL_COMMON_REG_BT)) { /*wlan is accessible.*/
			xradio_ahb_read_32(hw_priv, PWRCTRL_WLAN_DPLL_CTRL, &dpll_read);
			if (dpll_update != dpll_read) {
				xradio_dbg(XRADIO_DBG_WARN,
					"%s:dpll_read=0x%08x, new dpll_ctrl=0x%08x.\n",
					__func__, dpll_read, dpll_update);
				xradio_ahb_write_32(hw_priv,
					PWRCTRL_WLAN_DPLL_CTRL, (dpll_update|0x1));
				msleep(5); /*wait for it stable after change DPLL config. */
			} else {
				xradio_dbg(XRADIO_DBG_ALWY, "%s: DPLL_CTRL Sync=0x%08x.\n",
					__func__, dpll_read);
			}
			xradio_ahb_write_32(hw_priv, PWRCTRL_WLAN_COMMON_CFG,
					PWRCTRL_COMMON_REG_DONE); /*set done*/
			break;
		} else if (i < 100) {
			xradio_dbg(XRADIO_DBG_WARN, "%s: COMMON_REG=0x%08x.\n",
					__func__, val32);
			msleep(i);
			++i;
		} else {
			xradio_dbg(XRADIO_DBG_ERROR, "%s:DPLL access timeout=0x%08x.\n",
					__func__, val32);
			ret = -ETIMEDOUT;
			break;
		}
	} while (!ret);
	xradio_ahb_write_32(hw_priv, PWRCTRL_WLAN_START_CFG, 0);
	return ret;
}

static int xradio_firmware(struct xradio_common *hw_priv)
{
	int ret, block, num_blocks;
	unsigned i;
	u32 val32;
	u32 put = 0, get = 0;
	u8 *buf = NULL;
	const char *fw_path;
	const struct firmware *firmware = NULL;
	xradio_dbg(XRADIO_DBG_TRC, "%s\n", __func__);

	switch (hw_priv->hw_revision) {
	case XR829_HW_REV0:
		fw_path = XR829_FIRMWARE;
#ifdef CONFIG_XRADIO_ETF
		if (etf_is_connect())
			fw_path = etf_get_fwpath();
#endif
		break;
	default:
		xradio_dbg(XRADIO_DBG_ERROR, "%s: invalid silicon revision %d.\n",
			   __func__, hw_priv->hw_revision);
		return -EINVAL;
	}

	/* Initialize common registers */
	APB_WRITE(DOWNLOAD_IMAGE_SIZE_REG, DOWNLOAD_ARE_YOU_HERE);
	APB_WRITE(DOWNLOAD_PUT_REG, 0);
	APB_WRITE(DOWNLOAD_GET_REG, 0);
	APB_WRITE(DOWNLOAD_STATUS_REG, DOWNLOAD_PENDING);
	APB_WRITE(DOWNLOAD_FLAGS_REG, 0);

	/* Release CPU from RESET */
	xradio_reg_bit_operate(hw_priv, HIF_CONFIG_REG_ID,
						0, HIF_CONFIG_CPU_RESET_BIT);
	/* Enable Clock */
	xradio_reg_bit_operate(hw_priv, HIF_CONFIG_REG_ID,
						0, HIF_CONFIG_CPU_CLK_DIS_BIT);

	/* Load a firmware file */
	ret = request_firmware(&firmware, fw_path, hw_priv->pdev);
	if (ret) {
		xradio_dbg(XRADIO_DBG_ERROR, "%s: can't load firmware file %s.\n",
			   __func__, fw_path);
		goto error;
	}
	SYS_BUG(!firmware->data);

	buf = xr_kmalloc(DOWNLOAD_BLOCK_SIZE, true);
	if (!buf) {
		xradio_dbg(XRADIO_DBG_ERROR,
			   "%s: can't allocate firmware buffer.\n", __func__);
		ret = -ENOMEM;
		goto error;
	}

	/* Check if the bootloader is ready */
	for (i = 0; i < 100; i++ /*= 1 + i / 2*/) {
		APB_READ(DOWNLOAD_IMAGE_SIZE_REG, val32);
		if (val32 == DOWNLOAD_I_AM_HERE)
			break;
		mdelay(10);
	}			/* End of for loop */
	if (val32 != DOWNLOAD_I_AM_HERE) {
		xradio_dbg(XRADIO_DBG_ERROR, "%s: bootloader is not ready.\n",
			   __func__);
#ifdef BOOT_NOT_READY_FIX
		hw_priv->boot_not_ready_cnt++;
		hw_priv->boot_not_ready = 1;
#endif
		ret = -ETIMEDOUT;
		goto error;
	}

	/* Calculcate number of download blocks */
	num_blocks = (firmware->size - 1) / DOWNLOAD_BLOCK_SIZE + 1;

	/* Updating the length in Download Ctrl Area */
	val32 = firmware->size; /* Explicit cast from size_t to u32 */
	APB_WRITE(DOWNLOAD_IMAGE_SIZE_REG, val32);

	/*
	 * DOWNLOAD_BLOCK_SIZE must be divided exactly by sdio blocksize,
	 * otherwise it may cause bootloader error.
	 */
	val32 = hw_priv->sbus_ops->get_block_size(hw_priv->sbus_priv);
	if (val32 > DOWNLOAD_BLOCK_SIZE || DOWNLOAD_BLOCK_SIZE%val32) {
		xradio_dbg(XRADIO_DBG_WARN,
			"%s:change blocksize(%d->%d) during download fw.\n",
			__func__, val32, DOWNLOAD_BLOCK_SIZE>>1);
		hw_priv->sbus_ops->lock(hw_priv->sbus_priv);
		ret = hw_priv->sbus_ops->set_block_size(hw_priv->sbus_priv,
			DOWNLOAD_BLOCK_SIZE>>1);
		if (ret)
			xradio_dbg(XRADIO_DBG_ERROR,
				"%s: set blocksize error(%d).\n", __func__, ret);
		hw_priv->sbus_ops->unlock(hw_priv->sbus_priv);
	}

	/* Firmware downloading loop */
	for (block = 0; block < num_blocks; block++) {
		size_t tx_size;
		size_t block_size;

		/* check the download status */
		APB_READ(DOWNLOAD_STATUS_REG, val32);
		if (val32 != DOWNLOAD_PENDING) {
			xradio_dbg(XRADIO_DBG_ERROR,
				   "%s: bootloader reported error %d.\n",
				   __func__, val32);
			ret = -EIO;
			goto error;
		}

		/* calculate the block size */
		tx_size = block_size = min((size_t)(firmware->size - put),
					   (size_t)DOWNLOAD_BLOCK_SIZE);
		memcpy(buf, &firmware->data[put], block_size);
		if (block_size < DOWNLOAD_BLOCK_SIZE) {
			memset(&buf[block_size], 0, DOWNLOAD_BLOCK_SIZE - block_size);
			tx_size = DOWNLOAD_BLOCK_SIZE;
		}

		/* loop until put - get <= 24K */
		for (i = 0; i < 100; i++) {
			APB_READ(DOWNLOAD_GET_REG, get);
			if ((put - get) <= (DOWNLOAD_FIFO_SIZE - DOWNLOAD_BLOCK_SIZE))
				break;
			mdelay(i);
		}

		if ((put - get) > (DOWNLOAD_FIFO_SIZE - DOWNLOAD_BLOCK_SIZE)) {
			xradio_dbg(XRADIO_DBG_ERROR, "%s: Timeout waiting for FIFO.\n",
				   __func__);
			ret = -ETIMEDOUT;
			goto error;
		}

		/* send the block to sram */
		ret = xradio_apb_write(hw_priv, APB_ADDR(DOWNLOAD_FIFO_OFFSET + \
				       (put & (DOWNLOAD_FIFO_SIZE - 1))),
				       buf, tx_size);
		if (ret < 0) {
			xradio_dbg(XRADIO_DBG_ERROR,
				   "%s: can't write block at line %d.\n",
				   __func__, __LINE__);
			goto error;
		}

		/* update the put register */
		put += block_size;
		APB_WRITE(DOWNLOAD_PUT_REG, put);
	} /* End of firmware download loop */

	/* Wait for the download completion */
	for (i = 0; i < 300; i += 1 + i / 2) {
		APB_READ(DOWNLOAD_STATUS_REG, val32);
		if (val32 != DOWNLOAD_PENDING)
			break;
		mdelay(i);
	}
	if (val32 != DOWNLOAD_SUCCESS) {
		xradio_dbg(XRADIO_DBG_ERROR,
			   "%s: wait for download completion failed. " \
			   "Read: 0x%.8X\n", __func__, val32);
		ret = -ETIMEDOUT;
		goto error;
	} else {
		xradio_dbg(XRADIO_DBG_ALWY, "Firmware completed.\n");
		ret = 0;
	}

error:
	if (buf)
		kfree(buf);
	if (firmware) {
		release_firmware(firmware);
	}
	return ret;
}

static int xradio_bootloader(struct xradio_common *hw_priv)
{
	int ret = -1;
	u32 i = 0;
	const char *bl_path = XR829_BOOTLOADER;
	u32 addr = AHB_MEMORY_ADDRESS;
	u32 *data = NULL;
	const struct firmware *bootloader = NULL;
	xradio_dbg(XRADIO_DBG_TRC, "%s\n", __func__);

	/* Load a bootloader file */
	ret = request_firmware(&bootloader, bl_path, hw_priv->pdev);
	if (ret) {
		xradio_dbg(XRADIO_DBG_ERROR,
			   "%s: can't load bootloader file %s.\n",
			   __func__, bl_path);
		goto error;
	}

	xradio_dbg(XRADIO_DBG_NIY, "%s: bootloader size = %zu, loopcount = %zu\n",
		   __func__, bootloader->size, (bootloader->size) / 4);

	/* Down bootloader. */
	data = (u32 *)bootloader->data;
	for (i = 0; i < (bootloader->size)/4; i++) {
	ret = xradio_ahb_write_32(hw_priv, addr, data[i]);
	if (ret < 0) {
		sbus_printk(XRADIO_DBG_ERROR, "%s: xradio_ahb_write failed.\n", __func__);
		goto error;
	}
	if (i == 100 || i == 200 || i == 300 || i == 400 || i == 500 || i == 600)
		xradio_dbg(XRADIO_DBG_NIY, "%s: addr = 0x%x, data = 0x%x\n", __func__, addr, data[i]);
		addr += 4;
	}
	xradio_dbg(XRADIO_DBG_ALWY, "Bootloader complete\n");

error:
	if (bootloader) {
		release_firmware(bootloader);
	}
	return ret;
}

#if (DBG_XRADIO_HIF)

extern u16 	hif_test_rw; /*0: nothing to do; 1: write only; 2: write and read*/
extern u16 	hif_test_data_mode; /* hif test data mode, such as 0x55, 0xff etc*/
extern u16 	hif_test_data_len; /* hif test data len, every data len pre round*/
extern u16	hif_test_data_round;
extern u16 	hif_test_oper_delta; /* hif test operation delta time, give more time to analyze data tranx*/
int HIF_R_W_TEST(struct xradio_common *hw_priv)
{
	int time;
	int i;
	struct timeval start;   //linux5.4 commit 33e26418193f58d1895f2f968e1953b1caf8deb7
	struct timeval end;
	unsigned int addr;
	char *write_buf;
	char *read_buf;
	write_buf = kmalloc(hif_test_data_len * 4, GFP_KERNEL);
	if (!write_buf)
		return 0xff;
	read_buf = kmalloc(hif_test_data_len * 4, GFP_KERNEL);
	if (!read_buf) {
		kfree(write_buf);
		return 0xff;
	}

	xr_do_gettimeofday(&start);
	printk(KERN_ERR"[HIF test] --- <write> --- begin~~\n");
	addr = PAS_RAM_START_ADDR;
	memset(write_buf, hif_test_data_mode, hif_test_data_len * 4);
	i = 0;
	while (i < hif_test_data_round) {
		xradio_apb_write(hw_priv, addr, write_buf, hif_test_data_len * 4);
		msleep(hif_test_oper_delta);
		i++;
		if (0 == hif_test_rw)
			goto err;
	}

	if (1 == hif_test_rw) { // means write only
		kfree(write_buf);
		kfree(read_buf);
		printk(KERN_ERR"[HIF test] --- <write> --- end and return~~\n");
		return 0;
	}

	msleep(hif_test_oper_delta * 5);
	printk(KERN_ERR"[HIF test] --- <read> --- begin~~\n");

	addr = PAS_RAM_START_ADDR;
	memset(write_buf, hif_test_data_mode, hif_test_data_len * 4);
	i = 0;
	while (i < hif_test_data_round) {
		xradio_apb_read(hw_priv, addr, read_buf, hif_test_data_len * 4);
		msleep(hif_test_oper_delta);
		i++;
		if (0 == hif_test_rw)
			goto err;
	}

	printk(KERN_ERR"[HIF test] --- <read> --- end~~\n");
	xr_do_gettimeofday(&end);
	time = 1000 * (end.tv_sec - start.tv_sec) + (end.tv_usec - start.tv_usec) / 1000;
	kfree(write_buf);
	kfree(read_buf);
	return 0;
err:
	kfree(write_buf);
	kfree(read_buf);
	return 1;
}

#endif

int xradio_load_firmware(struct xradio_common *hw_priv)
{
	int ret;
	int i;
	u32 val32 = 0;
	u16 val16 = 0;
	u32 dpll = 0;
	int major_revision;

	xradio_dbg(XRADIO_DBG_TRC, "%s\n", __func__);
	SYS_BUG(!hw_priv);
	/* Read CONFIG Register Value - We will read 32 bits */

	ret = xradio_reg_read_32(hw_priv, HIF_CONFIG_REG_ID, &val32);
	if (ret < 0) {
		xradio_dbg(XRADIO_DBG_ERROR,
			   "%s: can't read config register, err=%d.\n",
			   __func__, ret);
		return ret;
	}

	/*check hardware type and revision.*/
	hw_priv->hw_type = xradio_get_hw_type(val32, &major_revision);
	switch (hw_priv->hw_type) {
	case HIF_HW_TYPE_XRADIO:
		xradio_dbg(XRADIO_DBG_NIY, "%s: HW_TYPE_XRADIO detected.\n",
			   __func__);
		break;
	default:
		xradio_dbg(XRADIO_DBG_ERROR, "%s: Unknown hardware: %d.\n",
			   __func__, hw_priv->hw_type);
		return -ENOTSUPP;
	}
	if (major_revision == 4) {
		hw_priv->hw_revision = XR829_HW_REV0;
		xradio_dbg(XRADIO_DBG_ALWY, "XRADIO_HW_REV 1.0 detected.\n");
	} else {
		xradio_dbg(XRADIO_DBG_ERROR, "%s: Unsupported major revision %d.\n",
			   __func__, major_revision);
		return -ENOTSUPP;
	}

	/*load sdd file, and get config from it.*/
	ret = xradio_parse_sdd(hw_priv, &dpll);
	if (ret < 0) {
		return ret;
	}

	/*set dpll initial value and check.*/
	ret = xradio_reg_write_32(hw_priv, HIF_TSET_GEN_R_W_REG_ID, dpll);
	if (ret < 0) {
		xradio_dbg(XRADIO_DBG_ERROR, "%s: can't write DPLL register.\n",
			   __func__);
		goto out;
	}
	msleep(5);
	ret = xradio_reg_read_32(hw_priv, HIF_TSET_GEN_R_W_REG_ID, &val32);
	if (ret < 0) {
		xradio_dbg(XRADIO_DBG_ERROR, "%s: can't read DPLL register.\n",
			   __func__);
		goto out;
	}
	if (val32 != dpll) {
		xradio_dbg(XRADIO_DBG_ERROR, "%s: unable to initialise " \
			   "DPLL register. Wrote 0x%.8X, read 0x%.8X.\n",
			   __func__, dpll, val32);
		ret = -EIO;
		goto out;
	}

	/* Set wakeup bit in device */
	ret = xradio_reg_bit_operate(hw_priv, HIF_CONTROL_REG_ID, HIF_CTRL_WUP_BIT, 0);
	if (ret < 0) {
		xradio_dbg(XRADIO_DBG_ERROR, "%s: device wake up failed.\n", __func__);
		hw_priv->hw_cant_wakeup = true;
		goto out;
	}

	/* Wait for wakeup */
	for (i = 0 ; i < 300 ; i += 1 + i / 2) {
		ret = xradio_reg_read_16(hw_priv, HIF_CONTROL_REG_ID, &val16);
		if (ret < 0) {
			xradio_dbg(XRADIO_DBG_ERROR, "%s: Wait_for_wakeup: "
				   "can't read control register.\n", __func__);
			goto out;
		}
		if (val16 & HIF_CTRL_RDY_BIT) {
			break;
		}
		msleep(i);
	}
	if ((val16 & HIF_CTRL_RDY_BIT) == 0) {
		xradio_dbg(XRADIO_DBG_ERROR, "%s: Wait for wakeup:"
			   "device is not responding.\n", __func__);
		hw_priv->hw_cant_wakeup = true;
#ifdef BOOT_NOT_READY_FIX
		hw_priv->boot_not_ready_cnt++;
		hw_priv->boot_not_ready = 1;
		xradio_dbg(XRADIO_DBG_ERROR, "Device will restart at %d times!\n",
			hw_priv->boot_not_ready_cnt);
	#ifdef ERROR_HANG_DRIVER
		if (hw_priv->boot_not_ready_cnt >= 6) {
			xradio_dbg(XRADIO_DBG_ERROR, "Boot not ready and device restart \
				more than 6, hang the driver.\n");
			error_hang_driver = 1;
		}
	#endif
#endif
		ret = -ETIMEDOUT;
		goto out;
	} else {
		xradio_dbg(XRADIO_DBG_NIY, "WLAN device is ready.\n");
		hw_priv->hw_cant_wakeup = false;
	}

	/* Checking for access mode and download firmware. */
	ret = xradio_reg_bit_operate(hw_priv, HIF_CONFIG_REG_ID,
								HIF_CONFIG_ACCESS_MODE_BIT, 0);
	if (ret < 0) {
		xradio_dbg(XRADIO_DBG_ERROR, "%s: check_access_mode: "
			   "can't read config register.\n", __func__);
		goto out;
	}

#ifdef SUPPORT_DPLL_CHECK
	/*Checking DPLL value and correct it if need.*/
	xradio_update_dpllctrl(hw_priv, xradio_dllctrl_convert(dpll));
#else
	xradio_dbg(XRADIO_DBG_ALWY, "%s: not need check dpll.\n", __func__);
#endif

#if (DBG_XRADIO_HIF)

	if (hif_test_rw) {
		ret = HIF_R_W_TEST(hw_priv);
		if (0 == ret) {
			printk(KERN_ERR "HIF Test OK!\n");
		} else if (1 == ret) {
			printk(KERN_ERR "HIF Test faied!\n");
		} else {
			printk(KERN_ERR "Unkmow error!\n");
		}
	}

#endif

	/* Down bootloader. */
	ret = xradio_bootloader(hw_priv);
	if (ret < 0) {
		xradio_dbg(XRADIO_DBG_ERROR, "%s: can't download bootloader.\n", __func__);
		goto out;
	}
	/* Down firmware. */
	ret = xradio_firmware(hw_priv);
	if (ret < 0) {
		xradio_dbg(XRADIO_DBG_ERROR, "%s: can't download firmware.\n", __func__);
		goto out;
	}

	/* Register Interrupt Handler */
	ret = hw_priv->sbus_ops->irq_subscribe(hw_priv->sbus_priv,
					      (sbus_irq_handler)xradio_irq_handler,
					       hw_priv);
	if (ret < 0) {
		xradio_dbg(XRADIO_DBG_ERROR, "%s: can't register IRQ handler.\n",
			   __func__);
		goto out;
	}

	if (HIF_HW_TYPE_XRADIO == hw_priv->hw_type) {
		/* If device is XRADIO the IRQ enable/disable bits
		 * are in CONFIG register */
		ret = xradio_reg_bit_operate(hw_priv, HIF_CONFIG_REG_ID,
									HIF_CONF_IRQ_RDY_ENABLE, 0);
		if (ret < 0) {
			xradio_dbg(XRADIO_DBG_ERROR, "%s: enable_irq: can't read " \
				   "config register.\n", __func__);
			goto unsubscribe;
		}
	} else {
		/* Enable device interrupts - Both DATA_RDY and WLAN_RDY */
		ret = xradio_reg_bit_operate(hw_priv, HIF_CONTROL_REG_ID,
									HIF_CTRL_IRQ_RDY_ENABLE, 0);
		if (ret < 0) {
			xradio_dbg(XRADIO_DBG_ERROR, "%s: enable_irq: can't read " \
				   "control register.\n", __func__);
			goto unsubscribe;
		}
	}

	/* Configure device for MESSSAGE MODE */
	ret = xradio_reg_bit_operate(hw_priv, HIF_CONFIG_REG_ID,
								0, HIF_CONFIG_ACCESS_MODE_BIT);
	if (ret < 0) {
		xradio_dbg(XRADIO_DBG_ERROR, "%s: set_mode: can't read config register.\n",
					__func__);
		goto unsubscribe;
	}

	/* Unless we read the CONFIG Register we are
	 * not able to get an interrupt */
	mdelay(10);
	xradio_reg_read_32(hw_priv, HIF_CONFIG_REG_ID, &val32);
	return 0;

unsubscribe:
	hw_priv->sbus_ops->irq_unsubscribe(hw_priv->sbus_priv);
out:
	if (hw_priv->sdd) {
		release_firmware(hw_priv->sdd);
		hw_priv->sdd = NULL;
	}
	return ret;
}

int xradio_dev_deinit(struct xradio_common *hw_priv)
{
	hw_priv->sbus_ops->irq_unsubscribe(hw_priv->sbus_priv);
	if (hw_priv->sdd) {
		release_firmware(hw_priv->sdd);
		hw_priv->sdd = NULL;
	}
	return 0;
}

#undef APB_WRITE
#undef APB_READ
#undef REG_WRITE
#undef REG_READ
