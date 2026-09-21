/*
 * Korg nanoKONTROL Studio emulation (device side).
 *
 * The MixPre discovers a nanoKONTROL Studio and then talks Korg SysEx:
 *   Search Device, Global Data Dump Request, Mode Request, Native Mode ...
 * It aborts with "connection failed" if the replies are missing/wrong.
 *
 * Reference: nanoKONTROL_Studio_MIDIimp.txt (Korg MIDI Implementation).
 */

#include <string.h>
#include "tusb.h"
#include "pico/bootrom.h"
#include "hardware/watchdog.h"
#include "hardware/flash.h"
#include "pico/flash.h"
#include "dbglog.h"
#include "korg_defaults.h"
#include "korg_emu.h"
#include "version.h"

#define SYSEX_MAX 800

static uint8_t  s_buf[SYSEX_MAX];
static uint16_t s_len = 0;

static bool     s_dbg_in = false;
static uint8_t  s_dbg[SYSEX_MAX];
static uint16_t s_dbg_len = 0;

// Debug read-out (hex of flash log), sent paced
static uint8_t  out_buf[2 * 8192 + 8];
static uint16_t out_len = 0, out_pos = 0;
static bool     out_active = false;

//--------------------------------------------------------------------+
// SCENE_CHANGE_4F removed: measured on a real Korg, it answers 0x14 with ACK only

// Send raw MIDI bytes as USB-MIDI SysEx packets
//--------------------------------------------------------------------+
static void send_raw(uint8_t const *data, uint16_t len) {
  uint16_t i = 0;
  while (i < len) {
    uint16_t rem = len - i;
    uint8_t pkt[4] = {0, 0, 0, 0};
    uint8_t n, cin;
    if (rem > 3) { n = 3; cin = 0x4; } else { n = (uint8_t) rem; cin = (uint8_t) (0x4 + rem); }
    pkt[0] = cin;                    // byte0 = (cable<<4) | CIN, cable = 0
    for (uint8_t k = 0; k < n; k++) pkt[1 + k] = data[i + k];
    dbglog_byte('<');
    dbglog_bytes(pkt, 4);
    tud_midi_packet_write(pkt);
    i += n;
  }
}


//--------------------------------------------------------------------+
// 7-bit packing (data -> MIDI bytes), see Korg doc NOTE 2
//--------------------------------------------------------------------+
static uint16_t pack7(uint8_t *out, const uint8_t *in, uint16_t inlen) {
  uint16_t o = 0, i = 0;
  while (i < inlen) {
    uint16_t n = inlen - i; if (n > 7) n = 7;
    uint8_t msb = 0;
    for (uint16_t k = 0; k < n; k++) if (in[i + k] & 0x80) msb |= (uint8_t) (1 << k);
    out[o++] = msb;
    for (uint16_t k = 0; k < n; k++) out[o++] = (uint8_t) (in[i + k] & 0x7F);
    i += n;
  }
  return o;
}

static uint16_t unpack7(uint8_t *out, const uint8_t *in, uint16_t inlen, uint16_t outmax) {
  uint16_t o = 0, i = 0;
  while (i < inlen) {
    uint8_t msb = in[i++];
    uint16_t n = inlen - i; if (n > 7) n = 7;
    for (uint16_t k = 0; k < n && o < outmax; k++)
      out[o++] = (uint8_t) (((msb >> k) & 1) << 7 | (in[i + k] & 0x7F));
    i += n;
  }
  return o;
}

//--------------------------------------------------------------------+
// Current device settings (start = real nanoKONTROL dump; updated when
// the MixPre writes new settings, so read-backs stay consistent)
//--------------------------------------------------------------------+
static uint8_t g_global[sizeof(korg_global_data)];
// The real nanoKONTROL keeps several scenes. Measured with a boot-armed log: on
// every connect the MixPre sends the scene data three times and follows each one
// with a "Scene Data Write Request" carrying a scene number (0, 1, 2 = Trim,
// LowCut, Pan). The bridge used to keep ONE scene, so each write overwrote the
// previous and only the last survived - the MixPre then found the controller's
// configuration did not match what it had written and asked to write it again on
// every connection. Keep them separately.
#define KORG_SCENE_SLOTS 5

// RECOVERY MODE. The bridge and a real nanoKONTROL are indistinguishable to the
// MixPre (same VID/PID, neither has a USB serial), so they share one stored
// configuration - and earlier sessions left that state damaged: the MixPre no
// longer arms the fader pickup for either of them, it simply adopts the
// controller's fader positions on connect (channel shows "off" instead of its
// own +10). The MixPre rewrites that configuration whenever it detects a
// mismatch, so we break one COSMETIC byte (the first character of the scene
// name - it carries no assignments) to force the prompt. Accept it once and the
// MixPre overwrites its stored state with its own correct configuration.
// Turn this off again after the recovery.
#define PERTURB_FOR_REPROVISION 0
static uint8_t g_scene[KORG_SCENE_SLOTS][sizeof(korg_scene_data)];
static uint8_t g_scene_stage[sizeof(korg_scene_data)];  // incoming dump, until 0x11 says where it goes
static uint8_t g_scene_cur = 0;                         // current scene number
static bool    g_init = false;

// Persist the settings the MixPre writes, like the real nanoKONTROL does in its
// own flash. Without this the MixPre re-writes them on every connect and never
// treats the device as "known" (suspceted cause of scene switching not working).
#define SETTINGS_MAGIC   0x4B534549u   // bumped: all five default scenes now come
                                       // from the hardware export, so existing
                                       // blobs must be rebuilt
#define SETTINGS_OFFSET  (PICO_FLASH_SIZE_BYTES - 3 * FLASH_SECTOR_SIZE)
#define SETTINGS_XIP     (XIP_BASE + SETTINGS_OFFSET)

static void __no_inline_not_in_flash_func(do_save_settings)(void *arg) {
  (void) arg;
  static uint8_t page[FLASH_PAGE_SIZE];
  uint8_t blob[4 + sizeof(korg_global_data) + KORG_SCENE_SLOTS * sizeof(korg_scene_data)];
  uint32_t magic = SETTINGS_MAGIC;
  memcpy(blob, &magic, 4);
  memcpy(blob + 4, g_global, sizeof(g_global));
  memcpy(blob + 4 + sizeof(g_global), g_scene, sizeof(g_scene));

  flash_range_erase(SETTINGS_OFFSET, FLASH_SECTOR_SIZE);
  for (uint32_t off = 0; off < sizeof(blob); off += FLASH_PAGE_SIZE) {
    uint32_t n = sizeof(blob) - off;
    if (n > FLASH_PAGE_SIZE) n = FLASH_PAGE_SIZE;
    memset(page, 0xFF, FLASH_PAGE_SIZE);
    memcpy(page, blob + off, n);
    flash_range_program(SETTINGS_OFFSET + off, page, FLASH_PAGE_SIZE);
  }
}

static void settings_save(void) {
  // flash_safe_execute() needs the second core parked, but core 1 is busy with
  // the Icon liveness probe and the status LED. It refused after 38 ms
  // (measured), so the settings were never persisted - which is why the MixPre
  // re-wrote its configuration on every connect and never treated the bridge as
  // a known device. The dbglog writes flash directly and has always worked on
  // this board, and do_save_settings() already runs from RAM
  // (__no_inline_not_in_flash_func), so the same direct path is safe here.
  uint32_t t0 = to_ms_since_boot(get_absolute_time());
  uint32_t irq = save_and_disable_interrupts();
  do_save_settings(NULL);
  restore_interrupts(irq);
  uint32_t dt = to_ms_since_boot(get_absolute_time()) - t0;
  uint8_t b[3] = { 1, (uint8_t) (dt & 0xFF), (uint8_t) ((dt >> 8) & 0xFF) };
  dbglog_byte('K');
  dbglog_bytes(b, 3);
}

// Deferred save: only write the flash once the MixPre has been quiet for a
// while. Serving 0x11 synchronously stalled the controller input path.
static volatile bool     g_settings_dirty = false;
static volatile uint32_t g_settings_last_ms = 0;

static void settings_mark_dirty(void) {
  g_settings_dirty = true;
  g_settings_last_ms = to_ms_since_boot(get_absolute_time());
}

// Called from the main loop.
void korg_emu_settings_flush(void) {
  if (!g_settings_dirty) return;
  if (to_ms_since_boot(get_absolute_time()) - g_settings_last_ms < 2000) return;
  g_settings_dirty = false;
  settings_save();
}

static void ensure_init(void) {
  if (g_init) return;
  g_init = true;
  const uint8_t *f = (const uint8_t *) SETTINGS_XIP;
  uint32_t magic;
  memcpy(&magic, f, 4);
  if (magic == SETTINGS_MAGIC) {
    memcpy(g_global, f + 4, sizeof(g_global));
    memcpy(g_scene, f + 4 + sizeof(g_global), sizeof(g_scene));
  } else {
    memcpy(g_global, korg_global_data, sizeof(g_global));
    // The real nanoKONTROL holds five scenes: 1-3 are the three the MixPre
    // writes (same bytes except the knob MIDI channel, which encodes the
    // function: 0 = Trim, 1 = LowCut, 2 = Pan), 4 and 5 are the Korg's own
    // factory scenes. All five come from a KORG KONTROL Editor export of the
    // hardware, so the bridge holds the same set instead of five copies of
    // scene 1.
    static const uint8_t *const scene_defs[KORG_SCENE_SLOTS] = {
      korg_scene_data, korg_scene_data2, korg_scene_data3,
      korg_scene_data4, korg_scene_data5,
    };
    for (int i = 0; i < KORG_SCENE_SLOTS; i++)
      memcpy(g_scene[i], scene_defs[i], sizeof(korg_scene_data));
  }
}

uint8_t korg_scene_get(uint16_t off) {
  ensure_init();
  return (off < sizeof(korg_scene_data)) ? g_scene[g_scene_cur][off] : 0;
}

//--------------------------------------------------------------------+
// Replies
//--------------------------------------------------------------------+
static void reply_identity(uint8_t g) {
  uint8_t m[] = {0xF0, 0x7E, (uint8_t) (0x00 | (g & 0x0F)), 0x06, 0x02,
                 0x42, 0x37, 0x01, 0x00, 0x00, 0x05, 0x00, 0x01, 0x00, 0xF7};
  send_raw(m, sizeof(m));
}

static void reply_search_device(uint8_t g, uint8_t echo) {
  uint8_t m[] = {0xF0, 0x42, 0x50, 0x01, (uint8_t) (0x00 | (g & 0x0F)), echo,
                 0x37, 0x01, 0x00, 0x00, 0x05, 0x00, 0x01, 0x00, 0xF7};
  send_raw(m, sizeof(m));
}

static void reply_native_mode(uint8_t g, uint8_t rr) {
  uint8_t m[] = {0xF0, 0x42, (uint8_t) (0x40 | (g & 0x0F)), 0x00, 0x01, 0x37,
                 0x02, 0x00, 0x00, 0x01, rr, 0xF7};
  send_raw(m, sizeof(m));
}

static void reply_mode_data(uint8_t g, uint8_t mm) {
  uint8_t m[] = {0xF0, 0x42, (uint8_t) (0x40 | (g & 0x0F)), 0x00, 0x01, 0x37,
                 0x02, 0x00, 0x00, 0x42, mm, 0xF7};
  send_raw(m, sizeof(m));
}

static void reply_ack(uint8_t g, uint8_t fn) {
  uint8_t m[] = {0xF0, 0x42, (uint8_t) (0x40 | (g & 0x0F)), 0x00, 0x01, 0x37,
                 0x01, 0x00, 0x00, fn, 0xF7};
  send_raw(m, sizeof(m));
}

// Transmit scene change to the host.  The real nanoKONTROL sends the native
// "Scene Button" parameter change plus the "Scene Change" message.
void korg_send_scene_change(uint8_t ss) {
  // Keep our notion of the current scene in step: the MixPre reads it back with
  // a scene dump request (0x10) and must get the scene it just switched to.
  if (ss < KORG_SCENE_SLOTS) g_scene_cur = ss;
#if SCENE_VARIANT
  uint8_t on[]  = {0xF0, 0x42, 0x40, 0x00, 0x01, 0x37, 0x05, 0x00, 0x00,
                   0x41, 0x40, 0x40, 0x7F, 0x00, 0xF7};
  uint8_t ch[]  = {0xF0, 0x42, 0x40, 0x00, 0x01, 0x37, 0x02, 0x00, 0x00,
                   0x4F, (uint8_t) (ss & 0x0F), 0xF7};
  uint8_t off[] = {0xF0, 0x42, 0x40, 0x00, 0x01, 0x37, 0x05, 0x00, 0x00,
                   0x41, 0x40, 0x40, 0x00, 0x00, 0xF7};
  send_raw(on, sizeof(on));
  send_raw(ch, sizeof(ch));
  send_raw(off, sizeof(off));
#else
  uint8_t m[] = {0xF0, 0x42, 0x40, 0x00, 0x01, 0x37,
                 0x02, 0x00, 0x00, 0x4F, (uint8_t) (ss & 0x0F), 0xF7};
  send_raw(m, sizeof(m));
#endif
}

// Global Data Dump (Func 51), 147 bytes payload — single SysEx
static void reply_global_dump(uint8_t g) {
  static uint8_t msg[256];
  ensure_init();
  uint8_t hdr[] = {0xF0, 0x42, (uint8_t) (0x40 | (g & 0x0F)), 0x00, 0x01, 0x37,
                   0x14, 0x01, 0x00, 0x51};
  uint16_t n = 0;
  memcpy(msg, hdr, sizeof(hdr));
  n += sizeof(hdr);
  n += pack7(msg + n, g_global, sizeof(g_global));
  msg[n++] = 0xF7;
  send_raw(msg, n);
}

// Current Scene Data Dump (Func 40), 640 bytes payload — single SysEx
// g_knob_ch: if != 0xFF, force the knobs' MIDI-channel field (experiment:
// suspected knob-mode selector: 0=Trim, 1=LowCut, 2=Pan)
static uint8_t g_knob_ch = 0xFF;

#define SCENE_VARIANT 0   // faithful: Scene Change only

static void reply_scene_dump(uint8_t g) {
  static uint8_t msg[700];
  static uint8_t sd[sizeof(korg_scene_data)];
  ensure_init();
  memcpy(sd, g_scene[g_scene_cur], sizeof(sd));
#if PERTURB_FOR_REPROVISION
  sd[0] ^= 0x01;   // cosmetic only: forces the MixPre to offer a settings write
#endif
  if (g_knob_ch != 0xFF) {
    for (uint8_t i = 0; i < 8; i++) sd[16 + i * 8] = g_knob_ch;
  }
  uint8_t hdr[] = {0xF0, 0x42, (uint8_t) (0x40 | (g & 0x0F)), 0x00, 0x01, 0x37,
                   0x01, 0x05, 0x00, 0x40};
  uint16_t n = 0;
  memcpy(msg, hdr, sizeof(hdr));
  n += sizeof(hdr);
  n += pack7(msg + n, sd, sizeof(sd));
  msg[n++] = 0xF7;
  send_raw(msg, n);
}

//--------------------------------------------------------------------+
// Debug read-out
//--------------------------------------------------------------------+
static void start_log_send(void) {
  static uint8_t logbuf[8192];
  uint32_t n = dbglog_read(logbuf, sizeof(logbuf));
  static const char hx[] = "0123456789ABCDEF";
  out_len = 0;
  out_buf[out_len++] = 0xF0;
  out_buf[out_len++] = 0x7D;
  out_buf[out_len++] = 0x7C;
  for (uint32_t i = 0; i < n; i++) {
    out_buf[out_len++] = (uint8_t) hx[logbuf[i] >> 4];
    out_buf[out_len++] = (uint8_t) hx[logbuf[i] & 0x0F];
  }
  out_buf[out_len++] = 0xF7;
  out_pos = 0;
  out_active = true;
  dbglog_disable();
}

// Debug: dump the stored scene as hex (F0 7D 6A F7)
static void start_scene_dump(void) {
  static const char hx[] = "0123456789ABCDEF";
  korg_scene_get(0);                            // ensure loaded from flash
  out_len = 0;
  out_buf[out_len++] = 0xF0;
  out_buf[out_len++] = 0x7D;
  out_buf[out_len++] = 0x6A;
  for (uint16_t i = 0; i < sizeof(korg_scene_data); i++) {
    uint8_t b = g_scene[g_scene_cur][i];
    out_buf[out_len++] = (uint8_t) hx[b >> 4];
    out_buf[out_len++] = (uint8_t) hx[b & 0x0F];
  }
  out_buf[out_len++] = 0xF7;
  out_pos = 0;
  out_active = true;
}

void korg_emu_service(void) {
  if (!out_active) return;
  while (out_pos < out_len) {
    uint16_t rem = (uint16_t) (out_len - out_pos);
    uint8_t n, cin;
    if (rem > 3) { n = 3; cin = 0x4; } else { n = (uint8_t) rem; cin = (uint8_t) (0x4 + rem); }
    uint8_t pkt[4] = {cin, 0, 0, 0};
    for (uint8_t k = 0; k < n; k++) pkt[1 + k] = out_buf[out_pos + k];
    if (!tud_midi_packet_write(pkt)) return;
    out_pos += n;
  }
  out_active = false;
}

//--------------------------------------------------------------------+
// SysEx dispatch
//--------------------------------------------------------------------+
static void handle_sysex(uint8_t const *m, uint16_t len) {
  if (len < 4 || m[0] != 0xF0) return;

  if (m[1] == 0x7D && m[2] == 0x7E && m[3] == 0xF7) { reset_usb_boot(0, 0); return; }
  if (m[1] == 0x7D && m[2] == 0x7A && m[3] == 0xF7) { watchdog_reboot(0, 0, 100); return; }
  if (m[1] == 0x7D && m[2] == 0x7C && m[3] == 0xF7) { start_log_send(); return; }
  if (m[1] == 0x7D && m[2] == 0x7B && m[3] == 0xF7) { dbglog_clear(); return; }
  if (m[1] == 0x7D && m[2] == 0x6A && m[3] == 0xF7) { start_scene_dump(); return; }

  // Debug: F0 7D 63 F7 -> diagnostics: counters + worst loop gaps
  if (m[1] == 0x7D && m[2] == 0x63 && m[3] == 0xF7) {
    extern volatile uint32_t d_icon_rx, d_drop_to_mix, d_drop_to_icon, d_tx_mix, d_gap1, d_gap0;
    static const char hx[] = "0123456789ABCDEF";
    extern volatile uint32_t d_tx_fail;
    uint32_t v[6] = {d_icon_rx, d_tx_mix, d_tx_fail, d_drop_to_mix, d_gap1, d_gap0};
    out_len = 0;
    out_buf[out_len++] = 0xF0; out_buf[out_len++] = 0x7D; out_buf[out_len++] = 0x63;
    for (int k = 0; k < 4; k++)
      for (int b = 28; b >= 0; b -= 4)
        out_buf[out_len++] = (uint8_t) hx[(v[k] >> (uint32_t) b) & 0xF];
    for (int k = 4; k < 6; k++)
      for (int b = 12; b >= 0; b -= 4)
        out_buf[out_len++] = (uint8_t) hx[(v[k] >> (uint32_t) b) & 0xF];
    out_buf[out_len++] = 0xF7;
    out_pos = 0; out_active = true;
    return;
  }

  // Debug: F0 7D 62 F7 -> histogram of Icon packet types (CIN 0..10)
  if (m[1] == 0x7D && m[2] == 0x62 && m[3] == 0xF7) {
    extern volatile uint32_t d_cin[16];
    static const char hx[] = "0123456789ABCDEF";
    out_len = 0;
    out_buf[out_len++] = 0xF0; out_buf[out_len++] = 0x7D; out_buf[out_len++] = 0x62;
    for (int k = 0; k <= 10; k++) {
      uint32_t v = d_cin[k];
      for (int b = 16; b >= 0; b -= 4)
        out_buf[out_len++] = (uint8_t) hx[(v >> (uint32_t) b) & 0xF];
    }
    out_buf[out_len++] = 0xF7;
    out_pos = 0; out_active = true;
    return;
  }

  // Debug: F0 7D 64 vv F7 -> force the status LED (GP2) level, 0xFF = automatic
  if (m[1] == 0x7D && m[2] == 0x64 && len >= 5 && m[4] == 0xF7) {
    extern void status_led_force(uint8_t);
    status_led_force(m[3]);
    return;
  }

  // Debug: F0 7D 65 F7 -> last HardFault info (CFSR HFSR BFAR core) as hex
  if (m[1] == 0x7D && m[2] == 0x65 && m[3] == 0xF7) {
    extern uint32_t g_f_cfsr, g_f_hfsr, g_f_bfar, g_f_core, g_f_pc, g_f_lr;
    static const char hx[] = "0123456789ABCDEF";
    uint32_t v[6] = {g_f_cfsr, g_f_hfsr, g_f_bfar, g_f_core, g_f_pc, g_f_lr};
    out_len = 0;
    out_buf[out_len++] = 0xF0;
    out_buf[out_len++] = 0x7D;
    out_buf[out_len++] = 0x65;
    for (int k = 0; k < 6; k++)
      for (int b = 28; b >= 0; b -= 4)
        out_buf[out_len++] = (uint8_t) hx[(v[k] >> (uint32_t) b) & 0xF];
    out_buf[out_len++] = 0xF7;
    out_pos = 0;
    out_active = true;
    return;
  }

  // Debug: F0 7D 67 F7 -> status (Icon mounted, VID, PID, mount count, RX count)
  if (m[1] == 0x7D && m[2] == 0x67 && m[3] == 0xF7) {
    extern void bridge_status(uint8_t *);
    uint8_t st[20];
    bridge_status(st);
    out_len = 0;
    out_buf[out_len++] = 0xF0;
    out_buf[out_len++] = 0x7D;
    out_buf[out_len++] = 0x67;
    for (int i = 0; i < 20; i++) out_buf[out_len++] = (uint8_t) (st[i] & 0x7F);
    out_buf[out_len++] = 0xF7;
    out_pos = 0;
    out_active = true;
    return;
  }

  // Debug: F0 7D 61 F7 -> firmware version as plain text, so which build sits on
  // a Pico can be read back instead of guessed
  if (m[1] == 0x7D && m[2] == 0x61 && m[3] == 0xF7) {
    out_len = 0;
    out_buf[out_len++] = 0xF0;
    out_buf[out_len++] = 0x7D;
    out_buf[out_len++] = 0x61;
    for (const char *p = FW_VERSION; *p; p++) out_buf[out_len++] = (uint8_t) *p;
    out_buf[out_len++] = 0xF7;
    out_pos = 0;
    out_active = true;
    return;
  }

  // Debug: F0 7D 68 nn vv F7 -> light Icon button note nn with velocity vv
  // (used to check whether the velocity selects the LED colour)
  if (m[1] == 0x7D && m[2] == 0x68 && len >= 6 && m[5] == 0xF7) {
    extern volatile int8_t g_dbg_led_note;
    extern volatile uint8_t g_dbg_led_val;
    g_dbg_led_val  = m[4];
    g_dbg_led_note = (int8_t) m[3];
    return;
  }

  // Debug: F0 7D 69 cc nn vv F7 -> note on a chosen MIDI channel (colour test)
  if (m[1] == 0x7D && m[2] == 0x69 && len >= 7 && m[6] == 0xF7) {
    extern volatile int8_t g_dbg_led_note;
    extern volatile uint8_t g_dbg_led_val, g_dbg_led_ch;
    g_dbg_led_ch   = m[3];
    g_dbg_led_val  = m[5];
    g_dbg_led_note = (int8_t) m[4];
    return;
  }

  // Debug: F0 7D 70 ch F7 -> force the SCENE knob-channel field (experiment)
  if (m[1] == 0x7D && m[2] == 0x70 && len >= 5 && m[4] == 0xF7) {
    g_knob_ch = m[3];
    return;
  }

  // Debug: F0 7D 66 ch F7 -> set the knob FUNCTION (0=Trim 1=LowCut 2=Pan)
  if (m[1] == 0x7D && m[2] == 0x66 && len >= 5 && m[4] == 0xF7) {
    extern void knob_mode_set(uint8_t);
    knob_mode_set(m[3]);
    return;
  }

  if (len < 5) return;

  // Universal non-realtime: F0 7E gg 06 01 F7
  if (m[1] == 0x7E && len >= 6 && m[3] == 0x06 && m[4] == 0x01) {
    reply_identity(m[2]);
    return;
  }

  if (m[1] != 0x42) return;

  // Search Device Request: F0 42 50 00 dd F7
  if (m[2] == 0x50 && m[3] == 0x00 && len >= 6) {
    reply_search_device(0, m[4]);
    return;
  }

  // Korg: F0 42 4g 00 01 37 ...
  if (len < 11 || m[3] != 0x00 || m[4] != 0x01 || m[5] != 0x37) return;
  uint8_t g  = m[2] & 0x0F;
  uint8_t fn = m[9];

  switch (fn) {
    case 0x00: if (len >= 12) reply_native_mode(g, (m[10] == 0x01) ? 0x03 : 0x02); break;
    case 0x12: reply_mode_data(g, 0x00); break;            // Mode Request.
      // Measured on a real nanoKONTROL Studio: it answers 0x00 = NORMAL MODE.
      // We were answering 0x01 = NATIVE MODE, which is a different protocol
      // (the one the Korg editor uses). That made the MixPre talk to a
      // controller it does not know, even though the USB descriptors matched.
    case 0x0E: reply_global_dump(g); break;                // Global Data Dump Request
    case 0x10: reply_scene_dump(g); break;                 // Scene Data Dump Request
    case 0x40: ensure_init(); if (len >= 10 + 640) unpack7(g_scene_stage, m + 10, (uint16_t)(len - 11), sizeof(g_scene_stage)); reply_ack(g, 0x23); break;
    case 0x51: ensure_init(); if (len >= 10 + 147) unpack7(g_global, m + 10, (uint16_t)(len - 11), sizeof(g_global)); reply_ack(g, 0x23); break;
    // Write Request. The ACK must go out immediately, but the flash save is
    // DEFERRED: flash_safe_execute() erases a sector and locks both cores out
    // of flash for tens of ms, during which core1 cannot read the controller -
    // the MixPre then "ignores" faders for a while. The MixPre-10 sends 0x11
    // far more often than the -6, which is why this only showed up there.
    case 0x11:                                               // Scene Data Write Request: store the
      ensure_init();                                         // last dump as scene ss
      {
        uint8_t ss = (len >= 11) ? (uint8_t) (m[10] & 0x07) : 0;
        if (ss < KORG_SCENE_SLOTS) {
          memcpy(g_scene[ss], g_scene_stage, sizeof(korg_scene_data));
          dbglog_byte('W'); dbglog_byte(ss);
        }
      }
      reply_ack(g, 0x21);
      settings_mark_dirty();
      break;
    case 0x14:                                               // Scene Change Request
      // The real nanoKONTROL answers ACK only (measured on hardware). We do the
      // same, but also follow the scene number so read-backs serve the scene the
      // MixPre has just selected.
      ensure_init();
      if (len >= 11) {
        uint8_t ss = (uint8_t) (m[10] & 0x07);
        if (ss < KORG_SCENE_SLOTS) g_scene_cur = ss;
        dbglog_byte('N'); dbglog_byte(g_scene_cur);
      }
      reply_ack(g, 0x23);
      break;
    default: break;
  }
}

//--------------------------------------------------------------------+
// Debug-only SysEx intercept (transparent sniffer: real Korg gets the rest)
//--------------------------------------------------------------------+
bool korg_emu_debug_feed(uint8_t const pkt[4]) {
  uint8_t cin = pkt[0] & 0x0F;

  if (!s_dbg_in) {
    if (cin == 0x4 && pkt[1] == 0xF0 && pkt[2] == 0x7D) {
      s_dbg_in = true;
      s_dbg_len = 0;
      s_dbg[s_dbg_len++] = pkt[1];
      s_dbg[s_dbg_len++] = pkt[2];
      s_dbg[s_dbg_len++] = pkt[3];
      if (pkt[3] == 0xF7) { s_dbg_in = false; handle_sysex(s_dbg, s_dbg_len); }
      return true;
    }
    return false;
  }

  uint8_t n = (uint8_t) ((cin == 0x5) ? 1 : ((cin == 0x6) ? 2 : 3));
  for (uint8_t k = 0; k < n; k++) {
    if (s_dbg_len < SYSEX_MAX) s_dbg[s_dbg_len++] = pkt[1 + k];
    if (pkt[1 + k] == 0xF7) { s_dbg_in = false; handle_sysex(s_dbg, s_dbg_len); break; }
  }
  return true;
}

//--------------------------------------------------------------------+
// Packet feed (SysEx reassembly)
//--------------------------------------------------------------------+
bool korg_emu_feed(uint8_t const pkt[4]) {
  uint8_t cin = pkt[0] & 0x0F;   // byte0 = (cable<<4) | CIN

  if (cin == 0x0) return true;   // reserved / padding: swallow, do not forward

  if (cin == 0x4) {
    if (s_len + 3 <= SYSEX_MAX) {
      s_buf[s_len++] = pkt[1];
      s_buf[s_len++] = pkt[2];
      s_buf[s_len++] = pkt[3];
    } else {
      s_len = 0;
    }
    return true;
  }

  if (cin >= 0x5 && cin <= 0x7) {
    uint8_t n = (uint8_t) (cin - 0x4);
    for (uint8_t k = 0; k < n && s_len < SYSEX_MAX; k++) s_buf[s_len++] = pkt[1 + k];
    handle_sysex(s_buf, s_len);
    s_len = 0;
    return true;
  }

  return false; // not SysEx -> forward to Icon
}
