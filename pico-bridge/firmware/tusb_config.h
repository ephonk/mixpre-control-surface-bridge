#ifndef _TUSB_CONFIG_H_
#define _TUSB_CONFIG_H_

#ifdef __cplusplus
extern "C" {
#endif

//--------------------------------------------------------------------+
// COMMON CONFIGURATION
//--------------------------------------------------------------------+

#define CFG_TUSB_OS               OPT_OS_PICO

// Device stack (native USB -> MixPre)
#define CFG_TUD_ENABLED     1

// Host stack (PIO USB -> Icon)
#define CFG_TUH_ENABLED     1
#define CFG_TUH_RPI_PIO_USB 1

#ifndef CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_SECTION
#endif

#ifndef CFG_TUSB_MEM_ALIGN
#define CFG_TUSB_MEM_ALIGN          __attribute__ ((aligned(4)))
#endif

//--------------------------------------------------------------------+
// DEVICE CONFIGURATION
//--------------------------------------------------------------------+

#ifndef CFG_TUD_ENDPOINT0_SIZE
// Must match the real nanoKONTROL Studio: ioreg reports bMaxPacketSize0 = 64 for
// the live device, and the official firmware declares 0x40 in both the normal
// (PID 0x0134) and bootloader (PID 0x1001) descriptors. We used to advertise 16,
// which is a real descriptor difference from the Korg we are spoofing.
#define CFG_TUD_ENDPOINT0_SIZE    64
#endif

#define CFG_TUD_MIDI               1
#define CFG_TUD_MIDI_EP_BUFSIZE    64
#define CFG_TUD_MIDI_RX_BUFSIZE    256
#define CFG_TUD_MIDI_TX_BUFSIZE    1024

//--------------------------------------------------------------------+
// HOST CONFIGURATION
//--------------------------------------------------------------------+

// Size of buffer to hold descriptors and other data used for enumeration
#define CFG_TUH_ENUMERATION_BUFSIZE 256

// Icon connects directly, no hub
#define CFG_TUH_HUB                 0
#define CFG_TUH_DEVICE_MAX          1

// Custom MIDI host class driver (see midi_host.c)
#define CFG_TUH_MIDI                1
#define CFG_TUH_MIDI_EP_BUFSIZE     64
#define CFG_TUH_MIDI_RX_BUFSIZE     512
#define CFG_TUH_MIDI_TX_BUFSIZE     512

#ifdef __cplusplus
}
#endif

#endif /* _TUSB_CONFIG_H_ */
