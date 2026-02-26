// SPDX-License-Identifier: GPL-2.0-only
/*
 * TurboSight TBS 5930 DVB-S/S2/S2X driver (dvb-usb-v2)
 *
 * Copyright (c) 2017 Davin <smailedavin@hotmail.com>
 */

#include "dvb_usb.h"
#include "m88rs6060.h"

#define TBS5930_FIRMWARE "dvb-usb-id5930.fw"

#define TBS5930_READ_MSG 0
#define TBS5930_WRITE_MSG 1

#define TBS5930_I2C_BUF_SIZE 64
#define TBS5930_I2C_MAX_LEN 60

/* Cypress FX2 firmware load request and CPU control registers */
#define FX2_REQ_FIRMWARE_LOAD	0xa0
#define FX2_ADDR_CPUCS		0xe600
#define FX2_ADDR_CPUCS_ALT	0x7f92
#define FX2_CHUNK_SIZE		0x40

/* TBS5930 USB vendor request codes */
#define TBS5930_REQ_I2C_WRITE	0x80
#define TBS5930_REQ_I2C_SETUP	0x90
#define TBS5930_REQ_I2C_READ	0x91
#define TBS5930_REQ_RESET	0xb7
#define TBS5930_REQ_CMD		0x8a

/* FX2 command IDs for TBS5930_REQ_CMD (byte[0] = cmd, byte[1] = value) */
#define TBS5930_CMD_TS_ENABLE	8

#define TBS5930_USB_TIMEOUT	2000

#define TBS5930_EEPROM_ADDR	0x50
#define TBS5930_EEPROM_MAC_OFF	16

DVB_DEFINE_MOD_OPT_ADAPTER_NR(adapter_nr);

struct tbs5930_state {
	struct i2c_client *i2c_client_demod;
};

static int tbs5930_op_rw(struct usb_device *dev, u8 request, u16 value,
			 u16 index, u8 *data, u16 len, int flags)
{
	int ret;
	void *u8buf;
	unsigned int pipe;
	u8 request_type;

	pipe = (flags == TBS5930_READ_MSG) ?
		usb_rcvctrlpipe(dev, 0) : usb_sndctrlpipe(dev, 0);
	request_type = (flags == TBS5930_READ_MSG) ? USB_DIR_IN : USB_DIR_OUT;

	u8buf = kmalloc(len, GFP_KERNEL);
	if (!u8buf)
		return -ENOMEM;

	if (flags == TBS5930_WRITE_MSG)
		memcpy(u8buf, data, len);

	ret = usb_control_msg(dev, pipe, request,
			      request_type | USB_TYPE_VENDOR,
			      value, index, u8buf, len,
			      TBS5930_USB_TIMEOUT);

	if (ret > 0 && flags == TBS5930_READ_MSG)
		memcpy(data, u8buf, ret);

	kfree(u8buf);
	return ret;
}

static int tbs5930_i2c_xfer(struct i2c_adapter *adap,
			    struct i2c_msg msg[], int num)
{
	struct dvb_usb_device *d = i2c_get_adapdata(adap);
	u8 buf[TBS5930_I2C_BUF_SIZE];
	int ret;

	if (!d)
		return -ENODEV;

	if (mutex_lock_interruptible(&d->i2c_mutex) < 0)
		return -EAGAIN;

	switch (num) {
	case 2:
		if (msg[0].len < 1 || msg[0].len > TBS5930_I2C_MAX_LEN ||
		    msg[1].len > TBS5930_I2C_MAX_LEN) {
			dev_err(&d->udev->dev,
				"i2c xfer: msg too long (%d/%d), max %d\n",
				msg[0].len, msg[1].len, TBS5930_I2C_MAX_LEN);
			ret = -EOPNOTSUPP;
			goto unlock;
		}

		buf[0] = msg[1].len;
		buf[1] = msg[0].addr << 1;
		buf[2] = msg[0].buf[0];

		ret = tbs5930_op_rw(d->udev, TBS5930_REQ_I2C_SETUP, 0, 0,
				    buf, 3, TBS5930_WRITE_MSG);
		if (ret < 0)
			goto unlock;

		ret = tbs5930_op_rw(d->udev, TBS5930_REQ_I2C_READ, 0, 0,
				    msg[1].buf, msg[1].len, TBS5930_READ_MSG);
		if (ret < 0)
			goto unlock;
		break;

	case 1:
		if (msg[0].len > TBS5930_I2C_MAX_LEN) {
			dev_err(&d->udev->dev,
				"i2c xfer: msg too long (%d), max %d\n",
				msg[0].len, TBS5930_I2C_MAX_LEN);
			ret = -EOPNOTSUPP;
			goto unlock;
		}

		buf[0] = msg[0].len + 1;
		buf[1] = msg[0].addr << 1;
		memcpy(&buf[2], msg[0].buf, msg[0].len);

		ret = tbs5930_op_rw(d->udev, TBS5930_REQ_I2C_WRITE, 0, 0,
				    buf, msg[0].len + 2, TBS5930_WRITE_MSG);
		if (ret < 0)
			goto unlock;
		break;

	default:
		dev_err(&d->udev->dev,
			"i2c xfer: unsupported msg count %d\n", num);
		ret = -EOPNOTSUPP;
		goto unlock;
	}

	ret = num;

unlock:
	mutex_unlock(&d->i2c_mutex);
	return ret;
}

static u32 tbs5930_i2c_func(struct i2c_adapter *adapter)
{
	return I2C_FUNC_I2C;
}

static struct i2c_algorithm tbs5930_i2c_algo = {
	.master_xfer = tbs5930_i2c_xfer,
	.functionality = tbs5930_i2c_func,
};

static int tbs5930_read_mac_addr(struct dvb_usb_adapter *adap, u8 mac[6])
{
	struct dvb_usb_device *d = adap_to_d(adap);
	u8 reg = TBS5930_EEPROM_MAC_OFF;
	struct i2c_msg msg[2] = {
		{
			.addr = TBS5930_EEPROM_ADDR,
			.flags = 0,
			.buf = &reg,
			.len = 1,
		}, {
			.addr = TBS5930_EEPROM_ADDR,
			.flags = I2C_M_RD,
			.buf = mac,
			.len = 6,
		}
	};

	if (i2c_transfer(&d->i2c_adap, msg, 2) != 2) {
		dev_err(&d->udev->dev, "eeprom MAC read failed\n");
		return -EIO;
	}

	return 0;
}

/* The FX2 firmware has no stream start/stop command — TS data flows
 * whenever the demod is outputting.  The 0x8a command dispatcher
 * provides a one-shot "TS output enable" (cmd 8), not a toggle.
 * Hence there is no streaming_ctrl callback.
 *
 * power_ctrl handles the FX2 peripheral reset (0xb7) and TS output
 * enable (0x8a cmd 8).  The framework calls this at probe, frontend
 * open, and reset-resume with refcounting so it only fires on 0→1
 * and 1→0 transitions.
 */
static int tbs5930_power_ctrl(struct dvb_usb_device *d, int onoff)
{
	u8 buf[2];

	if (!onoff)
		return 0;

	buf[0] = 0;
	buf[1] = 0;
	if (tbs5930_op_rw(d->udev, TBS5930_REQ_RESET, 0, 0,
			  buf, 2, TBS5930_WRITE_MSG) < 0)
		dev_warn(&d->udev->dev, "peripheral reset failed\n");

	buf[0] = TBS5930_CMD_TS_ENABLE;
	buf[1] = 1;
	if (tbs5930_op_rw(d->udev, TBS5930_REQ_CMD, 0, 0,
			  buf, 2, TBS5930_WRITE_MSG) < 0)
		dev_warn(&d->udev->dev, "TS output enable failed\n");

	msleep(10);

	return 0;
}

static int tbs5930_frontend_attach(struct dvb_usb_adapter *adap)
{
	struct dvb_usb_device *d = adap_to_d(adap);
	struct tbs5930_state *st = d_to_priv(d);
	struct m88rs6060_cfg cfg = {};

	cfg.fe = &adap->fe[0];
	cfg.clk = 27000000;
	cfg.i2c_wr_max = 33;
	cfg.ts_mode = MtFeTsOutMode_Common;
	cfg.demod_adr = 0x69;
	cfg.tuner_adr = 0x2c;
	cfg.repeater_value = 0x11;

	st->i2c_client_demod = dvb_module_probe("m88rs6060", NULL,
						&d->i2c_adap, 0x69, &cfg);
	if (!st->i2c_client_demod)
		return -ENODEV;

	strscpy(adap->fe[0]->ops.info.name, d->name,
		sizeof(adap->fe[0]->ops.info.name));

	return 0;
}

static int tbs5930_frontend_detach(struct dvb_usb_adapter *adap)
{
	struct dvb_usb_device *d = adap_to_d(adap);
	struct tbs5930_state *st = d_to_priv(d);

	dev_dbg(&d->udev->dev, "%s: adap=%d\n", __func__, adap->id);

	dvb_module_release(st->i2c_client_demod);

	return 0;
}

static int tbs5930_identify_state(struct dvb_usb_device *d, const char **name)
{
	/* After firmware upload the FX2 reconnects with string descriptors.
	 * Cold (no firmware): Mfr=0, Product=0
	 * Warm (firmware loaded): Manufacturer="TBS-Tech", Product="TBS 5930"
	 */
	if (d->udev->manufacturer)
		return WARM;

	*name = TBS5930_FIRMWARE;
	return COLD;
}

static int tbs5930_download_firmware(struct dvb_usb_device *d,
				     const struct firmware *fw)
{
	int ret;
	int i;
	u8 reset;

	dev_info(&d->udev->dev, "downloading TBS5930 firmware\n");

	/* Stop the FX2 CPU */
	reset = 1;
	ret = tbs5930_op_rw(d->udev, FX2_REQ_FIRMWARE_LOAD, FX2_ADDR_CPUCS_ALT, 0,
			    &reset, 1, TBS5930_WRITE_MSG);
	if (ret < 0)
		return ret;

	ret = tbs5930_op_rw(d->udev, FX2_REQ_FIRMWARE_LOAD, FX2_ADDR_CPUCS, 0,
			    &reset, 1, TBS5930_WRITE_MSG);
	if (ret < 0)
		return ret;

	/* Upload firmware in 64-byte chunks.
	 * tbs5930_op_rw already allocates a DMA-safe bounce buffer per
	 * transfer, so we can pass fw->data directly without copying.
	 */
	for (i = 0; i < fw->size; i += FX2_CHUNK_SIZE) {
		int len = min_t(int, FX2_CHUNK_SIZE, fw->size - i);

		if (tbs5930_op_rw(d->udev, FX2_REQ_FIRMWARE_LOAD, i, 0,
				  (u8 *)&fw->data[i], len,
				  TBS5930_WRITE_MSG) != len) {
			dev_err(&d->udev->dev,
				"error transferring firmware at offset %d\n", i);
			return -EINVAL;
		}
	}

	/* Restart the FX2 CPU */
	reset = 0;
	if (tbs5930_op_rw(d->udev, FX2_REQ_FIRMWARE_LOAD, FX2_ADDR_CPUCS_ALT, 0,
			  &reset, 1, TBS5930_WRITE_MSG) != 1) {
		dev_err(&d->udev->dev, "could not restart USB controller CPU\n");
		return -EINVAL;
	}
	if (tbs5930_op_rw(d->udev, FX2_REQ_FIRMWARE_LOAD, FX2_ADDR_CPUCS, 0,
			  &reset, 1, TBS5930_WRITE_MSG) != 1) {
		dev_err(&d->udev->dev, "could not restart USB controller CPU\n");
		return -EINVAL;
	}

	msleep(100);
	return RECONNECTS_USB;
}

static struct dvb_usb_device_properties tbs5930_props = {
	.driver_name = KBUILD_MODNAME,
	.owner = THIS_MODULE,
	.adapter_nr = adapter_nr,
	.size_of_priv = sizeof(struct tbs5930_state),

	.i2c_algo = &tbs5930_i2c_algo,
	.power_ctrl = tbs5930_power_ctrl,
	.frontend_attach = tbs5930_frontend_attach,
	.frontend_detach = tbs5930_frontend_detach,
	.identify_state = tbs5930_identify_state,
	.download_firmware = tbs5930_download_firmware,
	.read_mac_address = tbs5930_read_mac_addr,

	.num_adapters = 1,
	.adapter = {
		{ .stream = DVB_USB_STREAM_BULK(0x82, 8, 4096) }
	}
};

static const struct usb_device_id tbs5930_id_table[] = {
	{ DVB_USB_DEVICE(0x734c, 0x5930, &tbs5930_props,
		"TurboSight TBS 5930 DVB-S/S2/S2x", NULL) },
	{ }
};
MODULE_DEVICE_TABLE(usb, tbs5930_id_table);

static struct usb_driver tbs5930_usb_driver = {
	.name = KBUILD_MODNAME,
	.id_table = tbs5930_id_table,
	.probe = dvb_usbv2_probe,
	.disconnect = dvb_usbv2_disconnect,
	.suspend = dvb_usbv2_suspend,
	.resume = dvb_usbv2_resume,
	.reset_resume = dvb_usbv2_reset_resume,
	.no_dynamic_id = 1,
	.soft_unbind = 1,
};

module_usb_driver(tbs5930_usb_driver);

MODULE_AUTHOR("Davin zhang <Davin@tbsdtv.com>");
MODULE_DESCRIPTION("TurboSight TBS 5930 DVB-S/S2/S2X driver");
MODULE_FIRMWARE(TBS5930_FIRMWARE);
MODULE_LICENSE("GPL");
