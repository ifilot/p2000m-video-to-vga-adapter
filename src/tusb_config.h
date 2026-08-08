/*
 * SPDX-FileCopyrightText: 2026 Ivo Filot <ivo@ivofilot.nl>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef P2000M_TUSB_CONFIG_H
#define P2000M_TUSB_CONFIG_H

/* TinyUSB runs through pico_stdio_usb's low-priority worker on core 0. */
#define CFG_TUD_ENABLED 1
#define CFG_TUSB_RHPORT0_MODE OPT_MODE_DEVICE

#ifndef CFG_TUD_ENDPOINT0_SIZE
#define CFG_TUD_ENDPOINT0_SIZE 64
#endif

#define CFG_TUD_CDC 1
#define CFG_TUD_MSC 0
#define CFG_TUD_HID 0
#define CFG_TUD_MIDI 0
#define CFG_TUD_VENDOR 0

#ifndef CFG_TUD_CDC_RX_BUFSIZE
#define CFG_TUD_CDC_RX_BUFSIZE 256
#endif

#ifndef CFG_TUD_CDC_TX_BUFSIZE
#define CFG_TUD_CDC_TX_BUFSIZE 4096
#endif

/* Use TinyUSB's proven full-speed CDC packet-sized endpoint buffers. */
#define CFG_TUD_CDC_EP_BUFSIZE 64

#endif
