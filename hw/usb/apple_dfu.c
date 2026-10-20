/*
 * Apple DFU and Recovery Mode USB device.
 *
 * Copyright (c) 2026 Visual Ehrmanntraut (VisualEhrmanntraut).
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "hw/arm/boot.h"
#include "hw/qdev-properties.h"
#include "hw/usb.h"
#include "hw/usb/apple_dfu.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/timer.h"
#include "qemu/units.h"
#include "system/runstate.h"
#include "desc.h"

#define APPLE_VENDOR_ID           0x05AC
#define APPLE_DFU_PRODUCT_ID      0x1227
#define APPLE_RECOVERY_PRODUCT_ID 0x1281

#define APPLE_DFU_REENUMERATE_MS 2000
#define APPLE_DFU_UPLOAD_MAX     (512 * MiB)
#define APPLE_DFU_RESPONSE_MAX   256

#define DFU_REQ_DNLOAD    0x01
#define DFU_REQ_GETSTATUS 0x03
#define DFU_REQ_CLRSTATUS 0x04
#define DFU_REQ_GETSTATE  0x05
#define DFU_REQ_ABORT     0x06

#define DFU_STATE_IDLE                2
#define DFU_STATE_DNLOAD_IDLE         5
#define DFU_STATE_MANIFEST_SYNC       6
#define DFU_STATE_MANIFEST            7
#define DFU_STATE_MANIFEST_WAIT_RESET 8

#define RECOVERY_REQ_COMMAND  0x00
#define RECOVERY_REQ_EXEC     0x01
#define RECOVERY_REQ_RESPONSE 0x00
#define RECOVERY_REQ_UPLOAD   0x00

#define RECOVERY_UPLOAD_EP 4

enum
{
    STR_NONCE = 1,
    STR_MANUFACTURER,
    STR_PRODUCT,
    STR_SERIALNUMBER,
};

typedef enum
{
    APPLE_DFU_STAGE_IDLE,
    APPLE_DFU_STAGE_DFU,
    APPLE_DFU_STAGE_IBSS,
    APPLE_DFU_STAGE_IBEC,
    APPLE_DFU_STAGE_BOOTED,
} AppleDFUStage;

struct AppleDFUState
{
    USBDevice parent_obj;

    uint32_t cpid;
    uint32_t cprv;
    uint32_t cpfm;
    uint32_t scep;
    uint32_t bdid;
    uint32_t ibfl;
    uint64_t ecid;

    AppleDFUStage stage;
    QEMUTimer*    reenumerate_timer;
    char*         nonce;

    uint8_t dfu_state;

    GByteArray* upload;
    bool        upload_truncated;
    char        response[APPLE_DFU_RESPONSE_MAX];

    gchar* ticket_data;
    gsize  ticket_length;

    GHashTable* images;
};

static const USBDescStrings apple_dfu_strings = {
    [STR_MANUFACTURER] = "Apple Inc.",
};

static const USBDescIface apple_dfu_iface_dfu = {
    .bInterfaceNumber   = 0,
    .bInterfaceClass    = USB_CLASS_APP_SPEC,
    .bInterfaceSubClass = 0x01,
    .bInterfaceProtocol = 0x02,
};

static const USBDescIface apple_dfu_iface_recovery = {
    .bInterfaceNumber = 0,
    .bNumEndpoints    = 1,
    .bInterfaceClass  = USB_CLASS_VENDOR_SPEC,
    .eps =
        (USBDescEndpoint[]){
            {
                .bEndpointAddress = USB_DIR_OUT | RECOVERY_UPLOAD_EP,
                .bmAttributes     = USB_ENDPOINT_XFER_BULK,
                .wMaxPacketSize   = 512,
            },
        },
};

static const USBDescDevice apple_dfu_device_dfu = {
    .bcdUSB             = 0x0200,
    .bMaxPacketSize0    = 64,
    .bNumConfigurations = 1,
    .confs =
        (USBDescConfig[]){
            {
                .bNumInterfaces      = 1,
                .bConfigurationValue = 1,
                .bmAttributes        = USB_CFG_ATT_ONE,
                .nif                 = 1,
                .ifs                 = &apple_dfu_iface_dfu,
            },
        },
};

static const USBDescDevice apple_dfu_device_recovery = {
    .bcdUSB             = 0x0200,
    .bMaxPacketSize0    = 64,
    .bNumConfigurations = 1,
    .confs =
        (USBDescConfig[]){
            {
                .bNumInterfaces      = 1,
                .bConfigurationValue = 1,
                .bmAttributes        = USB_CFG_ATT_ONE,
                .nif                 = 1,
                .ifs                 = &apple_dfu_iface_recovery,
            },
        },
};

static const USBDesc apple_dfu_desc_dfu = {
    .id =
        {
            .idVendor      = APPLE_VENDOR_ID,
            .idProduct     = APPLE_DFU_PRODUCT_ID,
            .iManufacturer = STR_MANUFACTURER,
            .iProduct      = STR_PRODUCT,
            .iSerialNumber = STR_SERIALNUMBER,
        },
    .high = &apple_dfu_device_dfu,
    .str  = apple_dfu_strings,
};

static const USBDesc apple_dfu_desc_recovery = {
    .id =
        {
            .idVendor      = APPLE_VENDOR_ID,
            .idProduct     = APPLE_RECOVERY_PRODUCT_ID,
            .iManufacturer = STR_MANUFACTURER,
            .iProduct      = STR_PRODUCT,
            .iSerialNumber = STR_SERIALNUMBER,
        },
    .high = &apple_dfu_device_recovery,
    .str  = apple_dfu_strings,
};

static void apple_dfu_clear_upload(AppleDFUState* s)
{
    g_byte_array_set_size(s->upload, 0);
    s->upload_truncated = false;
}

static void apple_dfu_enter(AppleDFUState* s, AppleDFUStage stage)
{
    s->stage = stage;
    timer_mod(s->reenumerate_timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL));
}

static void apple_dfu_present(AppleDFUState* s)
{
    USBDevice*       dev    = USB_DEVICE(s);
    g_autofree char* serial = NULL;

    serial = g_strdup_printf("CPID:%04X CPRV:%02X CPFM:%02X SCEP:%02X BDID:%02X ECID:%016" PRIX64 " IBFL:%02X", s->cpid,
                             s->cprv, s->cpfm, s->scep, s->bdid, s->ecid, s->ibfl);

    dev->usb_desc = s->stage == APPLE_DFU_STAGE_DFU ? &apple_dfu_desc_dfu : &apple_dfu_desc_recovery;
    usb_desc_set_string(dev, STR_NONCE, s->nonce);
    usb_desc_set_string(dev, STR_PRODUCT,
                        s->stage == APPLE_DFU_STAGE_DFU ? "Apple Mobile Device (DFU Mode)" :
                                                          "Apple Mobile Device (Recovery Mode)");
    usb_desc_set_string(dev, STR_SERIALNUMBER, serial);

    s->dfu_state   = DFU_STATE_IDLE;
    s->response[0] = '\0';
    apple_dfu_clear_upload(s);
}

static void apple_dfu_attach(AppleDFUState* s)
{
    apple_dfu_present(s);
    usb_device_attach(USB_DEVICE(s), &error_abort);
}

static void apple_dfu_reenumerate(void* opaque)
{
    AppleDFUState* s   = opaque;
    USBDevice*     dev = USB_DEVICE(s);

    if (dev->attached) {
        usb_device_detach(dev);

        if (s->stage == APPLE_DFU_STAGE_BOOTED) { qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET); }
        else {
            timer_mod(s->reenumerate_timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + APPLE_DFU_REENUMERATE_MS);
        }
        return;
    }

    if (s->stage != APPLE_DFU_STAGE_BOOTED) { apple_dfu_attach(s); }
}

static void apple_dfu_consume_upload(AppleDFUState* s)
{
    char type[5] = {0};

    if (s->upload->len != 0 && !s->upload_truncated) {
        gchar* ticket_data   = NULL;
        gsize  ticket_length = 0;

        apple_boot_extract_manifest(s->upload->data, s->upload->len, &ticket_data, &ticket_length);
        if (ticket_data != NULL) {
            g_free(s->ticket_data);
            s->ticket_data   = ticket_data;
            s->ticket_length = ticket_length;
        }

        if (apple_boot_get_im4p_type(s->upload->data, s->upload->len, type)) {
            g_hash_table_insert(s->images, g_strdup(type), g_byte_array_free_to_bytes(s->upload));
            s->upload = g_byte_array_new();
        }
    }

    apple_dfu_clear_upload(s);
}

static void apple_dfu_handle_command(AppleDFUState* s, const char* cmd)
{
    const char* var;

    apple_dfu_consume_upload(s);
    s->response[0] = '\0';

    if (g_str_has_prefix(cmd, "getenv ")) {
        var = cmd + strlen("getenv ");
        if (strcmp(var, "boot-stage") == 0) {
            snprintf(s->response, sizeof(s->response), "%d", s->stage == APPLE_DFU_STAGE_IBSS ? 1 : 2);
        }
    }
    else if (strcmp(cmd, "go") == 0) {
        apple_dfu_enter(s, APPLE_DFU_STAGE_IBEC);
    }
    else if (strcmp(cmd, "bootx") == 0) {
        if (s->ticket_data == NULL) { warn_report("apple-dfu: bootx without a personalised image"); }
        apple_dfu_enter(s, APPLE_DFU_STAGE_BOOTED);
    }
}

static void apple_dfu_handle_dfu_control(AppleDFUState* s, USBPacket* p, int request, int length, uint8_t* data)
{
    switch (request) {
        case ClassInterfaceOutRequest | DFU_REQ_DNLOAD:
            s->dfu_state = length == 0 ? DFU_STATE_MANIFEST_SYNC : DFU_STATE_DNLOAD_IDLE;
            break;
        case ClassInterfaceRequest | DFU_REQ_GETSTATUS:
            if (length < 6) {
                p->status = USB_RET_STALL;
                break;
            }
            memset(data, 0, 6);
            data[4]          = s->dfu_state;
            p->actual_length = 6;

            if (s->dfu_state == DFU_STATE_MANIFEST_SYNC) { s->dfu_state = DFU_STATE_MANIFEST; }
            else if (s->dfu_state == DFU_STATE_MANIFEST) {
                s->dfu_state = DFU_STATE_MANIFEST_WAIT_RESET;
                apple_dfu_enter(s, APPLE_DFU_STAGE_IBSS);
            }
            break;
        case ClassInterfaceRequest | DFU_REQ_GETSTATE:
            if (length < 1) {
                p->status = USB_RET_STALL;
                break;
            }
            data[0]          = s->dfu_state;
            p->actual_length = 1;
            break;
        case ClassInterfaceOutRequest | DFU_REQ_CLRSTATUS:
        case ClassInterfaceOutRequest | DFU_REQ_ABORT    : s->dfu_state = DFU_STATE_IDLE; break;
        default                                          : p->status = USB_RET_STALL; break;
    }
}

static void apple_dfu_handle_recovery_control(AppleDFUState* s, USBPacket* p, int request, int length, uint8_t* data)
{
    g_autofree char* cmd = NULL;
    size_t           len;

    switch (request) {
        case VendorDeviceOutRequest | RECOVERY_REQ_COMMAND:
        case VendorDeviceOutRequest | RECOVERY_REQ_EXEC:
            cmd = g_strndup((const char*)data, length);
            apple_dfu_handle_command(s, cmd);
            break;
        case VendorDeviceRequest | RECOVERY_REQ_RESPONSE:
            len = MIN((size_t)length, strlen(s->response) + 1);
            memcpy(data, s->response, len);
            p->actual_length = len;
            break;
        case VendorInterfaceOutRequest | RECOVERY_REQ_UPLOAD: apple_dfu_clear_upload(s); break;
        default                                             : p->status = USB_RET_STALL; break;
    }
}

static void apple_dfu_handle_control(USBDevice* dev, USBPacket* p, int request, int value, int index, int length,
                                     uint8_t* data)
{
    AppleDFUState* s = APPLE_DFU(dev);

    if (usb_desc_handle_control(dev, p, request, value, index, length, data) >= 0) { return; }

    if (s->stage == APPLE_DFU_STAGE_DFU) { apple_dfu_handle_dfu_control(s, p, request, length, data); }
    else {
        apple_dfu_handle_recovery_control(s, p, request, length, data);
    }
}

static void apple_dfu_handle_data(USBDevice* dev, USBPacket* p)
{
    AppleDFUState* s    = APPLE_DFU(dev);
    size_t         size = p->iov.size;
    size_t         offset;

    if (s->stage == APPLE_DFU_STAGE_DFU || p->pid != USB_TOKEN_OUT || p->ep->nr != RECOVERY_UPLOAD_EP) {
        p->status = USB_RET_STALL;
        return;
    }

    if (s->upload_truncated || s->upload->len + size > APPLE_DFU_UPLOAD_MAX) {
        s->upload_truncated = true;
        g_byte_array_set_size(s->upload, 0);
        usb_packet_skip(p, size);
        return;
    }

    offset = s->upload->len;
    g_byte_array_set_size(s->upload, offset + size);
    usb_packet_copy(p, s->upload->data + offset, size);
}

static void apple_dfu_handle_attach(USBDevice* dev) { usb_desc_attach(dev); }

void apple_dfu_start(AppleDFUState* s, const uint8_t* ap_nonce, size_t ap_nonce_len, const uint8_t* sep_nonce,
                     size_t sep_nonce_len)
{
    GString* nonce = g_string_new("NONC:");
    size_t   i;

    for (i = 0; i < ap_nonce_len; i++) { g_string_append_printf(nonce, "%02X", ap_nonce[i]); }
    g_string_append(nonce, " SNON:");
    for (i = 0; i < sep_nonce_len; i++) { g_string_append_printf(nonce, "%02X", sep_nonce[i]); }

    g_free(s->nonce);
    s->nonce = g_string_free(nonce, false);

    g_hash_table_remove_all(s->images);
    apple_dfu_enter(s, APPLE_DFU_STAGE_DFU);
}

bool apple_dfu_booted(AppleDFUState* s) { return s->stage == APPLE_DFU_STAGE_BOOTED; }

bool apple_dfu_waiting(AppleDFUState* s) { return s->stage != APPLE_DFU_STAGE_IDLE && !apple_dfu_booted(s); }

bool apple_dfu_copy_ticket(AppleDFUState* s, gchar** data, gsize* length)
{
    if (!apple_dfu_booted(s) || s->ticket_data == NULL) { return false; }

    *data   = g_memdup2(s->ticket_data, s->ticket_length);
    *length = s->ticket_length;
    return true;
}

bool apple_dfu_copy_image(AppleDFUState* s, const char* type, uint8_t** data, gsize* length)
{
    GBytes*       image;
    gconstpointer image_data;

    if (!apple_dfu_booted(s)) { return false; }

    image = g_hash_table_lookup(s->images, type);
    if (image == NULL) { return false; }

    image_data = g_bytes_get_data(image, length);
    *data      = g_memdup2(image_data, *length);
    return true;
}

static void apple_dfu_realize(USBDevice* dev, Error** errp)
{
    AppleDFUState* s = APPLE_DFU(dev);

    dev->auto_attach = false;
    usb_desc_init(dev);

    s->upload            = g_byte_array_new();
    s->images            = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_bytes_unref);
    s->reenumerate_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, apple_dfu_reenumerate, s);
}

static void apple_dfu_unrealize(USBDevice* dev)
{
    AppleDFUState* s = APPLE_DFU(dev);

    timer_free(s->reenumerate_timer);
    g_byte_array_unref(s->upload);
    g_free(s->nonce);
    g_free(s->ticket_data);
    g_hash_table_destroy(s->images);
}

static const Property apple_dfu_properties[] = {
    DEFINE_PROP_UINT32("cpid", AppleDFUState, cpid, 0),    DEFINE_PROP_UINT32("cprv", AppleDFUState, cprv, 0),
    DEFINE_PROP_UINT32("cpfm", AppleDFUState, cpfm, 0x03), DEFINE_PROP_UINT32("scep", AppleDFUState, scep, 0x01),
    DEFINE_PROP_UINT32("bdid", AppleDFUState, bdid, 0),    DEFINE_PROP_UINT32("ibfl", AppleDFUState, ibfl, 0x1C),
    DEFINE_PROP_UINT64("ecid", AppleDFUState, ecid, 0),
};

static void apple_dfu_class_init(ObjectClass* klass, const void* data)
{
    DeviceClass*    dc = DEVICE_CLASS(klass);
    USBDeviceClass* uc = USB_DEVICE_CLASS(klass);

    uc->realize        = apple_dfu_realize;
    uc->unrealize      = apple_dfu_unrealize;
    uc->product_desc   = "Apple DFU";
    uc->usb_desc       = &apple_dfu_desc_dfu;
    uc->handle_attach  = apple_dfu_handle_attach;
    uc->handle_control = apple_dfu_handle_control;
    uc->handle_data    = apple_dfu_handle_data;
    device_class_set_props(dc, apple_dfu_properties);
}

OBJECT_DEFINE_SIMPLE_TYPE_CLASS_INIT(AppleDFUState, apple_dfu, APPLE_DFU, USB_DEVICE)
