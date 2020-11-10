/*
 * usbhsfs_drive.c
 *
 * Copyright (c) 2020, DarkMatterCore <pabloacurielz@gmail.com>.
 * Copyright (c) 2020, XorTroll.
 *
 * This file is part of libusbhsfs (https://github.com/DarkMatterCore/libusbhsfs).
 *
 * libusbhsfs is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * libusbhsfs is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "usbhsfs_utils.h"
#include "usbhsfs_request.h"
#include "usbhsfs_scsi.h"

bool usbHsFsDriveInitializeContext(UsbHsFsDriveContext *ctx, UsbHsInterface *usb_if)
{
    if (!ctx || !usb_if)
    {
        USBHSFS_LOG("Invalid parameters!");
        return false;
    }
    
    Result rc = 0;
    UsbHsClientIfSession *usb_if_session = &(ctx->usb_if_session);
    UsbHsClientEpSession *usb_in_ep_session = &(ctx->usb_in_ep_session);
    UsbHsClientEpSession *usb_out_ep_session = &(ctx->usb_out_ep_session);
    UsbHsFsDriveLogicalUnitContext lun_ctx = {0}, *tmp_lun_ctx = NULL;
    //u8 conf = 0;
    bool ret = false, ep_open = false;
    
    /* Clear output context. */
    memset(ctx, 0, sizeof(UsbHsFsDriveContext));
    
    /* Copy USB interface ID. */
    ctx->usb_if_id = usb_if->inf.ID;
    
    /* Allocate memory for the USB control transfer buffer. */
    ctx->ctrl_xfer_buf = usbHsFsRequestAllocateCtrlXferBuffer();
    if (!ctx->ctrl_xfer_buf)
    {
        USBHSFS_LOG("Failed to allocate USB control transfer buffer! (interface %d).", ctx->usb_if_id);
        goto end;
    }
    
    /* Open current interface. */
    rc = usbHsAcquireUsbIf(usb_if_session, usb_if);
    if (R_FAILED(rc))
    {
        USBHSFS_LOG("usbHsAcquireUsbIf failed! (0x%08X) (interface %d).", rc, ctx->usb_if_id);
        goto end;
    }
    
    /* TO DO: Add support for interfaces with alternate settings? */
    //struct usb_config_descriptor *usb_config_desc = &(usb_if_session->inf.config_desc);
    //struct usb_interface_descriptor *usb_interface_desc = &(usb_if_session->inf.inf.interface_desc);
    
    /* Open input endpoint. */
    for(u8 i = 0; i < 15; i++)
    {
        struct usb_endpoint_descriptor *ep_desc = &(usb_if_session->inf.inf.input_endpoint_descs[i]);
        if (ep_desc->bLength != 0 && (ep_desc->bEndpointAddress & USB_ENDPOINT_IN) && (ep_desc->bmAttributes & 0x3F) == USB_TRANSFER_TYPE_BULK)
        {
            rc = usbHsIfOpenUsbEp(usb_if_session, usb_in_ep_session, 1, ep_desc->wMaxPacketSize, ep_desc);
            if (R_SUCCEEDED(rc))
            {
                ep_open = true;
                break;
            } else {
                USBHSFS_LOG("usbHsIfOpenUsbEp failed for input endpoint %u! (0x%08X) (interface %d).", i, rc, ctx->usb_if_id);
            }
        }
    }
    
    if (!ep_open) goto end;
    
    /* Open output endpoint. */
    ep_open = false;
    for(u8 i = 0; i < 15; i++)
    {
        struct usb_endpoint_descriptor *ep_desc = &(usb_if_session->inf.inf.output_endpoint_descs[i]);
        if (ep_desc->bLength != 0 && !(ep_desc->bEndpointAddress & USB_ENDPOINT_IN) && (ep_desc->bmAttributes & 0x3F) == USB_TRANSFER_TYPE_BULK)
        {
            rc = usbHsIfOpenUsbEp(usb_if_session, usb_out_ep_session, 1, ep_desc->wMaxPacketSize, ep_desc);
            if (R_SUCCEEDED(rc))
            {
                ep_open = true;
                break;
            } else {
                USBHSFS_LOG("usbHsIfOpenUsbEp failed for output endpoint %u! (0x%08X) (interface %d).", i, rc, ctx->usb_if_id);
            }
        }
    }
    
    if (!ep_open) goto end;
    
    /* Get device configuration. */
    /*rc = usbHsFsRequestGetDeviceConfiguration(ctx, &conf);
    if (R_FAILED(rc))
    {
        USBHSFS_LOG("Failed to get device configuration! (interface %d).", ctx->usb_if_id);
        goto end;
    }*/
    
    /* Set device configuration, if needed. */
    /*if (conf != usb_if->config_desc.bConfigurationValue)
    {
        USBHSFS_LOG("Device configuration mismatch detected (0x%02X != 0x%02X) (interface %d). Setting preferred value.", conf, usb_if->config_desc.bConfigurationValue, ctx->usb_if_id);
        rc = usbHsFsRequestSetDeviceConfiguration(ctx, usb_if_session->inf.config_desc.bConfigurationValue);
        if (R_FAILED(rc))
        {
            USBHSFS_LOG("Failed to set device configuration to 0x%02X! (interface %d).", usb_if_session->inf.config_desc.bConfigurationValue, ctx->usb_if_id);
            goto end;
        }
    }*/
    
    /* Retrieve max supported logical units from this storage device. */
    /* If the request fails (e.g. unsupported by the device), we'll attempt to clear a possible STALL status from the input endpoint. */
    if (R_FAILED(usbHsFsRequestGetMaxLogicalUnits(ctx))) usbHsFsRequestClearEndpointHaltFeature(ctx, false);
    USBHSFS_LOG("Max LUN count: %u (interface %d).", ctx->max_lun, ctx->usb_if_id);
    
    /* Prepare LUNs using SCSI commands. */
    for(u8 i = 0; i < ctx->max_lun; i++)
    {
        /* Initialize LUN context. */
        if (!usbHsFsScsiInitializeDriveLogicalUnitContext(ctx, i, &lun_ctx))
        {
            USBHSFS_LOG("Failed to initialize context for drive LUN #%u! (interface %d).", i, ctx->usb_if_id);
            continue;
        }
        
        
        
        
        /* TO DO: initialize partition FS stuff here. */
        
        
        
        
        
        /* Reallocate LUN context buffer. */
        tmp_lun_ctx = realloc(ctx->lun_ctx, (ctx->lun_count + 1) * sizeof(UsbHsFsDriveLogicalUnitContext));
        if (!tmp_lun_ctx)
        {
            USBHSFS_LOG("Failed to allocate memory for LUN #%u context! (interface %d).", i, ctx->usb_if_id);
            goto end;
        }
        
        ctx->lun_ctx = tmp_lun_ctx;
        tmp_lun_ctx = &(ctx->lun_ctx[ctx->lun_count++]);    /* Increase initialized LUN count. */
        
        /* Copy initialized LUN context data. */
        memcpy(tmp_lun_ctx, &lun_ctx, sizeof(UsbHsFsDriveLogicalUnitContext));
    }
    
    if (!ctx->lun_count)
    {
        USBHSFS_LOG("Failed to initialize any LUN(s)! (interface %d).", ctx->usb_if_id);
        goto end;
    }
    
    /* Update return value. */
    ret = true;
    
end:
    /* Destroy drive context if something went wrong. */
    if (!ret) usbHsFsDriveDestroyContext(ctx);
    
    return ret;
}

void usbHsFsDriveDestroyContext(UsbHsFsDriveContext *ctx)
{
    if (!ctx) return;
    
    Mutex drive_mutex = ctx->mutex;
    mutexLock(&drive_mutex);
    
    UsbHsClientIfSession *usb_if_session = &(ctx->usb_if_session);
    UsbHsClientEpSession *usb_in_ep_session = &(ctx->usb_in_ep_session);
    UsbHsClientEpSession *usb_out_ep_session = &(ctx->usb_out_ep_session);
    
    
    
    /* TO DO: free any data related to drive LUN contexts here as well. */
    
    
    
    
    /* Free LUN context buffer. */
    if (ctx->lun_ctx) free(ctx->lun_ctx);
    
    /* Close USB interface and endpoint sessions. */
    if (serviceIsActive(&(usb_out_ep_session->s))) usbHsEpClose(usb_out_ep_session);
    if (serviceIsActive(&(usb_in_ep_session->s))) usbHsEpClose(usb_in_ep_session);
    if (usbHsIfIsActive(usb_if_session)) usbHsIfClose(usb_if_session);
    
    /* Free dedicated USB control transfer buffer. */
    if (ctx->ctrl_xfer_buf) free(ctx->ctrl_xfer_buf);
    
    /* Clear context. */
    memset(ctx, 0, sizeof(UsbHsFsDriveContext));
    
    mutexUnlock(&drive_mutex);
}
