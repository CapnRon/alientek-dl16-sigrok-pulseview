/*
 * This file is part of the libsigrok project.
 *
 * Copyright (C) 2024 (ALIENTEK DL16 driver)
 *
 * Driver for the ALIENTEK DL16 logic analyzer (USB ID 1a86:ffcc).
 * Protocol reverse-engineered from the ALIENTEK "ATK-Logic" host software
 * (GPLv3) and reimplemented under the GPL.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef LIBSIGROK_HARDWARE_ALIENTEK_DL16_PROTOCOL_H
#define LIBSIGROK_HARDWARE_ALIENTEK_DL16_PROTOCOL_H

#include <glib.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <libusb.h>
#include <libsigrok/libsigrok.h>
#include "libsigrok-internal.h"

#define LOG_PREFIX "alientek-dl16"

#define USB_INTERFACE		0
#define USB_CONFIGURATION	1

/* USB endpoints (bulk). */
#define EP_OUT			0x02	/* host -> MCU */
#define EP_IN			0x81	/* MCU -> host */

#define NUM_CHANNELS		16
#define NUM_PWM_CHANNELS	2
#define NUM_TRIGGER_STAGES	1

/* USB transfer geometry. Every block of data is 4-way interleaved in
 * 2048-byte chunks and must be de-/interleaved before/after framing. */
#define BLOCK_SIZE		2048
#define NUM_SIMUL_TRANSFERS	16
#define MAX_EMPTY_TRANSFERS	(NUM_SIMUL_TRANSFERS * 2)
#define TRANSFER_BUF_SIZE	(BLOCK_SIZE * 8)

/* Host -> device command codes (first payload byte). */
#define CMD_GET_DEVICE_DATA	0x10
#define CMD_PARAM_SETTING	0x11
#define CMD_SIMPLE_TRIGGER	0x12
#define CMD_STOP		0x15
#define CMD_PWM			0x17
#define CMD_EXIT		0x18

/* Device -> host order codes (second frame byte). */
#define ORDER_DATA		1	/* capture data */
#define ORDER_DEVICE_DATA	2	/* version / device info reply */
#define ORDER_TRIGGER_OFFSET	3	/* buffer-mode crop offsets */
#define ORDER_ACK		4	/* command acknowledgement */
#define ORDER_PROGRESS		5	/* capture/trigger progress */
#define ORDER_DONE		6	/* transfer complete / over-capacity */

/* Frame markers. */
#define FRAME_HEAD		0x0a
#define FRAME_TAIL		0x0b
#define FRAME_HEAD_OFFSET	8	/* zero padding before host->device frame */

/* ParameterSetting (0x11) flag bits. */
#define PARAM_FLAG_BUFFER	(1 << 7)
#define PARAM_FLAG_RLE		(1 << 6)

/* Minimum FPGA firmware version accepted (vendor gate). */
#define FPGA_MIN_VERSION_NUM	115

struct dev_context {
	/* Cached device identity. */
	uint16_t mcu_version;
	uint16_t fpga_version;

	/* Configuration. */
	const uint64_t *samplerates;
	int num_samplerates;
	uint64_t cur_samplerate;
	uint64_t limit_samples;
	uint64_t limit_frames;
	uint64_t capture_ratio;
	gboolean continuous;
	gboolean rle;
	float threshold;
	int rate_index;		/* ParameterSetting rate byte (1-based) */

	/* PWM output (two channels). */
	double pwm_freq[NUM_PWM_CHANNELS];
	double pwm_duty[NUM_PWM_CHANNELS];

	/* Runtime state. */
	struct sr_context *ctx;
	gboolean acq_aborted;
	uint64_t sent_samples;

	/* libusb asynchronous transfer management. */
	struct libusb_transfer **transfers;
	int num_transfers;
	int submitted_transfers;
	int empty_transfer_count;

	/* Frame reassembly buffer (de-interleaved stream). */
	GByteArray *rxbuf;

	/* Per-channel capture buffers (8 samples/byte, LSB-first). */
	GByteArray *chanbuf[NUM_CHANNELS];

	/* Buffer-mode trigger crop (order 3). */
	uint64_t trigger_offset;
	uint64_t chan_counts[NUM_CHANNELS];
	uint64_t crop_offset;	/* samples to skip from the start */
	gboolean have_crop;
};

SR_PRIV char *dl16_probe_model(libusb_device *dev);
SR_PRIV int dl16_read_fpga_version(const struct sr_dev_inst *sdi,
		uint16_t *version);
SR_PRIV int dl16_dev_open(struct sr_dev_inst *sdi);
SR_PRIV struct dev_context *dl16_dev_new(void);
SR_PRIV int dl16_start_acquisition(const struct sr_dev_inst *sdi);
SR_PRIV void dl16_abort_acquisition(struct dev_context *devc);
SR_PRIV int dl16_send_stop(const struct sr_dev_inst *sdi);
SR_PRIV int dl16_send_pwm(const struct sr_dev_inst *sdi, int ch,
		uint32_t hz, uint32_t duty);
SR_PRIV void dl16_stop_acquisition(struct sr_dev_inst *sdi);
SR_PRIV void dl16_handle_frame(struct sr_dev_inst *sdi, uint8_t order,
		const uint8_t *payload, size_t plen);

#endif
