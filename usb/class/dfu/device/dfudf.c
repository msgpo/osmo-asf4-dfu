/**
 * \file
 *
 * \brief USB Device Stack DFU Function Implementation.
 *
 * Copyright (c) 2018 sysmocom -s.f.m.c. GmbH, Author: Kevin Redon <kredon@sysmocom.de>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include "dfudf.h"
#include "usb_protocol_dfu.h"
#include "dfudf_desc.h"

/** USB Device DFU Function Specific Data */
struct dfudf_func_data {
	/** DFU Interface information */
	uint8_t func_iface;
	/** DFU Enable Flag */
	bool enabled;
};

static struct usbdf_driver _dfudf;
static struct dfudf_func_data _dfudf_funcd;

/** USB DFU functional descriptor (with DFU attributes) */
static const uint8_t usb_dfu_func_desc_bytes[] = {DFUD_IFACE_DESCB};
static const usb_dfu_func_desc_t* usb_dfu_func_desc = (usb_dfu_func_desc_t*)&usb_dfu_func_desc_bytes;

enum usb_dfu_state dfu_state = USB_DFU_STATE_DFU_IDLE;
enum usb_dfu_status dfu_status = USB_DFU_STATUS_OK;

uint8_t dfu_download_data[512];
uint16_t dfu_download_length = 0;
size_t dfu_download_offset = 0;
bool dfu_manifestation_complete = false;

/**
 * \brief Enable DFU Function
 * \param[in] drv Pointer to USB device function driver
 * \param[in] desc Pointer to USB interface descriptor
 * \return Operation status.
 */
static int32_t dfudf_enable(struct usbdf_driver *drv, struct usbd_descriptors *desc)
{
	struct dfudf_func_data *func_data = (struct dfudf_func_data *)(drv->func_data);

	usb_iface_desc_t ifc_desc;
	uint8_t *        ifc;

	ifc = desc->sod;
	if (NULL == ifc) {
		return ERR_NOT_FOUND;
	}

	ifc_desc.bInterfaceNumber = ifc[2];
	ifc_desc.bInterfaceClass  = ifc[5];

	if (USB_DFU_CLASS == ifc_desc.bInterfaceClass) {
		if (func_data->func_iface == ifc_desc.bInterfaceNumber) { // Initialized
			return ERR_ALREADY_INITIALIZED;
		} else if (func_data->func_iface != 0xFF) { // Occupied
			return ERR_NO_RESOURCE;
		} else {
			func_data->func_iface = ifc_desc.bInterfaceNumber;
		}
	} else { // Not supported by this function driver
		return ERR_NOT_FOUND;
	}

	// there are no endpoint to install since DFU uses only the control endpoint

	ifc = usb_find_desc(usb_desc_next(desc->sod), desc->eod, USB_DT_INTERFACE);

	// Installed
	_dfudf_funcd.enabled = true;
	return ERR_NONE;
}

/**
 * \brief Disable DFU Function
 * \param[in] drv Pointer to USB device function driver
 * \param[in] desc Pointer to USB device descriptor
 * \return Operation status.
 */
static int32_t dfudf_disable(struct usbdf_driver *drv, struct usbd_descriptors *desc)
{
	struct dfudf_func_data *func_data = (struct dfudf_func_data *)(drv->func_data);

	usb_iface_desc_t ifc_desc;

	if (desc) {
		ifc_desc.bInterfaceClass = desc->sod[5];
		// Check interface
		if (ifc_desc.bInterfaceClass != USB_DFU_CLASS) {
			return ERR_NOT_FOUND;
		}
	}

	func_data->func_iface = 0xFF;

	_dfudf_funcd.enabled = false;
	return ERR_NONE;
}

/**
 * \brief DFU Control Function
 * \param[in] drv Pointer to USB device function driver
 * \param[in] ctrl USB device general function control type
 * \param[in] param Parameter pointer
 * \return Operation status.
 */
static int32_t dfudf_ctrl(struct usbdf_driver *drv, enum usbdf_control ctrl, void *param)
{
	switch (ctrl) {
	case USBDF_ENABLE:
		return dfudf_enable(drv, (struct usbd_descriptors *)param);

	case USBDF_DISABLE:
		return dfudf_disable(drv, (struct usbd_descriptors *)param);

	case USBDF_GET_IFACE:
		return ERR_UNSUPPORTED_OP;

	default:
		return ERR_INVALID_ARG;
	}
}

/**
 * \brief Process the DFU IN request
 * \param[in] ep Endpoint address.
 * \param[in] req Pointer to the request.
 * \param[in] stage Stage of the request.
 * \return Operation status.
 */
static int32_t dfudf_in_req(uint8_t ep, struct usb_req *req, enum usb_ctrl_stage stage)
{
	if (USB_DATA_STAGE == stage) { // the data stage is only for IN data, which we sent
		return ERR_NONE; // send the IN data
	}

	int32_t to_return = ERR_NONE;
	uint8_t response[6]; // buffer for the response to this request
	switch (req->bRequest) {
	case USB_DFU_UPLOAD: // upload firmware from flash not supported
		dfu_state = USB_DFU_STATE_DFU_ERROR; // unsupported class request
		to_return = ERR_UNSUPPORTED_OP; // stall control pipe (don't reply to the request)
		break;
	case USB_DFU_GETSTATUS: // get status
		response[0] = dfu_status; // set status
		response[1] = 10; // set poll timeout (24 bits, in milliseconds) to small value for periodical poll
		response[2] = 0; // set poll timeout (24 bits, in milliseconds) to small value for periodical poll
		response[3] = 0; // set poll timeout (24 bits, in milliseconds) to small value for periodical poll
		response[4] = dfu_state; // set state
		response[5] = 0; // string not used
		to_return = usbdc_xfer(ep, response, 6, false); // send back status
		if (USB_DFU_STATE_DFU_DNLOAD_SYNC == dfu_state) { // download has not completed
			dfu_state = USB_DFU_STATE_DFU_DNBUSY; // switch to busy state
		} else if (USB_DFU_STATE_DFU_MANIFEST_SYNC == dfu_state) {
			if (!dfu_manifestation_complete) {
				dfu_state = USB_DFU_STATE_DFU_MANIFEST; // go to manifest mode
			} else if (usb_dfu_func_desc->bmAttributes & USB_DFU_ATTRIBUTES_MANIFEST_TOLERANT) {
				dfu_state = USB_DFU_STATE_DFU_IDLE; // go back to idle mode
			} else { // this should not happen (after manifestation the state should be dfuMANIFEST-WAIT-RESET if we are not manifest tolerant)
				dfu_state = USB_DFU_STATE_DFU_MANIFEST_WAIT_RESET; // wait for reset
			}
		}
		break;
	case USB_DFU_GETSTATE: // get state
		response[0] = dfu_state; // return state
		to_return = usbdc_xfer(ep, response, 1, false); // send back state
		break;
	default: // all other DFU class IN request
		dfu_state = USB_DFU_STATE_DFU_ERROR; // unknown or unsupported class request
		to_return = ERR_INVALID_ARG; // stall control pipe (don't reply to the request)
		break;
	}

	return to_return;
}

/**
 * \brief Process the DFU OUT request
 * \param[in] ep Endpoint address.
 * \param[in] req Pointer to the request.
 * \param[in] stage Stage of the request.
 * \return Operation status.
 */
static int32_t dfudf_out_req(uint8_t ep, struct usb_req *req, enum usb_ctrl_stage stage)
{
	int32_t to_return = ERR_NONE;
	switch (req->bRequest) {
	case USB_DFU_DETACH: // detach makes only sense in DFU run-time/application mode
		dfu_state = USB_DFU_STATE_DFU_ERROR; // unsupported class request
		to_return = ERR_UNSUPPORTED_OP; // stall control pipe (don't reply to the request)
		break;
	case USB_DFU_CLRSTATUS: // clear status
		if (USB_DFU_STATE_DFU_ERROR == dfu_state || USB_DFU_STATUS_OK != dfu_status) { // only clear in case there is an error
			dfu_status = USB_DFU_STATUS_OK; // clear error status
			dfu_state = USB_DFU_STATE_DFU_IDLE; // put back in idle state
		}
		to_return = usbdc_xfer(ep, NULL, 0, false); // send ACK
		break;
	case USB_DFU_ABORT: // abort current operation
		dfu_download_offset = 0; // reset download progress
		dfu_state = USB_DFU_STATE_DFU_IDLE; // put back in idle state (nothing else to do)
		to_return = usbdc_xfer(ep, NULL, 0, false); // send ACK
		break;
	case USB_DFU_DNLOAD: // download firmware on flash
		if (!(usb_dfu_func_desc->bmAttributes & USB_REQ_DFU_DNLOAD)) { // download is not enabled
			dfu_state = USB_DFU_STATE_DFU_ERROR; // unsupported class request
			to_return = ERR_UNSUPPORTED_OP; // stall control pipe (don't reply to the request)
		} else if (USB_DFU_STATE_DFU_IDLE != dfu_state && USB_DFU_STATE_DFU_DNLOAD_IDLE != dfu_state) { // wrong state to request download
			// warn about programming error
			dfu_status = USB_DFU_STATUS_ERR_PROG;
			dfu_state = USB_DFU_STATE_DFU_ERROR;
			to_return = ERR_INVALID_ARG; // stall control pipe to indicate error
		} else if (USB_DFU_STATE_DFU_IDLE == dfu_state && (0 == req->wLength)) { // download request should not start empty
			// warn about programming error
			dfu_status = USB_DFU_STATUS_ERR_PROG;
			dfu_state = USB_DFU_STATE_DFU_ERROR;
			to_return = ERR_INVALID_ARG; // stall control pipe to indicate error
		} else if (USB_DFU_STATE_DFU_DNLOAD_IDLE == dfu_state && (0 == req->wLength)) { // download completed
			dfu_manifestation_complete = false; // clear manifestation status
			dfu_state = USB_DFU_STATE_DFU_MANIFEST_SYNC; // prepare for manifestation phase
			to_return = usbdc_xfer(ep, NULL, 0, false); // send ACK
		} else if (req->wLength > sizeof(dfu_download_data)) { // there is more data to be flash then our buffer (the USB control buffer size should be less or equal)
			// warn about programming error
			dfu_status = USB_DFU_STATUS_ERR_PROG;
			dfu_state = USB_DFU_STATE_DFU_ERROR;
			to_return = ERR_INVALID_ARG; // stall control pipe to indicate error
		} else { // there is data to be flash
			if (USB_SETUP_STAGE == stage) { // there will be data to be flash
				to_return = usbdc_xfer(ep, dfu_download_data, req->wLength, false); // send ack to the setup request to get the data
			} else { // now there is data to be flashed
				dfu_download_offset = req->wValue * sizeof(dfu_download_data); // remember which block to flash
				dfu_download_length = req->wLength; // remember the data size to be flash
				dfu_state = USB_DFU_STATE_DFU_DNLOAD_SYNC; // go to sync state
				to_return = usbdc_xfer(ep, NULL, 0, false); // ACK the data
				// we let the main application flash the data because this can be long and would stall the USB ISR
			}
		}
		break;
	default: // all other DFU class OUT request
		dfu_state = USB_DFU_STATE_DFU_ERROR; // unknown class request
		to_return = ERR_INVALID_ARG; // stall control pipe (don't reply to the request)
		break;
	}

	return to_return;
}

/**
 * \brief Process the CDC class request
 * \param[in] ep Endpoint address.
 * \param[in] req Pointer to the request.
 * \param[in] stage Stage of the request.
 * \return Operation status.
 */
static int32_t dfudf_req(uint8_t ep, struct usb_req *req, enum usb_ctrl_stage stage)
{
	if (0x01 != ((req->bmRequestType >> 5) & 0x03)) { // class request
		return ERR_NOT_FOUND;
	}

	if ((req->wIndex == _dfudf_funcd.func_iface)) {
		if (req->bmRequestType & USB_EP_DIR_IN) {
			return dfudf_in_req(ep, req, stage);
		} else {
			return dfudf_out_req(ep, req, stage);
		}
	} else {
		return ERR_NOT_FOUND;
	}
	return ERR_NOT_FOUND;
}

/** USB Device DFU Handler Struct */
static struct usbdc_handler dfudf_req_h = {NULL, (FUNC_PTR)dfudf_req};

/**
 * \brief Initialize the USB DFU Function Driver
 */
int32_t dfudf_init(void)
{
	if (usbdc_get_state() > USBD_S_POWER) {
		return ERR_DENIED;
	}

	_dfudf.ctrl      = dfudf_ctrl;
	_dfudf.func_data = &_dfudf_funcd;

	usbdc_register_function(&_dfudf);
	usbdc_register_handler(USBDC_HDL_REQ, &dfudf_req_h);

	return ERR_NONE;
}

/**
 * \brief De-initialize the USB DFU Function Driver
 */
void dfudf_deinit(void)
{
}

/**
 * \brief Check whether DFU Function is enabled
 */
bool dfudf_is_enabled(void)
{
	return _dfudf_funcd.enabled;
}
