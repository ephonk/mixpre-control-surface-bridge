#ifndef _KORG_EMU_H_
#define _KORG_EMU_H_

#include <stdbool.h>
#include <stdint.h>

// Feed one 4-byte USB-MIDI packet received FROM the MixPre (device side).
// Returns true if the packet was consumed (Korg SysEx handled locally),
// false if it should be forwarded to the Icon (normal MIDI / feedback).
bool korg_emu_feed(uint8_t const pkt[4]);
void korg_emu_service(void);
void korg_send_scene_change(uint8_t ss);
// Debug-only intercept (0x7D family) for the transparent sniffer
bool korg_emu_debug_feed(uint8_t const pkt[4]);

// Current scene (set by the MixPre when it writes settings); offset per Korg
// doc Table 1. Returns the raw byte.
uint8_t korg_scene_get(uint16_t off);
void korg_emu_settings_flush(void);

#endif
