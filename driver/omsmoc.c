/*
 * OMS Match-on-Chip driver for libfprint
 * Copyright (C) 2026 The OMS Linux driver contributors
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
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/*
 * Reverse engineered driver for the Jiangxi OMS Microelectronics
 * fingerprint module (33a7:2388).  The protocol was reconstructed from the
 * vendor Windows driver and verified against usbmon captures of the vendor
 * driver as well as with a pyusb prototype on the real hardware.
 *
 * The transport consists of two layers:
 *
 *   1. An 8 byte "USBC" request header (host -> device, bulk OUT):
 *        55 53 42 43 | dir | size | 00 00
 *        dir = 0x00: the host will write `size` bytes afterwards
 *        dir = 0x80: request that the device returns at most `size` bytes
 *      The device acknowledges every request with a 4 byte "USBS" reply.
 *
 *   2. A 64 byte EF01 frame, padded with 0x00:
 *        EF 01 FF FF | FF FF | kind | 00 | L | data[L]
 *        kind = 0x01 request / 0x07 response
 *        the last two bytes of data are sum (frame[6 : 9 + L - 2]),
 *        stored big endian.
 *
 * A command transaction is therefore:
 *   OUT USBC(00, 64) -> OUT 64 byte command frame -> IN USBS
 *   -> OUT USBC(80, 64) -> IN 64 byte response frame -> IN USBS
 *
 * The device answers a read request with the ack alone when it has no data
 * to report yet.  Identify results are polled with 32 byte windows.
 *
 * Everything runs in plain text: neither the vendor driver nor this driver
 * performs a handshake, a key exchange or any encryption.
 */

#define FP_COMPONENT "omsmoc"

#include "drivers_api.h"

#include "omsmoc.h"

/* USB identifiers */
#define OMS_VENDOR_ID  0x33a7
#define OMS_PRODUCT_ID 0x2388

/* Bulk endpoints */
#define OMS_EP_OUT (0x01 | FPI_USB_ENDPOINT_OUT)
#define OMS_EP_IN  (0x81 | FPI_USB_ENDPOINT_IN)

/* Transport layer */
#define OMS_USBC_SIZE   8
#define OMS_USBS_SIZE   4
#define OMS_DIR_WRITE   0x00
#define OMS_DIR_READ    0x80
#define OMS_PACKET_SIZE 64
#define OMS_POLL_SIZE   32

/* EF01 frame */
#define OMS_FRAME_HEADER_SIZE   9
#define OMS_FRAME_TRAILER_SIZE  2
#define OMS_FRAME_KIND_REQUEST  0x01
#define OMS_FRAME_KIND_RESPONSE 0x07
#define OMS_MAX_PARAM_SIZE      (OMS_PACKET_SIZE - OMS_FRAME_HEADER_SIZE - 1 - OMS_FRAME_TRAILER_SIZE)

/* Timeouts and pacing (the vendor driver polls the device every ~60 ms) */
#define OMS_CMD_TIMEOUT_MS    2000
#define OMS_POLL_TIMEOUT_MS   200
#define OMS_READ_DELAY_MS     60
#define OMS_POLL_INTERVAL_MS  60
#define OMS_MAX_SESSION_RETRY 10
#define OMS_MAX_READ_RETRY    5
#define OMS_MAX_DRAIN_READS   8
#define OMS_MAX_CAPTURE_POLLS     1200  /* ~72 s waiting for a finger press */
#define OMS_MAX_CAPTURE_END_POLLS 30    /* ~1.8 s to finish the capture */
#define OMS_MAX_ENROLL_POLLS      1200  /* ~72 s between two finger presses */

/* Number of finger presses the chip expects during enrollment */
#define OMS_ENROLL_STAGES 6

/* Commands (all verified against usbmon captures of the vendor driver) */
typedef enum {
  OMS_CMD_TEMPLATE_OP  = 0x0c,
  OMS_CMD_TEMPLATE_NUM = 0x1d,
  OMS_CMD_INDEX_TABLE  = 0x1f,
  OMS_CMD_CANCEL       = 0x30,
  OMS_CMD_ENROLL       = 0x31,
  OMS_CMD_IDENTIFY     = 0x32,
  OMS_CMD_CHIP_SN      = 0x34,
} OmsCommand;

/* AutoIdentify (0x32) response payloads, verified on hardware:
 *
 *   00 00 XX XX 00 00   waiting for a finger (XX XX echoes the parameters)
 *   00 01 XX XX 00 00   finger image captured, the result is still pending
 *   00 05 00 SS 03 e8   matched an enrolled template, SS is its slot
 *   09 05 FF FF 00 00   the search finished without a match
 *
 * Note that "00 01" alone does *not* mean that the finger was recognised:
 * an unenrolled finger produces it as well, followed by "09 05". The two
 * parameter bytes are echoed by the chip but do not restrict the search.
 */
#define OMS_ID_STATUS_OK      0x00
#define OMS_ID_STATUS_NOMATCH 0x09
#define OMS_ID_RESULT_CAPTURED 0x01
#define OMS_ID_RESULT_MATCHED  0x05
#define OMS_ID_PAYLOAD_SLOT    3

/* Enroll progress matrix */
#define OMS_ENROLL_TRAILER_INDEX 6

struct _FpiDeviceOmsMoc
{
  FpDevice parent;

  FpiSsm   *task_ssm;

  /* Buffers of the transaction currently on the wire */
  guint8    usbc[OMS_USBC_SIZE];
  guint8    out_frame[OMS_PACKET_SIZE];
  guint8    in_data[OMS_PACKET_SIZE];
  gsize     in_len;
  guint8    ack[OMS_USBS_SIZE];

  /* Cached device state */
  guint8    nr_templates;
  guint16   template_bitmap;

  /* Identify/verify bookkeeping */
  gint      matched_slot;       /* slot the chip matched, -1 for "no match" */
  gboolean  result_received;
  GError   *action_error;
  guint     session_retries;
  guint     read_retries;

  /* Enroll bookkeeping */
  gboolean  captured;
  guint8    enroll_slot;
  guint8    enroll_level;
  guint     poll_count;
  guint     capture_retries;
};

G_DEFINE_TYPE (FpiDeviceOmsMoc, fpi_device_omsmoc, FP_TYPE_DEVICE)

typedef void (*OmsCallback) (FpiDeviceOmsMoc *self, GError *error);

/* ------------------------------------------------------------------ *
 * Protocol helpers
 * ------------------------------------------------------------------ */

static void
oms_fill_usbc (guint8 *buf,
               guint8  direction,
               guint8  size)
{
  buf[0] = 'U';
  buf[1] = 'S';
  buf[2] = 'B';
  buf[3] = 'C';
  buf[4] = direction;
  buf[5] = size;
  buf[6] = 0x00;
  buf[7] = 0x00;
}

static gsize
oms_build_frame (guint8        *buf,
                 guint8         cmd,
                 const guint8  *params,
                 gsize          params_len)
{
  gsize len;
  gsize i;
  guint16 checksum = 0;

  g_return_val_if_fail (params_len <= OMS_MAX_PARAM_SIZE, 0);

  len = 1 + params_len + OMS_FRAME_TRAILER_SIZE;

  memset (buf, 0x00, OMS_PACKET_SIZE);
  buf[0] = 0xef;
  buf[1] = 0x01;
  buf[2] = 0xff;
  buf[3] = 0xff;
  buf[4] = 0xff;
  buf[5] = 0xff;
  buf[6] = OMS_FRAME_KIND_REQUEST;
  buf[7] = 0x00;
  buf[8] = len;
  buf[9] = cmd;
  if (params_len > 0)
    memcpy (&buf[10], params, params_len);

  for (i = 6; i < OMS_FRAME_HEADER_SIZE + len - OMS_FRAME_TRAILER_SIZE; i++)
    checksum += buf[i];

  buf[OMS_FRAME_HEADER_SIZE + len - 2] = (checksum >> 8) & 0xff;
  buf[OMS_FRAME_HEADER_SIZE + len - 1] = checksum & 0xff;

  return OMS_FRAME_HEADER_SIZE + len;
}

static gboolean
oms_parse_frame (const guint8  *buf,
                 gsize          buf_len,
                 const guint8 **payload,
                 gsize         *payload_len)
{
  gsize frame_len;
  gsize i;
  guint16 checksum = 0;
  guint16 expected;

  if (buf_len < OMS_FRAME_HEADER_SIZE)
    return FALSE;

  if (buf[0] != 0xef || buf[1] != 0x01 || buf[2] != 0xff || buf[3] != 0xff ||
      buf[4] != 0xff || buf[5] != 0xff || buf[6] != OMS_FRAME_KIND_RESPONSE)
    return FALSE;

  frame_len = buf[8];
  if (frame_len < OMS_FRAME_TRAILER_SIZE ||
      OMS_FRAME_HEADER_SIZE + frame_len > buf_len)
    return FALSE;

  for (i = 6; i < OMS_FRAME_HEADER_SIZE + frame_len - OMS_FRAME_TRAILER_SIZE; i++)
    checksum += buf[i];

  expected = (buf[OMS_FRAME_HEADER_SIZE + frame_len - 2] << 8) |
             buf[OMS_FRAME_HEADER_SIZE + frame_len - 1];
  if (checksum != expected)
    {
      fp_warn ("Response frame checksum mismatch (0x%04x != 0x%04x)",
               checksum, expected);
      return FALSE;
    }

  *payload = &buf[OMS_FRAME_HEADER_SIZE];
  *payload_len = frame_len - OMS_FRAME_TRAILER_SIZE;

  return TRUE;
}

/* The device pads its answers to the requested window size and reports
 * "nothing to say" with an all zero packet. */
static gboolean
oms_data_is_empty (const guint8 *data,
                   gsize         len)
{
  gsize i;

  for (i = 0; i < len; i++)
    {
      if (data[i] != 0x00)
        return FALSE;
    }

  return TRUE;
}

/* ------------------------------------------------------------------ *
 * USB transfers
 * ------------------------------------------------------------------ */

typedef struct
{
  OmsCallback callback;
  guint       window;
  guint       timeout_ms;
  gboolean    cancelable;
} OmsTransferData;

static void
oms_usbs_cb (FpiUsbTransfer *transfer,
             FpDevice       *device,
             gpointer        user_data,
             GError         *error)
{
  if (error)
    {
      fpi_ssm_mark_failed (transfer->ssm, error);
      return;
    }

  if (transfer->actual_length != OMS_USBS_SIZE ||
      memcmp (transfer->buffer, "USBS", OMS_USBS_SIZE) != 0)
    {
      fpi_ssm_mark_failed (transfer->ssm,
                           fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                     "Invalid USBS acknowledgement"));
      return;
    }

  fpi_ssm_next_state (transfer->ssm);
}

/* Command transfers: USBC write header + command frame + USBS, followed by
 * an optional response read (USBC read header + data + USBS). */
enum {
  OMS_CMD_SEND_HEADER,
  OMS_CMD_SEND_FRAME,
  OMS_CMD_WAIT_ACK,
  OMS_CMD_READ_DELAY,
  OMS_CMD_SEND_READ_HEADER,
  OMS_CMD_READ_DATA,
  OMS_CMD_READ_ACK,
  OMS_CMD_COMPLETE,
  OMS_CMD_NUM_STATES,
};

/* Read transfers: USBC read header + data + USBS */
enum {
  OMS_READ_SEND_HEADER,
  OMS_READ_WAIT_DATA,
  OMS_READ_WAIT_ACK,
  OMS_READ_COMPLETE,
  OMS_READ_NUM_STATES,
};

static void
oms_data_cb (FpiUsbTransfer *transfer,
             FpDevice       *device,
             gpointer        user_data,
             GError         *error)
{
  FpiDeviceOmsMoc *self = FPI_DEVICE_OMS_MOC (device);
  gint final_state = GPOINTER_TO_INT (user_data);

  if (error)
    {
      fpi_ssm_mark_failed (transfer->ssm, error);
      return;
    }

  self->in_len = transfer->actual_length;

  /* The device may answer a read request with the ack alone, in which case
   * no separate USBS transfer follows. */
  if (self->in_len == OMS_USBS_SIZE &&
      memcmp (self->in_data, "USBS", OMS_USBS_SIZE) == 0)
    {
      self->in_len = 0;
      fpi_ssm_jump_to_state (transfer->ssm, final_state);
      return;
    }

  fpi_ssm_next_state (transfer->ssm);
}

static void
oms_cmd_data_cb (FpiUsbTransfer *transfer,
                 FpDevice       *device,
                 gpointer        user_data,
                 GError         *error)
{
  FpiDeviceOmsMoc *self = FPI_DEVICE_OMS_MOC (device);

  if (error)
    {
      fpi_ssm_mark_failed (transfer->ssm, error);
      return;
    }

  self->in_len = transfer->actual_length;

  if (self->in_len == OMS_USBS_SIZE &&
      memcmp (self->in_data, "USBS", OMS_USBS_SIZE) == 0)
    {
      self->in_len = 0;
      fpi_ssm_jump_to_state (transfer->ssm, OMS_CMD_COMPLETE);
      return;
    }

  if ((self->in_len == 0 || oms_data_is_empty (self->in_data, self->in_len)) &&
      self->read_retries++ < OMS_MAX_READ_RETRY)
    {
      /* The device was not ready to answer yet, ask again. */
      fpi_ssm_jump_to_state_delayed (transfer->ssm, OMS_CMD_SEND_READ_HEADER,
                                     OMS_READ_DELAY_MS);
      return;
    }

  fpi_ssm_next_state (transfer->ssm);
}

static void
oms_cmd_run_state (FpiSsm   *ssm,
                   FpDevice *device)
{
  FpiDeviceOmsMoc *self = FPI_DEVICE_OMS_MOC (device);
  OmsTransferData *data = fpi_ssm_get_data (ssm);
  FpiUsbTransfer *transfer;

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case OMS_CMD_SEND_HEADER:
      transfer = fpi_usb_transfer_new (device);
      transfer->ssm = ssm;
      transfer->short_is_error = TRUE;
      fpi_usb_transfer_fill_bulk_full (transfer, OMS_EP_OUT, self->usbc,
                                       OMS_USBC_SIZE, NULL);
      fpi_usb_transfer_submit (transfer, data->timeout_ms, NULL,
                               fpi_ssm_usb_transfer_cb, NULL);
      break;

    case OMS_CMD_SEND_FRAME:
      transfer = fpi_usb_transfer_new (device);
      transfer->ssm = ssm;
      transfer->short_is_error = TRUE;
      fpi_usb_transfer_fill_bulk_full (transfer, OMS_EP_OUT, self->out_frame,
                                       OMS_PACKET_SIZE, NULL);
      fpi_usb_transfer_submit (transfer, data->timeout_ms, NULL,
                               fpi_ssm_usb_transfer_cb, NULL);
      break;

    case OMS_CMD_WAIT_ACK:
      transfer = fpi_usb_transfer_new (device);
      transfer->ssm = ssm;
      transfer->short_is_error = TRUE;
      fpi_usb_transfer_fill_bulk_full (transfer, OMS_EP_IN, self->ack,
                                       OMS_USBS_SIZE, NULL);
      fpi_usb_transfer_submit (transfer, data->timeout_ms, NULL,
                               oms_usbs_cb, NULL);
      break;

    case OMS_CMD_READ_DELAY:
      if (data->window == 0)
        {
          fpi_ssm_jump_to_state (ssm, OMS_CMD_COMPLETE);
          break;
        }

      /* The device needs a moment to prepare the response; the vendor
       * driver waits about 64 ms before it requests it. */
      self->read_retries = 0;
      fpi_ssm_next_state_delayed (ssm, OMS_READ_DELAY_MS);
      break;

    case OMS_CMD_SEND_READ_HEADER:
      oms_fill_usbc (self->usbc, OMS_DIR_READ, data->window);
      transfer = fpi_usb_transfer_new (device);
      transfer->ssm = ssm;
      transfer->short_is_error = TRUE;
      fpi_usb_transfer_fill_bulk_full (transfer, OMS_EP_OUT, self->usbc,
                                       OMS_USBC_SIZE, NULL);
      fpi_usb_transfer_submit (transfer, data->timeout_ms, NULL,
                               fpi_ssm_usb_transfer_cb, NULL);
      break;

    case OMS_CMD_READ_DATA:
      self->in_len = 0;
      transfer = fpi_usb_transfer_new (device);
      transfer->ssm = ssm;
      transfer->short_is_error = FALSE;
      fpi_usb_transfer_fill_bulk_full (transfer, OMS_EP_IN, self->in_data,
                                       data->window, NULL);
      fpi_usb_transfer_submit (transfer, data->timeout_ms, NULL,
                               oms_cmd_data_cb, NULL);
      break;

    case OMS_CMD_READ_ACK:
      transfer = fpi_usb_transfer_new (device);
      transfer->ssm = ssm;
      transfer->short_is_error = TRUE;
      fpi_usb_transfer_fill_bulk_full (transfer, OMS_EP_IN, self->ack,
                                       OMS_USBS_SIZE, NULL);
      fpi_usb_transfer_submit (transfer, data->timeout_ms, NULL,
                               oms_usbs_cb, NULL);
      break;

    case OMS_CMD_COMPLETE:
      fpi_ssm_mark_completed (ssm);
      break;

    default:
      g_assert_not_reached ();
    }
}

static void
oms_read_run_state (FpiSsm   *ssm,
                    FpDevice *device)
{
  FpiDeviceOmsMoc *self = FPI_DEVICE_OMS_MOC (device);
  OmsTransferData *data = fpi_ssm_get_data (ssm);
  FpiUsbTransfer *transfer;

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case OMS_READ_SEND_HEADER:
      transfer = fpi_usb_transfer_new (device);
      transfer->ssm = ssm;
      transfer->short_is_error = TRUE;
      fpi_usb_transfer_fill_bulk_full (transfer, OMS_EP_OUT, self->usbc,
                                       OMS_USBC_SIZE, NULL);
      fpi_usb_transfer_submit (transfer, data->timeout_ms, NULL,
                               fpi_ssm_usb_transfer_cb, NULL);
      break;

    case OMS_READ_WAIT_DATA:
      self->in_len = 0;
      transfer = fpi_usb_transfer_new (device);
      transfer->ssm = ssm;
      transfer->short_is_error = FALSE;
      fpi_usb_transfer_fill_bulk_full (transfer, OMS_EP_IN, self->in_data,
                                       data->window, NULL);
      fpi_usb_transfer_submit (transfer, data->timeout_ms,
                               data->cancelable ? fpi_device_get_cancellable (device) : NULL,
                               oms_data_cb, GINT_TO_POINTER (OMS_READ_COMPLETE));
      break;

    case OMS_READ_WAIT_ACK:
      transfer = fpi_usb_transfer_new (device);
      transfer->ssm = ssm;
      transfer->short_is_error = TRUE;
      fpi_usb_transfer_fill_bulk_full (transfer, OMS_EP_IN, self->ack,
                                       OMS_USBS_SIZE, NULL);
      fpi_usb_transfer_submit (transfer, data->timeout_ms, NULL,
                               oms_usbs_cb, NULL);
      break;

    case OMS_READ_COMPLETE:
      fpi_ssm_mark_completed (ssm);
      break;

    default:
      g_assert_not_reached ();
    }
}

static void
oms_cmd_ssm_done (FpiSsm   *ssm,
                  FpDevice *device,
                  GError   *error)
{
  OmsTransferData *data = fpi_ssm_get_data (ssm);
  FpiDeviceOmsMoc *self = FPI_DEVICE_OMS_MOC (device);

  if (data->callback)
    data->callback (self, error);
}

static void
oms_read_ssm_done (FpiSsm   *ssm,
                   FpDevice *device,
                   GError   *error)
{
  OmsTransferData *data = fpi_ssm_get_data (ssm);
  FpiDeviceOmsMoc *self = FPI_DEVICE_OMS_MOC (device);

  if (data->callback)
    data->callback (self, error);
}

static void
oms_command (FpiDeviceOmsMoc *self,
             guint8           cmd,
             const guint8    *params,
             gsize            params_len,
             guint8           window,
             OmsCallback      callback)
{
  FpiSsm *ssm;
  OmsTransferData *data;

  oms_build_frame (self->out_frame, cmd, params, params_len);
  oms_fill_usbc (self->usbc, OMS_DIR_WRITE, OMS_PACKET_SIZE);

  data = g_new0 (OmsTransferData, 1);
  data->callback = callback;
  data->window = window;
  data->timeout_ms = OMS_CMD_TIMEOUT_MS;

  ssm = fpi_ssm_new (FP_DEVICE (self), oms_cmd_run_state, OMS_CMD_NUM_STATES);
  fpi_ssm_set_data (ssm, data, g_free);

  fpi_ssm_start (ssm, oms_cmd_ssm_done);
}

static void
oms_read (FpiDeviceOmsMoc *self,
          guint8           window,
          guint            timeout_ms,
          gboolean         cancelable,
          OmsCallback      callback)
{
  FpiSsm *ssm;
  OmsTransferData *data;

  g_return_if_fail (window <= OMS_PACKET_SIZE);

  oms_fill_usbc (self->usbc, OMS_DIR_READ, window);

  data = g_new0 (OmsTransferData, 1);
  data->callback = callback;
  data->window = window;
  data->timeout_ms = timeout_ms;
  data->cancelable = cancelable;

  ssm = fpi_ssm_new (FP_DEVICE (self), oms_read_run_state, OMS_READ_NUM_STATES);
  fpi_ssm_set_data (ssm, data, g_free);

  fpi_ssm_start (ssm, oms_read_ssm_done);
}

/* ------------------------------------------------------------------ *
 * Initialization
 * ------------------------------------------------------------------ */

enum {
  OMS_INIT_DRAIN,
  OMS_INIT_CHIP_SN,
  OMS_INIT_TEMPLATE_NUM,
  OMS_INIT_INDEX_TABLE,
  OMS_INIT_NUM_STATES,
};

static void
oms_init_cb (FpiDeviceOmsMoc *self,
             GError          *error)
{
  FpiSsm *ssm = self->task_ssm;
  const guint8 *payload;
  gsize payload_len;

  if (error)
    {
      /* A timeout while draining just means that there was nothing left. */
      if (fpi_ssm_get_cur_state (ssm) == OMS_INIT_DRAIN &&
          g_error_matches (error, G_USB_DEVICE_ERROR, G_USB_DEVICE_ERROR_TIMED_OUT))
        {
          g_error_free (error);
          fpi_ssm_next_state (ssm);
          return;
        }

      fpi_ssm_mark_failed (ssm, error);
      return;
    }

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case OMS_INIT_DRAIN:
      /* Discard whatever a previous session left in the device queue. */
      if (!oms_data_is_empty (self->in_data, self->in_len) &&
          self->read_retries++ < OMS_MAX_DRAIN_READS)
        {
          fp_dbg ("Draining stale device data");
          fpi_ssm_jump_to_state (ssm, OMS_INIT_DRAIN);
          return;
        }
      fpi_ssm_next_state (ssm);
      break;

    case OMS_INIT_CHIP_SN:
      if (oms_parse_frame (self->in_data, self->in_len, &payload, &payload_len) &&
          payload_len > 1)
        {
          g_autofree gchar *sn = NULL;

          /* [推测] The response mirrors 0x1f: a status byte followed by the
           * 32 byte chip serial number. */
          sn = g_strndup ((const gchar *) &payload[1], payload_len - 1);
          fp_info ("Chip serial: %s", sn);
        }
      else
        {
          fp_warn ("No chip serial number reported");
        }
      fpi_ssm_next_state (ssm);
      break;

    case OMS_INIT_TEMPLATE_NUM:
      if (!oms_parse_frame (self->in_data, self->in_len, &payload, &payload_len) ||
          payload_len < 3)
        {
          fpi_ssm_mark_failed (ssm,
                               fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                         "Failed to read the template count"));
          return;
        }

      self->nr_templates = payload[2];
      fp_info ("%d template(s) stored on the chip", self->nr_templates);
      fpi_ssm_next_state (ssm);
      break;

    case OMS_INIT_INDEX_TABLE:
      if (!oms_parse_frame (self->in_data, self->in_len, &payload, &payload_len) ||
          payload_len < 3)
        {
          fpi_ssm_mark_failed (ssm,
                               fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                         "Failed to read the index table"));
          return;
        }

      self->template_bitmap = payload[1] | (payload[2] << 8);
      fp_info ("Template bitmap: 0x%04x", self->template_bitmap);
      fpi_ssm_mark_completed (ssm);
      break;

    default:
      g_assert_not_reached ();
    }
}

static void
oms_init_run_state (FpiSsm   *ssm,
                    FpDevice *device)
{
  FpiDeviceOmsMoc *self = FPI_DEVICE_OMS_MOC (device);
  const guint8 p_zero = 0x00;

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case OMS_INIT_DRAIN:
      oms_read (self, OMS_PACKET_SIZE, 100, FALSE, oms_init_cb);
      break;

    case OMS_INIT_CHIP_SN:
      oms_command (self, OMS_CMD_CHIP_SN, &p_zero, 1, OMS_PACKET_SIZE, oms_init_cb);
      break;

    case OMS_INIT_TEMPLATE_NUM:
      oms_command (self, OMS_CMD_TEMPLATE_NUM, NULL, 0, OMS_PACKET_SIZE, oms_init_cb);
      break;

    case OMS_INIT_INDEX_TABLE:
      oms_command (self, OMS_CMD_INDEX_TABLE, &p_zero, 1, OMS_PACKET_SIZE, oms_init_cb);
      break;

    default:
      g_assert_not_reached ();
    }
}

/* ------------------------------------------------------------------ *
 * Print data helpers
 * ------------------------------------------------------------------ */

/* Templates live in 16 chip slots; the slot number is the only identity we
 * can store, as the chip does not keep any user metadata. */
#define OMS_NR_SLOTS 16

static GVariant *
oms_print_data_new (guint16 slot)
{
  return g_variant_new ("(q)", slot);
}

static gboolean
oms_print_data_get_slot (GVariant *data,
                         guint16  *slot)
{
  if (!data || !g_variant_check_format_string (data, "(q)", FALSE))
    return FALSE;

  g_variant_get (data, "(q)", slot);

  return TRUE;
}

static gboolean
oms_print_get_slot (FpPrint *print,
                    guint16 *slot)
{
  g_autoptr(GVariant) data = NULL;

  g_object_get (print, "fpi-data", &data, NULL);

  return oms_print_data_get_slot (data, slot);
}

static FpPrint *
oms_print_from_slot (FpiDeviceOmsMoc *self,
                     guint16          slot)
{
  FpPrint *print;
  g_autofree gchar *description = NULL;

  print = fp_print_new (FP_DEVICE (self));

  fpi_print_set_type (print, FPI_PRINT_RAW);
  fpi_print_set_device_stored (print, TRUE);
  g_object_set (print, "fpi-data", oms_print_data_new (slot), NULL);

  description = g_strdup_printf ("Slot %u", slot);
  g_object_set (print, "description", description, NULL);

  return print;
}

/* ------------------------------------------------------------------ *
 * Identify / verify
 * ------------------------------------------------------------------ */

enum {
  OMS_VERIFY_PRE_DRAIN,
  OMS_VERIFY_START,
  OMS_VERIFY_WAIT,
  OMS_VERIFY_CANCEL,
  OMS_VERIFY_DRAIN,
  OMS_VERIFY_NUM_STATES,
};

static void
oms_verify_cb (FpiDeviceOmsMoc *self,
               GError          *error)
{
  FpiSsm *ssm = self->task_ssm;
  const guint8 *payload;
  gsize payload_len;

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case OMS_VERIFY_PRE_DRAIN:
      /* Drop frames of a previous session before starting a new one. */
      if (error)
        {
          g_error_free (error);
          fpi_ssm_next_state (ssm);
          break;
        }

      if (!oms_data_is_empty (self->in_data, self->in_len) &&
          self->poll_count++ < OMS_MAX_DRAIN_READS)
        {
          fp_dbg ("Draining stale device data before the session");
          fpi_ssm_jump_to_state (ssm, OMS_VERIFY_PRE_DRAIN);
          break;
        }

      fpi_ssm_next_state (ssm);
      break;

    case OMS_VERIFY_START:
      if (error)
        {
          fpi_ssm_mark_failed (ssm, error);
          return;
        }
      fpi_ssm_next_state (ssm);
      break;

    case OMS_VERIFY_WAIT:
      if (error)
        {
          if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED) ||
              g_error_matches (error, G_USB_DEVICE_ERROR, G_USB_DEVICE_ERROR_CANCELLED))
            {
              self->action_error = g_steal_pointer (&error);
              fpi_ssm_jump_to_state (ssm, OMS_VERIFY_CANCEL);
              return;
            }

          /* No answer inside the poll window: keep polling. */
          if (g_error_matches (error, G_USB_DEVICE_ERROR, G_USB_DEVICE_ERROR_TIMED_OUT))
            {
              g_error_free (error);
              fpi_ssm_jump_to_state_delayed (ssm, OMS_VERIFY_WAIT, OMS_POLL_INTERVAL_MS);
              return;
            }

          fpi_ssm_mark_failed (ssm, error);
          return;
        }

      if (!oms_parse_frame (self->in_data, self->in_len, &payload, &payload_len) ||
          payload_len < 2)
        {
          /* Nothing (or nothing useful) to report yet. */
          fpi_ssm_jump_to_state_delayed (ssm, OMS_VERIFY_WAIT, OMS_POLL_INTERVAL_MS);
          return;
        }

      fp_dbg ("Identify payload: %02x %02x (%" G_GSIZE_FORMAT " bytes)",
              payload[0], payload[1], payload_len);

      if (payload[0] == OMS_ID_STATUS_OK && payload[1] == OMS_ID_RESULT_CAPTURED)
        {
          /* The finger image was captured; the chip reports the match
           * result in the next frame(s). */
          fpi_device_report_finger_status_changes (FP_DEVICE (self),
                                                   FP_FINGER_STATUS_PRESENT,
                                                   FP_FINGER_STATUS_NEEDED);
          fpi_ssm_jump_to_state_delayed (ssm, OMS_VERIFY_WAIT, OMS_POLL_INTERVAL_MS);
          return;
        }

      if (payload[0] == OMS_ID_STATUS_OK && payload[1] == OMS_ID_RESULT_MATCHED &&
          payload_len > OMS_ID_PAYLOAD_SLOT)
        {
          self->matched_slot = payload[OMS_ID_PAYLOAD_SLOT];
          self->result_received = TRUE;
          fp_info ("Chip matched template in slot %d", self->matched_slot);
          fpi_ssm_jump_to_state (ssm, OMS_VERIFY_CANCEL);
          return;
        }

      if (payload[0] == OMS_ID_STATUS_NOMATCH)
        {
          self->matched_slot = -1;
          self->result_received = TRUE;
          fp_dbg ("Chip found no matching template");
          fpi_ssm_jump_to_state (ssm, OMS_VERIFY_CANCEL);
          return;
        }

      /* Still waiting for a finger. */
      fpi_device_report_finger_status_changes (FP_DEVICE (self),
                                               FP_FINGER_STATUS_NEEDED,
                                               FP_FINGER_STATUS_NONE);
      fpi_ssm_jump_to_state_delayed (ssm, OMS_VERIFY_WAIT, OMS_POLL_INTERVAL_MS);
      break;

    case OMS_VERIFY_CANCEL:
      if (error)
        fp_dbg ("Cancelling the session failed: %s", error->message);
      self->poll_count = 0;
      fpi_ssm_next_state (ssm);
      break;

    case OMS_VERIFY_DRAIN:
      /* The chip may still report the end of the previous session; drop
       * those frames so that the next command starts from a clean queue. */
      if (error)
        {
          fp_dbg ("Drain read failed: %s", error->message);
          fpi_ssm_mark_completed (ssm);
          break;
        }

      if (!oms_data_is_empty (self->in_data, self->in_len) &&
          self->poll_count++ < OMS_MAX_DRAIN_READS)
        {
          fpi_ssm_jump_to_state (ssm, OMS_VERIFY_DRAIN);
          break;
        }

      fpi_ssm_mark_completed (ssm);
      break;

    default:
      g_assert_not_reached ();
    }
}

static void
oms_verify_run_state (FpiSsm   *ssm,
                      FpDevice *device)
{
  FpiDeviceOmsMoc *self = FPI_DEVICE_OMS_MOC (device);
  static const guint8 identify_params[] = { 0x05, 0xff, 0xff, 0x00, 0x00 };

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case OMS_VERIFY_PRE_DRAIN:
      self->poll_count = 0;
      oms_read (self, OMS_PACKET_SIZE, OMS_POLL_TIMEOUT_MS, FALSE, oms_verify_cb);
      break;

    case OMS_VERIFY_START:
      fpi_device_report_finger_status_changes (device,
                                               FP_FINGER_STATUS_NEEDED,
                                               FP_FINGER_STATUS_NONE);
      oms_command (self, OMS_CMD_IDENTIFY, identify_params,
                   G_N_ELEMENTS (identify_params), 0, oms_verify_cb);
      break;

    case OMS_VERIFY_WAIT:
      oms_read (self, OMS_POLL_SIZE, OMS_POLL_TIMEOUT_MS, TRUE, oms_verify_cb);
      break;

    case OMS_VERIFY_CANCEL:
      fpi_device_report_finger_status_changes (device,
                                               FP_FINGER_STATUS_NONE,
                                               FP_FINGER_STATUS_NEEDED);
      oms_command (self, OMS_CMD_CANCEL, NULL, 0, 0, oms_verify_cb);
      break;

    case OMS_VERIFY_DRAIN:
      oms_read (self, OMS_PACKET_SIZE, OMS_POLL_TIMEOUT_MS, FALSE, oms_verify_cb);
      break;

    default:
      g_assert_not_reached ();
    }
}

static void
oms_identify_ssm_done (FpiSsm   *ssm,
                       FpDevice *device,
                       GError   *error)
{
  FpiDeviceOmsMoc *self = FPI_DEVICE_OMS_MOC (device);
  FpPrint *match = NULL;
  GPtrArray *gallery = NULL;
  guint i;

  self->task_ssm = NULL;

  if (self->action_error)
    {
      fpi_device_identify_complete (device, g_steal_pointer (&self->action_error));
      return;
    }

  if (error)
    {
      fpi_device_identify_complete (device, error);
      return;
    }

  if (self->matched_slot >= 0)
    {
      fpi_device_get_identify_data (device, &gallery);

      for (i = 0; i < gallery->len; i++)
        {
          FpPrint *print = g_ptr_array_index (gallery, i);
          guint16 slot;

          if (oms_print_get_slot (print, &slot) && slot == self->matched_slot)
            {
              match = print;
              break;
            }
        }

      if (!match)
        fp_warn ("The chip matched slot %d, which is not part of the gallery",
                 self->matched_slot);
    }

  fpi_device_identify_report (device, match, NULL, NULL);
  fpi_device_identify_complete (device, NULL);
}

static void
oms_identify (FpDevice *device)
{
  FpiDeviceOmsMoc *self = FPI_DEVICE_OMS_MOC (device);

  self->matched_slot = -1;
  self->result_received = FALSE;
  self->session_retries = 0;
  g_clear_pointer (&self->action_error, g_error_free);

  self->task_ssm = fpi_ssm_new (device, oms_verify_run_state,
                                OMS_VERIFY_NUM_STATES);
  fpi_ssm_start (self->task_ssm, oms_identify_ssm_done);
}

static void
oms_verify_ssm_done (FpiSsm   *ssm,
                     FpDevice *device,
                     GError   *error)
{
  FpiDeviceOmsMoc *self = FPI_DEVICE_OMS_MOC (device);
  FpPrint *enrolled_print = NULL;
  guint16 slot = 0;

  self->task_ssm = NULL;

  if (self->action_error)
    {
      fpi_device_verify_complete (device, g_steal_pointer (&self->action_error));
      return;
    }

  if (error)
    {
      fpi_device_verify_complete (device, error);
      return;
    }

  fpi_device_get_verify_data (device, &enrolled_print);

  /* Only a match in the slot of the requested print counts as a success;
   * the chip searches all its templates, so another enrolled finger would
   * otherwise be reported as a match. */
  if (self->matched_slot >= 0 && oms_print_get_slot (enrolled_print, &slot) &&
      slot == self->matched_slot)
    fpi_device_verify_report (device, FPI_MATCH_SUCCESS, NULL, NULL);
  else
    fpi_device_verify_report (device, FPI_MATCH_FAIL, NULL, NULL);

  fpi_device_verify_complete (device, NULL);
}

static void
oms_verify (FpDevice *device)
{
  FpiDeviceOmsMoc *self = FPI_DEVICE_OMS_MOC (device);
  FpPrint *enrolled_print = NULL;
  guint16 slot = 0;

  fpi_device_get_verify_data (device, &enrolled_print);

  if (!oms_print_get_slot (enrolled_print, &slot))
    {
      fpi_device_verify_complete (device,
                                  fpi_device_error_new (FP_DEVICE_ERROR_DATA_INVALID));
      return;
    }

  self->matched_slot = -1;
  self->result_received = FALSE;
  self->session_retries = 0;
  g_clear_pointer (&self->action_error, g_error_free);

  self->task_ssm = fpi_ssm_new (device, oms_verify_run_state,
                                OMS_VERIFY_NUM_STATES);
  fpi_ssm_start (self->task_ssm, oms_verify_ssm_done);
}

/* ------------------------------------------------------------------ *
 * Enrollment
 * ------------------------------------------------------------------ */

enum {
  OMS_ENROLL_CAPTURE,
  OMS_ENROLL_CAPTURE_POLL,
  OMS_ENROLL_SLOT,
  OMS_ENROLL_BUILD,
  OMS_ENROLL_PROGRESS,
  OMS_ENROLL_CONFIRM_NUM,
  OMS_ENROLL_CONFIRM_TABLE,
  OMS_ENROLL_CANCEL,
  OMS_ENROLL_NUM_STATES,
};

static gboolean
oms_find_free_slot (FpiDeviceOmsMoc *self,
                    guint8          *slot)
{
  guint i;

  for (i = 0; i < OMS_NR_SLOTS; i++)
    {
      if (!(self->template_bitmap & (1 << i)))
        {
          *slot = i;
          return TRUE;
        }
    }

  return FALSE;
}

static void
oms_enroll_ssm_done (FpiSsm   *ssm,
                     FpDevice *device,
                     GError   *error)
{
  FpiDeviceOmsMoc *self = FPI_DEVICE_OMS_MOC (device);
  FpPrint *print = NULL;

  self->task_ssm = NULL;

  fpi_device_report_finger_status_changes (device,
                                           FP_FINGER_STATUS_NONE,
                                           FP_FINGER_STATUS_PRESENT |
                                           FP_FINGER_STATUS_NEEDED);

  if (self->action_error)
    {
      fpi_device_enroll_complete (device, NULL, g_steal_pointer (&self->action_error));
      return;
    }

  if (error)
    {
      fpi_device_enroll_complete (device, NULL, error);
      return;
    }

  fpi_device_get_enroll_data (device, &print);
  fp_dbg ("Enroll done for slot %u, print %p", self->enroll_slot, print);
  fpi_print_set_type (print, FPI_PRINT_RAW);
  fpi_print_set_device_stored (print, TRUE);
  g_object_set (print, "fpi-data", oms_print_data_new (self->enroll_slot), NULL);

  fp_info ("Enrollment complete, template stored in slot %u", self->enroll_slot);

  fpi_device_enroll_complete (device, g_object_ref (print), NULL);
  fp_dbg ("Enrollment completion reported");
}

static void
oms_enroll_cb (FpiDeviceOmsMoc *self,
               GError          *error)
{
  FpiSsm *ssm = self->task_ssm;
  const guint8 *payload;
  gsize payload_len;
  guint8 status, result;

  if (error)
    {
      if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED) ||
          g_error_matches (error, G_USB_DEVICE_ERROR, G_USB_DEVICE_ERROR_CANCELLED))
        {
          self->action_error = g_steal_pointer (&error);
          fpi_ssm_jump_to_state (ssm, OMS_ENROLL_CANCEL);
          return;
        }

      if (g_error_matches (error, G_USB_DEVICE_ERROR, G_USB_DEVICE_ERROR_TIMED_OUT))
        {
          g_error_free (error);
          error = NULL;
        }
      else
        {
          fpi_ssm_mark_failed (ssm, error);
          return;
        }
    }

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case OMS_ENROLL_CAPTURE:
      fpi_ssm_next_state (ssm);
      break;

    case OMS_ENROLL_CAPTURE_POLL:
      if (!oms_parse_frame (self->in_data, self->in_len, &payload, &payload_len) ||
          payload_len < 2)
        {
          if (self->poll_count++ > OMS_MAX_CAPTURE_POLLS)
            {
              fpi_ssm_mark_failed (ssm,
                                   fpi_device_retry_new (FP_DEVICE_RETRY_GENERAL));
              return;
            }
          fpi_ssm_jump_to_state_delayed (ssm, OMS_ENROLL_CAPTURE_POLL, OMS_POLL_INTERVAL_MS);
          return;
        }

      status = payload[0];
      result = payload[1];

      if (status == OMS_ID_STATUS_OK && result == OMS_ID_RESULT_CAPTURED)
        {
          fp_dbg ("Finger captured, waiting for the search result");
          self->captured = TRUE;
          self->poll_count = 0;
          fpi_device_report_finger_status_changes (FP_DEVICE (self),
                                                   FP_FINGER_STATUS_PRESENT,
                                                   FP_FINGER_STATUS_NEEDED);
          fpi_ssm_jump_to_state_delayed (ssm, OMS_ENROLL_CAPTURE_POLL, OMS_POLL_INTERVAL_MS);
          return;
        }

      /* The chip always searches its templates first: a match means that
       * this finger is already enrolled and must not be added again. */
      if (status == OMS_ID_STATUS_OK && result == OMS_ID_RESULT_MATCHED &&
          payload_len > OMS_ID_PAYLOAD_SLOT)
        {
          fpi_ssm_mark_failed (ssm,
                               fpi_device_error_new_msg (FP_DEVICE_ERROR_DATA_DUPLICATE,
                                                         "Finger is already enrolled in slot %u",
                                                         payload[OMS_ID_PAYLOAD_SLOT]));
          return;
        }

      if (status == OMS_ID_STATUS_NOMATCH)
        {
          if (!self->captured)
            {
              /* The chip gave up without an image, ask for another press. */
              if (self->capture_retries++ < OMS_MAX_SESSION_RETRY)
                {
                  fpi_ssm_jump_to_state_delayed (ssm, OMS_ENROLL_CAPTURE, OMS_POLL_INTERVAL_MS);
                  return;
                }

              fpi_ssm_mark_failed (ssm,
                                   fpi_device_retry_new (FP_DEVICE_RETRY_CENTER_FINGER));
              return;
            }

          fpi_ssm_next_state (ssm);
          return;
        }

      /* Not ready yet, or still capturing. */
      if (self->captured && self->poll_count++ > OMS_MAX_CAPTURE_END_POLLS)
        {
          fp_dbg ("No capture result notification, continuing anyway");
          fpi_ssm_next_state (ssm);
          return;
        }

      fpi_ssm_jump_to_state_delayed (ssm, OMS_ENROLL_CAPTURE_POLL, OMS_POLL_INTERVAL_MS);
      break;

    case OMS_ENROLL_SLOT:
      if (!oms_parse_frame (self->in_data, self->in_len, &payload, &payload_len) ||
          payload_len < 3)
        {
          fpi_ssm_mark_failed (ssm,
                               fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                         "Failed to read the index table"));
          return;
        }

      self->template_bitmap = payload[1] | (payload[2] << 8);

      if (!oms_find_free_slot (self, &self->enroll_slot))
        {
          fpi_ssm_mark_failed (ssm, fpi_device_error_new (FP_DEVICE_ERROR_DATA_FULL));
          return;
        }

      fp_info ("Enrolling into slot %u", self->enroll_slot);
      fpi_ssm_next_state (ssm);
      break;

    case OMS_ENROLL_BUILD:
      self->enroll_level = 0;
      self->poll_count = 0;
      fpi_ssm_next_state (ssm);
      break;

    case OMS_ENROLL_PROGRESS:
      if (!oms_parse_frame (self->in_data, self->in_len, &payload, &payload_len) ||
          payload_len < 3)
        {
          if (self->poll_count++ > OMS_MAX_ENROLL_POLLS)
            {
              fpi_ssm_mark_failed (ssm,
                                   fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                             "Enrollment timed out"));
              return;
            }
          fpi_ssm_jump_to_state_delayed (ssm, OMS_ENROLL_PROGRESS, OMS_POLL_INTERVAL_MS);
          return;
        }

      fp_dbg ("Enroll progress: %02x %02x %02x", payload[0], payload[1], payload[2]);

      if (payload[1] >= 1 && payload[1] <= 3 &&
          payload[2] >= 1 && payload[2] <= OMS_ENROLL_STAGES &&
          payload[2] > self->enroll_level)
        {
          self->enroll_level = payload[2];
          fpi_device_enroll_progress (FP_DEVICE (self), self->enroll_level, NULL, NULL);

          /* Each level needs a new finger press, ask for the next one. */
          fpi_device_report_finger_status_changes (FP_DEVICE (self),
                                                   FP_FINGER_STATUS_NEEDED,
                                                   FP_FINGER_STATUS_PRESENT);
        }
      else
        {
          fpi_device_report_finger_status_changes (FP_DEVICE (self),
                                                   FP_FINGER_STATUS_PRESENT,
                                                   FP_FINGER_STATUS_NONE);
        }

      if (payload[1] == OMS_ENROLL_TRAILER_INDEX)
        {
          fpi_ssm_next_state (ssm);
          return;
        }

      fpi_ssm_jump_to_state_delayed (ssm, OMS_ENROLL_PROGRESS, OMS_POLL_INTERVAL_MS);
      break;

    case OMS_ENROLL_CONFIRM_NUM:
      if (!oms_parse_frame (self->in_data, self->in_len, &payload, &payload_len) ||
          payload_len < 3)
        {
          fpi_ssm_mark_failed (ssm,
                               fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                         "Failed to verify the enrollment"));
          return;
        }

      self->nr_templates = payload[2];
      fpi_ssm_next_state (ssm);
      break;

    case OMS_ENROLL_CONFIRM_TABLE:
      if (!oms_parse_frame (self->in_data, self->in_len, &payload, &payload_len) ||
          payload_len < 3)
        {
          fpi_ssm_mark_failed (ssm,
                               fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                         "Failed to verify the enrollment"));
          return;
        }

      self->template_bitmap = payload[1] | (payload[2] << 8);

      if (!(self->template_bitmap & (1 << self->enroll_slot)))
        {
          fpi_ssm_mark_failed (ssm,
                               fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                         "The chip did not store the template"));
          return;
        }

      fpi_ssm_mark_completed (ssm);
      break;

    case OMS_ENROLL_CANCEL:
      fpi_ssm_mark_completed (ssm);
      break;

    default:
      g_assert_not_reached ();
    }
}

static void
oms_enroll_run_state (FpiSsm   *ssm,
                      FpDevice *device)
{
  FpiDeviceOmsMoc *self = FPI_DEVICE_OMS_MOC (device);
  static const guint8 identify_params[] = { 0x05, 0xff, 0xff, 0x00, 0x00 };
  const guint8 p_zero = 0x00;
  guint8 build_params[] = { 0x00, 0x00, OMS_ENROLL_STAGES, 0x00, 0x00 };

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case OMS_ENROLL_CAPTURE:
      self->poll_count = 0;
      fpi_device_report_finger_status_changes (device,
                                               FP_FINGER_STATUS_NEEDED,
                                               FP_FINGER_STATUS_NONE);
      oms_command (self, OMS_CMD_IDENTIFY, identify_params,
                   G_N_ELEMENTS (identify_params), 0, oms_enroll_cb);
      break;

    case OMS_ENROLL_CAPTURE_POLL:
      oms_read (self, OMS_POLL_SIZE, OMS_POLL_TIMEOUT_MS, TRUE, oms_enroll_cb);
      break;

    case OMS_ENROLL_SLOT:
      oms_command (self, OMS_CMD_INDEX_TABLE, &p_zero, 1, OMS_PACKET_SIZE, oms_enroll_cb);
      break;

    case OMS_ENROLL_BUILD:
      build_params[1] = self->enroll_slot;
      oms_command (self, OMS_CMD_ENROLL, build_params,
                   G_N_ELEMENTS (build_params), 0, oms_enroll_cb);
      break;

    case OMS_ENROLL_PROGRESS:
      oms_read (self, OMS_POLL_SIZE, OMS_POLL_TIMEOUT_MS, TRUE, oms_enroll_cb);
      break;

    case OMS_ENROLL_CONFIRM_NUM:
      oms_command (self, OMS_CMD_TEMPLATE_NUM, NULL, 0, OMS_PACKET_SIZE, oms_enroll_cb);
      break;

    case OMS_ENROLL_CONFIRM_TABLE:
      oms_command (self, OMS_CMD_INDEX_TABLE, &p_zero, 1, OMS_PACKET_SIZE, oms_enroll_cb);
      break;

    case OMS_ENROLL_CANCEL:
      oms_command (self, OMS_CMD_CANCEL, NULL, 0, 0, oms_enroll_cb);
      break;

    default:
      g_assert_not_reached ();
    }
}

static void
oms_enroll (FpDevice *device)
{
  FpiDeviceOmsMoc *self = FPI_DEVICE_OMS_MOC (device);

  self->captured = FALSE;
  self->enroll_slot = 0;
  self->enroll_level = 0;
  self->capture_retries = 0;
  g_clear_pointer (&self->action_error, g_error_free);

  self->task_ssm = fpi_ssm_new (device, oms_enroll_run_state, OMS_ENROLL_NUM_STATES);
  fpi_ssm_start (self->task_ssm, oms_enroll_ssm_done);
}

/* ------------------------------------------------------------------ *
 * Delete / list
 * ------------------------------------------------------------------ */

enum {
  OMS_DELETE_SEND,
  OMS_DELETE_COMPLETE,
  OMS_DELETE_NUM_STATES,
};

static void
oms_delete_cb (FpiDeviceOmsMoc *self,
               GError          *error)
{
  FpiSsm *ssm = self->task_ssm;
  const guint8 *payload;
  gsize payload_len;

  if (error)
    {
      fpi_ssm_mark_failed (ssm, error);
      return;
    }

  if (fpi_ssm_get_cur_state (ssm) == OMS_DELETE_SEND)
    {
      if (oms_parse_frame (self->in_data, self->in_len, &payload, &payload_len))
        fp_dbg ("Delete response: %02x", payload[0]);
      fpi_ssm_next_state (ssm);
      return;
    }

  fpi_ssm_mark_completed (ssm);
}

static void
oms_delete_run_state (FpiSsm   *ssm,
                      FpDevice *device)
{
  FpiDeviceOmsMoc *self = FPI_DEVICE_OMS_MOC (device);
  guint8 params[] = { 0x00, 0x00, 0x00, 0x01 };

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case OMS_DELETE_SEND:
      params[1] = self->enroll_slot;
      fp_info ("Deleting template in slot %u", self->enroll_slot);
      oms_command (self, OMS_CMD_TEMPLATE_OP, params, G_N_ELEMENTS (params),
                   OMS_PACKET_SIZE, oms_delete_cb);
      break;

    case OMS_DELETE_COMPLETE:
      fpi_ssm_mark_completed (ssm);
      break;

    default:
      g_assert_not_reached ();
    }
}

static void
oms_delete_ssm_done (FpiSsm   *ssm,
                     FpDevice *device,
                     GError   *error)
{
  FpiDeviceOmsMoc *self = FPI_DEVICE_OMS_MOC (device);

  self->task_ssm = NULL;
  fpi_device_delete_complete (device, error);
}

static void
oms_delete (FpDevice *device)
{
  FpiDeviceOmsMoc *self = FPI_DEVICE_OMS_MOC (device);
  FpPrint *print = NULL;
  g_autoptr(GVariant) data = NULL;
  guint16 slot;

  fpi_device_get_delete_data (device, &print);
  g_object_get (print, "fpi-data", &data, NULL);

  if (!oms_print_data_get_slot (data, &slot))
    {
      fpi_device_delete_complete (device,
                                  fpi_device_error_new (FP_DEVICE_ERROR_DATA_INVALID));
      return;
    }

  self->enroll_slot = slot;
  self->task_ssm = fpi_ssm_new (device, oms_delete_run_state, OMS_DELETE_NUM_STATES);
  fpi_ssm_start (self->task_ssm, oms_delete_ssm_done);
}

enum {
  OMS_CLEAR_SEND,
  OMS_CLEAR_COMPLETE,
  OMS_CLEAR_NUM_STATES,
};

static void
oms_clear_cb (FpiDeviceOmsMoc *self,
              GError          *error)
{
  FpiSsm *ssm = self->task_ssm;

  if (error)
    {
      fpi_ssm_mark_failed (ssm, error);
      return;
    }

  if (fpi_ssm_get_cur_state (ssm) == OMS_CLEAR_SEND)
    {
      fpi_ssm_next_state (ssm);
      return;
    }

  fpi_ssm_mark_completed (ssm);
}

static void
oms_clear_run_state (FpiSsm   *ssm,
                     FpDevice *device)
{
  FpiDeviceOmsMoc *self = FPI_DEVICE_OMS_MOC (device);
  static const guint8 params[] = { 0x00, 0x00, 0x00, 0x1e };

  switch (fpi_ssm_get_cur_state (ssm))
    {
    case OMS_CLEAR_SEND:
      fp_info ("Clearing the template storage");
      oms_command (self, OMS_CMD_TEMPLATE_OP, params, G_N_ELEMENTS (params),
                   OMS_PACKET_SIZE, oms_clear_cb);
      break;

    case OMS_CLEAR_COMPLETE:
      fpi_ssm_mark_completed (ssm);
      break;

    default:
      g_assert_not_reached ();
    }
}

static void
oms_clear_ssm_done (FpiSsm   *ssm,
                    FpDevice *device,
                    GError   *error)
{
  FpiDeviceOmsMoc *self = FPI_DEVICE_OMS_MOC (device);

  self->task_ssm = NULL;

  if (!error)
    {
      self->template_bitmap = 0;
      self->nr_templates = 0;
    }

  fpi_device_clear_storage_complete (device, error);
}

static void
oms_clear_storage (FpDevice *device)
{
  FpiDeviceOmsMoc *self = FPI_DEVICE_OMS_MOC (device);

  self->task_ssm = fpi_ssm_new (device, oms_clear_run_state, OMS_CLEAR_NUM_STATES);
  fpi_ssm_start (self->task_ssm, oms_clear_ssm_done);
}

static void
oms_list_cb (FpiDeviceOmsMoc *self,
             GError          *error)
{
  const guint8 *payload;
  gsize payload_len;
  g_autoptr(GPtrArray) gallery = NULL;
  guint i;

  if (error)
    {
      fpi_device_list_complete (FP_DEVICE (self), NULL, error);
      self->task_ssm = NULL;
      return;
    }

  if (!oms_parse_frame (self->in_data, self->in_len, &payload, &payload_len) ||
      payload_len < 3)
    {
      fpi_device_list_complete (FP_DEVICE (self), NULL,
                                fpi_device_error_new_msg (FP_DEVICE_ERROR_PROTO,
                                                          "Failed to read the index table"));
      self->task_ssm = NULL;
      return;
    }

  self->template_bitmap = payload[1] | (payload[2] << 8);
  self->nr_templates = 0;

  gallery = g_ptr_array_new_with_free_func (g_object_unref);

  for (i = 0; i < OMS_NR_SLOTS; i++)
    {
      if (self->template_bitmap & (1 << i))
        {
          self->nr_templates++;
          g_ptr_array_add (gallery, g_object_ref_sink (oms_print_from_slot (self, i)));
        }
    }

  fp_info ("%u template(s) in the chip storage", self->nr_templates);

  self->task_ssm = NULL;
  fpi_device_list_complete (FP_DEVICE (self), g_steal_pointer (&gallery), NULL);
}

static void
oms_list_run_state (FpiSsm   *ssm,
                    FpDevice *device)
{
  FpiDeviceOmsMoc *self = FPI_DEVICE_OMS_MOC (device);
  const guint8 p_zero = 0x00;

  oms_command (self, OMS_CMD_INDEX_TABLE, &p_zero, 1, OMS_PACKET_SIZE, oms_list_cb);
}

static void
oms_list_ssm_done (FpiSsm   *ssm,
                   FpDevice *device,
                   GError   *error)
{
  FpiDeviceOmsMoc *self = FPI_DEVICE_OMS_MOC (device);

  self->task_ssm = NULL;

  if (error)
    fpi_device_list_complete (device, NULL, error);
}

static void
oms_list (FpDevice *device)
{
  FpiDeviceOmsMoc *self = FPI_DEVICE_OMS_MOC (device);

  self->task_ssm = fpi_ssm_new (device, oms_list_run_state, 1);
  fpi_ssm_start (self->task_ssm, oms_list_ssm_done);
}

/* ------------------------------------------------------------------ *
 * Device lifecycle
 * ------------------------------------------------------------------ */

static void
oms_probe (FpDevice *device)
{
  g_autoptr(GError) error = NULL;
  g_autofree gchar *serial = NULL;
  GUsbDevice *usb_dev = fpi_device_get_usb_device (device);

  if (!g_usb_device_open (usb_dev, &error))
    {
      fpi_device_probe_complete (device, NULL, NULL, g_steal_pointer (&error));
      return;
    }

  if (!g_usb_device_claim_interface (usb_dev, 0, 0, &error))
    {
      g_usb_device_close (usb_dev, NULL);
      fpi_device_probe_complete (device, NULL, NULL, g_steal_pointer (&error));
      return;
    }

  serial = g_usb_device_get_string_descriptor (
    usb_dev, g_usb_device_get_serial_number_index (usb_dev), &error);
  g_usb_device_release_interface (usb_dev, 0, 0, NULL);
  g_usb_device_close (usb_dev, NULL);

  if (serial)
    fp_info ("Device serial: %s", serial);

  fpi_device_probe_complete (device, serial, NULL, NULL);
}

static void
oms_init_ssm_done (FpiSsm   *ssm,
                   FpDevice *device,
                   GError   *error)
{
  FpiDeviceOmsMoc *self = FPI_DEVICE_OMS_MOC (device);

  self->task_ssm = NULL;
  fpi_device_open_complete (device, error);
}

static void
oms_open (FpDevice *device)
{
  FpiDeviceOmsMoc *self = FPI_DEVICE_OMS_MOC (device);
  g_autoptr(GError) error = NULL;
  GUsbDevice *usb_dev = fpi_device_get_usb_device (device);

  if (!g_usb_device_claim_interface (usb_dev, 0, 0, &error))
    {
      fpi_device_open_complete (device, g_steal_pointer (&error));
      return;
    }

  fpi_device_set_nr_enroll_stages (device, OMS_ENROLL_STAGES);

  self->read_retries = 0;
  self->task_ssm = fpi_ssm_new (device, oms_init_run_state, OMS_INIT_NUM_STATES);
  fpi_ssm_start (self->task_ssm, oms_init_ssm_done);
}

static void
oms_close_ssm_done (FpiSsm   *ssm,
                    FpDevice *device,
                    GError   *error)
{
  GUsbDevice *usb_dev = fpi_device_get_usb_device (device);
  g_autoptr(GError) release_error = NULL;

  g_usb_device_release_interface (usb_dev, 0, 0, &release_error);

  if (error == NULL)
    error = g_steal_pointer (&release_error);

  fpi_device_close_complete (device, error);
}

static void
oms_close_cb (FpiDeviceOmsMoc *self,
              GError          *error)
{
  if (error)
    fp_dbg ("Cancelling the session failed: %s", error->message);

  fpi_ssm_mark_completed (self->task_ssm);
}

static void
oms_close_run_state (FpiSsm   *ssm,
                     FpDevice *device)
{
  FpiDeviceOmsMoc *self = FPI_DEVICE_OMS_MOC (device);

  oms_command (self, OMS_CMD_CANCEL, NULL, 0, 0, oms_close_cb);
}

static void
oms_close (FpDevice *device)
{
  FpiDeviceOmsMoc *self = FPI_DEVICE_OMS_MOC (device);

  g_clear_error (&self->action_error);

  self->task_ssm = fpi_ssm_new (device, oms_close_run_state, 1);
  fpi_ssm_start (self->task_ssm, oms_close_ssm_done);
}

/* ------------------------------------------------------------------ *
 * GObject boilerplate
 * ------------------------------------------------------------------ */

static const FpIdEntry id_table[] = {
  { .vid = OMS_VENDOR_ID, .pid = OMS_PRODUCT_ID },
  { .vid = 0, .pid = 0, .driver_data = 0 },     /* terminating entry */
};

static void
fpi_device_omsmoc_init (FpiDeviceOmsMoc *self)
{
}

static void
fpi_device_omsmoc_class_init (FpiDeviceOmsMocClass *klass)
{
  FpDeviceClass *dev_class = FP_DEVICE_CLASS (klass);

  dev_class->id = FP_COMPONENT;
  dev_class->full_name = "OMS Match-on-Chip Fingerprint Sensor";
  dev_class->type = FP_DEVICE_TYPE_USB;
  dev_class->scan_type = FP_SCAN_TYPE_PRESS;
  dev_class->id_table = id_table;
  dev_class->nr_enroll_stages = OMS_ENROLL_STAGES;
  dev_class->temp_hot_seconds = -1;

  dev_class->probe = oms_probe;
  dev_class->open = oms_open;
  dev_class->close = oms_close;
  dev_class->enroll = oms_enroll;
  dev_class->identify = oms_identify;
  dev_class->verify = oms_verify;
  dev_class->list = oms_list;
  dev_class->delete = oms_delete;
  dev_class->clear_storage = oms_clear_storage;

  fpi_device_class_auto_initialize_features (dev_class);

  /* The chip searches all its templates before an enrollment, so an
   * already enrolled finger can be detected. */
  dev_class->features |= FP_DEVICE_FEATURE_DUPLICATES_CHECK;
}
