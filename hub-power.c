/*
 *    hub-power
 *    Copyright (C) 2012 Grigori Inozemtsev <greg@nzmsv.com>
 *
 *    Portions of this program are based on
 *    hub-ctrl by Niibe Yutaka <gniibe@fsij.org>
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation, either version 3 of the License, or
 *    (at your option) any later version.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *
 *    You should have received a copy of the GNU General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <stdint.h>
#include <unistd.h>

#include <libusb.h>

#define CTRL_TIMEOUT 1000

#define USB_RT_HUB			(LIBUSB_REQUEST_TYPE_CLASS | LIBUSB_RECIPIENT_DEVICE)
#define USB_RT_PORT			(LIBUSB_REQUEST_TYPE_CLASS | LIBUSB_RECIPIENT_OTHER)
#define USB_PORT_FEAT_POWER		8

struct hub_descriptor {
	uint8_t bDescLength;
	uint8_t bDescriptorType;
	uint8_t bNbrPorts;
	uint16_t wHubCharacteristics;
	uint8_t bPwrOn2PwrGood;
	uint8_t bHubContrCurrent;
} __attribute__ ((__packed__)) ;


static void print_usage()
{
	printf("Usage: hub-power [-h] [-b BUS_NUM] [-d DEV_NUM] [-v VENDOR_ID] [-p PRODUCT_ID] PORT_NUM { 0 | 1 }\n");
}

int main(int argc, char **argv)
{
	uint8_t match_bus = 0;
	uint8_t match_dev = 0;
	uint16_t match_vid = 0;
	uint16_t match_pid = 0;

	{
		char c;
		int val;
		while ((c = getopt(argc, argv, "b:d:v:p:h")) != -1) {
			val = 0;
			switch (c) {
			case 'b':
				sscanf(optarg, "%d", &val);
				match_bus = val;
				break;
			case 'd':
				sscanf(optarg, "%d", &val);
				match_dev = val;
				break;
			case 'v':
				if (sscanf(optarg, "0x%x", &val) != 1)
					sscanf(optarg, "%x", &val);
				match_vid = val;
				break;
			case 'p':
				if (sscanf(optarg, "0x%x", &val) != 1)
				    sscanf(optarg, "%x", &val);
				match_pid = val;
				break;
			case 'h':
				print_usage();
				return 1;
			default:
				print_usage();
				return 1;
			}
		}
	}

	if (optind != argc - 2) {
		print_usage();
		return 1;
	}

	uint16_t port;
	{
		int val = 0;
		sscanf(argv[optind], "%d", &val);
		port = val;
	}

	enum libusb_standard_request req;
	switch (argv[optind+1][0]) {
	case '0':
		req = LIBUSB_REQUEST_CLEAR_FEATURE;
		break;
	case '1':
		req = LIBUSB_REQUEST_SET_FEATURE;
		break;
	default:
		print_usage();
		return 1;
	}


	struct libusb_context *ctx;
	if (libusb_init(&ctx)) {
		fprintf(stderr, "libusb init failed\n");
		return -1;
	}

	libusb_device_handle *handle = 0;
	{
		libusb_device **list;
		const ssize_t ndev = libusb_get_device_list(ctx, &list);
		if (ndev == LIBUSB_ERROR_NO_MEM) goto fn_fail;

		ssize_t i;
		for (i = 0; i < ndev; i++) {
			libusb_device * const dev = list[i];

			struct libusb_device_descriptor desc;
			if (libusb_get_device_descriptor(dev, &desc))
				break;

			const unsigned short dev_vid = libusb_le16_to_cpu(desc.idVendor);
			const unsigned short dev_pid = libusb_le16_to_cpu(desc.idProduct);

			int found = (desc.bDeviceClass == LIBUSB_CLASS_HUB);
			if (match_bus)
				found &= (match_bus == libusb_get_bus_number(dev));
			if (match_dev)
				found &= (match_dev == libusb_get_device_address(dev));
			if (match_vid)
				found &= (match_vid == dev_vid);
			if (match_pid)
				found &= (match_pid == dev_pid);

			if (found) {
				if (libusb_open(dev, &handle)) {
					fprintf(stderr, "could not open device\n");
					handle = 0;
				}
				break;
			}
		}

		libusb_free_device_list(list, 1);
	}

	if (handle) {
		{
			struct libusb_config_descriptor *config;
			if (libusb_get_active_config_descriptor(libusb_get_device(handle), &config))
				goto fn_fail;

			if (config->bNumInterfaces != 1) {
				fprintf(stderr, "multiple interfaces found\n");
				libusb_free_config_descriptor(config);
				goto fn_exit;
			}
			libusb_free_config_descriptor(config);
		}

		{
			struct hub_descriptor hub_desc;
			if (libusb_control_transfer(handle,
			                            LIBUSB_ENDPOINT_IN | USB_RT_HUB,
			                            LIBUSB_REQUEST_GET_DESCRIPTOR,
			                            LIBUSB_DT_HUB<<8, 0,
			                            (void*)&hub_desc,
			                            sizeof(hub_desc),
			                            CTRL_TIMEOUT) < LIBUSB_DT_HUB_NONVAR_SIZE)
				goto fn_fail;

			if ((port < 1) || (port > hub_desc.bNbrPorts)) {
				fprintf(stderr, "invalid port number\n");
				goto fn_exit;
			}


			switch (hub_desc.wHubCharacteristics & 0x3) {
			case 0x0:
			case 0x1:
				break;
			default:
				fprintf(stderr, "power switching not supported\n");
				goto fn_exit;
			}
		}

		if (libusb_control_transfer(handle,
		                            USB_RT_PORT,
		                            req,
		                            USB_PORT_FEAT_POWER,
		                            port,
		                            0, 0, CTRL_TIMEOUT) < 0)
			goto fn_fail;
	}

fn_exit:
	if (handle)
		libusb_close(handle);
	libusb_exit(ctx);
	return 0;

fn_fail:
	fprintf(stderr, "libusb error\n");
	if (handle)
		libusb_close(handle);
	libusb_exit(ctx);
	return 2;
}
