/*
 * This file is part of the sigrok project.
 *
 * Copyright (C) 2010-2012 Bert Vermeulen <bert@biot.com>
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

#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <inttypes.h>
#include <glib.h>
#include <libusb.h>
#include "config.h"
#include "sigrok.h"
#include "sigrok-internal.h"
#include "saleae-logic.h"

static struct fx2_profile supported_fx2[] = {
	/* Saleae Logic */
	{ 0x0925, 0x3881, 0x0925, 0x3881, "Saleae", "Logic", NULL, 8 },
	/* default Cypress FX2 without EEPROM */
	{ 0x04b4, 0x8613, 0x0925, 0x3881, "Cypress", "FX2", NULL, 16 },
	{ 0, 0, 0, 0, 0, 0, 0, 0 }
};

static int hwcaps[] = {
	SR_HWCAP_LOGIC_ANALYZER,
	SR_HWCAP_SAMPLERATE,

	/* These are really implemented in the driver, not the hardware. */
	SR_HWCAP_LIMIT_SAMPLES,
	SR_HWCAP_CONTINUOUS,
	0,
};

/*
 * Probes are numbered 1-8.
 *
 * TODO: FX2 eval boards with the standard Cypress VID/PID can have 16 pins
 * or probes in theory, which is not supported by the Saleae Logic firmware.
 */
static const char *probe_names[] = {
	"0",
	"1",
	"2",
	"3",
	"4",
	"5",
	"6",
	"7",
	"8",
	"9",
	"10",
	"11",
	"12",
	"13",
	"14",
	"15",
	NULL,
};

static uint64_t supported_samplerates[] = {
	SR_KHZ(200),
	SR_KHZ(250),
	SR_KHZ(500),
	SR_MHZ(1),
	SR_MHZ(2),
	SR_MHZ(4),
	SR_MHZ(8),
	SR_MHZ(12),
	SR_MHZ(16),
	SR_MHZ(24),
	0,
};

static struct sr_samplerates samplerates = {
	SR_KHZ(200),
	SR_MHZ(24),
	SR_HZ(0),
	supported_samplerates,
};

/* List of struct sr_dev_inst, maintained by dev_open()/dev_close(). */
static GSList *dev_insts = NULL;
static libusb_context *usb_context = NULL;

static int new_saleae_logic_firmware = 0;

static int hw_dev_config_set(int dev_index, int hwcap, void *value);
static int hw_dev_acquisition_stop(int dev_index, gpointer session_dev_id);

/**
 * Check the USB configuration to determine if this is a Saleae Logic.
 *
 * @return 1 if the device's configuration profile match the Logic firmware's
 *         configuration, 0 otherwise.
 */
static int check_conf_profile(libusb_device *dev)
{
	struct libusb_device_descriptor des;
	struct libusb_config_descriptor *conf_dsc = NULL;
	const struct libusb_interface_descriptor *intf_dsc;
	int ret = -1;

	while (ret == -1) {
		/* Assume it's not a Saleae Logic unless proven wrong. */
		ret = 0;

		if (libusb_get_device_descriptor(dev, &des) != 0)
			break;

		if (des.bNumConfigurations != 1)
			/* Need exactly 1 configuration. */
			break;

		if (libusb_get_config_descriptor(dev, 0, &conf_dsc) != 0)
			break;

		if (conf_dsc->bNumInterfaces != 1)
			/* Need exactly 1 interface. */
			break;

		if (conf_dsc->interface[0].num_altsetting != 1)
			/* Need just one alternate setting. */
			break;

		intf_dsc = &(conf_dsc->interface[0].altsetting[0]);
		if (intf_dsc->bNumEndpoints == 4) {
			/* The new Saleae Logic firmware has 4 endpoints. */
			new_saleae_logic_firmware = 1;
		} else if (intf_dsc->bNumEndpoints == 2) {
			/* The old Saleae Logic firmware has 2 endpoints. */
			new_saleae_logic_firmware = 0;
		} else {
			/* Other number of endpoints -> not a Saleae Logic. */
			break;
		}

		if ((intf_dsc->endpoint[0].bEndpointAddress & 0x8f) !=
		    (1 | LIBUSB_ENDPOINT_OUT))
			/* The first endpoint should be 1 (outbound). */
			break;

		if ((intf_dsc->endpoint[1].bEndpointAddress & 0x8f) !=
		    (2 | LIBUSB_ENDPOINT_IN))
			/* The second endpoint should be 2 (inbound). */
			break;

		/* TODO: The new firmware has 4 endpoints... */

		/* If we made it here, it must be a Saleae Logic. */
		ret = 1;
	}

	if (conf_dsc)
		libusb_free_config_descriptor(conf_dsc);

	return ret;
}

static int sl_open_dev(int dev_index)
{
	libusb_device **devlist;
	struct libusb_device_descriptor des;
	struct sr_dev_inst *sdi;
	struct context *ctx;
	int err, skip, i;

	if (!(sdi = sr_dev_inst_get(dev_insts, dev_index)))
		return SR_ERR;
	ctx = sdi->priv;

	if (sdi->status == SR_ST_ACTIVE)
		/* already in use */
		return SR_ERR;

	skip = 0;
	libusb_get_device_list(usb_context, &devlist);
	for (i = 0; devlist[i]; i++) {
		if ((err = libusb_get_device_descriptor(devlist[i], &des))) {
			sr_err("logic: failed to get device descriptor: %d", err);
			continue;
		}

		if (des.idVendor != ctx->profile->fw_vid
		    || des.idProduct != ctx->profile->fw_pid)
			continue;

		if (sdi->status == SR_ST_INITIALIZING) {
			if (skip != dev_index) {
				/* Skip devices of this type that aren't the one we want. */
				skip += 1;
				continue;
			}
		} else if (sdi->status == SR_ST_INACTIVE) {
			/*
			 * This device is fully enumerated, so we need to find
			 * this device by vendor, product, bus and address.
			 */
			if (libusb_get_bus_number(devlist[i]) != ctx->usb->bus
				|| libusb_get_device_address(devlist[i]) != ctx->usb->address)
				/* this is not the one */
				continue;
		}

		if (!(err = libusb_open(devlist[i], &ctx->usb->devhdl))) {
			if (ctx->usb->address == 0xff)
				/*
				 * first time we touch this device after firmware upload,
				 * so we don't know the address yet.
				 */
				ctx->usb->address = libusb_get_device_address(devlist[i]);

			sdi->status = SR_ST_ACTIVE;
			sr_info("logic: opened device %d on %d.%d interface %d",
				sdi->index, ctx->usb->bus,
				ctx->usb->address, USB_INTERFACE);
		} else {
			sr_err("logic: failed to open device: %d", err);
		}

		/* if we made it here, we handled the device one way or another */
		break;
	}
	libusb_free_device_list(devlist, 1);

	if (sdi->status != SR_ST_ACTIVE)
		return SR_ERR;

	return SR_OK;
}

static void close_dev(struct sr_dev_inst *sdi)
{
	struct context *ctx;

	ctx = sdi->priv;

	if (ctx->usb->devhdl == NULL)
		return;

	sr_info("logic: closing device %d on %d.%d interface %d", sdi->index,
		ctx->usb->bus, ctx->usb->address, USB_INTERFACE);
	libusb_release_interface(ctx->usb->devhdl, USB_INTERFACE);
	libusb_close(ctx->usb->devhdl);
	ctx->usb->devhdl = NULL;
	sdi->status = SR_ST_INACTIVE;
}

static int configure_probes(struct context *ctx, GSList *probes)
{
	struct sr_probe *probe;
	GSList *l;
	int probe_bit, stage, i;
	char *tc;

	ctx->probe_mask = 0;
	for (i = 0; i < NUM_TRIGGER_STAGES; i++) {
		ctx->trigger_mask[i] = 0;
		ctx->trigger_value[i] = 0;
	}

	stage = -1;
	for (l = probes; l; l = l->next) {
		probe = (struct sr_probe *)l->data;
		if (probe->enabled == FALSE)
			continue;
		probe_bit = 1 << (probe->index - 1);
		ctx->probe_mask |= probe_bit;
		if (!(probe->trigger))
			continue;

		stage = 0;
		for (tc = probe->trigger; *tc; tc++) {
			ctx->trigger_mask[stage] |= probe_bit;
			if (*tc == '1')
				ctx->trigger_value[stage] |= probe_bit;
			stage++;
			if (stage > NUM_TRIGGER_STAGES)
				return SR_ERR;
		}
	}

	if (stage == -1)
		/*
		 * We didn't configure any triggers, make sure acquisition
		 * doesn't wait for any.
		 */
		ctx->trigger_stage = TRIGGER_FIRED;
	else
		ctx->trigger_stage = 0;

	return SR_OK;
}

static struct context *fx2_dev_new(void)
{
	struct context *ctx;

	if (!(ctx = g_try_malloc0(sizeof(struct context)))) {
		sr_err("logic: %s: ctx malloc failed", __func__);
		return NULL;
	}
	ctx->trigger_stage = TRIGGER_FIRED;
	ctx->usb = NULL;

	return ctx;
}


/*
 * API callbacks
 */

static int hw_init(const char *devinfo)
{
	struct sr_dev_inst *sdi;
	struct libusb_device_descriptor des;
	struct fx2_profile *fx2_prof;
	struct context *ctx;
	libusb_device **devlist;
	int err, devcnt, i, j;

	/* Avoid compiler warnings. */
	(void)devinfo;

	if (libusb_init(&usb_context) != 0) {
		sr_err("logic: Failed to initialize USB.");
		return 0;
	}

	/* Find all Saleae Logic devices and upload firmware to all of them. */
	devcnt = 0;
	libusb_get_device_list(usb_context, &devlist);
	for (i = 0; devlist[i]; i++) {
		fx2_prof = NULL;
		err = libusb_get_device_descriptor(devlist[i], &des);
		if (err != 0) {
			sr_err("logic: failed to get device descriptor: %d",
			       err);
			continue;
		}

		for (j = 0; supported_fx2[j].orig_vid; j++) {
			if (des.idVendor == supported_fx2[j].orig_vid
				&& des.idProduct == supported_fx2[j].orig_pid) {
				fx2_prof = &supported_fx2[j];
				break;
			}
		}
		if (!fx2_prof)
			/* not a supported VID/PID */
			continue;

		sdi = sr_dev_inst_new(devcnt, SR_ST_INITIALIZING,
			fx2_prof->vendor, fx2_prof->model, fx2_prof->model_version);
		if (!sdi)
			return 0;
		ctx = fx2_dev_new();
		ctx->profile = fx2_prof;
		sdi->priv = ctx;
		dev_insts = g_slist_append(dev_insts, sdi);

		if (check_conf_profile(devlist[i])) {
			/* Already has the firmware, so fix the new address. */
			sr_dbg("logic: Found a Saleae Logic with %s firmware.",
			       new_saleae_logic_firmware ? "new" : "old");
			sdi->status = SR_ST_INACTIVE;
			ctx->usb = sr_usb_dev_inst_new
			    (libusb_get_bus_number(devlist[i]),
			     libusb_get_device_address(devlist[i]), NULL);
		} else {
			if (ezusb_upload_firmware(devlist[i], USB_CONFIGURATION, FIRMWARE) == SR_OK)
				/* Remember when the firmware on this device was updated */
				g_get_current_time(&ctx->fw_updated);
			else
				sr_err("logic: firmware upload failed for "
				       "device %d", devcnt);
			ctx->usb = sr_usb_dev_inst_new
				(libusb_get_bus_number(devlist[i]), 0xff, NULL);
		}
		devcnt++;
	}
	libusb_free_device_list(devlist, 1);

	return devcnt;
}

static int hw_dev_open(int dev_index)
{
	GTimeVal cur_time;
	struct sr_dev_inst *sdi;
	struct context *ctx;
	int timediff, err;

	if (!(sdi = sr_dev_inst_get(dev_insts, dev_index)))
		return SR_ERR;
	ctx = sdi->priv;

	/*
	 * if the firmware was recently uploaded, wait up to MAX_RENUM_DELAY ms
	 * for the FX2 to renumerate
	 */
	err = 0;
	if (GTV_TO_MSEC(ctx->fw_updated) > 0) {
		sr_info("logic: waiting for device to reset");
		/* takes at least 300ms for the FX2 to be gone from the USB bus */
		g_usleep(300 * 1000);
		timediff = 0;
		while (timediff < MAX_RENUM_DELAY) {
			if ((err = sl_open_dev(dev_index)) == SR_OK)
				break;
			g_usleep(100 * 1000);
			g_get_current_time(&cur_time);
			timediff = GTV_TO_MSEC(cur_time) - GTV_TO_MSEC(ctx->fw_updated);
		}
		sr_info("logic: device came back after %d ms", timediff);
	} else {
		err = sl_open_dev(dev_index);
	}

	if (err != SR_OK) {
		sr_err("logic: unable to open device");
		return SR_ERR;
	}
	ctx = sdi->priv;

	err = libusb_claim_interface(ctx->usb->devhdl, USB_INTERFACE);
	if (err != 0) {
		sr_err("logic: Unable to claim interface: %d", err);
		return SR_ERR;
	}

	if (ctx->cur_samplerate == 0) {
		/* Samplerate hasn't been set; default to the slowest one. */
		if (hw_dev_config_set(dev_index, SR_HWCAP_SAMPLERATE,
		    &supported_samplerates[0]) == SR_ERR)
			return SR_ERR;
	}

	return SR_OK;
}

static int hw_dev_close(int dev_index)
{
	struct sr_dev_inst *sdi;

	if (!(sdi = sr_dev_inst_get(dev_insts, dev_index))) {
		sr_err("logic: %s: sdi was NULL", __func__);
		return SR_ERR; /* TODO: SR_ERR_ARG? */
	}

	/* TODO */
	close_dev(sdi);

	return SR_OK;
}

static int hw_cleanup(void)
{
	GSList *l;
	struct sr_dev_inst *sdi;
	struct context *ctx;
	int ret = SR_OK;

	/* Properly close and free all devices. */
	for (l = dev_insts; l; l = l->next) {
		if (!(sdi = l->data)) {
			/* Log error, but continue cleaning up the rest. */
			sr_err("logic: %s: sdi was NULL, continuing", __func__);
			ret = SR_ERR_BUG;
			continue;
		}
		if (!(ctx = sdi->priv)) {
			/* Log error, but continue cleaning up the rest. */
			sr_err("logic: %s: sdi->priv was NULL, continuing",
			       __func__);
			ret = SR_ERR_BUG;
			continue;
		}
		close_dev(sdi);
		sr_usb_dev_inst_free(ctx->usb);
		sr_dev_inst_free(sdi);
	}

	g_slist_free(dev_insts);
	dev_insts = NULL;

	if (usb_context)
		libusb_exit(usb_context);
	usb_context = NULL;

	return ret;
}

static void *hw_dev_info_get(int dev_index, int dev_info_id)
{
	struct sr_dev_inst *sdi;
	struct context *ctx;
	void *info = NULL;

	if (!(sdi = sr_dev_inst_get(dev_insts, dev_index)))
		return NULL;
	ctx = sdi->priv;

	switch (dev_info_id) {
	case SR_DI_INST:
		info = sdi;
		break;
	case SR_DI_NUM_PROBES:
		info = GINT_TO_POINTER(ctx->profile->num_probes);
		break;
	case SR_DI_PROBE_NAMES:
		info = probe_names;
		break;
	case SR_DI_SAMPLERATES:
		info = &samplerates;
		break;
	case SR_DI_TRIGGER_TYPES:
		info = TRIGGER_TYPES;
		break;
	case SR_DI_CUR_SAMPLERATE:
		info = &ctx->cur_samplerate;
		break;
	}

	return info;
}

static int hw_dev_status_get(int dev_index)
{
	struct sr_dev_inst *sdi;

	sdi = sr_dev_inst_get(dev_insts, dev_index);
	if (sdi)
		return sdi->status;
	else
		return SR_ST_NOT_FOUND;
}

static int *hw_hwcap_get_all(void)
{
	return hwcaps;
}

static uint8_t new_firmware_divider_value(uint64_t samplerate)
{
	switch (samplerate) {
	case SR_MHZ(24):
		return 0xe0;
		break;
	case SR_MHZ(16):
		return 0xd5;
		break;
	case SR_MHZ(12):
		return 0xe2;
		break;
	case SR_MHZ(8):
		return 0xd4;
		break;
	case SR_MHZ(4):
		return 0xda;
		break;
	case SR_MHZ(2):
		return 0xe6;
		break;
	case SR_MHZ(1):
		return 0x8e;
		break;
	case SR_KHZ(500):
		return 0xfe;
		break;
	case SR_KHZ(250):
		return 0x9e;
		break;
	case SR_KHZ(200):
		return 0x4e;
		break;
	}

	/* Shouldn't happen. */
	sr_err("logic: %s: Invalid samplerate %" PRIu64 "",
	       __func__, samplerate);
	return 0;
}

static int set_samplerate(struct sr_dev_inst *sdi, uint64_t samplerate)
{
	struct context *ctx;
	uint8_t divider;
	int ret, result, i;
	unsigned char buf[2];

	ctx = sdi->priv;
	for (i = 0; supported_samplerates[i]; i++) {
		if (supported_samplerates[i] == samplerate)
			break;
	}
	if (supported_samplerates[i] == 0)
		return SR_ERR_SAMPLERATE;

	if (new_saleae_logic_firmware)
		divider = new_firmware_divider_value(samplerate);
	else
		divider = (uint8_t) (48 / (samplerate / 1000000.0)) - 1;

	sr_info("logic: setting samplerate to %" PRIu64 " Hz (divider %d)",
		samplerate, divider);

	buf[0] = (new_saleae_logic_firmware) ? 0xd5 : 0x01;
	buf[1] = divider;
	ret = libusb_bulk_transfer(ctx->usb->devhdl, 1 | LIBUSB_ENDPOINT_OUT,
				   buf, 2, &result, 500);
	if (ret != 0) {
		sr_err("logic: failed to set samplerate: %d", ret);
		return SR_ERR;
	}
	ctx->cur_samplerate = samplerate;

	return SR_OK;
}

static int hw_dev_config_set(int dev_index, int hwcap, void *value)
{
	struct sr_dev_inst *sdi;
	struct context *ctx;
	int ret;

	if (!(sdi = sr_dev_inst_get(dev_insts, dev_index)))
		return SR_ERR;
	ctx = sdi->priv;

	if (hwcap == SR_HWCAP_SAMPLERATE) {
		ret = set_samplerate(sdi, *(uint64_t *)value);
	} else if (hwcap == SR_HWCAP_PROBECONFIG) {
		ret = configure_probes(ctx, (GSList *) value);
	} else if (hwcap == SR_HWCAP_LIMIT_SAMPLES) {
		ctx->limit_samples = *(uint64_t *)value;
		ret = SR_OK;
	} else {
		ret = SR_ERR;
	}

	return ret;
}

static int receive_data(int fd, int revents, void *user_data)
{
	struct timeval tv;

	/* Avoid compiler warnings. */
	(void)fd;
	(void)revents;
	(void)user_data;

	tv.tv_sec = tv.tv_usec = 0;
	libusb_handle_events_timeout(usb_context, &tv);

	return TRUE;
}

static void receive_transfer(struct libusb_transfer *transfer)
{
	/* TODO: These statics have to move to the ctx struct. */
	static int num_samples = 0;
	static int empty_transfer_count = 0;
	struct sr_datafeed_packet packet;
	struct sr_datafeed_logic logic;
	struct context *ctx;
	int cur_buflen, trigger_offset, i;
	unsigned char *cur_buf, *new_buf;

	/* hw_dev_acquisition_stop() is telling us to stop. */
	if (transfer == NULL)
		num_samples = -1;

	/*
	 * If acquisition has already ended, just free any queued up
	 * transfer that come in.
	 */
	if (num_samples == -1) {
		if (transfer)
			libusb_free_transfer(transfer);
		return;
	}

	sr_info("logic: receive_transfer(): status %d received %d bytes",
		transfer->status, transfer->actual_length);

	/* Save incoming transfer before reusing the transfer struct. */
	cur_buf = transfer->buffer;
	cur_buflen = transfer->actual_length;
	ctx = transfer->user_data;

	/* Fire off a new request. */
	if (!(new_buf = g_try_malloc(4096))) {
		sr_err("logic: %s: new_buf malloc failed", __func__);
		return; /* TODO: SR_ERR_MALLOC */
	}

	transfer->buffer = new_buf;
	transfer->length = 4096;
	if (libusb_submit_transfer(transfer) != 0) {
		/* TODO: Stop session? */
		/* TODO: Better error message. */
		sr_err("logic: %s: libusb_submit_transfer error", __func__);
	}

	if (cur_buflen == 0) {
		empty_transfer_count++;
		if (empty_transfer_count > MAX_EMPTY_TRANSFERS) {
			/*
			 * The FX2 gave up. End the acquisition, the frontend
			 * will work out that the samplecount is short.
			 */
			hw_dev_acquisition_stop(-1, ctx->session_data);
		}
		return;
	} else {
		empty_transfer_count = 0;
	}

	trigger_offset = 0;
	if (ctx->trigger_stage >= 0) {
		for (i = 0; i < cur_buflen; i++) {

			if ((cur_buf[i] & ctx->trigger_mask[ctx->trigger_stage]) == ctx->trigger_value[ctx->trigger_stage]) {
				/* Match on this trigger stage. */
				ctx->trigger_buffer[ctx->trigger_stage] = cur_buf[i];
				ctx->trigger_stage++;

				if (ctx->trigger_stage == NUM_TRIGGER_STAGES || ctx->trigger_mask[ctx->trigger_stage] == 0) {
					/* Match on all trigger stages, we're done. */
					trigger_offset = i + 1;

					/*
					 * TODO: Send pre-trigger buffer to session bus.
					 * Tell the frontend we hit the trigger here.
					 */
					packet.type = SR_DF_TRIGGER;
					packet.payload = NULL;
					sr_session_send(ctx->session_data, &packet);

					/*
					 * Send the samples that triggered it, since we're
					 * skipping past them.
					 */
					packet.type = SR_DF_LOGIC;
					packet.payload = &logic;
					logic.length = ctx->trigger_stage;
					logic.unitsize = 1;
					logic.data = ctx->trigger_buffer;
					sr_session_send(ctx->session_data, &packet);

					ctx->trigger_stage = TRIGGER_FIRED;
					break;
				}
				return;
			}

			/*
			 * We had a match before, but not in the next sample. However, we may
			 * have a match on this stage in the next bit -- trigger on 0001 will
			 * fail on seeing 00001, so we need to go back to stage 0 -- but at
			 * the next sample from the one that matched originally, which the
			 * counter increment at the end of the loop takes care of.
			 */
			if (ctx->trigger_stage > 0) {
				i -= ctx->trigger_stage;
				if (i < -1)
					i = -1; /* Oops, went back past this buffer. */
				/* Reset trigger stage. */
				ctx->trigger_stage = 0;
			}
		}
	}

	if (ctx->trigger_stage == TRIGGER_FIRED) {
		/* Send the incoming transfer to the session bus. */
		packet.type = SR_DF_LOGIC;
		packet.payload = &logic;
		logic.length = cur_buflen - trigger_offset;
		logic.unitsize = 1;
		logic.data = cur_buf + trigger_offset;
		sr_session_send(ctx->session_data, &packet);
		g_free(cur_buf);

		num_samples += cur_buflen;
		if (ctx->limit_samples && (unsigned int) num_samples > ctx->limit_samples) {
			hw_dev_acquisition_stop(-1, ctx->session_data);
		}
	} else {
		/*
		 * TODO: Buffer pre-trigger data in capture
		 * ratio-sized buffer.
		 */
	}
}

static int hw_dev_acquisition_start(int dev_index, gpointer session_data)
{
	struct sr_dev_inst *sdi;
	struct sr_datafeed_packet *packet;
	struct sr_datafeed_header *header;
	struct context *ctx;
	struct libusb_transfer *transfer;
	const struct libusb_pollfd **lupfd;
	int size, i;
	unsigned char *buf;

	if (!(sdi = sr_dev_inst_get(dev_insts, dev_index)))
		return SR_ERR;
	ctx = sdi->priv;
	ctx->session_data = session_data;

	if (!(packet = g_try_malloc(sizeof(struct sr_datafeed_packet)))) {
		sr_err("logic: %s: packet malloc failed", __func__);
		return SR_ERR_MALLOC;
	}

	if (!(header = g_try_malloc(sizeof(struct sr_datafeed_header)))) {
		sr_err("logic: %s: header malloc failed", __func__);
		return SR_ERR_MALLOC;
	}

	/* Start with 2K transfer, subsequently increased to 4K. */
	size = 2048;
	for (i = 0; i < NUM_SIMUL_TRANSFERS; i++) {
		if (!(buf = g_try_malloc(size))) {
			sr_err("logic: %s: buf malloc failed", __func__);
			return SR_ERR_MALLOC;
		}
		transfer = libusb_alloc_transfer(0);
		libusb_fill_bulk_transfer(transfer, ctx->usb->devhdl,
				2 | LIBUSB_ENDPOINT_IN, buf, size,
				receive_transfer, ctx, 40);
		if (libusb_submit_transfer(transfer) != 0) {
			/* TODO: Free them all. */
			libusb_free_transfer(transfer);
			g_free(buf);
			return SR_ERR;
		}
		size = 4096;
	}

	lupfd = libusb_get_pollfds(usb_context);
	for (i = 0; lupfd[i]; i++)
		sr_source_add(lupfd[i]->fd, lupfd[i]->events, 40, receive_data,
			      NULL);
	free(lupfd); /* NOT g_free()! */

	packet->type = SR_DF_HEADER;
	packet->payload = header;
	header->feed_version = 1;
	gettimeofday(&header->starttime, NULL);
	header->samplerate = ctx->cur_samplerate;
	header->num_logic_probes = ctx->profile->num_probes;
	sr_session_send(session_data, packet);
	g_free(header);
	g_free(packet);

	return SR_OK;
}

/* This stops acquisition on ALL devices, ignoring dev_index. */
static int hw_dev_acquisition_stop(int dev_index, gpointer session_data)
{
	struct sr_datafeed_packet packet;

	/* Avoid compiler warnings. */
	(void)dev_index;

	packet.type = SR_DF_END;
	sr_session_send(session_data, &packet);

	receive_transfer(NULL);

	/* TODO: Need to cancel and free any queued up transfers. */

	return SR_OK;
}

SR_PRIV struct sr_dev_driver saleae_logic_driver_info = {
	.name = "saleae-logic",
	.longname = "Saleae Logic",
	.api_version = 1,
	.init = hw_init,
	.cleanup = hw_cleanup,
	.dev_open = hw_dev_open,
	.dev_close = hw_dev_close,
	.dev_info_get = hw_dev_info_get,
	.dev_status_get = hw_dev_status_get,
	.hwcap_get_all = hw_hwcap_get_all,
	.dev_config_set = hw_dev_config_set,
	.dev_acquisition_start = hw_dev_acquisition_start,
	.dev_acquisition_stop = hw_dev_acquisition_stop,
};
