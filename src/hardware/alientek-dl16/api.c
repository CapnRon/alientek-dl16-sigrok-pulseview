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
#include "protocol.h"

#define DL16_VID	0x1a86
#define DL16_PID	0xffcc

static const uint32_t scanopts[] = {
	SR_CONF_CONN,
};

static const uint32_t drvopts[] = {
	SR_CONF_LOGIC_ANALYZER,
};

static const uint32_t devopts[] = {
	SR_CONF_CONTINUOUS | SR_CONF_GET | SR_CONF_SET,
	SR_CONF_LIMIT_SAMPLES | SR_CONF_GET | SR_CONF_SET,
	SR_CONF_LIMIT_FRAMES | SR_CONF_GET | SR_CONF_SET,
	SR_CONF_SAMPLERATE | SR_CONF_GET | SR_CONF_SET | SR_CONF_LIST,
	SR_CONF_CAPTURE_RATIO | SR_CONF_GET | SR_CONF_SET,
	SR_CONF_RLE | SR_CONF_GET | SR_CONF_SET,
	SR_CONF_VOLTAGE_THRESHOLD | SR_CONF_GET | SR_CONF_SET | SR_CONF_LIST,
	SR_CONF_TRIGGER_MATCH | SR_CONF_LIST,
	SR_CONF_CONN | SR_CONF_GET,
};

static const uint64_t samplerates[] = {
	SR_MHZ(1),
	SR_MHZ(2),
	SR_MHZ(4),
	SR_MHZ(5),
	SR_MHZ(10),
	SR_MHZ(20),
	SR_MHZ(25),
	SR_MHZ(40),
	SR_MHZ(50),
	SR_MHZ(100),
	SR_MHZ(200),
	SR_MHZ(250),
	SR_MHZ(500),
};

/* Stream mode is limited to 20 MHz (16ch) on the DL16 Plus. */
static const uint64_t samplerates_stream[] = {
	SR_MHZ(1),
	SR_MHZ(2),
	SR_MHZ(4),
	SR_MHZ(5),
	SR_MHZ(10),
	SR_MHZ(20),
};

static const int32_t trigger_matches[] = {
	SR_TRIGGER_ZERO,
	SR_TRIGGER_ONE,
	SR_TRIGGER_RISING,
	SR_TRIGGER_FALLING,
	SR_TRIGGER_EDGE,
};

static const char *channel_names[] = {
	"D0", "D1", "D2", "D3", "D4", "D5", "D6", "D7",
	"D8", "D9", "D10", "D11", "D12", "D13", "D14", "D15",
};

static gboolean is_plausible(const struct libusb_device_descriptor *des)
{
	return des->idVendor == DL16_VID && des->idProduct == DL16_PID;
}

static GSList *scan(struct sr_dev_driver *di, GSList *options)
{
	struct drv_context *drvc;
	struct dev_context *devc;
	struct sr_dev_inst *sdi;
	struct sr_channel *ch;
	struct sr_channel_group *cg;
	struct sr_config *src;
	GSList *l, *devices, *conn_devices;
	libusb_device **devlist;
	struct libusb_device_descriptor des;
	const char *conn;
	char connection_id[64];
	int i, j;

	drvc = di->context;

	conn = NULL;
	for (l = options; l; l = l->next) {
		src = l->data;
		switch (src->key) {
		case SR_CONF_CONN:
			conn = g_variant_get_string(src->data, NULL);
			break;
		}
	}
	if (conn)
		conn_devices = sr_usb_find(drvc->sr_ctx->libusb_ctx, conn);
	else
		conn_devices = NULL;

	devices = NULL;
	libusb_get_device_list(drvc->sr_ctx->libusb_ctx, &devlist);
	for (i = 0; devlist[i]; i++) {
		if (conn) {
			struct sr_usb_dev_inst *usb;
			for (l = conn_devices; l; l = l->next) {
				usb = l->data;
				if (usb->bus == libusb_get_bus_number(devlist[i])
					&& usb->address == libusb_get_device_address(devlist[i]))
					break;
			}
			if (!l)
				continue;
		}

		libusb_get_device_descriptor(devlist[i], &des);
		if (!is_plausible(&des))
			continue;

		if (usb_get_port_path(devlist[i], connection_id,
				sizeof(connection_id)) < 0)
			continue;

		sdi = g_malloc0(sizeof(struct sr_dev_inst));
		sdi->status = SR_ST_INACTIVE;
		sdi->inst_type = SR_INST_USB;
		sdi->vendor = g_strdup("ALIENTEK");
		sdi->model = g_strdup("DL16");
		sdi->connection_id = g_strdup(connection_id);
		sdi->conn = sr_usb_dev_inst_new(
			libusb_get_bus_number(devlist[i]),
			libusb_get_device_address(devlist[i]), NULL);

		devc = dl16_dev_new();
		devc->samplerates = samplerates;
		devc->num_samplerates = ARRAY_SIZE(samplerates);
		sdi->priv = devc;
		devices = g_slist_append(devices, sdi);

		cg = sr_channel_group_new(sdi, "Logic", NULL);
		for (j = 0; j < NUM_CHANNELS; j++) {
			ch = sr_channel_new(sdi, j, SR_CHANNEL_LOGIC, TRUE,
				channel_names[j]);
			cg->channels = g_slist_append(cg->channels, ch);
		}
	}
	libusb_free_device_list(devlist, 1);
	g_slist_free_full(conn_devices, (GDestroyNotify)sr_usb_dev_inst_free);

	return std_scan_complete(di, devices);
}

static void clear_helper(struct dev_context *devc)
{
	int i;

	if (devc->rxbuf) {
		g_byte_array_free(devc->rxbuf, TRUE);
		devc->rxbuf = NULL;
	}
	for (i = 0; i < NUM_CHANNELS; i++) {
		if (devc->chanbuf[i]) {
			g_byte_array_free(devc->chanbuf[i], TRUE);
			devc->chanbuf[i] = NULL;
		}
	}
}

static int dev_clear(const struct sr_dev_driver *di)
{
	return std_dev_clear_with_callback(di,
		(std_dev_clear_callback)clear_helper);
}

static int dev_open(struct sr_dev_inst *sdi)
{
	struct drv_context *drvc = sdi->driver->context;
	struct sr_usb_dev_inst *usb = sdi->conn;
	struct dev_context *devc = sdi->priv;
	int ret;

	ret = sr_usb_open(drvc->sr_ctx->libusb_ctx, usb);
	if (ret != SR_OK)
		return ret;

	ret = dl16_dev_open(sdi);
	if (ret != SR_OK)
		return ret;

	if (devc->cur_samplerate == 0)
		devc->cur_samplerate = samplerates[0];

	return SR_OK;
}

static int dev_close(struct sr_dev_inst *sdi)
{
	struct sr_usb_dev_inst *usb = sdi->conn;

	if (!usb->devhdl)
		return SR_ERR_BUG;

	libusb_release_interface(usb->devhdl, USB_INTERFACE);
	libusb_close(usb->devhdl);
	usb->devhdl = NULL;

	return SR_OK;
}

static int config_get(uint32_t key, GVariant **data,
	const struct sr_dev_inst *sdi, const struct sr_channel_group *cg)
{
	struct dev_context *devc;
	struct sr_usb_dev_inst *usb;

	(void)cg;

	if (!sdi)
		return SR_ERR_ARG;

	devc = sdi->priv;

	switch (key) {
	case SR_CONF_CONN:
		if (!sdi->conn)
			return SR_ERR_ARG;
		usb = sdi->conn;
		*data = g_variant_new_printf("%d.%d", usb->bus, usb->address);
		break;
	case SR_CONF_LIMIT_SAMPLES:
		*data = g_variant_new_uint64(devc->limit_samples);
		break;
	case SR_CONF_LIMIT_FRAMES:
		*data = g_variant_new_uint64(devc->limit_frames);
		break;
	case SR_CONF_SAMPLERATE:
		*data = g_variant_new_uint64(devc->cur_samplerate);
		break;
	case SR_CONF_CAPTURE_RATIO:
		*data = g_variant_new_uint64(devc->capture_ratio);
		break;
	case SR_CONF_CONTINUOUS:
		*data = g_variant_new_boolean(devc->continuous);
		break;
	case SR_CONF_RLE:
		*data = g_variant_new_boolean(devc->rle);
		break;
	case SR_CONF_VOLTAGE_THRESHOLD:
		*data = g_variant_new_double(devc->threshold);
		break;
	default:
		return SR_ERR_NA;
	}

	return SR_OK;
}

static int config_set(uint32_t key, GVariant *data,
	const struct sr_dev_inst *sdi, const struct sr_channel_group *cg)
{
	struct dev_context *devc;
	int idx;

	(void)cg;

	if (!sdi)
		return SR_ERR_ARG;

	devc = sdi->priv;

	switch (key) {
	case SR_CONF_SAMPLERATE:
		if (devc->continuous) {
			if ((idx = std_u64_idx(data, samplerates_stream,
					ARRAY_SIZE(samplerates_stream))) < 0)
				return SR_ERR_ARG;
			devc->cur_samplerate = samplerates_stream[idx];
		} else {
			if ((idx = std_u64_idx(data, devc->samplerates,
					devc->num_samplerates)) < 0)
				return SR_ERR_ARG;
			devc->cur_samplerate = devc->samplerates[idx];
		}
		devc->rate_index = idx + 1;
		break;
	case SR_CONF_LIMIT_SAMPLES:
		devc->limit_samples = g_variant_get_uint64(data);
		break;
	case SR_CONF_LIMIT_FRAMES:
		devc->limit_frames = g_variant_get_uint64(data);
		break;
	case SR_CONF_CAPTURE_RATIO:
		devc->capture_ratio = g_variant_get_uint64(data);
		break;
	case SR_CONF_CONTINUOUS:
		devc->continuous = g_variant_get_boolean(data);
		break;
	case SR_CONF_RLE:
		devc->rle = g_variant_get_boolean(data);
		break;
	case SR_CONF_VOLTAGE_THRESHOLD:
		devc->threshold = g_variant_get_double(data);
		break;
	default:
		return SR_ERR_NA;
	}

	return SR_OK;
}

static int config_list(uint32_t key, GVariant **data,
	const struct sr_dev_inst *sdi, const struct sr_channel_group *cg)
{
	struct dev_context *devc;

	devc = (sdi) ? sdi->priv : NULL;

	switch (key) {
	case SR_CONF_SCAN_OPTIONS:
	case SR_CONF_DEVICE_OPTIONS:
		if (cg)
			return SR_ERR_NA;
		return STD_CONFIG_LIST(key, data, sdi, cg,
			scanopts, drvopts, devopts);
	case SR_CONF_SAMPLERATE:
		if (!devc)
			return SR_ERR_NA;
		if (devc->continuous)
			*data = std_gvar_samplerates(samplerates_stream,
				ARRAY_SIZE(samplerates_stream));
		else
			*data = std_gvar_samplerates(devc->samplerates,
				devc->num_samplerates);
		break;
	case SR_CONF_TRIGGER_MATCH:
		*data = std_gvar_array_i32(ARRAY_AND_SIZE(trigger_matches));
		break;
	case SR_CONF_VOLTAGE_THRESHOLD:
		*data = std_gvar_min_max_step_thresholds(-6.0, 6.0, 0.1);
		break;
	default:
		return SR_ERR_NA;
	}

	return SR_OK;
}

static int dev_acquisition_start(const struct sr_dev_inst *sdi)
{
	return dl16_start_acquisition(sdi);
}

static int dev_acquisition_stop(struct sr_dev_inst *sdi)
{
	dl16_stop_acquisition(sdi);

	return SR_OK;
}

static struct sr_dev_driver alientek_dl16_driver_info = {
	.name = "alientek-dl16",
	.longname = "ALIENTEK DL16",
	.api_version = 1,
	.init = std_init,
	.cleanup = std_cleanup,
	.scan = scan,
	.dev_list = std_dev_list,
	.dev_clear = dev_clear,
	.config_get = config_get,
	.config_set = config_set,
	.config_list = config_list,
	.dev_open = dev_open,
	.dev_close = dev_close,
	.dev_acquisition_start = dev_acquisition_start,
	.dev_acquisition_stop = dev_acquisition_stop,
	.context = NULL,
};
SR_REGISTER_DEV_DRIVER(alientek_dl16_driver_info);
