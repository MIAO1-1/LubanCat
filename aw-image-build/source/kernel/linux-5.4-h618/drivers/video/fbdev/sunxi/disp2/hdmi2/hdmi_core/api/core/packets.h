/*
 * Allwinner SoCs hdmi2.0 driver.
 *
 * Copyright (C) 2016 Allwinner.
 *
 * This file is licensed under the terms of the GNU General Public
 * License version 2.  This program is licensed "as is" without any
 * warranty of any kind, whether express or implied.
 */


#ifndef PACKETS_H_
#define PACKETS_H_

#include "../hdmitx_dev.h"
#include "video.h"
#include "audio.h"

#include "frame_composer_reg.h"
#include "../log.h"
#include "../edid.h"
#include "../general_ops.h"
#include "hdr10p.h"

#define ACP_TX	0
#define ISRC1_TX	1
#define ISRC2_TX	2
#define SPD_TX		4
#define VSD_TX		3

typedef struct fc_spd_info {
	const u8 *vName;
	u8 vLength;
	const u8 *pName;
	u8 pLength;
	u8 code;
	u8 autoSend;
} fc_spd_info_t;

void fc_drm_up(hdmi_tx_dev_t *dev, fc_drm_pb_t *pb);
void fc_drm_disable(hdmi_tx_dev_t *dev);
void fc_acp_type(hdmi_tx_dev_t *dev, u8 type);
void fc_acp_type_dependent_fields(hdmi_tx_dev_t *dev,
				u8 *fields, u8 fieldsLength);

void fc_avi_config(hdmi_tx_dev_t *dev, videoParams_t *videoParams);

void fc_gamut_config(hdmi_tx_dev_t *dev);
void fc_gamut_packet_config(hdmi_tx_dev_t *dev,
			const u8 *gbdContent, u8 length);

/*
 * Configure the ISRC packet status
 * @param code
 * 001 - Starting Position
 * 010 - Intermediate Position
 * 100 - Ending Position
 * @param baseAddr block base address
 */
void fc_isrc_status(hdmi_tx_dev_t *dev, u8 code);

/*
 * Configure the validity bit in the ISRC packets
 * @param validity: 1 if valid
 * @param baseAddr block base address
 */
void fc_isrc_valid(hdmi_tx_dev_t *dev, u8 validity);

/*
 * Configure the cont bit in the ISRC1 packets
 * When a subsequent ISRC2 Packet is transmitted,
 * the ISRC_Cont field shall be set and shall be clear otherwise.
 * @param isContinued 1 when set
 * @param baseAddr block base address
 */
void fc_isrc_cont(hdmi_tx_dev_t *dev, u8 isContinued);

/*
 * Configure the ISRC 1 Codes
 * @param codes
 * @param length
 * @param baseAddr block base address
 */
void fc_isrc_isrc1_codes(hdmi_tx_dev_t *dev, u8 *codes, u8 length);

/*
 * Configure the ISRC 2 Codes
 * @param codes
 * @param length
 * @param baseAddr block base address
 */
void fc_isrc_isrc2_codes(hdmi_tx_dev_t *dev, u8 *codes, u8 length);



void fc_packets_metadata_config(hdmi_tx_dev_t *dev);
void fc_packets_AutoSend(hdmi_tx_dev_t *dev, u8 enable, u8 mask);
void fc_packets_ManualSend(hdmi_tx_dev_t *dev, u8 mask);
void fc_packets_disable_all(hdmi_tx_dev_t *dev);


int fc_spd_config(hdmi_tx_dev_t *dev, fc_spd_info_t *spd_data);

/*
 * Configure the 24 bit IEEE Registration Identifier
 * @param baseAddr Block base address
 * @param id vendor unique identifier
 */
void fc_vsd_vendor_OUI(hdmi_tx_dev_t *dev, u32 id);

/*
 * Configure the Vendor Payload to be carried by the InfoFrame
 * @param info array
 * @param length of the array
 * @return 0 when successful and 1 on error
 */
u8 fc_vsd_vendor_payload(hdmi_tx_dev_t *dev,
			const u8 *data, unsigned short length);
void fc_vsif_enable(hdmi_tx_dev_t *dev, u8 enable);
int fc_vsif_config(hdmi_tx_dev_t *dev, u8 enable);


/**
 * Configure Source Product Description, Vendor Specific and Auxiliary
 * Video InfoFrames.
 * @param dev Device structure
 * @param video  Video Parameters to set up AVI InfoFrame (and all
 * other video parameters)
 * @param prod Description of Vendor and Product to set up Vendor
 * Specific InfoFrame and Source Product Description InfoFrame
 * @return TRUE when successful
 */
int packets_Configure(hdmi_tx_dev_t *dev, videoParams_t *video,
		      productParams_t *prod);

/**
 * Configure Audio Content Protection packets.
 * @param type Content protection type (see HDMI1.3a Section 9.3)
 * @param fields  ACP Type Dependent Fields
 * @param length of the ACP fields
 * @param autoSend Send Packets Automatically
 */
void packets_AudioContentProtection(hdmi_tx_dev_t *dev, u8 type,
				const u8 *fields, u8 length, u8 autoSend);

/**
 * Configure ISRC 1 & 2 Packets
 * @param dev Device structure
 * @param initStatus Initial status
 * which the packets are sent with (usually starting position)
 * @param codes ISRC codes array
 * @param length of the ISRC codes array
 * @param autoSend Send ISRC Automatically
 * @note Automatic sending does not change status automatically,
 * it does the insertion of the packets in the data
 * islands.
 */
void packets_IsrcPackets(hdmi_tx_dev_t *dev, u8 initStatus, const u8 *codes,
			 u8 length, u8 autoSend);

/**
 * Send/stop sending AV Mute in the General Control Packet
 * @param dev Device structure
 * @param enable (TRUE) /disable (FALSE) the AV Mute
 */
void packets_AvMute(hdmi_tx_dev_t *dev, u8 enable);
u8 packets_get_AvMute(hdmi_tx_dev_t *dev);

/**
 * Set ISRC status that is changing during play back
 * depending on position (see HDMI 1.3a Section 8.8)
 * @param dev Device structure
 * @param status the ISRC status code according to position of track
 */
void packets_IsrcStatus(hdmi_tx_dev_t *dev, u8 status);


/**
 * Stop sending ACP packets when in auto send mode
 * @param dev Device structure
 */
void packets_StopSendAcp(hdmi_tx_dev_t *dev);

/**
 * Stop sending ISRC 1 & 2 packets when in auto send mode
 * (ISRC 2 packets cannot be send without ISRC 1)
 * @param dev Device structure
 */
void packets_StopSendIsrc1(hdmi_tx_dev_t *dev);

/**
 * Stop sending ISRC 2 packets when in auto send mode
 * @param dev Device structure
 */
void packets_StopSendIsrc2(hdmi_tx_dev_t *dev);

/**
 * Stop sending Source Product Description InfoFrame packets
 * when in auto send mode
 * @param dev Device structure
 */
void packets_StopSendSpd(hdmi_tx_dev_t *dev);

/**
 * Stop sending Vendor Specific InfoFrame packets when in auto send mode
 * @param dev Device structure
 */
void packets_StopSendVsd(hdmi_tx_dev_t *dev);

/**
 * Disable all metadata packets from being sent automatically.
 * (ISRC 1& 2, ACP, VSD and SPD)
 * @param dev Device structure
 */
void packets_DisableAllPackets(hdmi_tx_dev_t *dev);

/**
 * Configure Vendor Specific InfoFrames.
 * @param dev Device structure
 * @param oui Vendor Organisational Unique Identifier 24 bit IEEE
 * Registration Identifier
 * @param payload Vendor Specific Info Payload
 * @param length of the payload array
 * @param autoSend Start send Vendor Specific InfoFrame automatically
 */
int packets_VendorSpecificInfoFrame(hdmi_tx_dev_t *dev, u32 oui,
				const u8 *payload, u8 length, u8 autoSend);

/**
 * Configure Colorimetry packets
 * @param dev Device structure
 * @param video Video information structure
 */
void packets_colorimetry_config(hdmi_tx_dev_t *dev, videoParams_t *video);
void fc_QuantizationRange(hdmi_tx_dev_t *dev, u8 range);
void fc_ScanInfo(hdmi_tx_dev_t *dev, u8 left);
void fc_set_aspect_ratio(hdmi_tx_dev_t *dev, u8 left);

u8 fc_Colorimetry_get(hdmi_tx_dev_t *dev);
void fc_set_colorimetry(hdmi_tx_dev_t *dev, u8 metry, u8 ex_metry);
u8 fc_RgbYcc_get(hdmi_tx_dev_t *dev);
u8 fc_VideoCode_get(hdmi_tx_dev_t *dev);
void fc_VideoCode_set(hdmi_tx_dev_t *dev, u8 data);
void fc_vsif_get(hdmi_tx_dev_t *dev, u8 *data);
void fc_vsif_set(hdmi_tx_dev_t *dev, u8 *data);
void fc_get_vsd_vendor_payload(hdmi_tx_dev_t *dev, u8 *video_format, u32 *code);
#endif	/* PACKETS_H_ */
