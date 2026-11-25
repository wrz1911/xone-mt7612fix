// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * XInput (Xbox 360) transport driver for xone.
 *
 * The GIP transport drivers only handle Microsoft's GIP protocol. This
 * transport adds support for controllers that emulate a wired Xbox 360
 * pad (XInput), such as the ShanWan Q34U (045e:028e). The packet format
 * and button/axis mapping follow the xpad driver's Xbox 360 support.
 */

#include <linux/bits.h>
#include <linux/unaligned.h>
#include <linux/input.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/usb.h>

#define XONE_XINPUT_VENDOR 0x045e
#define XONE_XINPUT_PRODUCT 0x028e

#define XONE_XINPUT_INTF_CLASS 0xff
#define XONE_XINPUT_INTF_SUBCLASS 0x5d
#define XONE_XINPUT_INTF_PROTOCOL 0x01

#define XONE_XINPUT_PKT_LEN 32
#define XONE_XINPUT_MIN_LEN 14

struct xone_xinput {
	struct usb_device *udev;
	struct usb_interface *intf;
	struct input_dev *input;

	struct usb_endpoint_descriptor *ep_in;
	struct usb_endpoint_descriptor *ep_out;

	struct urb *urb_in;
	void *data_in;
	dma_addr_t data_in_dma;

	u8 last[XONE_XINPUT_MIN_LEN];
	bool has_last;
};

static void xone_xinput_process_packet(struct xone_xinput *xinput,
				       const u8 *data)
{
	struct input_dev *dev = xinput->input;

	/* valid pad data */
	if (data[0] != 0x00)
		return;

	/* digital pad */
	input_report_abs(dev, ABS_HAT0X,
			 !!(data[2] & 0x08) - !!(data[2] & 0x04));
	input_report_abs(dev, ABS_HAT0Y,
			 !!(data[2] & 0x02) - !!(data[2] & 0x01));

	/* start/back, stick press */
	input_report_key(dev, BTN_START, data[2] & BIT(4));
	input_report_key(dev, BTN_SELECT, data[2] & BIT(5));
	input_report_key(dev, BTN_THUMBL, data[2] & BIT(6));
	input_report_key(dev, BTN_THUMBR, data[2] & BIT(7));

	/* A/B/X/Y, bumpers and guide */
	input_report_key(dev, BTN_A, data[3] & BIT(4));
	input_report_key(dev, BTN_B, data[3] & BIT(5));
	input_report_key(dev, BTN_X, data[3] & BIT(6));
	input_report_key(dev, BTN_Y, data[3] & BIT(7));
	input_report_key(dev, BTN_TL, data[3] & BIT(0));
	input_report_key(dev, BTN_TR, data[3] & BIT(1));
	input_report_key(dev, BTN_MODE, data[3] & BIT(2));

	/* left stick */
	input_report_abs(dev, ABS_X,
			 (__s16)get_unaligned_le16(data + 6));
	input_report_abs(dev, ABS_Y,
			 ~(__s16)get_unaligned_le16(data + 8));

	/* right stick */
	input_report_abs(dev, ABS_RX,
			 (__s16)get_unaligned_le16(data + 10));
	input_report_abs(dev, ABS_RY,
			 ~(__s16)get_unaligned_le16(data + 12));

	/* triggers */
	input_report_abs(dev, ABS_Z, data[4]);
	input_report_abs(dev, ABS_RZ, data[5]);

	input_sync(dev);
}

static void xone_xinput_complete_in(struct urb *urb)
{
	struct xone_xinput *xinput = urb->context;
	struct device *dev = &xinput->intf->dev;
	int err;

	switch (urb->status) {
	case 0:
		break;
	case -ENOENT:
	case -ECONNRESET:
	case -ESHUTDOWN:
		return;
	default:
		goto resubmit;
	}

	if (urb->actual_length >= XONE_XINPUT_MIN_LEN) {
		const u8 *data = urb->transfer_buffer;

		if (!xinput->has_last ||
		    memcmp(xinput->last, data, XONE_XINPUT_MIN_LEN) != 0) {
			memcpy(xinput->last, data, XONE_XINPUT_MIN_LEN);
			xinput->has_last = true;
			xone_xinput_process_packet(xinput, data);
		}
	}

resubmit:
	err = usb_submit_urb(urb, GFP_ATOMIC);
	if (err)
		dev_dbg(dev, "%s: submit failed: %d\n", __func__, err);
}

static int xone_xinput_play_effect(struct input_dev *dev, void *data,
				   struct ff_effect *effect)
{
	struct xone_xinput *xinput = input_get_drvdata(dev);
	u8 packet[8] = { 0x00, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };

	if (effect->type != FF_RUMBLE)
		return 0;

	packet[3] = effect->u.rumble.strong_magnitude / 256;
	packet[4] = effect->u.rumble.weak_magnitude / 256;

	return usb_interrupt_msg(xinput->udev,
				 usb_sndintpipe(xinput->udev,
						xinput->ep_out->bEndpointAddress),
				 packet, sizeof(packet), NULL, 1000);
}

static void xone_xinput_init_input(struct xone_xinput *xinput)
{
	struct input_dev *dev = xinput->input;

	set_bit(EV_KEY, dev->evbit);
	set_bit(EV_ABS, dev->evbit);
	set_bit(EV_FF, dev->evbit);

	set_bit(BTN_A, dev->keybit);
	set_bit(BTN_B, dev->keybit);
	set_bit(BTN_X, dev->keybit);
	set_bit(BTN_Y, dev->keybit);
	set_bit(BTN_TL, dev->keybit);
	set_bit(BTN_TR, dev->keybit);
	set_bit(BTN_TL2, dev->keybit);
	set_bit(BTN_TR2, dev->keybit);
	set_bit(BTN_SELECT, dev->keybit);
	set_bit(BTN_START, dev->keybit);
	set_bit(BTN_MODE, dev->keybit);
	set_bit(BTN_THUMBL, dev->keybit);
	set_bit(BTN_THUMBR, dev->keybit);

	set_bit(ABS_X, dev->absbit);
	set_bit(ABS_Y, dev->absbit);
	set_bit(ABS_RX, dev->absbit);
	set_bit(ABS_RY, dev->absbit);
	set_bit(ABS_Z, dev->absbit);
	set_bit(ABS_RZ, dev->absbit);
	set_bit(ABS_HAT0X, dev->absbit);
	set_bit(ABS_HAT0Y, dev->absbit);

	input_set_abs_params(dev, ABS_X, -32768, 32767, 16, 128);
	input_set_abs_params(dev, ABS_Y, -32768, 32767, 16, 128);
	input_set_abs_params(dev, ABS_RX, -32768, 32767, 16, 128);
	input_set_abs_params(dev, ABS_RY, -32768, 32767, 16, 128);
	input_set_abs_params(dev, ABS_Z, 0, 255, 0, 0);
	input_set_abs_params(dev, ABS_RZ, 0, 255, 0, 0);
	input_set_abs_params(dev, ABS_HAT0X, -1, 1, 0, 0);
	input_set_abs_params(dev, ABS_HAT0Y, -1, 1, 0, 0);

	input_set_capability(dev, EV_FF, FF_RUMBLE);
}

static int xone_xinput_probe(struct usb_interface *intf,
			     const struct usb_device_id *id)
{
	struct usb_host_interface *iface = intf->cur_altsetting;
	struct usb_device *udev = interface_to_usbdev(intf);
	struct xone_xinput *xinput;
	struct input_dev *input;
	int err, i;

	dev_info(&intf->dev, "%s: class=%02x sub=%02x proto=%02x\n",
		 __func__, iface->desc.bInterfaceClass,
		 iface->desc.bInterfaceSubClass,
		 iface->desc.bInterfaceProtocol);

	if (iface->desc.bInterfaceClass != XONE_XINPUT_INTF_CLASS ||
	    iface->desc.bInterfaceSubClass != XONE_XINPUT_INTF_SUBCLASS ||
	    iface->desc.bInterfaceProtocol != XONE_XINPUT_INTF_PROTOCOL) {
		dev_err(&intf->dev, "%s: interface mismatch\n", __func__);
		return -ENODEV;
	}

	xinput = devm_kzalloc(&intf->dev, sizeof(*xinput), GFP_KERNEL);
	if (!xinput)
		return -ENOMEM;

	xinput->udev = udev;
	xinput->intf = intf;

	for (i = 0; i < iface->desc.bNumEndpoints; i++) {
		struct usb_endpoint_descriptor *ep = &iface->endpoint[i].desc;

		if (usb_endpoint_is_int_in(ep))
			xinput->ep_in = ep;
		else if (usb_endpoint_is_int_out(ep))
			xinput->ep_out = ep;
	}

	dev_info(&intf->dev, "%s: ep_in=%02x ep_out=%02x\n", __func__,
		 xinput->ep_in ? xinput->ep_in->bEndpointAddress : 0,
		 xinput->ep_out ? xinput->ep_out->bEndpointAddress : 0);

	if (!xinput->ep_in || !xinput->ep_out ||
	    xinput->ep_in->bEndpointAddress != 0x81 ||
	    xinput->ep_out->bEndpointAddress != 0x03) {
		dev_err(&intf->dev, "%s: unexpected endpoints\n", __func__);
		return -ENODEV;
	}

	input = devm_input_allocate_device(&intf->dev);
	if (!input)
		return -ENOMEM;

	xinput->input = input;
	input->name = "Xbox 360 Controller (ShanWan)";
	input->phys = "xone-xinput/input0";
	input->id.bustype = BUS_USB;
	input->id.vendor = XONE_XINPUT_VENDOR;
	input->id.product = XONE_XINPUT_PRODUCT;
	input_set_drvdata(input, xinput);

	xone_xinput_init_input(xinput);

	err = input_ff_create_memless(input, NULL, xone_xinput_play_effect);
	if (err)
		return err;

	xinput->urb_in = usb_alloc_urb(0, GFP_KERNEL);
	if (!xinput->urb_in)
		return -ENOMEM;

	xinput->data_in = usb_alloc_coherent(udev, XONE_XINPUT_PKT_LEN,
					     GFP_KERNEL, &xinput->data_in_dma);
	if (!xinput->data_in) {
		usb_free_urb(xinput->urb_in);
		return -ENOMEM;
	}

	usb_fill_int_urb(xinput->urb_in, udev,
			 usb_rcvintpipe(udev,
					xinput->ep_in->bEndpointAddress),
			 xinput->data_in, XONE_XINPUT_PKT_LEN,
			 xone_xinput_complete_in, xinput,
			 xinput->ep_in->bInterval);
	xinput->urb_in->transfer_dma = xinput->data_in_dma;
	xinput->urb_in->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;

	err = input_register_device(input);
	if (err)
		goto err_free_urb;

	usb_set_intfdata(intf, xinput);

	err = usb_submit_urb(xinput->urb_in, GFP_KERNEL);
	if (err) {
		input_unregister_device(input);
		usb_set_intfdata(intf, NULL);
		goto err_free_urb;
	}

	return 0;

err_free_urb:
	usb_free_coherent(udev, XONE_XINPUT_PKT_LEN, xinput->data_in,
			  xinput->data_in_dma);
	usb_free_urb(xinput->urb_in);
	return err;
}

static void xone_xinput_disconnect(struct usb_interface *intf)
{
	struct xone_xinput *xinput = usb_get_intfdata(intf);

	usb_set_intfdata(intf, NULL);
	if (!xinput)
		return;

	usb_kill_urb(xinput->urb_in);
	usb_free_coherent(xinput->udev, XONE_XINPUT_PKT_LEN,
			  xinput->data_in, xinput->data_in_dma);
	usb_free_urb(xinput->urb_in);
}

static const struct usb_device_id xone_xinput_ids[] = {
	{ USB_DEVICE(XONE_XINPUT_VENDOR, XONE_XINPUT_PRODUCT) },
	{ }
};
MODULE_DEVICE_TABLE(usb, xone_xinput_ids);

static struct usb_driver xone_xinput_driver = {
	.name = "xone-xinput",
	.id_table = xone_xinput_ids,
	.probe = xone_xinput_probe,
	.disconnect = xone_xinput_disconnect,
};

module_usb_driver(xone_xinput_driver);

MODULE_AUTHOR("xone contributors");
MODULE_DESCRIPTION("XInput (Xbox 360) transport driver for xone");
MODULE_LICENSE("GPL");
