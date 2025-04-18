/*    -*- linux-c -*-

GTCO digitizer USB driver

TO CHECK:  Is pressure done right on report 5?

Copyright (C) 2006  GTCO CalComp

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; version 2
of the License.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.

Permission to use, copy, modify, distribute, and sell this software and its
documentation for any purpose is hereby granted without fee, provided that
the above copyright notice appear in all copies and that both that
copyright notice and this permission notice appear in supporting
documentation, and that the name of GTCO-CalComp not be used in advertising
or publicity pertaining to distribution of the software without specific,
written prior permission. GTCO-CalComp makes no representations about the
suitability of this software for any purpose.  It is provided "as is"
without express or implied warranty.

GTCO-CALCOMP DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN NO
EVENT SHALL GTCO-CALCOMP BE LIABLE FOR ANY SPECIAL, INDIRECT OR
CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE,
DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
TORTIOUS ACTIONS, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
PERFORMANCE OF THIS SOFTWARE.

GTCO CalComp, Inc.
7125 Riverwood Drive
Columbia, MD 21046

Jeremy Roberson jroberson@gtcocalcomp.com
Scott Hill shill@gtcocalcomp.com
*/



/*#define DEBUG*/

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/input.h>
#include <linux/usb.h>
#include <linux/uaccess.h>
#include <linux/unaligned.h>
#include <asm/byteorder.h>
#include <linux/bitops.h>

#include <linux/usb/input.h>

/* Version with a Major number of 2 is for kernel inclusion only. */
#define  GTCO_VERSION   "2.00.0006"


/*   MACROS  */

#define VENDOR_ID_GTCO	      0x078C
#define PID_400               0x400
#define PID_401               0x401
#define PID_1000              0x1000
#define PID_1001              0x1001
#define PID_1002              0x1002

/* Max size of a single report */
#define REPORT_MAX_SIZE       10
#define MAX_COLLECTION_LEVELS  10


/* Bitmask whether pen is in range */
#define MASK_INRANGE 0x20
#define MASK_BUTTON  0x01F

#define  PATHLENGTH     64

/* DATA STRUCTURES */

/* Device table */
static const struct usb_device_id gtco_usbid_table[] = {
	{ USB_DEVICE(VENDOR_ID_GTCO, PID_400) },
	{ USB_DEVICE(VENDOR_ID_GTCO, PID_401) },
	{ USB_DEVICE(VENDOR_ID_GTCO, PID_1000) },
	{ USB_DEVICE(VENDOR_ID_GTCO, PID_1001) },
	{ USB_DEVICE(VENDOR_ID_GTCO, PID_1002) },
	{ }
};
MODULE_DEVICE_TABLE (usb, gtco_usbid_table);


/* Structure to hold all of our device specific stuff */
struct gtco {

	struct input_dev  *inputdevice; /* input device struct pointer  */
	struct usb_interface *intf;	/* the usb interface for this device */
	struct urb        *urbinfo;	 /* urb for incoming reports      */
	dma_addr_t        buf_dma;  /* dma addr of the data buffer*/
	unsigned char *   buffer;   /* databuffer for reports */

	char  usbpath[PATHLENGTH];
	int   openCount;

	/* Information pulled from Report Descriptor */
	u32  usage;
	u32  min_X;
	u32  max_X;
	u32  min_Y;
	u32  max_Y;
	s8   mintilt_X;
	s8   maxtilt_X;
	s8   mintilt_Y;
	s8   maxtilt_Y;
	u32  maxpressure;
	u32  minpressure;
};



/*   Code for parsing the HID REPORT DESCRIPTOR          */

/* From HID1.11 spec */
struct hid_descriptor
{
	struct usb_descriptor_header header;
	__le16   bcdHID;
	u8       bCountryCode;
	u8       bNumDescriptors;
	u8       bDescriptorType;
	__le16   wDescriptorLength;
} __attribute__ ((packed));


#define HID_DESCRIPTOR_SIZE   9
#define HID_DEVICE_TYPE       33
#define REPORT_DEVICE_TYPE    34


#define PREF_TAG(x)     ((x)>>4)
#define PREF_TYPE(x)    ((x>>2)&0x03)
#define PREF_SIZE(x)    ((x)&0x03)

#define TYPE_MAIN       0
#define TYPE_GLOBAL     1
#define TYPE_LOCAL      2
#define TYPE_RESERVED   3

#define TAG_MAIN_INPUT        0x8
#define TAG_MAIN_OUTPUT       0x9
#define TAG_MAIN_FEATURE      0xB
#define TAG_MAIN_COL_START    0xA
#define TAG_MAIN_COL_END      0xC

#define TAG_GLOB_USAGE        0
#define TAG_GLOB_LOG_MIN      1
#define TAG_GLOB_LOG_MAX      2
#define TAG_GLOB_PHYS_MIN     3
#define TAG_GLOB_PHYS_MAX     4
#define TAG_GLOB_UNIT_EXP     5
#define TAG_GLOB_UNIT         6
#define TAG_GLOB_REPORT_SZ    7
#define TAG_GLOB_REPORT_ID    8
#define TAG_GLOB_REPORT_CNT   9
#define TAG_GLOB_PUSH         10
#define TAG_GLOB_POP          11

#define TAG_GLOB_MAX          12

#define DIGITIZER_USAGE_TIP_PRESSURE   0x30
#define DIGITIZER_USAGE_TILT_X         0x3D
#define DIGITIZER_USAGE_TILT_Y         0x3E


/*
 *   This is an abbreviated parser for the HID Report Descriptor.  We
 *   know what devices we are talking to, so this is by no means meant
 *   to be generic.  We can make some safe assumptions:
 *
 *   - We know there are no LONG tags, all short
 *   - We know that we have no MAIN Feature and MAIN Output items
 *   - We know what the IRQ reports are supposed to look like.
 *
 *   The main purpose of this is to use the HID report desc to figure
 *   out the mins and maxs of the fields in the IRQ reports.  The IRQ
 *   reports for 400/401 change slightly if the max X is bigger than 64K.
 *
 */
static void parse_hid_report_descriptor(struct gtco *device, char * report,
					int length)
{
	struct device *ddev = &device->intf->dev;
	int   x, i = 0;

	/* Tag primitive vars */
	__u8   prefix;
	__u8   size;
	__u8   tag;
	__u8   type;
	__u8   data   = 0;
	__u16  data16 = 0;
	__u32  data32 = 0;

	/* For parsing logic */
	int   inputnum = 0;
	__u32 usage = 0;

	/* Global Values, indexed by TAG */
	__u32 globalval[TAG_GLOB_MAX];
	__u32 oldval[TAG_GLOB_MAX];

	/* Debug stuff */
	char  maintype = 'x';
	char  globtype[12];
	int   indent = 0;
	char  indentstr[MAX_COLLECTION_LEVELS + 1] = { 0 };

	dev_dbg(ddev, "======>>>>>>PARSE<<<<<<======\n");

	/* Walk  this report and pull out the info we need */
	while (i < length) {
		prefix = report[i++];

		/* Determine data size and save the data in the proper variable */
		size = (1U << PREF_SIZE(prefix)) >> 1;
		if (i + size > length) {
			dev_err(ddev,
				"Not enough data (need %d, have %d)\n",
				i + size, length);
			break;
		}

		switch (size) {
		case 1:
			data = report[i];
			break;
		case 2:
			data16 = get_unaligned_le16(&report[i]);
			break;
		case 4:
			data32 = get_unaligned_le32(&report[i]);
			break;
		}

		/* Skip size of data */
		i += size;

		/* What we do depends on the tag type */
		tag  = PREF_TAG(prefix);
		type = PREF_TYPE(prefix);
		switch (type) {
		case TYPE_MAIN:
			strcpy(globtype, "");
			switch (tag) {

			case TAG_MAIN_INPUT:
				/*
				 * The INPUT MAIN tag signifies this is
				 * information from a report.  We need to
				 * figure out what it is and store the
				 * min/max values
				 */

				maintype = 'I';
				if (data == 2)
					strcpy(globtype, "Variable");
				else if (data == 3)
					strcpy(globtype, "Var|Const");

				dev_dbg(ddev, "::::: Saving Report: %d input #%d Max: 0x%X(%d) Min:0x%X(%d) of %d bits\n",
					globalval[TAG_GLOB_REPORT_ID], inputnum,
					globalval[TAG_GLOB_LOG_MAX], globalval[TAG_GLOB_LOG_MAX],
					globalval[TAG_GLOB_LOG_MIN], globalval[TAG_GLOB_LOG_MIN],
					globalval[TAG_GLOB_REPORT_SZ] * globalval[TAG_GLOB_REPORT_CNT]);


				/*
				  We can assume that the first two input items
				  are always the X and Y coordinates.  After
				  that, we look for everything else by
				  local usage value
				 */
				switch (inputnum) {
				case 0:  /* X coord */
					dev_dbg(ddev, "GER: X Usage: 0x%x\n", usage);
					if (device->max_X == 0) {
						device->max_X = globalval[TAG_GLOB_LOG_MAX];
						device->min_X = globalval[TAG_GLOB_LOG_MIN];
					}
					break;

				case 1:  /* Y coord */
					dev_dbg(ddev, "GER: Y Usage: 0x%x\n", usage);
					if (device->max_Y == 0) {
						device->max_Y = globalval[TAG_GLOB_LOG_MAX];
						device->min_Y = globalval[TAG_GLOB_LOG_MIN];
					}
					break;

				default:
					/* Tilt X */
					if (usage == DIGITIZER_USAGE_TILT_X) {
						if (device->maxtilt_X == 0) {
							device->maxtilt_X = globalval[TAG_GLOB_LOG_MAX];
							device->mintilt_X = globalval[TAG_GLOB_LOG_MIN];
						}
					}

					/* Tilt Y */
					if (usage == DIGITIZER_USAGE_TILT_Y) {
						if (device->maxtilt_Y == 0) {
							device->maxtilt_Y = globalval[TAG_GLOB_LOG_MAX];
							device->mintilt_Y = globalval[TAG_GLOB_LOG_MIN];
						}
					}

					/* Pressure */
					if (usage == DIGITIZER_USAGE_TIP_PRESSURE) {
						if (device->maxpressure == 0) {
							device->maxpressure = globalval[TAG_GLOB_LOG_MAX];
							device->minpressure = globalval[TAG_GLOB_LOG_MIN];
						}
					}

					break;
				}

				inputnum++;
				break;

			case TAG_MAIN_OUTPUT:
				maintype = 'O';
				break;

			case TAG_MAIN_FEATURE:
				maintype = 'F';
				break;

			case TAG_MAIN_COL_START:
				maintype = 'S';

				if (indent == MAX_COLLECTION_LEVELS) {
					dev_err(ddev, "Collection level %d would exceed limit of %d\n",
						indent + 1,
						MAX_COLLECTION_LEVELS);
					break;
				}

				if (data == 0) {
					dev_dbg(ddev, "======>>>>>> Physical\n");
					strcpy(globtype, "Physical");
				} else
					dev_dbg(ddev, "======>>>>>>\n");

				/* Indent the debug output */
				indent++;
				for (x = 0; x < indent; x++)
					indentstr[x] = '-';
				indentstr[x] = 0;

				/* Save global tags */
				for (x = 0; x < TAG_GLOB_MAX; x++)
					oldval[x] = globalval[x];

				break;

			case TAG_MAIN_COL_END:
				maintype = 'E';

				if (indent == 0) {
					dev_err(ddev, "Collection level already at zero\n");
					break;
				}

				dev_dbg(ddev, "<<<<<<======\n");

				indent--;
				for (x = 0; x < indent; x++)
					indentstr[x] = '-';
				indentstr[x] = 0;

				/* Copy global tags back */
				for (x = 0; x < TAG_GLOB_MAX; x++)
					globalval[x] = oldval[x];

				break;
			}

			switch (size) {
			case 1:
				dev_dbg(ddev, "%sMAINTAG:(%d) %c SIZE: %d Data: %s 0x%x\n",
					indentstr, tag, maintype, size, globtype, data);
				break;

			case 2:
				dev_dbg(ddev, "%sMAINTAG:(%d) %c SIZE: %d Data: %s 0x%x\n",
					indentstr, tag, maintype, size, globtype, data16);
				break;

			case 4:
				dev_dbg(ddev, "%sMAINTAG:(%d) %c SIZE: %d Data: %s 0x%x\n",
					indentstr, tag, maintype, size, globtype, data32);
				break;
			}
			break;

		case TYPE_GLOBAL:
			switch (tag) {
			case TAG_GLOB_USAGE:
				/*
				 * First time we hit the global usage tag,
				 * it should tell us the type of device
				 */
				if (device->usage == 0)
					device->usage = data;

				strcpy(globtype, "USAGE");
				break;

			case TAG_GLOB_LOG_MIN:
				strcpy(globtype, "LOG_MIN");
				break;

			case TAG_GLOB_LOG_MAX:
				strcpy(globtype, "LOG_MAX");
				break;

			case TAG_GLOB_PHYS_MIN:
				strcpy(globtype, "PHYS_MIN");
				break;

			case TAG_GLOB_PHYS_MAX:
				strcpy(globtype, "PHYS_MAX");
				break;

			case TAG_GLOB_UNIT_EXP:
				strcpy(globtype, "EXP");
				break;

			case TAG_GLOB_UNIT:
				strcpy(globtype, "UNIT");
				break;

			case TAG_GLOB_REPORT_SZ:
				strcpy(globtype, "REPORT_SZ");
				break;

			case TAG_GLOB_REPORT_ID:
				strcpy(globtype, "REPORT_ID");
				/* New report, restart numbering */
				inputnum = 0;
				break;

			case TAG_GLOB_REPORT_CNT:
				strcpy(globtype, "REPORT_CNT");
				break;

			case TAG_GLOB_PUSH:
				strcpy(globtype, "PUSH");
				break;

			case TAG_GLOB_POP:
				strcpy(globtype, "POP");
				break;
			}

			/* Check to make sure we have a good tag number
			   so we don't overflow array */
			if (tag < TAG_GLOB_MAX) {
				switch (size) {
				case 1:
					dev_dbg(ddev, "%sGLOBALTAG:%s(%d) SIZE: %d Data: 0x%x\n",
						indentstr, globtype, tag, size, data);
					globalval[tag] = data;
					break;

				case 2:
					dev_dbg(ddev, "%sGLOBALTAG:%s(%d) SIZE: %d Data: 0x%x\n",
						indentstr, globtype, tag, size, data16);
					globalval[tag] = data16;
					break;

				case 4:
					dev_dbg(ddev, "%sGLOBALTAG:%s(%d) SIZE: %d Data: 0x%x\n",
						indentstr, globtype, tag, size, data32);
					globalval[tag] = data32;
					break;
				}
			} else {
				dev_dbg(ddev, "%sGLOBALTAG: ILLEGAL TAG:%d SIZE: %d\n",
					indentstr, tag, size);
			}
			break;

		case TYPE_LOCAL:
			switch (tag) {
			case TAG_GLOB_USAGE:
				strcpy(globtype, "USAGE");
				/* Always 1 byte */
				usage = data;
				break;

			case TAG_GLOB_LOG_MIN:
				strcpy(globtype, "MIN");
				break;

			case TAG_GLOB_LOG_MAX:
				strcpy(globtype, "MAX");
				break;

			default:
				strcpy(globtype, "UNKNOWN");
				break;
			}

			switch (size) {
			case 1:
				dev_dbg(ddev, "%sLOCALTAG:(%d) %s SIZE: %d Data: 0x%x\n",
					indentstr, tag, globtype, size, data);
				break;

			case 2:
				dev_dbg(ddev, "%sLOCALTAG:(%d) %s SIZE: %d Data: 0x%x\n",
					indentstr, tag, globtype, size, data16);
				break;

			case 4:
				dev_dbg(ddev, "%sLOCALTAG:(%d) %s SIZE: %d Data: 0x%x\n",
					indentstr, tag, globtype, size, data32);
				break;
			}

			break;
		}
	}
}

/*   INPUT DRIVER Routines                               */

/*
 * Called when opening the input device.  This will submit the URB to
 * the usb system so we start getting reports
 */
static int gtco_input_open(struct input_dev *inputdev)
{
	struct gtco *device = input_get_drvdata(inputdev);

	device->urbinfo->dev = interface_to_usbdev(device->intf);
	if (usb_submit_urb(device->urbinfo, GFP_KERNEL))
		return -EIO;

	return 0;
}

/*
 * Called when closing the input device.  This will unlink the URB
 */
static void gtco_input_close(struct input_dev *inputdev)
{
	struct gtco *device = input_get_drvdata(inputdev);

	usb_kill_urb(device->urbinfo);
}


/*
 *  Setup input device capabilities.  Tell the input system what this
 *  device is capable of generating.
 *
 *  This information is based on what is read from the HID report and
 *  placed in the struct gtco structure
 *
 */
static void gtco_setup_caps(struct input_dev *inputdev)
{
	struct gtco *device = input_get_drvdata(inputdev);

	/* Which events */
	inputdev->evbit[0] = BIT_MASK(EV_KEY) | BIT_MASK(EV_ABS) |
		BIT_MASK(EV_MSC);

	/* Misc event menu block */
	inputdev->mscbit[0] = BIT_MASK(MSC_SCAN) | BIT_MASK(MSC_SERIAL) |
		BIT_MASK(MSC_RAW);

	/* Absolute values based on HID report info */
	input_set_abs_params(inputdev, ABS_X, device->min_X, device->max_X,
			     0, 0);
	input_set_abs_params(inputdev, ABS_Y, device->min_Y, device->max_Y,
			     0, 0);

	/* Proximity */
	input_set_abs_params(inputdev, ABS_DISTANCE, 0, 1, 0, 0);

	/* Tilt & pressure */
	input_set_abs_params(inputdev, ABS_TILT_X, device->mintilt_X,
			     device->maxtilt_X, 0, 0);
	input_set_abs_params(inputdev, ABS_TILT_Y, device->mintilt_Y,
			     device->maxtilt_Y, 0, 0);
	input_set_abs_params(inputdev, ABS_PRESSURE, device->minpressure,
			     device->maxpressure, 0, 0);

	/* Transducer */
	input_set_abs_params(inputdev, ABS_MISC, 0, 0xFF, 0, 0);
}

/*   USB Routines  */

/*
 * URB callback routine.  Called when we get IRQ reports from the
 *  digitizer.
 *
 *  This bridges the USB and input device worlds.  It generates events
 *  on the input device based on the USB reports.
 */
static void gtco_urb_callback(struct urb *urbinfo)
{
	struct gtco *device = urbinfo->context;
	struct input_dev  *inputdev;
	int               rc;
	u32               val = 0;
	char              le_buffer[2];

	inputdev = device->inputdevice;

	/* Was callback OK? */
	if (urbinfo->status == -ECONNRESET ||
	    urbinfo->status == -ENOENT ||
	    urbinfo->status == -ESHUTDOWN) {

		/* Shutdown is occurring. Return and don't queue up any more */
		return;
	}

	if (urbinfo->status != 0) {
		/*
		 * Some unknown error.  Hopefully temporary. Just go and
		 * requeue an URB
		 */
		goto resubmit;
	}

	/*
	 * Good URB, now process
	 */

	/* PID dependent when we interpret the report */
	if (inputdev->id.product == PID_1000 ||
	    inputdev->id.product == PID_1001 ||
	    inputdev->id.product == PID_1002) {

		/*
		 * Switch on the report ID
		 * Conveniently, the reports have more information, the higher
		 * the report number.  We can just fall through the case
		 * statements if we start with the highest number report
		 */
		switch (device->buffer[0]) {
		case 5:
			/* Pressure is 9 bits */
			val = ((u16)(device->buffer[8]) << 1);
			val |= (u16)(device->buffer[7] >> 7);
			input_report_abs(inputdev, ABS_PRESSURE,
					 device->buffer[8]);

			/* Mask out the Y tilt value used for pressure */
			device->buffer[7] = (u8)((device->buffer[7]) & 0x7F);

			/* Fall thru */
		case 4:
			/* Tilt */
			input_report_abs(inputdev, ABS_TILT_X,
					 sign_extend32(device->buffer[6], 6));

			input_report_abs(inputdev, ABS_TILT_Y,
					 sign_extend32(device->buffer[7], 6));

			/* Fall thru */
		case 2:
		case 3:
			/* Convert buttons, only 5 bits possible */
			val = (device->buffer[5]) & MASK_BUTTON;

			/* We don't apply any meaning to the bitmask,
			   just report */
			input_event(inputdev, EV_MSC, MSC_SERIAL, val);

			/*  Fall thru */
		case 1:
			/* All reports have X and Y coords in the same place */
			val = get_unaligned_le16(&device->buffer[1]);
			input_report_abs(inputdev, ABS_X, val);

			val = get_unaligned_le16(&device->buffer[3]);
			input_report_abs(inputdev, ABS_Y, val);

			/* Ditto for proximity bit */
			val = device->buffer[5] & MASK_INRANGE ? 1 : 0;
			input_report_abs(inputdev, ABS_DISTANCE, val);

			/* Report 1 is an exception to how we handle buttons */
			/* Buttons are an index, not a bitmask */
			if (device->buffer[0] == 1) {

				/*
				 * Convert buttons, 5 bit index
				 * Report value of index set as one,
				 * the rest as 0
				 */
				val = device->buffer[5] & MASK_BUTTON;
				dev_dbg(&device->intf->dev,
					"======>>>>>>REPORT 1: val 0x%X(%d)\n",
					val, val);

				/*
				 * We don't apply any meaning to the button
				 * index, just report it
				 */
				input_event(inputdev, EV_MSC, MSC_SERIAL, val);
			}
			break;

		case 7:
			/* Menu blocks */
			input_event(inputdev, EV_MSC, MSC_SCAN,
				    device->buffer[1]);
			break;
		}
	}

	/* Other pid class */
	if (inputdev->id.product == PID_400 ||
	    inputdev->id.product == PID_401) {

		/* Report 2 */
		if (device->buffer[0] == 2) {
			/* Menu blocks */
			input_event(inputdev, EV_MSC, MSC_SCAN, device->buffer[1]);
		}

		/*  Report 1 */
		if (device->buffer[0] == 1) {
			char buttonbyte;

			/*  IF X max > 64K, we still a bit from the y report */
			if (device->max_X > 0x10000) {

				val = (u16)(((u16)(device->buffer[2] << 8)) | (u8)device->buffer[1]);
				val |= (u32)(((u8)device->buffer[3] & 0x1) << 16);

				input_report_abs(inputdev, ABS_X, val);

				le_buffer[0]  = (u8)((u8)(device->buffer[3]) >> 1);
				le_buffer[0] |= (u8)((device->buffer[3] & 0x1) << 7);

				le_buffer[1]  = (u8)(device->buffer[4] >> 1);
				le_buffer[1] |= (u8)((device->buffer[5] & 0x1) << 7);

				val = get_unaligned_le16(le_buffer);
				input_report_abs(inputdev, ABS_Y, val);

				/*
				 * Shift the button byte right by one to
				 * make it look like the standard report
				 */
				buttonbyte = device->buffer[5] >> 1;
			} else {

				val = get_unaligned_le16(&device->buffer[1]);
				input_report_abs(inputdev, ABS_X, val);

				val = get_unaligned_le16(&device->buffer[3]);
				input_report_abs(inputdev, ABS_Y, val);

				buttonbyte = device->buffer[5];
			}

			/* BUTTONS and PROXIMITY */
			val = buttonbyte & MASK_INRANGE ? 1 : 0;
			input_report_abs(inputdev, ABS_DISTANCE, val);

			/* Convert buttons, only 4 bits possible */
			val = buttonbyte & 0x0F;
#ifdef USE_BUTTONS
			for (i = 0; i < 5; i++)
				input_report_key(inputdev, BTN_DIGI + i, val & (1 << i));
#else
			/* We don't apply any meaning to the bitmask, just report */
			input_event(inputdev, EV_MSC, MSC_SERIAL, val);
#endif

			/* TRANSDUCER */
			input_report_abs(inputdev, ABS_MISC, device->buffer[6]);
		}
	}

	/* Everybody gets report ID's */
	input_event(inputdev, EV_MSC, MSC_RAW,  device->buffer[0]);

	/* Sync it up */
	input_sync(inputdev);

 resubmit:
	rc = usb_submit_urb(urbinfo, GFP_ATOMIC);
	if (rc != 0)
		dev_err(&device->intf->dev,
			"usb_submit_urb failed rc=0x%x\n", rc);
}

/*
 *  The probe routine.  This is called when the kernel find the matching USB
 *   vendor/product.  We do the following:
 *
 *    - Allocate mem for a local structure to manage the device
 *    - Request a HID Report Descriptor from the device and parse it to
 *      find out the device parameters
 *    - Create an input device and assign it attributes
 *   - Allocate an URB so the device can talk to us when the input
 *      queue is open
 */
static int gtco_probe(struct usb_interface *usbinterface,
		      const struct usb_device_id *id)
{

	struct gtco             *gtco;
	struct input_dev        *input_dev;
	struct hid_descriptor   *hid_desc;
	char                    *report;
	int                     result = 0, retry;
	int			error;
	struct usb_endpoint_descriptor *endpoint;
	struct usb_device	*udev = interface_to_usbdev(usbinterface);

	/* Allocate memory for device structure */
	gtco = kzalloc(sizeof(struct gtco), GFP_KERNEL);
	input_dev = input_allocate_device();
	if (!gtco || !input_dev) {
		dev_err(&usbinterface->dev, "No more memory\n");
		error = -ENOMEM;
		goto err_free_devs;
	}

	/* Set pointer to the input device */
	gtco->inputdevice = input_dev;

	/* Save interface information */
	gtco->intf = usbinterface;

	/* Allocate some data for incoming reports */
	gtco->buffer = usb_alloc_coherent(udev, REPORT_MAX_SIZE,
					  GFP_KERNEL, &gtco->buf_dma);
	if (!gtco->buffer) {
		dev_err(&usbinterface->dev, "No more memory for us buffers\n");
		error = -ENOMEM;
		goto err_free_devs;
	}

	/* Allocate URB for reports */
	gtco->urbinfo = usb_alloc_urb(0, GFP_KERNEL);
	if (!gtco->urbinfo) {
		dev_err(&usbinterface->dev, "Failed to allocate URB\n");
		error = -ENOMEM;
		goto err_free_buf;
	}

	/* Sanity check that a device has an endpoint */
	if (usbinterface->cur_altsetting->desc.bNumEndpoints < 1) {
		dev_err(&usbinterface->dev,
			"Invalid number of endpoints\n");
		error = -EINVAL;
		goto err_free_urb;
	}

	endpoint = &usbinterface->cur_altsetting->endpoint[0].desc;

	/* Some debug */
	dev_dbg(&usbinterface->dev, "gtco # interfaces: %d\n", usbinterface->num_altsetting);
	dev_dbg(&usbinterface->dev, "num endpoints:     %d\n", usbinterface->cur_altsetting->desc.bNumEndpoints);
	dev_dbg(&usbinterface->dev, "interface class:   %d\n", usbinterface->cur_altsetting->desc.bInterfaceClass);
	dev_dbg(&usbinterface->dev, "endpoint: attribute:0x%x type:0x%x\n", endpoint->bmAttributes, endpoint->bDescriptorType);
	if (usb_endpoint_xfer_int(endpoint))
		dev_dbg(&usbinterface->dev, "endpoint: we have interrupt endpoint\n");

	dev_dbg(&usbinterface->dev, "endpoint extra len:%d\n", usbinterface->altsetting[0].extralen);

	/*
	 * Find the HID descriptor so we can find out the size of the
	 * HID report descriptor
	 */
	if (usb_get_extra_descriptor(usbinterface->cur_altsetting,
				     HID_DEVICE_TYPE, &hid_desc) != 0) {
		dev_err(&usbinterface->dev,
			"Can't retrieve exta USB descriptor to get hid report descriptor length\n");
		error = -EIO;
		goto err_free_urb;
	}

	dev_dbg(&usbinterface->dev,
		"Extra descriptor success: type:%d  len:%d\n",
		hid_desc->bDescriptorType,  hid_desc->wDescriptorLength);

	report = kzalloc(le16_to_cpu(hid_desc->wDescriptorLength), GFP_KERNEL);
	if (!report) {
		dev_err(&usbinterface->dev, "No more memory for report\n");
		error = -ENOMEM;
		goto err_free_urb;
	}

	/* Couple of tries to get reply */
	for (retry = 0; retry < 3; retry++) {
		result = usb_control_msg(udev,
					 usb_rcvctrlpipe(udev, 0),
					 USB_REQ_GET_DESCRIPTOR,
					 USB_RECIP_INTERFACE | USB_DIR_IN,
					 REPORT_DEVICE_TYPE << 8,
					 0, /* interface */
					 report,
					 le16_to_cpu(hid_desc->wDescriptorLength),
					 5000); /* 5 secs */

		dev_dbg(&usbinterface->dev, "usb_control_msg result: %d\n", result);
		if (result == le16_to_cpu(hid_desc->wDescriptorLength)) {
			parse_hid_report_descriptor(gtco, report, result);
			break;
		}
	}

	kfree(report);

	/* If we didn't get the report, fail */
	if (result != le16_to_cpu(hid_desc->wDescriptorLength)) {
		dev_err(&usbinterface->dev,
			"Failed to get HID Report Descriptor of size: %d\n",
			hid_desc->wDescriptorLength);
		error = -EIO;
		goto err_free_urb;
	}

	/* Create a device file node */
	usb_make_path(udev, gtco->usbpath, sizeof(gtco->usbpath));
	strlcat(gtco->usbpath, "/input0", sizeof(gtco->usbpath));

	/* Set Input device functions */
	input_dev->open = gtco_input_open;
	input_dev->close = gtco_input_close;

	/* Set input device information */
	input_dev->name = "GTCO_CalComp";
	input_dev->phys = gtco->usbpath;

	input_set_drvdata(input_dev, gtco);

	/* Now set up all the input device capabilities */
	gtco_setup_caps(input_dev);

	/* Set input device required ID information */
	usb_to_input_id(udev, &input_dev->id);
	input_dev->dev.parent = &usbinterface->dev;

	/* Setup the URB, it will be posted later on open of input device */
	endpoint = &usbinterface->cur_altsetting->endpoint[0].desc;

	usb_fill_int_urb(gtco->urbinfo,
			 udev,
			 usb_rcvintpipe(udev,
					endpoint->bEndpointAddress),
			 gtco->buffer,
			 REPORT_MAX_SIZE,
			 gtco_urb_callback,
			 gtco,
			 endpoint->bInterval);

	gtco->urbinfo->transfer_dma = gtco->buf_dma;
	gtco->urbinfo->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;

	/* Save gtco pointer in USB interface gtco */
	usb_set_intfdata(usbinterface, gtco);

	/* All done, now register the input device */
	error = input_register_device(input_dev);
	if (error)
		goto err_free_urb;

	return 0;

 err_free_urb:
	usb_free_urb(gtco->urbinfo);
 err_free_buf:
	usb_free_coherent(udev, REPORT_MAX_SIZE,
			  gtco->buffer, gtco->buf_dma);
 err_free_devs:
	input_free_device(input_dev);
	kfree(gtco);
	return error;
}

/*
 *  This function is a standard USB function called when the USB device
 *  is disconnected.  We will get rid of the URV, de-register the input
 *  device, and free up allocated memory
 */
static void gtco_disconnect(struct usb_interface *interface)
{
	/* Grab private device ptr */
	struct gtco *gtco = usb_get_intfdata(interface);
	struct usb_device *udev = interface_to_usbdev(interface);

	/* Now reverse all the registration stuff */
	if (gtco) {
		input_unregister_device(gtco->inputdevice);
		usb_kill_urb(gtco->urbinfo);
		usb_free_urb(gtco->urbinfo);
		usb_free_coherent(udev, REPORT_MAX_SIZE,
				  gtco->buffer, gtco->buf_dma);
		kfree(gtco);
	}

	dev_info(&interface->dev, "gtco driver disconnected\n");
}

/*   STANDARD MODULE LOAD ROUTINES  */

static struct usb_driver gtco_driverinfo_table = {
	.name		= "gtco",
	.id_table	= gtco_usbid_table,
	.probe		= gtco_probe,
	.disconnect	= gtco_disconnect,
};

module_usb_driver(gtco_driverinfo_table);

MODULE_DESCRIPTION("GTCO digitizer USB driver");
MODULE_LICENSE("GPL");
