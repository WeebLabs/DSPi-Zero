#pragma once

// USB loader protocol, shared with tools/pizero_load.py. Vendor requests go to the device recipient.
// An image is sent as LOAD_BEGIN, then the image bytes on bulk OUT EP 0x01, then GO once STATUS shows
// every byte received. GO checks the CRC and starts the image at 0x8000.

#define LOADER_VID              0xCAFE
#define LOADER_PID              0x5A4C      // "ZL"

#define LOADER_REQ_LOAD_BEGIN   0x01        // OUT, 8 bytes: u32 size, u32 CRC-32 (zlib)
#define LOADER_REQ_STATUS       0x02        // IN, 16 bytes: u32 state, size, received, max size
#define LOADER_REQ_GO           0x03        // OUT, no data. Stalls unless complete and the CRC matches
#define LOADER_REQ_REBOOT       0x04        // OUT, no data. Watchdog reset

#define LOADER_STATE_IDLE       0
#define LOADER_STATE_RECEIVING  1
#define LOADER_STATE_COMPLETE   2
#define LOADER_STATE_CRC_BAD    3
#define LOADER_STATE_TOO_BIG    4
#define LOADER_STATE_STARTING   5

#define LOADER_IMAGE_BASE       0x8000u
#define LOADER_IMAGE_MAX        (0x2000000u - LOADER_IMAGE_BASE)   // up to the loader itself

// Images built on the platform answer this to drop back to the loader (watchdog reset). It is one of
// the codes DSPi's own command set leaves free.
#define APP_REQ_REBOOT_TO_LOADER 0x40

// The loader passes this in r3 when it starts an image, so startup.S can skip the sign-of-life flicker.
#define LOADER_LAUNCH_MAGIC     0x4C4F4144u     // "LOAD"

// A user's card carries the loader as a second file at LOADER_BASE beside DSPi (boot/config_card.txt). An
// image asked to drop to the loader leaves this marker, and its complement in the next word, in RAM,
// which the watchdog reset keeps. startup.S finds it on the restart and hands over to the loader before
// touching any hardware. Images must stay below LOADER_HANDOFF_ADDR.
#define LOADER_BASE             0x02000000u
#define LOADER_HANDOFF_ADDR     0x01F00000u
#define LOADER_HANDOFF_MAGIC    0x48414E44u     // "HAND"

