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

#include <config.h>
#include <math.h>
#include "protocol.h"

/* ------------------------------------------------------------------ */
/* CRC-32                                                             */
/* ------------------------------------------------------------------ */
/*
 * The device uses the standard reflected CRC-32 (poly 0xEDB88320) but with a
 * zero initial value and a final XOR of 0xffffffff (matching the gCRC32()
 * routine in the ATK-Logic host software). This differs from the zlib
 * variant which seeds the register with 0xffffffff.
 */
static uint32_t crc32_table[256];
static gboolean crc32_table_ready;

static void crc32_init(void)
{
	uint32_t c;
	unsigned int i, j;

	for (i = 0; i < 256; i++) {
		c = i;
		for (j = 0; j < 8; j++)
			c = (c & 1) ? 0xedb88320U ^ (c >> 1) : c >> 1;
		crc32_table[i] = c;
	}
	crc32_table_ready = TRUE;
}

static uint32_t dl16_crc32(const uint8_t *buf, size_t len)
{
	uint32_t crc;
	size_t i;

	if (len == 0)
		return 0xffffffffU;

	if (!crc32_table_ready)
		crc32_init();

	crc = 0;
	for (i = 0; i < len; i++)
		crc = crc32_table[(crc ^ buf[i]) & 0xff] ^ (crc >> 8);

	return crc ^ 0xffffffffU;
}

/* ------------------------------------------------------------------ */
/* 2048-byte 4-way interleave                                          */
/* ------------------------------------------------------------------ */
/*
 * The DL16 transports data as 2048-byte blocks split into four 512-byte
 * regions. On the wire the regions are word-interleaved (16-bit units);
 * the host must de-interleave on receive and interleave on transmit.
 *
 * deinterleave (convert_to_pc):  out[8j+0..1] = in[2j+0..1]          region 0
 *                                out[8j+2..3] = in[512+2j..]         region 1
 *                                out[8j+4..5] = in[1024+2j..]        region 2
 *                                out[8j+6..7] = in[1536+2j..]        region 3
 */
static void dl16_deinterleave(const uint8_t *src, uint8_t *dst, unsigned len)
{
	unsigned int i, j;

	for (i = 0; i < len; i += BLOCK_SIZE) {
		for (j = 0; j < 256; j++) {
			dst[8 * j + 0] = src[i + 2 * j + 0];
			dst[8 * j + 1] = src[i + 2 * j + 1];
			dst[8 * j + 2] = src[i + 512 + 2 * j + 0];
			dst[8 * j + 3] = src[i + 512 + 2 * j + 1];
			dst[8 * j + 4] = src[i + 1024 + 2 * j + 0];
			dst[8 * j + 5] = src[i + 1024 + 2 * j + 1];
			dst[8 * j + 6] = src[i + 1536 + 2 * j + 0];
			dst[8 * j + 7] = src[i + 1536 + 2 * j + 1];
		}
		src += BLOCK_SIZE;
		dst += BLOCK_SIZE;
	}
}

static void dl16_interleave(const uint8_t *src, uint8_t *dst, unsigned len)
{
	unsigned int i, j;

	for (i = 0; i < len; i += BLOCK_SIZE) {
		for (j = 0; j < 256; j++) {
			dst[2 * j + 0] = src[8 * j + 0];
			dst[2 * j + 1] = src[8 * j + 1];
			dst[512 + 2 * j + 0] = src[8 * j + 2];
			dst[512 + 2 * j + 1] = src[8 * j + 3];
			dst[1024 + 2 * j + 0] = src[8 * j + 4];
			dst[1024 + 2 * j + 1] = src[8 * j + 5];
			dst[1536 + 2 * j + 0] = src[8 * j + 6];
			dst[1536 + 2 * j + 1] = src[8 * j + 7];
		}
		src += BLOCK_SIZE;
		dst += BLOCK_SIZE;
	}
}

/* ------------------------------------------------------------------ */
/* Command transport                                                  */
/* ------------------------------------------------------------------ */
/*
 * Host -> device frame (before interleaving):
 *   [8 bytes 0x00] 0x0A [cmd] [len] [payload(len-1)] 0x0B [CRC32:4]
 * where len = payload_len + 1. The CRC32 covers [cmd][len][payload]
 * (NOT the 0x0A header), matching the ATK-Logic Write() implementation.
 */
static int dl16_write_command(const struct sr_dev_inst *sdi,
		uint8_t cmd, const uint8_t *payload, size_t payload_len)
{
	struct sr_usb_dev_inst *usb = sdi->conn;
	size_t frame_len = FRAME_HEAD_OFFSET + 1 + 2 + payload_len + 1 + 4;
	uint8_t *frame, *txbuf;
	size_t tx_len;
	uint32_t crc;
	int transferred = 0;
	int ret;

	frame = g_malloc0(frame_len);
	frame[FRAME_HEAD_OFFSET + 0] = FRAME_HEAD;
	frame[FRAME_HEAD_OFFSET + 1] = cmd;
	frame[FRAME_HEAD_OFFSET + 2] = (uint8_t)(payload_len + 1);
	if (payload_len)
		memcpy(frame + FRAME_HEAD_OFFSET + 3, payload, payload_len);
	frame[FRAME_HEAD_OFFSET + 3 + payload_len] = FRAME_TAIL;

	/* CRC covers cmd + len + payload (i.e. everything after 0x0A). */
	crc = dl16_crc32(frame + FRAME_HEAD_OFFSET + 1, payload_len + 2);
	frame[FRAME_HEAD_OFFSET + 4 + payload_len + 0] = crc & 0xff;
	frame[FRAME_HEAD_OFFSET + 4 + payload_len + 1] = (crc >> 8) & 0xff;
	frame[FRAME_HEAD_OFFSET + 4 + payload_len + 2] = (crc >> 16) & 0xff;
	frame[FRAME_HEAD_OFFSET + 4 + payload_len + 3] = (crc >> 24) & 0xff;

	/* Pad to a 2048-byte multiple, then 4-way interleave. */
	tx_len = (frame_len + BLOCK_SIZE - 1) / BLOCK_SIZE * BLOCK_SIZE;
	txbuf = g_malloc0(tx_len);
	memcpy(txbuf, frame, frame_len);
	dl16_interleave(txbuf, txbuf, tx_len);

	ret = libusb_bulk_transfer(usb->devhdl, EP_OUT, txbuf, tx_len,
			&transferred, 100);

	g_free(txbuf);
	g_free(frame);

	if (ret != LIBUSB_SUCCESS) {
		sr_err("Failed to send command 0x%02x: %s.",
			cmd, libusb_error_name(ret));
		return SR_ERR;
	}

	return SR_OK;
}

SR_PRIV int dl16_send_stop(const struct sr_dev_inst *sdi)
{
	return dl16_write_command(sdi, CMD_STOP, NULL, 0);
}

/*
 * 0x17: PWM output.
 *
 * The device uses a 200 MHz base clock. For frequency hz and duty percent,
 * divider = round(200e6 / hz), duty = round(divider * duty/100), both 4-byte
 * LE. hz == 0 stops the channel. ch 0 uses 0x11/0x10, ch 1 uses 0x21/0x20.
 */
SR_PRIV int dl16_send_pwm(const struct sr_dev_inst *sdi, int ch,
		uint32_t hz, uint32_t duty)
{
	uint8_t payload[9];
	uint32_t divider, duty_val;
	int i;

	if (hz == 0) {
		payload[0] = ch ? 0x20 : 0x10;
		return dl16_write_command(sdi, CMD_PWM, payload, 1);
	}

	divider = (200000000ULL + hz / 2) / hz;
	duty_val = ((uint64_t)divider * duty + 50) / 100;

	payload[0] = ch ? 0x21 : 0x11;
	for (i = 0; i < 4; i++)
		payload[1 + i] = (divider >> (i * 8)) & 0xff;
	for (i = 0; i < 4; i++)
		payload[5 + i] = (duty_val >> (i * 8)) & 0xff;

	return dl16_write_command(sdi, CMD_PWM, payload, sizeof(payload));
}

/* Raw MCU sub-command (0x80..0x88): [0x0A][subcmd][val][...] 512 bytes.
 * No CRC, no interleave (matches ATK SendToMCU). */
static int dl16_send_raw(const struct sr_dev_inst *sdi,
		uint8_t subcmd, uint8_t val)
{
	struct sr_usb_dev_inst *usb = sdi->conn;
	uint8_t buf[512];
	int transferred = 0;
	int ret;

	memset(buf, 0, sizeof(buf));
	buf[0] = FRAME_HEAD;
	buf[1] = subcmd;
	buf[2] = val;

	ret = libusb_bulk_transfer(usb->devhdl, EP_OUT, buf, sizeof(buf),
			&transferred, 2000);
	if (ret != LIBUSB_SUCCESS) {
		sr_err("Failed to send raw command 0x%02x: %s.",
			subcmd, libusb_error_name(ret));
		return SR_ERR;
	}

	return SR_OK;
}

/* 0x87: set FPGA reset state (1 = wake, 0 = sleep). */
static int dl16_set_reset_state(const struct sr_dev_inst *sdi, uint8_t state)
{
	return dl16_send_raw(sdi, 0x87, state);
}

/* 0x11: ParameterSetting payload (13 bytes). */
static int dl16_send_param_setting(const struct sr_dev_inst *sdi)
{
	struct dev_context *devc = sdi->priv;
	uint8_t param[13];
	uint64_t depth, trig_depth;
	int i;

	memset(param, 0, sizeof(param));

	/* flags: bit7 = buffer mode, bit6 = RLE. */
	if (!devc->continuous)
		param[0] |= PARAM_FLAG_BUFFER;
	if (devc->rle)
		param[0] |= PARAM_FLAG_RLE;

	/* threshold: bit7 = sign, low 7 bits = abs(volts * 10). */
	if (devc->threshold < 0)
		param[1] |= 0x80;
	param[1] |= (uint8_t)(fabs(devc->threshold) * 10.0 + 0.5);

	param[2] = devc->rate_index;

	depth = devc->limit_samples ? devc->limit_samples : 1000000ULL;
	for (i = 0; i < 5; i++)
		param[3 + i] = (depth >> (i * 8)) & 0xff;

	trig_depth = depth * devc->capture_ratio / 100;
	for (i = 0; i < 5; i++)
		param[8 + i] = (trig_depth >> (i * 8)) & 0xff;

	sr_spew("param: %02x %02x %02x | %02x %02x %02x %02x %02x | %02x %02x %02x %02x %02x",
		param[0], param[1], param[2], param[3], param[4], param[5],
		param[6], param[7], param[8], param[9], param[10], param[11], param[12]);

	return dl16_write_command(sdi, CMD_PARAM_SETTING, param, sizeof(param));
}

/* 0x12: SimpleTrigger payload (9 bytes).
 *
 * Per channel pair byte: bit7/3 = enable (even/odd), bit4/0 = rising edge,
 * bit5/1 = falling edge, bit6/2 = high level. Low level sets no bits.
 * Byte 8 is isInstantly (1 = capture immediately, no trigger wait).
 */
static int dl16_send_simple_trigger(const struct sr_dev_inst *sdi)
{
	uint8_t trig[9];
	struct sr_trigger *trigger;
	GSList *l, *m;
	gboolean has_trigger = FALSE;
	gboolean matched[NUM_CHANNELS] = { 0 };

	memset(trig, 0, sizeof(trig));

	for (l = sdi->channels; l; l = l->next) {
		struct sr_channel *ch = l->data;
		if (!ch->enabled)
			continue;
		if (ch->index % 2 == 0)
			trig[ch->index / 2] |= 0x80;
		else
			trig[ch->index / 2] |= 0x08;
	}

	/* Map the first trigger stage's matches to the trigger bits. */
	trigger = sr_session_trigger_get(sdi->session);
	if (trigger && trigger->stages) {
		struct sr_trigger_stage *stage = trigger->stages->data;
		if (stage) {
			for (m = stage->matches; m; m = m->next) {
				struct sr_trigger_match *match = m->data;
				int ch = match->channel->index;
				int even = (ch % 2 == 0);
				uint8_t bit_r = even ? 0x10 : 0x01;
				uint8_t bit_f = even ? 0x20 : 0x02;
				uint8_t bit_h = even ? 0x40 : 0x04;

				if (ch < 0 || ch >= NUM_CHANNELS)
					continue;

				has_trigger = TRUE;
				matched[ch] = TRUE;
				switch (match->match) {
				case SR_TRIGGER_RISING:
					trig[ch / 2] |= bit_r;
					break;
				case SR_TRIGGER_FALLING:
					trig[ch / 2] |= bit_f;
					break;
				case SR_TRIGGER_ONE:
					trig[ch / 2] |= bit_h;
					break;
				case SR_TRIGGER_EDGE:
					trig[ch / 2] |= bit_r | bit_f;
					break;
				case SR_TRIGGER_ZERO:
					break;	/* low level = no trigger bits */
				default:
					break;
				}
			}
		}
	}

	/* Selected channels without an explicit trigger get the vendor's
	 * default capture bits (rising + falling + high). The device aborts
	 * multi-channel captures if these are left clear. */
	for (l = sdi->channels; l; l = l->next) {
		struct sr_channel *ch = l->data;
		if (!ch->enabled || matched[ch->index])
			continue;
		if (ch->index % 2 == 0)
			trig[ch->index / 2] |= 0x70;
		else
			trig[ch->index / 2] |= 0x07;
	}

	trig[8] = has_trigger ? 0 : 1;

	sr_spew("trig: %02x %02x %02x %02x %02x %02x %02x %02x | %02x",
		trig[0], trig[1], trig[2], trig[3], trig[4], trig[5],
		trig[6], trig[7], trig[8]);

	return dl16_write_command(sdi, CMD_SIMPLE_TRIGGER, trig, sizeof(trig));
}

/* ------------------------------------------------------------------ */
/* Device identity                                                    */
/* ------------------------------------------------------------------ */

/*
 * Probe the model name by reading the MCU version (raw sub-command 0x81).
 * Reply: [0x0A][0x81][0x01][state][...][level]; level==1 means "Plus".
 * Opens the device briefly; safe to call during scan.
 */
SR_PRIV char *dl16_probe_model(libusb_device *dev)
{
	libusb_device_handle *hdl = NULL;
	uint8_t buf[512];
	int transferred = 0, r;
	const char *model = "DL16";

	if (libusb_open(dev, &hdl) != LIBUSB_SUCCESS)
		return g_strdup(model);

	if (libusb_claim_interface(hdl, USB_INTERFACE) != LIBUSB_SUCCESS) {
		libusb_close(hdl);
		return g_strdup(model);
	}

	/* Drain stale data from any previous capture. */
	do {
		transferred = 0;
		r = libusb_bulk_transfer(hdl, EP_IN, buf, sizeof(buf),
			&transferred, 10);
	} while (r == LIBUSB_SUCCESS && transferred > 0);

	memset(buf, 0, sizeof(buf));
	buf[0] = FRAME_HEAD;
	buf[1] = 0x81;
	libusb_bulk_transfer(hdl, EP_OUT, buf, sizeof(buf), &transferred, 1000);

	memset(buf, 0, sizeof(buf));
	transferred = 0;
	r = libusb_bulk_transfer(hdl, EP_IN, buf, sizeof(buf), &transferred, 1000);

	sr_spew("mcu probe: r=%d transferred=%d buf=%02x %02x %02x %02x .. %02x",
		r, transferred, buf[0], buf[1], buf[2], buf[3], buf[8]);

	if (r == LIBUSB_SUCCESS && transferred >= 9 &&
		buf[0] == FRAME_HEAD && buf[1] == 0x81 && buf[2] == 0x01 &&
		buf[3] == 0x61 && buf[8] == 1)
		model = "DL16 Plus";

	libusb_release_interface(hdl, USB_INTERFACE);
	libusb_close(hdl);

	return g_strdup(model);
}

/*
 * Read the FPGA firmware version (order-2 reply to CMD_GET_DEVICE_DATA).
 * Requires an open device; wakes the FPGA first. version is payload[5]*100
 * + payload[6] (e.g. 119 == 1.19).
 */
SR_PRIV int dl16_read_fpga_version(const struct sr_dev_inst *sdi,
		uint16_t *version)
{
	struct sr_usb_dev_inst *usb = sdi->conn;
	uint8_t buf[BLOCK_SIZE];
	uint8_t db[BLOCK_SIZE];
	uint16_t plen;
	int transferred = 0, r;

	/* Wake FPGA and drain stale data. */
	dl16_set_reset_state(sdi, 0);
	g_usleep(10 * 1000);
	dl16_set_reset_state(sdi, 1);
	g_usleep(20 * 1000);
	while (libusb_bulk_transfer(usb->devhdl, EP_IN, buf, sizeof(buf),
			&transferred, 50) == LIBUSB_SUCCESS && transferred > 0)
		;

	if (dl16_write_command(sdi, CMD_GET_DEVICE_DATA, NULL, 0) != SR_OK)
		return SR_ERR;

	memset(buf, 0, sizeof(buf));
	transferred = 0;
	r = libusb_bulk_transfer(usb->devhdl, EP_IN, buf, BLOCK_SIZE,
			&transferred, 2000);
	if (r != LIBUSB_SUCCESS || transferred < BLOCK_SIZE)
		return SR_ERR;

	dl16_deinterleave(buf, db, BLOCK_SIZE);
	if (db[0] != FRAME_HEAD || db[1] != ORDER_DEVICE_DATA)
		return SR_ERR;

	plen = db[2] | (db[3] << 8);
	if (plen < 9)
		return SR_ERR;

	*version = db[4 + 5] * 100 + db[4 + 6];

	return SR_OK;
}

/* ------------------------------------------------------------------ */
/* Device open / close                                                */
/* ------------------------------------------------------------------ */

SR_PRIV struct dev_context *dl16_dev_new(void)
{
	struct dev_context *devc;
	int i;

	devc = g_malloc0(sizeof(struct dev_context));
	devc->rxbuf = g_byte_array_new();
	devc->threshold = 1.6f;	/* 3.3V CMOS midpoint */
	devc->capture_ratio = 50;
	devc->rate_index = 1;	/* 1 MHz default */
	devc->continuous = TRUE;	/* stream mode (matches vendor default) */
	devc->pwm_duty[0] = devc->pwm_duty[1] = 50;
	for (i = 0; i < NUM_CHANNELS; i++)
		devc->chanbuf[i] = g_byte_array_new();

	return devc;
}

SR_PRIV int dl16_dev_open(struct sr_dev_inst *sdi)
{
	struct sr_usb_dev_inst *usb = sdi->conn;
	int ret;

	ret = libusb_claim_interface(usb->devhdl, USB_INTERFACE);
	if (ret != LIBUSB_SUCCESS) {
		sr_err("Failed to claim interface: %s.",
			libusb_error_name(ret));
		return SR_ERR;
	}

	return SR_OK;
}

/* ------------------------------------------------------------------ */
/* Frame parsing                                                      */
/* ------------------------------------------------------------------ */
/*
 * Device -> host frame (after de-interleaving):
 *   0x0A [order:1] [len:2 LE] [payload(len)] 0x00 0x0B [CRC32:4]
 * The reference host only validates the 0x00 0x0B tail and resynchronises
 * over any trailing bytes, so we do the same here (no receive CRC check).
 */
static void dl16_parse_frames(struct sr_dev_inst *sdi, const uint8_t *buf,
		size_t len)
{
	struct dev_context *devc = sdi->priv;
	size_t i, avail;
	uint8_t order;
	uint16_t plen;
	const uint8_t *p;

	g_byte_array_append(devc->rxbuf, buf, len);

	i = 0;
	while (i < devc->rxbuf->len) {
		avail = devc->rxbuf->len - i;
		p = devc->rxbuf->data + i;

		if (avail < 6 || p[0] != FRAME_HEAD || p[1] == 0 || p[1] > 6) {
			i++;	/* resync */
			continue;
		}

		order = p[1];
		plen = p[2] | (p[3] << 8);
		if (avail < (size_t)plen + 6)
			break;	/* incomplete frame */

		if (p[4 + plen] != 0x00 || p[5 + plen] != FRAME_TAIL) {
			i++;	/* malformed, resync */
			continue;
		}

		dl16_handle_frame(sdi, order, p + 4, plen);

		/* Consume header + len + payload + tail. Trailing CRC bytes
		 * (if any) are skipped by the resync logic above. */
		i += (size_t)plen + 6;
	}

	if (i > 0)
		g_byte_array_remove_range(devc->rxbuf, 0, i);
}

/* ------------------------------------------------------------------ */
/* Data path                                                          */
/* ------------------------------------------------------------------ */

static gboolean dl16_channel_enabled(const struct sr_dev_inst *sdi, int idx)
{
	GSList *l;

	for (l = sdi->channels; l; l = l->next) {
		struct sr_channel *ch = l->data;
		if (ch->index == idx)
			return ch->enabled;
	}

	return FALSE;
}

/*
 * Transpose per-channel byte streams (8 samples/byte, LSB-first) into
 * sample-interleaved SR_DF_LOGIC packets (unitsize 2, bit i = channel i).
 */
static void dl16_emit_logic(struct sr_dev_inst *sdi)
{
	struct dev_context *devc = sdi->priv;
	gboolean enabled[NUM_CHANNELS];
	size_t minlen = G_MAXSIZE;
	uint8_t *out;
	size_t j, nsamples;
	int ch, b;
	gboolean have = FALSE;

	/* Apply buffer-mode trigger crop once (skip pre-trigger excess). */
	if (devc->have_crop && devc->crop_offset > 0) {
		size_t skip = devc->crop_offset / 8;
		for (ch = 0; ch < NUM_CHANNELS; ch++) {
			if (devc->chanbuf[ch]->len > skip)
				g_byte_array_remove_range(devc->chanbuf[ch], 0, skip);
			else
				g_byte_array_set_size(devc->chanbuf[ch], 0);
		}
		devc->have_crop = FALSE;
	}

	for (ch = 0; ch < NUM_CHANNELS; ch++) {
		enabled[ch] = dl16_channel_enabled(sdi, ch);
		if (enabled[ch]) {
			have = TRUE;
			if (devc->chanbuf[ch]->len < minlen)
				minlen = devc->chanbuf[ch]->len;
		}
	}
	if (!have || minlen == 0 || minlen == G_MAXSIZE) {
		sr_spew("emit: stall (have=%d minlen=%zu)", have, minlen);
		return;
	}
	sr_spew("emit: minlen=%zu", minlen);

	nsamples = minlen * 8;
	out = g_malloc(nsamples * 2);

	for (j = 0; j < minlen; j++) {
		for (b = 0; b < 8; b++) {
			uint16_t w = 0;
			for (ch = 0; ch < NUM_CHANNELS; ch++) {
				if (!enabled[ch])
					continue;
				if (devc->chanbuf[ch]->data[j] & (1 << b))
					w |= (1 << ch);
			}
			out[(j * 8 + b) * 2 + 0] = w & 0xff;
			out[(j * 8 + b) * 2 + 1] = (w >> 8) & 0xff;
		}
	}

	{
		const struct sr_datafeed_logic logic = {
			.length = nsamples * 2,
			.unitsize = 2,
			.data = out,
		};
		const struct sr_datafeed_packet packet = {
			.type = SR_DF_LOGIC,
			.payload = &logic,
		};
		sr_session_send(sdi, &packet);
	}
	g_free(out);

	devc->sent_samples += nsamples;

	for (ch = 0; ch < NUM_CHANNELS; ch++) {
		if (!enabled[ch])
			continue;
		g_byte_array_remove_range(devc->chanbuf[ch], 0, minlen);
	}

	if (devc->limit_samples && devc->sent_samples >= devc->limit_samples) {
		sr_dbg("Reached limit_samples (%" PRIu64 ").", devc->limit_samples);
		devc->acq_aborted = TRUE;
	}
}

SR_PRIV void dl16_handle_frame(struct sr_dev_inst *sdi, uint8_t order,
		const uint8_t *payload, size_t plen)
{
	struct dev_context *devc = sdi->priv;

	switch (order) {
	case ORDER_DATA:
	{
		int ch;
		const uint8_t *data;
		size_t dlen;
		GByteArray *rle = NULL;

		if (plen < 2)
			break;

		ch = payload[0];
		data = payload + 2;	/* skip [chID][reserved] */
		dlen = plen - 2;
		if (ch < 0 || ch >= NUM_CHANNELS)
			break;
		sr_spew("order 1 (data): ch=%d dlen=%zu", ch, dlen);

		if (devc->rle) {
			size_t i;
			uint8_t val;

			rle = g_byte_array_new();
			for (i = 0; i + 1 < dlen; i += 2) {
				uint8_t cnt = data[i];
				val = data[i + 1];
				while (cnt--)
					g_byte_array_append(rle, &val, 1);
			}
			data = rle->data;
			dlen = rle->len;
		}

		g_byte_array_append(devc->chanbuf[ch], data, dlen);
		if (rle)
			g_byte_array_free(rle, TRUE);

		dl16_emit_logic(sdi);
		break;
	}
	case ORDER_TRIGGER_OFFSET:
	{
		/* payload: [reserved:2][trigger offset:5][ch counts:5*16][flags?] */
		uint64_t trig_depth;
		int i;

		if (plen >= 7)
			devc->trigger_offset =
				(uint64_t)payload[2] | ((uint64_t)payload[3] << 8) |
				((uint64_t)payload[4] << 16) | ((uint64_t)payload[5] << 24) |
				((uint64_t)payload[6] << 32);
		sr_spew("order 3: plen=%zu trig_off=%" PRIu64, plen,
			devc->trigger_offset);

		for (i = 0; i < NUM_CHANNELS && (size_t)7 + (i + 1) * 5 <= plen; i++) {
			const uint8_t *p = payload + 7 + i * 5;
			devc->chan_counts[i] =
				(uint64_t)p[0] | ((uint64_t)p[1] << 8) |
				((uint64_t)p[2] << 16) | ((uint64_t)p[3] << 24) |
				((uint64_t)p[4] << 32);
		}

		/* Crop (buffer mode): skip samples so the trigger lands at the
		 * capture-ratio position. offset = count*8 - trig_depth - trig_pos. */
		if (!devc->continuous && devc->chan_counts[0] > 0 &&
			devc->chan_counts[0] * 8 >
				(devc->limit_samples ? devc->limit_samples : 1000000ULL)) {
			trig_depth = (devc->limit_samples ? devc->limit_samples : 1000000ULL)
				* devc->capture_ratio / 100;
			devc->crop_offset = devc->chan_counts[0] * 8 - trig_depth
				- devc->trigger_offset;
			devc->have_crop = TRUE;
			sr_dbg("order 3: crop=%" PRIu64 " (count=%" PRIu64
				", trig_depth=%" PRIu64 ", trig_off=%" PRIu64 ")",
				devc->crop_offset, devc->chan_counts[0] * 8,
				trig_depth, devc->trigger_offset);
		}
		break;
	}
	case ORDER_DONE:
		sr_spew("order 6 (done): plen=%zu payload=%02x %02x %02x",
			plen, plen > 0 ? payload[0] : 0,
			plen > 1 ? payload[1] : 0,
			plen > 2 ? payload[2] : 0);
		devc->acq_aborted = TRUE;
		break;
	case ORDER_ACK:
		/* ACK for a host command. payload[2] = echoed cmd,
		 * payload[3] = status (3 = accepted). */
		sr_spew("order 4 (ack): plen=%zu payload=%02x %02x %02x %02x",
			plen, plen > 0 ? payload[0] : 0,
			plen > 1 ? payload[1] : 0,
			plen > 2 ? payload[2] : 0,
			plen > 3 ? payload[3] : 0);
		if (plen >= 3 && payload[2] == CMD_STOP)
			devc->acq_aborted = TRUE;
		break;
	default:
		break;
	}
}

/* ------------------------------------------------------------------ */
/* Acquisition                                                        */
/* ------------------------------------------------------------------ */

/* Session-loop callback: drain pending libusb events (async transfers). */
static int dl16_receive_data(int fd, int revents, void *cb_data)
{
	struct drv_context *drvc = cb_data;
	struct timeval tv;

	(void)fd;
	(void)revents;

	tv.tv_sec = tv.tv_usec = 0;
	libusb_handle_events_timeout(drvc->sr_ctx->libusb_ctx, &tv);

	return TRUE;
}

static void finish_acquisition(struct sr_dev_inst *sdi)
{
	struct dev_context *devc = sdi->priv;

	std_session_send_df_end(sdi);
	usb_source_remove(sdi->session, devc->ctx);

	devc->num_transfers = 0;
	g_free(devc->transfers);
	devc->transfers = NULL;
}

static void free_transfer(struct libusb_transfer *transfer)
{
	struct sr_dev_inst *sdi;
	struct dev_context *devc;
	unsigned int i;

	if (!transfer)
		return;

	sdi = transfer->user_data;
	devc = sdi->priv;

	g_free(transfer->buffer);
	transfer->buffer = NULL;
	libusb_free_transfer(transfer);

	for (i = 0; i < (unsigned int)devc->num_transfers; i++) {
		if (devc->transfers[i] == transfer) {
			devc->transfers[i] = NULL;
			break;
		}
	}

	devc->submitted_transfers--;
	if (devc->submitted_transfers == 0)
		finish_acquisition(sdi);
}

static void resubmit_transfer(struct libusb_transfer *transfer)
{
	int ret;

	if ((ret = libusb_submit_transfer(transfer)) == LIBUSB_SUCCESS)
		return;

	sr_err("Failed to resubmit transfer: %s.", libusb_error_name(ret));
	free_transfer(transfer);
}

static void LIBUSB_CALL dl16_receive_transfer(struct libusb_transfer *transfer)
{
	struct sr_dev_inst *sdi = transfer->user_data;
	struct dev_context *devc = sdi->priv;
	uint8_t *deinterleaved;
	size_t nblocks, out_len;

	if (devc->acq_aborted) {
		free_transfer(transfer);
		return;
	}

	switch (transfer->status) {
	case LIBUSB_TRANSFER_NO_DEVICE:
		dl16_abort_acquisition(devc);
		free_transfer(transfer);
		return;
	case LIBUSB_TRANSFER_COMPLETED:
	case LIBUSB_TRANSFER_TIMED_OUT:
		break;
	default:
		free_transfer(transfer);
		return;
	}

	if (transfer->actual_length == 0) {
		devc->empty_transfer_count++;
		if (devc->empty_transfer_count > MAX_EMPTY_TRANSFERS) {
			dl16_abort_acquisition(devc);
			free_transfer(transfer);
		} else {
			resubmit_transfer(transfer);
		}
		return;
	}
	devc->empty_transfer_count = 0;

	/* De-interleave the 2048-aligned portion of the transfer. */
	nblocks = transfer->actual_length / BLOCK_SIZE;
	out_len = nblocks * BLOCK_SIZE;
	if (out_len) {
		deinterleaved = g_malloc(out_len);
		dl16_deinterleave(transfer->buffer, deinterleaved, out_len);
		dl16_parse_frames(sdi, deinterleaved, out_len);
		g_free(deinterleaved);
	}

	if (devc->acq_aborted)
		free_transfer(transfer);
	else
		resubmit_transfer(transfer);
}

SR_PRIV int dl16_start_acquisition(const struct sr_dev_inst *sdi)
{
	struct sr_dev_driver *di = sdi->driver;
	struct drv_context *drvc = di->context;
	struct sr_usb_dev_inst *usb = sdi->conn;
	struct dev_context *devc = sdi->priv;
	struct libusb_transfer *transfer;
	uint8_t dummy[512];
	int transferred;
	int i, ret;

	devc->ctx = drvc->sr_ctx;
	usb_source_add(sdi->session, devc->ctx, 1, dl16_receive_data, drvc);

	devc->acq_aborted = FALSE;
	devc->sent_samples = 0;
	devc->empty_transfer_count = 0;
	devc->have_crop = FALSE;
	devc->crop_offset = 0;
	g_byte_array_set_size(devc->rxbuf, 0);
	for (i = 0; i < NUM_CHANNELS; i++)
		g_byte_array_set_size(devc->chanbuf[i], 0);

	/* Full FPGA reset cycle so every capture starts from a clean state. */
	dl16_set_reset_state(sdi, 0);
	g_usleep(10 * 1000);
	dl16_set_reset_state(sdi, 1);
	g_usleep(20 * 1000);
	while (libusb_bulk_transfer(usb->devhdl, EP_IN, dummy, sizeof(dummy),
			&transferred, 50) == LIBUSB_SUCCESS && transferred > 0)
		;

	/* Configure and arm. */
	if (dl16_send_param_setting(sdi) != SR_OK)
		return SR_ERR;
	g_usleep(30 * 1000);
	if (dl16_send_simple_trigger(sdi) != SR_OK)
		return SR_ERR;

	devc->transfers = g_malloc0(sizeof(struct libusb_transfer *) *
		NUM_SIMUL_TRANSFERS);
	devc->num_transfers = NUM_SIMUL_TRANSFERS;
	devc->submitted_transfers = 0;

	for (i = 0; i < devc->num_transfers; i++) {
		transfer = libusb_alloc_transfer(0);
		transfer->dev_handle = usb->devhdl;
		transfer->flags = 0;
		transfer->endpoint = EP_IN;
		transfer->type = LIBUSB_TRANSFER_TYPE_BULK;
		transfer->timeout = 100;	/* ms; finite so stop can reap transfers */
		transfer->buffer = g_malloc(TRANSFER_BUF_SIZE);
		transfer->length = TRANSFER_BUF_SIZE;
		transfer->callback = dl16_receive_transfer;
		transfer->user_data = (void *)sdi;

		if ((ret = libusb_submit_transfer(transfer)) != LIBUSB_SUCCESS) {
			sr_err("Failed to submit transfer: %s.",
				libusb_error_name(ret));
			free_transfer(transfer);
			devc->transfers[i] = NULL;
			continue;
		}
		devc->transfers[i] = transfer;
		devc->submitted_transfers++;
	}

	if (devc->submitted_transfers == 0) {
		g_free(devc->transfers);
		devc->transfers = NULL;
		return SR_ERR;
	}

	std_session_send_df_header(sdi);

	return SR_OK;
}

SR_PRIV void dl16_abort_acquisition(struct dev_context *devc)
{
	/* Only set the flag. Transfers are reaped by the session loop when they
	 * complete or time out (calling libusb_cancel_transfer from here would
	 * deadlock against the session thread's event loop). */
	devc->acq_aborted = TRUE;
}

SR_PRIV void dl16_stop_acquisition(struct sr_dev_inst *sdi)
{
	struct dev_context *devc = sdi->priv;

	dl16_send_stop(sdi);
	dl16_set_reset_state(sdi, 0);	/* sleep FPGA */
	dl16_abort_acquisition(devc);
}
