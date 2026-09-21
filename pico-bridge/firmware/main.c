/*
 * MixPre <-> Icon Platform M+ USB MIDI bridge
 *
 * Pico 2 W, two USB roles:
 *   - Native USB  = device, spoofs Korg nanoKONTROL Studio (to MixPre)
 *   - PIO USB     = host, talks to Icon Platform M+ (Mackie Control)
 *
 * Core 0: device stack (tud_task) + MixPre side + translation
 * Core 1: host stack (tuh_task) + Icon side
 *
 * The MixPre writes its own "scene" (CC assignments) to the nanoKONTROL,
 * so the translation reads the CC map from that scene dynamically
 * (korg_scene_get) instead of hardcoding it.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "pico/util/queue.h"
#include "hardware/clocks.h"

#include "pio_usb.h"
#include "tusb.h"
#include "pico/flash.h"
#include "midi_host.h"
#include "korg_emu.h"
#include "dbglog.h"
#include "hardware/pwm.h"
#include "hardware/gpio.h"
#include "hardware/watchdog.h"
#include "pico/time.h"
#include "pico/platform.h"

//--------------------------------------------------------------------+
// Queues between cores (4-byte USB-MIDI packets)
//--------------------------------------------------------------------+
static queue_t q_icon_to_mixpre;
static queue_t q_mixpre_to_icon;

#define MIDI_QUEUE_DEPTH 256

static inline uint32_t midi_pack(uint8_t const pkt[4]) {
  return (uint32_t) pkt[0] | ((uint32_t) pkt[1] << 8) |
         ((uint32_t) pkt[2] << 16) | ((uint32_t) pkt[3] << 24);
}

static inline void midi_unpack(uint32_t p, uint8_t pkt[4]) {
  pkt[0] = (uint8_t) (p & 0xff);
  pkt[1] = (uint8_t) ((p >> 8) & 0xff);
  pkt[2] = (uint8_t) ((p >> 16) & 0xff);
  pkt[3] = (uint8_t) ((p >> 24) & 0xff);
}

// Sniffer mode: 1 = transparent MIDI bridge (real nanoKONTROL <-> MixPre),
// used to capture how the MixPre reacts to a genuine scene change.
#define SNIFFER 0

static volatile bool g_icon_mounted = false;

// TEST: the bridge used to detach from the MixPre 20 s after the Icon went
// quiet, then re-attach when it came back. From the MixPre that is a device
// that keeps appearing and disappearing - and a device that disappears is not
// remembered, which matches the user's report that the MixPre asks to save its
// configuration on every single connect (a real nanoKONTROL did that once,
// years ago). The panel also returns to normal on its own when the controller
// is unplugged, so the detach was not buying anything. Set to 1 only to bring
// the old behaviour back.
#define DP_DETACH 0

#define USB_DP_PIN 0                       // PIO USB D+ (see the wiring table)
static volatile bool g_usb_dp = false;     // live level of the USB D+ line
static volatile uint32_t g_icon_alive_ms = 0;  // last MCU Host Connection Reply
static volatile uint8_t  g_probe_tx = 0;       // probes sent (diagnostics)
static volatile uint8_t  g_probe_rx = 0;       // replies matched
static volatile uint8_t  g_probe_state = 0;    // reply matcher state
static volatile bool     g_in_sysex = false;   // inside a SysEx from the Icon
static volatile uint32_t g_in_sysex_ms = 0;    // when it started (timeout guard)
static bool              g_slate_held = false;    // WRITE held = Slate active
static bool              g_mute[8];            // MixPre mute state per channel (safety gate)
static volatile bool     g_dp_attached = false; // device side (MixPre) enabled
static volatile uint32_t g_core1_beat = 0;   // watchdog heartbeat from core1

// --- diagnostics (F0 7D 63 F7) -------------------------------------------
// Where does the delay come from? If core1's max loop gap grows, the Pico is
// stalling (flash/USB). If it stays small while the MixPre is deaf, the Pico
// is pushing data out fine and the MixPre itself is the one busy.
volatile uint32_t d_icon_rx = 0;      // packets read from the Icon
volatile uint32_t d_drop_to_mix = 0;  // dropped: Icon -> MixPre queue
volatile uint32_t d_drop_to_icon = 0; // dropped: MixPre -> Icon queue
volatile uint32_t d_tx_mix = 0;       // packets written to the MixPre
volatile uint32_t d_gap1 = 0;         // worst core1 loop gap [ms]
volatile uint32_t d_gap0 = 0;         // worst core0 loop gap [ms]
volatile uint32_t d_tx_fail = 0;      // writes the MixPre would not take
volatile uint32_t d_cin[16];          // histogram of USB-MIDI packet types from the Icon

// Retry queue for packets the MixPre could not take right now.
// tud_midi_packet_write() returns false when TinyUSB's MIDI FIFO is full, which
// happens in bursts (several faders at once). Without this the packet was lost
// and the MixPre never saw that movement at all.
#define MIX_TXQ_LEN 128
static uint32_t mix_txq[MIX_TXQ_LEN];
static uint16_t mix_txq_h = 0, mix_txq_t = 0;

static void mtx_push(uint32_t q) {
  uint16_t nh = (uint16_t) ((mix_txq_h + 1) % MIX_TXQ_LEN);
  if (nh == mix_txq_t) { d_tx_fail++; return; }        // queue full too: lost
  mix_txq[mix_txq_h] = q;
  mix_txq_h = nh;
}

static void mtx_flush(void) {
  while (mix_txq_t != mix_txq_h) {
    uint8_t pp[4];
    midi_unpack(mix_txq[mix_txq_t], pp);
    if (!tud_midi_packet_write(pp)) return;            // host still busy
    mix_txq_t = (uint16_t) ((mix_txq_t + 1) % MIX_TXQ_LEN);
  }
}

// Black box: a marker in uninitialised RAM. It survives a watchdog reset, so
// after a freeze the next boot can tell where the code was when it stopped.
//   0x1000 + n = core1 code, 0x2000 + n = core0 code, 0xB007 = fresh boot
uint32_t __uninitialized_ram(g_bb1);       // last marker written by core1
uint32_t __uninitialized_ram(g_bb0);       // last marker written by core0
uint32_t __uninitialized_ram(g_hf);        // 1 = a HardFault happened
uint32_t __uninitialized_ram(g_bootcnt);   // counts resets (survives them)
uint32_t __uninitialized_ram(g_f_cfsr);    // fault status registers, captured
uint32_t __uninitialized_ram(g_f_hfsr);
uint32_t __uninitialized_ram(g_f_bfar);
uint32_t __uninitialized_ram(g_f_core);    // which core faulted
uint32_t __uninitialized_ram(g_f_pc);      // stacked PC - the instruction that faulted
uint32_t __uninitialized_ram(g_f_lr);
uint32_t __uninitialized_ram(g_rr);        // reset reason: 1 = watchdog, 2 = other

// A watchdog reset is a *software* reset - the SRAM survives. A power cycle
// does not, so the black box only helps for the former.
static uint32_t g_bb1_prev = 0, g_bb0_prev = 0, g_hf_prev = 0;

// HardFault handler. It MUST NOT spin - that turned a fault into a dead bridge
// (the PIO-USB host faults when the controller disappears). Record it in the
// black box (survives a software reset) and reboot cleanly instead.
// The exception frame is pushed on the stack the faulting code was using, so
// the handler has to pick MSP or PSP itself - hence the naked entry point.
void hardfault_report(uint32_t *frame);

__attribute__((naked)) void isr_hardfault(void) {
  __asm volatile (
      "mov r0, lr\n"
      "movs r1, #4\n"
      "tst r0, r1\n"
      "ite eq\n"
      "mrseq r0, msp\n"
      "mrsne r0, psp\n"
      "b hardfault_report\n"
  );
}

void hardfault_report(uint32_t *frame) {
  g_hf = 1;
  g_f_pc = frame[6];                              // PC at the fault
  g_f_lr = frame[5];
  g_f_cfsr = *(volatile uint32_t *) 0xE000ED28;   // why
  g_f_hfsr = *(volatile uint32_t *) 0xE000ED2C;
  g_f_bfar = *(volatile uint32_t *) 0xE000ED38;   // where (if BFARVALID)
  g_f_core = (uint32_t) get_core_num();
  watchdog_reboot(0, 0, 10);
  while (true) { tight_loop_contents(); }         // not reached
}

// Safety gate: a motorized controller runs a fader calibration sweep right
// after power-up / replug. Forwarding those values would move the MixPre's
// mix (dangerous during a recording). The real nanoKONTROL never sends
// anything until a control is touched, so the MixPre then asks the operator
// to match the value first ("calibration"). We mimic that: ignore everything
// from the controller during the first CONTROLLER_SETTLE_MS after it mounts.
//
// NOTE: previously (before this gate) we forwarded the start-up sweep too;
// it made the MixPre immediately accept the faders WITHOUT matching, which is
// convenient but unsafe on power loss / cable replug. Kept here for reference.
#define CONTROLLER_SETTLE_MS 5000
#define FADER_ECHO 1   // echo fader position back so the faders hold
#define FADER_AS_PITCHBEND 0   // send faders as Pitch Bend (Korg docs) not CC 82-89
// A real nanoKONTROL sends Scene Change (0x4F) with the scene number every time a
// scene changes on the controller (measured twice - see
// measurements/DOKUMENTACJA_TESTOW.md). Scene = knob function: 0=Trim, 1=LowCut,
// 2=Pan. With this on, switching the knob function also tells the MixPre that the
// scene changed, exactly like the real device. It is kept as a switch because an
// earlier session recorded the opposite finding - that sending Scene Change resets
// the MixPre's match state - and the two have to be settled on real hardware.
// MEASURED, DO NOT ENABLE: sending Scene Change (0x4F) makes the MixPre drop the
// match state it has already learned for the knobs. With this on, switching
// Trim -> LowCut -> Pan and back forces the operator to catch the value again
// every time. The real nanoKONTROL does send 0x4F on its own scene changes, but
// for this bridge it costs more than it gives, so it stays off.
#define SCENE_CHANGE_ON_KNOB_MODE 0
// The real nanoKONTROL announces the scene it loaded as soon as it comes up:
// the Korg doc says "Scene Change -,T - when the nanoKONTROL Studio completed to
// load the scene, it transmits this message". The MixPre is waiting for that on
// connect. Without it the session never looks agreed and it asks to write its
// settings every time, and the faders never arm their pickup.
// Not needed: the "write settings on every connect" problem was fixed by keeping
// all the scenes the MixPre writes, not by announcing one. It also sends 0x4F,
// which (see above) costs the knobs their learned match state.
#define SCENE_ANNOUNCE_AT_ATTACH 0
// How long after the device side comes up before announcing. The real Korg does
// it at power-up, i.e. essentially at once; too early and the host may not have
// opened the MIDI streams yet, too late and the MixPre has already decided.
#define SCENE_ANNOUNCE_DELAY_MS 150
static uint32_t g_gate_until_ms = 0;
static uint32_t g_icon_lost_ms = 0;
static bool     g_icon_prev = false;

// Fader touch sensors. The Icon Platform M+ has touch-sensitive motorized
// faders and reports grabbing/releasing them with the MCU touch notes
// 0x68-0x6F. The real nanoKONTROL has no such message: it sends a fader value
// only when the operator moves it, and the MixPre answers by asking the
// operator to match the stored value first (its "calibration"). That is the
// pickup. So we forward a fader only while it is actually touched, which makes
// the Icon behave like the Korg and lets the MixPre's own pickup work. The
// Icon's power-up dump is never touched, so it dies here instead of becoming
// the channel values. g_touch_seen is the safety net: if this controller never
// reports touch, faders are forwarded as before rather than staying mute.
static uint8_t g_fader_touch = 0;   // bit per channel, 1 = operator holds it
static bool    g_touch_seen = false;

// Continuity of fader values. Measured on a real nanoKONTROL: it sends every
// consecutive value (B0 52 00, 01, 02, 03 ...) with no gaps. Our conversion
// from the Icon's 10-bit position to a 7-bit CC jumped over values, and the
// MixPre's pickup waits for the value to REACH its stored one - skipping over
// it means the match never happens. So fill the gap in between.
static uint8_t g_fader_last[8];
static bool    g_fader_last_ok[8];

// Safety: forward a fader to the MixPre only after the operator has actually
// moved it since the controller connected. The MixPre then asks to match the
// value first (like with a real nanoKONTROL), so a power glitch / cable replug
// can never move the mix by itself.
// gate the faders until the controller's power-up calibration sweep is over:
// forward fader values only after no fader message was seen for SETTLE ms.
#define FADER_SETTLE_MS 1200
#define SWEEP_WINDOW_MS 300
#define SWEEP_MIN_CH    3
static uint32_t g_fader_last_ms = 0;
static uint32_t g_sweep_win_ms = 0;
static uint8_t  g_sweep_chan_mask = 0;
static bool     g_faders_sweep = false;

// Per-fader arm: after the settle window the controller's faders are at rest.
// Forward a fader only once the operator moves it away from that rest value,
// otherwise the controller's start-up positions would be applied by the MixPre
// immediately (channel jumping to 0 / OFF).
static bool     g_fader_arm[8];
static uint16_t g_fader_rest[8];

// Host-side mount diagnostics (core1 -> core0 -> dbglog)
static volatile uint16_t g_mount_vid = 0, g_mount_pid = 0;
static volatile uint8_t  g_mount_dev = 0;
static volatile uint32_t g_mount_seq = 0;
static volatile uint32_t g_icon_rx = 0;   // MIDI packets received from the Icon

// Status read-out used by the debug SysEx F0 7D 67 F7 (see korg_emu.c)
void bridge_status(uint8_t *o) {
  o[8]  = (uint8_t) (g_bb1_prev & 0x7F);
  o[9]  = (uint8_t) ((g_bb1_prev >> 7) & 0x7F);
  o[10] = (uint8_t) ((g_bb1_prev >> 14) & 0x7F);
  o[11] = (uint8_t) (g_bb0_prev & 0x7F);
  o[12] = (uint8_t) ((g_bb0_prev >> 7) & 0x7F);
  o[13] = (uint8_t) ((g_bb0_prev >> 14) & 0x7F);
  o[13] = (uint8_t) (g_hf_prev | (g_rr << 1));   // bit0 = HardFault, bits1-2 = reset reason
  o[14] = (uint8_t) (g_bootcnt & 0x7F);
  o[15] = (uint8_t) ((g_bootcnt >> 7) & 0x7F);
  o[16] = g_probe_tx;
  o[17] = g_probe_rx;
  o[18] = (uint8_t) (((to_ms_since_boot(get_absolute_time()) - g_icon_alive_ms) / 100) & 0x7F);
  o[19] = (uint8_t) ((g_probe_state & 0x3) | (g_dp_attached ? 0x08 : 0) | ((to_ms_since_boot(get_absolute_time()) - g_icon_alive_ms > 12000) ? 0x10 : 0));
  o[0] = (uint8_t) (g_icon_mounted ? 1 : 0);
  o[1] = (uint8_t) (g_mount_vid & 0x7F);
  o[2] = (uint8_t) ((g_mount_vid >> 7) & 0x7F);
  o[3] = (uint8_t) (g_mount_pid & 0x7F);
  o[4] = (uint8_t) ((g_mount_pid >> 7) & 0x7F);
  o[5] = (uint8_t) (g_mount_seq & 0x7F);
  o[6] = (uint8_t) (g_icon_rx & 0x7F);
  o[7] = (uint8_t) ((g_icon_rx >> 7) & 0x7F);
}

extern volatile uint32_t midih_dbg_n;
extern volatile uint8_t  midih_dbg_cls, midih_dbg_sub;
extern volatile uint8_t  midih_dbg_in, midih_dbg_out, midih_dbg_res;

void tuh_mount_cb(uint8_t daddr) {
  uint16_t vid = 0, pid = 0;
  tuh_vid_pid_get(daddr, &vid, &pid);
  g_mount_vid = vid; g_mount_pid = pid; g_mount_dev = daddr;
  g_mount_seq++;
}

void tuh_umount_cb(uint8_t daddr) {
  (void) daddr;
  g_mount_vid = 0; g_mount_pid = 0;
  g_mount_seq++;
}

//--------------------------------------------------------------------+
// Scene offsets (Korg nanoKONTROL Studio doc, Table 1)
//   base+0 MIDI Ch, base+1 Assign Type, base+2 CC#/Note#, ...
//--------------------------------------------------------------------+
#define SC_OFF_KNOB      16
#define SC_OFF_SLIDER    80
#define SC_OFF_SOLO      144
#define SC_OFF_MUTE      208
#define SC_OFF_REC       272
#define SC_OFF_SELECT    336
#define SC_OFF_TRANSPORT 400

static uint8_t scene_cc(uint16_t base)     { return korg_scene_get(base + 2); }
static uint8_t scene_status(uint16_t base) {
  uint8_t ch = korg_scene_get(base);
  if (ch > 15) ch = 0;                 // 16 = Global; we keep ch 0
  return (uint8_t) (0xB0 | ch);
}

// Knob values, accumulated here because the Icon's knobs are endless encoders
// and send relative deltas. Starting at 0 was a bug: the first turn
// counter-clockwise clamped at 0 and that absolute 0 was sent to the MixPre,
// so Trim jumped hard left (measured on hardware). Seeded mid-scale so no
// direction is favoured and the MixPre's own pickup decides when it takes over.
static uint8_t enc_val[8] = {64, 64, 64, 64, 64, 64, 64, 64};

// Knob duplicate suppression. Measured on a real nanoKONTROL: it never sends the
// same value twice (see measurements/DOKUMENTACJA_TESTOW.md). Without this the
// knob kept re-sending its value once the accumulator hit the end of range -
// turning further at 0 produced a stream of identical B0 0D 00 messages.
// The channel is tracked as well, because the knob's MIDI channel is its function
// (0=Trim 1=LowCut 2=Pan), so the same value on another channel is a new message.
static uint8_t g_knob_last[8];
static uint8_t g_knob_last_ch[8];
static bool    g_knob_last_ok[8];

//--- User-definable shortcut combos (MixPre): "Cycle + <button>" -------------
// The Icon's master-section buttons (Read/Write/Mute) must emit the MixPre's
// shortcut combination, because Tone / L-R Mute / Undo / Q / Shuttle only
// exist as user-assignable shortcuts on the MixPre (see Korg doc).
static void sc_request(uint8_t cc, bool wrap);   // forward declaration
static bool     sc_pend_w[4];    // whether each queued request needs the Cycle wrap
static bool     sc_wrap = false; // current request wraps the click with Cycle
static uint32_t sc_verify_at = 0;// when to re-check the mode after a wrapped run
static uint8_t  sc_pend[4];      // queued shortcut requests (FIFO)
static uint8_t  sc_pend_n = 0;
static uint8_t  sc_btn_cc = 0;   // scene CC of the shortcut button
static uint8_t  sc_state  = 0;   // 0 idle, 1 Cycle down, 2 btn down, 3 btn up, 4 Cycle up
static uint32_t sc_t      = 0;
static bool     g_shuttle = false;  // jog push latches the MixPre's shuttle (FFwd)
static bool     g_tone_on = false;  // local LED state (MixPre reports neither)
static bool     g_lrmute_on = false;
static bool     g_cycle_on = false;  // MixPre shortcut mode (Cycle toggle)

// Debug hook: light an Icon button with an arbitrary velocity so we can find
// out whether the velocity selects the LED colour (set via F0 7D 68 nn vv F7).
volatile int8_t  g_dbg_led_note = -1;
volatile uint8_t g_dbg_led_val  = 0;
volatile uint8_t g_dbg_led_ch   = 0;

static uint8_t g_scene = 0;      // scene index sent to the MixPre
static uint8_t g_knob_ch = 0;    // knob MIDI channel = knob function: 0=Trim 1=LowCut 2=Pan
static int8_t  g_sel   = -1;     // locally selected channel (-1 = none)

static void icon_note(uint8_t note, uint8_t vel);

// Knob-function indicator on the master WRITE button (Note 75).
// A full 0..127 scan showed the host can only light the channel rows
// (Rec/Solo/Mute/SEL), the transport and the master Read/Write/Mute row - the
// two Bank buttons have no host-addressable LED at all (only the local press
// flash). Read and Mute already show the Tone / L-R mute state, so the free
// LED is WRITE. To stay independent of the channel count (a MixPre-10 uses all
// eight strips) the mode indicator lives here:
//   Trim   -> LED off
//   LowCut -> LED steady
//   Pan    -> LED blinking
static bool     g_blink_on = false;
static uint32_t g_blink_t  = 0;

void knob_mode_indicator(void) {
  g_blink_on = false;
  icon_note(75, (uint8_t) (g_knob_ch == 1 ? 0x7F : 0x00));
}

// Called from the main loop: blink the WRITE LED while Pan is selected.
// Set the knob function from outside (debug command F0 7D 66 ch F7)
void knob_mode_set(uint8_t ch) {
  if (ch > 2) ch = 2;
  g_knob_ch = ch;
  g_scene   = ch;
  knob_mode_indicator();
}

static void knob_mode_blink_service(void) {
  if (g_knob_ch != 2) return;
  uint32_t now = to_ms_since_boot(get_absolute_time());
  if (now - g_blink_t < 400) return;
  g_blink_t = now;
  g_blink_on = !g_blink_on;
  icon_note(75, (uint8_t) (g_blink_on ? 0x7F : 0x00));
}

static void icon_note(uint8_t note, uint8_t vel) {
  uint8_t p[4] = {0x09, 0x90, note, vel};
  uint32_t q = midi_pack(p);
  queue_try_add(&q_mixpre_to_icon, &q);
}

//--------------------------------------------------------------------+
// Translation Icon (Mackie Control) -> MixPre (Korg scene CCs)
//--------------------------------------------------------------------+
static bool translate_icon_to_mixpre(uint8_t pkt[4]) {
  uint8_t cin = pkt[0] & 0x0F;
  uint8_t status = pkt[1], d1 = pkt[2], d2 = pkt[3];

  if (cin == 0x8 || cin == 0x9) {                 // Note On/Off -> button
    // Knob push buttons (notes 32-39) / Bank << >> (46/47): cycle the knob
    // function. It is selected by the knob's MIDI channel: 0=Trim 1=LowCut 2=Pan
    if (d1 >= 32 && d1 <= 39) {
      if (cin == 0x9 && d2 > 0) {
        g_knob_ch = (uint8_t) ((g_knob_ch >= 2) ? 0 : (g_knob_ch + 1));
        g_scene = g_knob_ch;
        knob_mode_indicator();
        dbglog_byte('S'); dbglog_byte(g_knob_ch);
#if SCENE_CHANGE_ON_KNOB_MODE
        korg_send_scene_change(g_knob_ch);   // measured: the real Korg does this
#endif
      }
      return false;
    }

    // Mode buttons: |< (Note 48) = back, |> (Note 49) = forward.
    // Notes 46/47 (Bank <<8 / 8>>) are deliberately left unhandled so they can
    // be used later for the channel-bank jump (channels 9-10).
    if (d1 == 48 || d1 == 49) {
      if (cin == 0x9 && d2 > 0) {
        if (d1 == 48) g_knob_ch = (uint8_t) ((g_knob_ch == 0) ? 2 : (g_knob_ch - 1));
        else          g_knob_ch = (uint8_t) ((g_knob_ch >= 2) ? 0 : (g_knob_ch + 1));
        g_scene = g_knob_ch;
        knob_mode_indicator();
        dbglog_byte('S'); dbglog_byte(g_knob_ch);
#if SCENE_CHANGE_ON_KNOB_MODE
        korg_send_scene_change(g_knob_ch);   // measured: the real Korg does this
#endif
      }
      return false;
    }

    // Jog wheel push (note 101): latch/release the MixPre's Fast Forward,
    // which is what engages its shuttle mode (holding >> and turning the wheel
    // is what the operator does manually). The jog wheel then sets direction
    // and speed.
    if (d1 == 101) {
      if (cin == 0x9 && d2 > 0) {
        g_shuttle = !g_shuttle;
        uint16_t b = (uint16_t) (SC_OFF_TRANSPORT + 4 * 8);      // Forward (CC 76)
        uint8_t s[4] = {0x0B, scene_status(b), scene_cc(b), (uint8_t) (g_shuttle ? 0x7F : 0x00)};
        tud_midi_packet_write(s);
      }
      return false;
    }

    // Master-section buttons -> MixPre shortcut combos (Cycle + button).
    // Icon notes: 74 = Read, 75 = Write, 84 = Mute (measured).
    if (d1 == 74 || d1 == 75 || d1 == 84) {
      bool press   = (cin == 0x9 && d2 > 0);
      bool release = (cin == 0x8) || (cin == 0x9 && d2 == 0);

      // SAFETY: Tone / L-R mute are MixPre shortcuts that only exist while the
      // Cycle (shortcut mode) is ON. Ignore the buttons otherwise, exactly like
      // on the real nanoKONTROL.
      //
      // EXCEPTION - WRITE (Slate): the operator wants it available at any time
      // (it is momentary and momentary Slate is not destructive), so it does
      // not require the Cycle mode. The combination itself still pulses Cycle
      // around the button, so the mode is left exactly as it was.
      if (press && (g_cycle_on || d1 == 75)) {
        uint8_t idx = (uint8_t) ((d1 == 74) ? 7 : (d1 == 75) ? 8 : 9); // MarkerSet / MarkerLeft / MarkerRight
        sc_request(scene_cc((uint16_t) (SC_OFF_TRANSPORT + idx * 8)), !g_cycle_on);
        // Tone and L-R mute are MixPre shortcuts; the MixPre never reports
        // their state over MIDI, so keep the LEDs locally (toggle).
        if (d1 == 74) { g_tone_on   = !g_tone_on;   icon_note(74, (uint8_t) (g_tone_on   ? 0x7F : 0x00)); }
        if (d1 == 84) { g_lrmute_on = !g_lrmute_on; icon_note(84, (uint8_t) (g_lrmute_on ? 0x7F : 0x00)); }
        // WRITE (Slate) is MOMENTARY: the MixPre toggles the function on every
        // combination, so send it again on release -> active only while held.
        if (d1 == 75) { g_slate_held = true; icon_note(75, 0x7F); }

      // MOMENTARY release for WRITE: send the same combination again, which
      // toggles the Slate back off. The Cycle toggles twice, so it ends as ON.
      } else if (release && d1 == 75 && g_slate_held) {
        g_slate_held = false;
        sc_request(scene_cc((uint16_t) (SC_OFF_TRANSPORT + 8 * 8)), !g_cycle_on);   // Marker Left
        knob_mode_indicator();        // Write LED returns to the mode indicator
      }
      return false;
    }

    // Channel MUTE safety: the Mute buttons sit right next to the faders, and
    // an accidental mute during recording kills an ISO track. Muting is only
    // allowed while the Cycle (shortcut mode) is ON; UNmuting always works.
    if (d1 >= 0x10 && d1 <= 0x17 && cin == 0x9 && d2 > 0) {
      uint8_t midx = (uint8_t) (d1 - 0x10);
      if (!g_mute[midx] && !g_cycle_on) return false;
    }

    uint16_t base;
    // Select row (notes 24-31): the MixPre never reports the Select state, so
    // keep the SEL LED (and the channel LED) locally, radio style.
    if (d1 >= 24 && d1 <= 31 && cin == 0x9 && d2 > 0) {
      uint8_t idx = (uint8_t) (d1 - 24);
      if (g_sel >= 0) {                       // turn the previous one off
        icon_note((uint8_t) (24 + g_sel), 0x00);
        icon_note((uint8_t) (104 + g_sel), 0x00);
      }
      if (g_sel == (int8_t) idx) {
        g_sel = -1;                           // toggle off
      } else {
        g_sel = (int8_t) idx;                 // select this channel
        icon_note(d1, 0x7F);
        icon_note((uint8_t) (104 + idx), 0x7F);
      }
    }
    if (d1 <= 0x07)               base = (uint16_t) (SC_OFF_REC + d1 * 8);            // Rec 1-8
    else if (d1 <= 0x0F)          base = (uint16_t) (SC_OFF_SOLO + (d1 - 0x08) * 8);  // Solo 1-8
    else if (d1 <= 0x17)          base = (uint16_t) (SC_OFF_MUTE + (d1 - 0x10) * 8);  // Mute 1-8
    else if (d1 <= 0x1F)          base = (uint16_t) (SC_OFF_SELECT + (d1 - 0x18) * 8);// Select 1-8
    else {
      int idx = -1;
      switch (d1) {
        case 0x5E: idx = 0;  break; // Play
        case 0x5D: idx = 1;  break; // Stop
        case 0x5F: idx = 2;  break; // Record
        case 0x5B: idx = 3;  break; // Rewind
        case 0x5C: idx = 4;  break; // Forward
        case 0x56: idx = 5;  break; // Cycle
        case 0x4D: idx = 7;  break; // Marker Set
        case 0x4C: idx = 9;  break; // Next Marker
        // NOTE: notes 0x54 (84) and 0x4B (75) are the master Mute/Write buttons
        //       and are handled as shortcut combos above.
        case 0x50: idx = 10; break; // Previous Track
        case 0x51: idx = 11; break; // Next Track
        // Fader touch sensors, never forwarded: they only gate the faders.
        case 0x68: case 0x69: case 0x6A: case 0x6B:
        case 0x6C: case 0x6D: case 0x6E: case 0x6F: {
          uint8_t tch = (uint8_t) (d1 - 0x68);
          if (d2 > 0) {
            g_fader_touch |= (uint8_t) (1u << tch);
            // Measured: after releasing and grabbing the fader again, the real
            // Korg does NOT resend the value it last sent - it continues with
            // the next CHANGED value (test 3 in measurements/WZORZEC_korg_fader1.md).
            // So g_fader_last is deliberately not reset here.
          } else {
            g_fader_touch &= (uint8_t) ~(1u << tch);
          }
          g_touch_seen = true;
          return false;
        }
        default: break;
      }
      if (idx < 0) return false;
      base = (uint16_t) (SC_OFF_TRANSPORT + idx * 8);
    }
    uint8_t on = (cin == 0x9 && d2 > 0) ? 0x7F : 0x00;
    pkt[0] = 0x0B; pkt[1] = scene_status(base); pkt[2] = scene_cc(base); pkt[3] = on;
    return true;
  }

  if (cin == 0xE) {                               // Pitch Bend -> fader
    uint8_t ch = status & 0x0F;
    if (ch > 7) return false;

    // NOTE: the multi-channel "sweep detector" that used to live here has been
    // REMOVED. It treated moving 3+ faders within 300 ms as the controller's
    // calibration sweep and then blocked ALL faders for 1.2 s - i.e. it blocked
    // normal playing (fine with one fader at a time, unusable with several).
    // The controller's real power-up calibration is covered by the start-up
    // time gate in the main loop.
    // Forward only while the operator actually holds this fader, so the Icon
    // never applies a value the hand did not produce. See g_fader_touch above.
    if (!(g_fader_touch & (1u << ch)) && g_touch_seen) return false;

#if FADER_AS_PITCHBEND
    // The Korg sends its sliders as Pitch Bend - see the Korg MIDI
    // implementation kept in the project docs - not as Control Change. The
    // MixPre arms its pickup per message type, so CC 82-89 kept the faders
    // outside it (the knobs are CC and pick up correctly). Pass the pitch bend
    // through on the fader's own channel instead.
    pkt[0] = (uint8_t) (0xE0 | ch);
    pkt[1] = d1;
    pkt[2] = d2;
    pkt[3] = 0;
    return true;
#else
    // Tested and WRONG: the Korg doc's "Native Mode Slider Output" (BF 00+n)
    // is for the Korg's own editor, not what the MixPre listens to. With that
    // format the faders stopped responding entirely, while the CC 82-89 layout
    // (which the MixPre itself writes into the scene) works. Back to CC 82-89.
    {
      uint16_t base = (uint16_t) (SC_OFF_SLIDER + ch * 8);
      uint16_t v14 = (uint16_t) d1 | ((uint16_t) d2 << 7);
      uint8_t val = (uint8_t) (v14 >> 7);
      uint8_t st = scene_status(base), cc = scene_cc(base);

      // Measured on a real nanoKONTROL (measurements/WZORZEC_korg_fader1.md):
      // it sends a value only when it CHANGES - never the same value twice,
      // neither slow nor fast. Skipping values is normal there too (up to +-7
      // when the fader is moved quickly), so gaps are NOT a problem and we do
      // not fill them. Duplicates are the one thing the real device never
      // sends, so drop them.
      if (g_fader_last_ok[ch] && g_fader_last[ch] == val) return false;
      g_fader_last[ch] = val;
      g_fader_last_ok[ch] = true;

      pkt[0] = 0x0B; pkt[1] = st; pkt[2] = cc;
      pkt[3] = val;
      return true;
    }
#endif
  }

  if (cin == 0xB && d1 == 60) {                   // Jog wheel -> MixPre jog
    uint8_t vt = korg_scene_get(513);             // value type
    uint8_t ch = korg_scene_get(512); if (ch > 15) ch = 0;
    uint8_t cc = 0, val = d2;
    if (vt == 3) {                                // Sign Magnitude: single CC
      cc = korg_scene_get(515);
    } else if (vt <= 1) {                         // Inc/Dec: separate CW / CCW CCs
      if (d2 <= 0x3F) { cc = korg_scene_get(516); val = (d2 > 7) ? 7 : d2; }
      else            { cc = korg_scene_get(517); val = (uint8_t) (0x40 + ((d2 - 0x40) > 7 ? 7 : (d2 - 0x40))); }
    } else if (vt == 2) {                         // Continuous
      cc = korg_scene_get(518);
    }
    if (cc == 0 || cc == 0xFF) return false;
    pkt[0] = 0x0B; pkt[1] = (uint8_t) (0xB0 | ch); pkt[2] = cc; pkt[3] = val;
    return true;
  }

  if (cin == 0xB) {                               // CC -> encoder (relative)
    if (d1 < 0x10 || d1 > 0x17) return false;
    uint8_t idx = (uint8_t) (d1 - 0x10);
    int8_t delta = (d2 <= 0x07) ? (int8_t) d2 : ((d2 >= 0x41) ? (int8_t) - (d2 - 0x40) : 0);
    int v = (int) enc_val[idx] + delta;
    if (v < 0) v = 0;
    if (v > 127) v = 127;
    enc_val[idx] = (uint8_t) v;
    uint16_t base = (uint16_t) (SC_OFF_KNOB + idx * 8);
    // Knob MIDI channel selects the function: 0=Trim, 1=LowCut, 2=Pan
    uint8_t kch = (uint8_t) (0xB0 | g_knob_ch);
    // Suppress duplicates like the real nanoKONTROL does.
    if (g_knob_last_ok[idx] && g_knob_last[idx] == (uint8_t) v && g_knob_last_ch[idx] == kch) return false;
    g_knob_last[idx] = (uint8_t) v;
    g_knob_last_ch[idx] = kch;
    g_knob_last_ok[idx] = true;
    pkt[0] = 0x0B; pkt[1] = kch; pkt[2] = scene_cc(base); pkt[3] = (uint8_t) v;
    return true;
  }

  return false;
}

//--------------------------------------------------------------------+
// Translation MixPre (scene CC) -> Icon (MCU note = LED)
// The MixPre sends button state as CC on channel 0; the Icon lights its
// LEDs from Note On/Off (MCU).
//--------------------------------------------------------------------+
// Emit the queued shortcut. The Cycle button (a toggle on the MixPre) enables
// the shortcut mode; while it is ON a single click of the shortcut button runs
// the assigned function. So we only have to click the button here - and only
// when the mode is actually on (safety, see the handler).
// Queue a shortcut request. A sequence takes ~600 ms (6 x 100 ms), so a quick
// tap on a momentary button would otherwise overwrite the running sequence and
// the "release" combination would never be emitted - the Slate then stayed on.
// wrap = true means the shortcut mode (Cycle) is currently OFF, so the click
// must be wrapped: Cycle ON -> click -> Cycle OFF. That is what makes WRITE
// (Slate) work without the operator enabling the mode first.
static void sc_request(uint8_t cc, bool wrap) {
  if (sc_state == 0 && sc_pend_n == 0) {
    sc_btn_cc = cc;
    sc_wrap   = wrap;
    sc_state  = 1;
    sc_t      = to_ms_since_boot(get_absolute_time());
    return;
  }
  if (sc_pend_n < sizeof(sc_pend)) {
    sc_pend[sc_pend_n]   = cc;
    sc_pend_w[sc_pend_n] = wrap;
    sc_pend_n++;
  }
}

static void shortcut_service(void) {
  uint32_t now = to_ms_since_boot(get_absolute_time());

  // Safety net for the wrapped (WRITE/Slate) sequences: if anything interrupted
  // them - rapid presses overflowing the request queue - the MixPre can be left
  // with the shortcut mode (Cycle) engaged. The MixPre reports that state on
  // CC 57, so after the dust settles we check and turn it back off.
  if (sc_verify_at && now >= sc_verify_at && sc_state == 0 && sc_pend_n == 0) {
    sc_verify_at = 0;
    if (g_cycle_on) {
      uint8_t z[4] = {0x0B, scene_status(SC_OFF_TRANSPORT),
                      scene_cc((uint16_t) (SC_OFF_TRANSPORT + 5 * 8)), 0x00};
      tud_midi_packet_write(z);
    }
  }

  if (sc_state == 0) {
    if (sc_pend_n == 0) return;
    sc_btn_cc = sc_pend[0];                      // start the next queued request
    sc_wrap   = sc_pend_w[0];
    for (uint8_t i = 1; i < sc_pend_n; i++) {
      sc_pend[i - 1]   = sc_pend[i];
      sc_pend_w[i - 1] = sc_pend_w[i];
    }
    sc_pend_n--;
    sc_state = 1;
    sc_t     = now;
  }
  // The wrapped (WRITE/Slate) sequence is six steps, so it uses a much tighter
  // spacing to stay snappy: the plain click keeps 100 ms, the wrap uses 15 ms.
  if (now - sc_t < (sc_wrap ? 15u : 100u)) return;
  sc_t = now;
  uint8_t s[4] = {0x0B, scene_status(SC_OFF_TRANSPORT), 0, 0};
  uint8_t cycle_cc = scene_cc((uint16_t) (SC_OFF_TRANSPORT + 5 * 8));
  if (sc_wrap) {
    // The MixPre toggles the shortcut mode on the Cycle DOWN edge (127); the
    // release (0) does NOT toggle. So the mode must be toggled twice with a
    // FULL Cycle click on each side, otherwise it is left ON:
    //   Cycle down, Cycle up, button down, button up, Cycle down, Cycle up
    switch (sc_state++) {
      case 1: s[2] = cycle_cc;  s[3] = 0x7F; break;   // mode ON
      case 2: s[2] = cycle_cc;  s[3] = 0x00; break;
      case 3: s[2] = sc_btn_cc; s[3] = 0x7F; break;   // shortcut click
      case 4: s[2] = sc_btn_cc; s[3] = 0x00; break;
      case 5: s[2] = cycle_cc;  s[3] = 0x7F; break;   // mode back OFF
      case 6: s[2] = cycle_cc;  s[3] = 0x00; break;
      default: sc_state = 0; sc_verify_at = now + 400; return;
    }
  } else {
    switch (sc_state++) {
      case 1: s[2] = sc_btn_cc; s[3] = 0x7F; break;   // click down
      case 2: s[2] = sc_btn_cc; s[3] = 0x00; break;   // click up
      default: sc_state = 0; return;
    }
  }
  tud_midi_packet_write(s);
}

static bool translate_mixpre_to_icon(uint8_t pkt[4]) {
  if ((pkt[0] & 0x0F) != 0xB) return false;      // only CC
  uint8_t cc = pkt[2], val = pkt[3];

  // Track the MixPre's Cycle (shortcut mode) state - it gates the master-button
  // shortcuts. Leaving the mode also drops the local Tone / L-R mute LEDs.
  if (cc == scene_cc((uint16_t) (SC_OFF_TRANSPORT + 5 * 8))) {
    g_cycle_on = (val != 0);
    if (!g_cycle_on) {
      if (g_tone_on)   { g_tone_on = false;   icon_note(74, 0x00); }
      if (g_lrmute_on) { g_lrmute_on = false; icon_note(84, 0x00); }
    }
  }

  int note = -1;
  if (cc >= 21 && cc <= 28) {                              // Mute 1-8
    g_mute[cc - 21] = (val != 0);                          // remember the MixPre's state
    note = 16 + (cc - 21);
  }
  else if (cc >= 33 && cc <= 40) note = 8  + (cc - 33);  // Solo 1-8
  else if (cc >= 41 && cc <= 48) note = 0  + (cc - 41);  // Rec 1-8
  else if (cc >= 49 && cc <= 56) return false;           // Select: MixPre never
                                                         // asserts it; keep our local LED
  else {
    switch (cc) {
      case 75: note = 91; break;  // Rewind
      case 76: note = 92; break;  // Forward
      case 78: note = 93; break;  // Stop
      case 79: note = 94; break;  // Play
      case 80: note = 95; break;  // Record
      case 57: note = 86; break;  // Cycle
      default: return false;
    }
  }
  pkt[0] = 0x09;                  // Note On
  pkt[1] = 0x90;
  pkt[2] = (uint8_t) note;
  pkt[3] = (uint8_t) (val ? 0x7F : 0x00);
  return true;
}

//--------------------------------------------------------------------+
// Icon liveness probe (Mackie Control "Host Connection Query")
// The Icon answers F0 00 00 66 14 00 7F F7 with F0 00 00 66 14 01 F7.
// This is the only reliable way to tell "powered off but cable still plugged":
// after a mount the PIO-USB host holds the data lines in the idle J state, so
// both the D+ level and the TinyUSB "mounted" flag stop changing.
//--------------------------------------------------------------------+
static const uint8_t MCU_QUERY[8] = {0xF0, 0x00, 0x00, 0x66, 0x14, 0x00, 0x7F, 0xF7};
static uint32_t g_probe_ms = 0;

// Queue a raw SysEx to the Icon (split into USB-MIDI packets).
static void icon_sysex(const uint8_t *d, uint16_t n) {
  uint16_t i = 0;
  while (i < n) {
    uint16_t rem = n - i;
    uint8_t cnt = (rem >= 3) ? 3 : (uint8_t) rem;
    uint8_t cin;
    if (i + cnt == n) cin = (cnt == 3) ? 0x07 : (cnt == 2) ? 0x06 : 0x05;
    else              cin = 0x04;
    uint8_t pkt[4] = {cin, 0, 0, 0};
    for (uint8_t k = 0; k < cnt; k++) pkt[1 + k] = d[i + k];
    uint32_t q = midi_pack(pkt);
    queue_try_add(&q_mixpre_to_icon, &q);
    i += cnt;
  }
}

//--------------------------------------------------------------------+
// Core 1: USB host (Icon)
//--------------------------------------------------------------------+
void core1_main(void) {
  sleep_ms(10);

  // Allow core0 to lock this core out during flash writes (debug log)
  flash_safe_execute_core_init();

  pio_usb_configuration_t pio_cfg = PIO_USB_DEFAULT_CONFIG;
  tuh_configure(1, TUH_CFGID_RPI_PIO_USB_CONFIGURATION, &pio_cfg);
  tuh_init(1);

  uint8_t const dev = 1;

  while (true) {
    g_bb1 = 0x1001;                      // core1: entering tuh_task()
    tuh_task();
    g_bb1 = 0x1002;                      // core1: leaving tuh_task()
    g_core1_beat++;
    g_icon_mounted = tuh_midi_mounted(dev);

    if (g_icon_mounted) {
      uint8_t pkt[4];

      // Liveness probe every 2 s.
      // Sent DIRECTLY, not through q_mixpre_to_icon: the MixPre floods that
      // queue with LED updates (button states) and the probe was getting
      // dropped when it filled up - which looked like "the iCON disappeared".
      {
        uint32_t now = to_ms_since_boot(get_absolute_time());
        if (now - g_probe_ms >= 2000) {
          g_probe_ms = now;
          g_probe_state = 0;
          if (g_probe_tx < 255) g_probe_tx++;
          uint8_t p4[4];
          p4[0] = 0x04; p4[1] = MCU_QUERY[0]; p4[2] = MCU_QUERY[1]; p4[3] = MCU_QUERY[2];
          tuh_midi_packet_write(dev, p4);
          p4[0] = 0x04; p4[1] = MCU_QUERY[3]; p4[2] = MCU_QUERY[4]; p4[3] = MCU_QUERY[5];
          tuh_midi_packet_write(dev, p4);
          p4[0] = 0x06; p4[1] = MCU_QUERY[6]; p4[2] = MCU_QUERY[7]; p4[3] = 0x00;
          tuh_midi_packet_write(dev, p4);
        }
      }

      // Icon -> MixPre (raw, translated on core0)
      while (tuh_midi_packet_read(dev, pkt)) {
        g_bb1 = 0x1003;                  // core1: reading from the Icon
        g_icon_rx++; d_icon_rx++;
        d_cin[pkt[0] & 0x0F]++;

        // Host Connection Reply matcher: F0 00 00 66 14 01 ... F7
        if (g_probe_state == 0 && pkt[1] == 0xF0 && pkt[2] == 0x00 && pkt[3] == 0x00) {
          g_probe_state = 1;
        } else if (g_probe_state == 1 && pkt[1] == 0x66 && pkt[2] == 0x14 && pkt[3] == 0x01) {
          g_probe_state = 2;
        } else if (g_probe_state == 2 && (pkt[0] & 0x0F) >= 0x05) {
          g_probe_state = 0;
          if (g_probe_rx < 255) g_probe_rx++;
          g_icon_alive_ms = to_ms_since_boot(get_absolute_time());
        } else if (g_probe_state == 2 && pkt[1] == 0x66) {
          /* keep waiting */
        } else {
          g_probe_state = 0;
        }

        // SysEx filter. The Icon sends the middle packet of its reply with
        // CIN 0x0B - which means "Control Change" - so the translator read its
        // payload as CC 0x14 (5th knob) value +1 and slowly drifted the Pan of
        // channel 5, once per probe. Bytes inside a SysEx may look like any
        // MIDI message, so nothing between F0 and F7 may reach the translator.
        //
        // CRITICAL: the filter MUST time out. The Icon does not always complete
        // its reply (we see many F0 starts but almost no closing F7), and a
        // stuck "in SysEx" flag silently discarded EVERYTHING that followed -
        // the Slate, the faders, the knobs. 200 ms is far longer than a real
        // reply needs.
        {
          uint32_t now_sx = to_ms_since_boot(get_absolute_time());
          if (pkt[1] == 0xF0) {
            g_in_sysex = true;
            g_in_sysex_ms = now_sx;
            continue;                     // start byte: nothing to translate
          }
          if (g_in_sysex) {
            if (pkt[1] == 0xF7 || (now_sx - g_in_sysex_ms) > 200) g_in_sysex = false;
            continue;                     // payload (or the end) - never forward
          }
        }

        uint32_t p = midi_pack(pkt);
        if (!queue_try_add(&q_icon_to_mixpre, &p)) d_drop_to_mix++;
      }

      // MixPre -> Icon
      uint32_t p;
      while (queue_try_remove(&q_mixpre_to_icon, &p)) {
        midi_unpack(p, pkt);
        g_bb1 = 0x1004;                  // core1: writing to the Icon
        tuh_midi_packet_write(dev, pkt);
      }
    }
  }
}

//--------------------------------------------------------------------+
// Status LED (external, default GP2: LED + resistor to GND)
//   Icon connected     -> steady on
//   Icon not connected -> blinking (alert "szukam iCONa")
// The series resistor sets the brightness - the same in both states; PWM is used
// only as an on/off switch (that is how the blink is made). Change
// STATUS_LED_PIN if you wire it elsewhere.
//--------------------------------------------------------------------+
#define STATUS_LED_PIN   2
#define STATUS_LED_LEVEL 255      // 0..255 - PWM duty when the state is "on"

static void status_led_init(void) {
  gpio_set_function(STATUS_LED_PIN, GPIO_FUNC_PWM);
  uint slice = pwm_gpio_to_slice_num(STATUS_LED_PIN);
  pwm_config cfg = pwm_get_default_config();
  pwm_config_set_clkdiv(&cfg, 125.0f);      // 125 MHz / 125 = 1 MHz
  pwm_config_set_wrap(&cfg, 255);            // ~3.9 kHz - no visible flicker
  pwm_init(slice, &cfg, true);
  pwm_set_gpio_level(STATUS_LED_PIN, 0);
}

// Runs from a 100 ms timer IRQ, NOT from the main loop: if the PIO-USB host
// wedges when the controller disappears, both loops can stall, but the timer
// interrupt keeps firing - so the LED reacts within ~200 ms instead of waiting
// for the watchdog reboot.
static repeating_timer_t g_led_timer;

static uint32_t now_ms_led(void) { return to_ms_since_boot(get_absolute_time()); }

static volatile int16_t g_led_force = -1;   // >=0: forced PWM level, -1 = automatic

void status_led_force(uint8_t v) { g_led_force = (v == 0xFF) ? -1 : (int16_t) v; }

static bool status_led_timer_cb(repeating_timer_t *t) {
  (void) t;
  if (g_led_force >= 0) { pwm_set_gpio_level(STATUS_LED_PIN, (uint16_t) g_led_force); return true; }
  static uint32_t beat_prev = 0;
  static uint8_t  still = 0, tick = 0;
  static bool     on = false;

  if (g_core1_beat == beat_prev) { if (still < 10) still++; }
  else { still = 0; beat_prev = g_core1_beat; }

  // Ground truth: the USB D+ line. A full-speed device pulls D+ high through
  // its 1.5k pull-up; with nothing attached the Pico-PIO-USB internal pull-down
  // keeps it low. The TinyUSB "mounted" flag can stay stale, this cannot.
  // NOTE: Pico-PIO-USB sets GPIO_OVERRIDE_INVERT on both USB pins, so the
  // pad value read here is the INVERSE of the physical line level.
  g_usb_dp = !gpio_get(USB_DP_PIN);

  // Both conditions are needed:
  //   g_usb_dp        - the cable is plugged (D+ pull-up, powered by the Pico)
  //   g_icon_mounted  - the controller actually enumerated, i.e. it is powered
  //                     on and alive; a stale "mounted" is caught by D+
  // The controller counts as present only if it recently answered the MCU
  // liveness probe - that also covers "powered off but cable still plugged".
  // Probe every 2 s, so allow a comfortable margin: a reply that is merely
  // late must not make the LED blink while the controller is fine.
  // NOTE: do NOT gate on g_usb_dp here. Once a device has been mounted the
  // PIO-USB host drives the data lines itself, so the D+ level read back no
  // longer says anything - it made the LED blink while everything was fine.
  // The MCU liveness reply is the reliable signal.
  // Tolerant window: a single missed probe (or two) must NOT be mistaken for
  // "the controller is gone" - that used to blink the LED and, worse, made the
  // bridge detach from the MixPre while everything was fine.
  bool alive = (now_ms_led() - g_icon_alive_ms) < 12000;
  bool ok = g_icon_mounted && alive && (still < 2);

  if (ok) { on = true; pwm_set_gpio_level(STATUS_LED_PIN, STATUS_LED_LEVEL); return true; }

  if (++tick >= 4) { tick = 0; on = !on; }      // blink at ~1.25 Hz
  pwm_set_gpio_level(STATUS_LED_PIN, (uint16_t) (on ? STATUS_LED_LEVEL : 0));
  return true;
}

static void status_led_start(void) {
  add_repeating_timer_ms(100, status_led_timer_cb, NULL, &g_led_timer);
}

//--------------------------------------------------------------------+
// Core 0: USB device (MixPre, spoofed Korg)
//--------------------------------------------------------------------+
// Arm the log only ONCE per boot. It used to re-arm on every Korg SysEx from the
// MixPre, and arming resets the log - so the log always began at the LAST such
// message and everything before it, including the whole connection handshake,
// was wiped. That is what hid the interesting part of every session.
static void dbglog_arm_once(void) {
  static bool done = false;
  if (done) return;
  done = true;
  dbglog_arm();
}

int main(void) {
  g_bb1_prev = g_bb1; g_bb0_prev = g_bb0; g_hf_prev = g_hf;   // black box
  g_bb1 = 0; g_bb0 = 0; g_hf = 0;
  g_bootcnt++;                              // any reset bumps this
  g_rr = watchdog_caused_reboot() ? 1 : 2;  // 1 = watchdog reset the chip

  // The uninitialised RAM holds garbage after a flash (the bootloader uses it),
  // so clamp the values that are supposed to be small - otherwise the black box
  // reports nonsense like a boot count of 11240 or "HardFault" on a clean boot.
  if (g_bootcnt == 0 || g_bootcnt > 1000) g_bootcnt = 1;
  if (g_hf_prev > 1) g_hf_prev = 0;
  if (g_bb1_prev > 0xFFFFF) g_bb1_prev = 0;
  if (g_bb0_prev > 0xFFFFF) g_bb0_prev = 0;
  set_sys_clock_khz(120000, true);
  sleep_ms(10);
  dbglog_arm();   // DIAGNOSTIC: log from boot so the attach exchange is captured

  queue_init(&q_icon_to_mixpre, sizeof(uint32_t), MIDI_QUEUE_DEPTH);
  queue_init(&q_mixpre_to_icon, sizeof(uint32_t), MIDI_QUEUE_DEPTH);

  multicore_reset_core1();
  multicore_launch_core1(core1_main);

  tud_init(0);
  // NOTE: we deliberately stay attached at boot. Hiding immediately made the
  // bridge unreachable for debugging/flashing without an iCON connected. The
  // "controller presence" logic below detaches after ICON_GONE_MS without a
  // controller, which frees the MixPre panel just the same.
  dbglog_init();
  status_led_init();
  status_led_start();

  // Watchdog: if either core stalls (e.g. the PIO-USB host wedging when the
  // controller is unplugged) the bridge reboots itself instead of going dead.
  // core0 feeds it only while core1's heartbeat keeps moving.
  // NO watchdog. Measured with the reset-reason black box: when the controller
  // disappears the PIO-USB host stalls core1 for >8 s (it recovers by itself),
  // and every watchdog timeout therefore reset the Pico in the middle of a
  // session - the MixPre saw a brand new controller and asked to write the
  // settings again. A stalled core1 only means "no input from the controller"
  // (safe: nothing reaches the MixPre), so a reset is worse than the stall.
  // g_rr in the status still reports why the last boot happened.
  // (A HardFault does its own reboot, see isr_hardfault.)

  // drain USB control requests logged by the (patched) MIDI device driver
  extern volatile uint8_t  midid_ctl_cnt;
  extern volatile uint8_t  midid_ctl_rt[], midid_ctl_rq[];
  extern volatile uint16_t midid_ctl_wv[], midid_ctl_wi[], midid_ctl_wl[];
  static uint8_t ctl_last = 0;

  while (true) {
    g_bb0 = 0x2001;                      // core0: entering tud_task()
    tud_task();
    g_bb0 = 0x2002;                      // core0: leaving tud_task()

    // controller fader sweep finished? (quiet for FADER_SETTLE_MS)
    if (g_faders_sweep && (to_ms_since_boot(get_absolute_time()) - g_fader_last_ms) > FADER_SETTLE_MS) {
      g_faders_sweep = false;
      g_sweep_chan_mask = 0;
    }

    while (ctl_last < midid_ctl_cnt) {
      uint8_t i = ctl_last;
      uint8_t rec[8] = {'C', midid_ctl_rt[i], midid_ctl_rq[i],
                        (uint8_t) midid_ctl_wv[i], (uint8_t) (midid_ctl_wv[i] >> 8),
                        (uint8_t) midid_ctl_wi[i], (uint8_t) (midid_ctl_wl[i] & 0xFF), 0};
      dbglog_bytes(rec, 8);
      ctl_last++;
    }
    g_bb0 = 0x2003;
    dbglog_tick(to_ms_since_boot(get_absolute_time()));
    korg_emu_service();
    korg_emu_settings_flush();      // deferred flash save (debounced)
    g_bb0 = 0x2004;                      // core0: korg_emu serviced
    {                                  // worst core0 loop gap
      static uint32_t t_last0 = 0;
      uint32_t t0 = to_ms_since_boot(get_absolute_time());
      if (t_last0 && (t0 - t_last0) > d_gap0) d_gap0 = t0 - t_last0;
      t_last0 = t0;
    }
    knob_mode_blink_service();

#if SCENE_ANNOUNCE_AT_ATTACH
    // Announce the current scene once, shortly after the device side comes up -
    // exactly what the real nanoKONTROL does when it finishes loading a scene.
    {
      static bool     dev_prev = false;
      static bool     announced = false;
      static uint32_t mounted_ms = 0;
      bool dm = tud_midi_mounted();
      uint32_t now = to_ms_since_boot(get_absolute_time());
      if (dm && !dev_prev) { mounted_ms = now; announced = false; }
      if (!dm) announced = false;
      if (dm && !announced && (now - mounted_ms) > SCENE_ANNOUNCE_DELAY_MS) {
        korg_send_scene_change((uint8_t) g_knob_ch);
        announced = true;
        dbglog_byte('A'); dbglog_byte((uint8_t) g_knob_ch);
      }
      dev_prev = dm;
    }
#endif

    // --- controller presence -> device side -------------------------------
    // Only occupy the MixPre while the iCON answers the liveness probe. When it
    // goes away (>6 s of silence) soft-disconnect so the MixPre frees its panel
    // and can be operated by hand; reconnect automatically when it is back.
    {
      uint32_t now = to_ms_since_boot(get_absolute_time());
      // Only detach from the MixPre when the controller is REALLY gone.
      // 20 s = 10 missed probes, so transient misses never disturb the MixPre.
      bool icon_ok = g_icon_mounted && (now - g_icon_alive_ms) < 12000;
      if (icon_ok && !g_dp_attached) {
        tud_connect();
        g_dp_attached = true;
      } else if (!icon_ok && g_dp_attached && (now - g_icon_alive_ms) > 20000) {
#if DP_DETACH
        tud_disconnect();
        g_dp_attached = false;
#endif
      }
    }

    // Feed the watchdog only if core1 is still alive (it owns the USB host).
    {
      static uint32_t beat_last = 0, beat_same = 0;
      if (g_core1_beat != beat_last) { beat_last = g_core1_beat; beat_same = 0; }
      else if (beat_same < 100000) beat_same++;
      if (beat_same < 100000) watchdog_update();
    }
    if (g_dbg_led_note >= 0) {          // debug: light an Icon LED
      uint8_t lp[4] = {0x09, (uint8_t) (0x90 | (g_dbg_led_ch & 0x0F)),
                       (uint8_t) g_dbg_led_note, g_dbg_led_val};
      uint32_t lq = midi_pack(lp);
      queue_try_add(&q_mixpre_to_icon, &lq);
      g_dbg_led_note = -1;
    }
    if (tud_midi_mounted()) shortcut_service();

    if (tud_midi_mounted()) {
      uint8_t pkt[4];

#if SNIFFER
      // transparent: host (real nanoKONTROL) -> MixPre, no translation
      // start the settle window on a fresh controller mount
      {
        bool m = g_icon_mounted;
        if (m && !g_icon_prev) {
          g_gate_until_ms = to_ms_since_boot(get_absolute_time()) + CONTROLLER_SETTLE_MS;
          g_fader_last_ms = to_ms_since_boot(get_absolute_time());
          g_faders_sweep = false;
          g_sweep_chan_mask = 0;
          for (int i = 0; i < 8; i++) { g_fader_arm[i] = false; g_fader_rest[i] = 0xFFFF; }
        }
        g_icon_prev = m;
      }

      uint32_t p;
      while (queue_try_remove(&q_icon_to_mixpre, &p)) {
        midi_unpack(p, pkt);
        // SNIFFER: true passthrough - the MixPre<->Korg handshake must not be gated
        if ((pkt[0] & 0x0F) != 0) { dbglog_byte('i'); dbglog_bytes(pkt, 4); }
        if (!tud_midi_packet_write(pkt)) mtx_push(midi_pack(pkt));   // retry later
      }
      // MixPre -> host: TRUE passthrough (real Korg gets everything except our
      // 0x7D debug family), so the MixPre<->Korg interaction is authentic
      while (tud_midi_packet_read(pkt)) {
        // A Korg SysEx from the MixPre marks the start of a session: arm the log
        // (once - arming resets it, and we want the whole session in one piece)
        if ((pkt[0] & 0x0F) == 0x4 && pkt[1] == 0xF0 && pkt[2] == 0x42) dbglog_arm_once();
        if ((pkt[0] & 0x0F) != 0) { dbglog_byte('>'); dbglog_bytes(pkt, 4); }
        if (korg_emu_debug_feed(pkt)) continue;
        uint32_t pp = midi_pack(pkt);
        queue_try_add(&q_mixpre_to_icon, &pp);
      }
#else

      // Icon -> MixPre (translated)
      // start the settle window on a fresh controller mount
      {
        bool m = g_icon_mounted;
        if (m && !g_icon_prev) {
          uint32_t now = to_ms_since_boot(get_absolute_time());
          // Re-arm the fader gate only on a genuine replug. If the host merely
          // re-detects an already connected controller the gate must not be
          // re-armed, because that swallowed the first event of every burst.
          if (now - g_icon_lost_ms >= 500) {
            knob_mode_indicator();          // show the current knob function
            g_gate_until_ms = now + CONTROLLER_SETTLE_MS;
            g_fader_last_ms = now;
            g_faders_sweep = false;
            g_sweep_chan_mask = 0;
          }
        }
        if (!m) g_icon_lost_ms = to_ms_since_boot(get_absolute_time());
        g_icon_prev = m;
      }

      uint32_t p;
      while (queue_try_remove(&q_icon_to_mixpre, &p)) {
        midi_unpack(p, pkt);
        // The start-up gate covers the FADERS only (Pitch Bend). Buttons,
        // knobs and the jog must always pass.
        if (((pkt[0] & 0x0F) == 0xE) &&
            to_ms_since_boot(get_absolute_time()) < g_gate_until_ms) continue;
        if ((pkt[0] & 0x0F) != 0) { dbglog_byte('i'); dbglog_bytes(pkt, 4); }

        // A fader is a fader before the translator rewrites the packet.
        bool is_fader = (((pkt[0] & 0x0F) == 0xE) != 0);

        if (!translate_icon_to_mixpre(pkt)) continue;

        // Motorized fader feedback: echo the position back to the Icon, but
        // ONLY for values we actually forwarded. Echoing the Icon's power-up
        // dump made its motors move, and motor movement reads back as an
        // operator touch, which reopened the touch gate - so the dump reached
        // the MixPre anyway. The motor is meant to reproduce what the MixPre
        // got, not what the Icon happened to spit out.
#if FADER_ECHO
        if (is_fader) {
          uint32_t echo = p;
          queue_try_add(&q_mixpre_to_icon, &echo);
        }
#endif

        mtx_flush();                       // push out the backlog first
        d_tx_mix++;
        dbglog_byte('=');
        dbglog_bytes(pkt, 4);
        if (!tud_midi_packet_write(pkt)) mtx_push(midi_pack(pkt));   // retry later
      }

      // MixPre -> Icon (Korg SysEx handled locally)
      while (tud_midi_packet_read(pkt)) {
        if ((pkt[0] & 0x0F) == 0x4 && pkt[1] == 0xF0 && pkt[2] == 0x42) dbglog_arm_once();
        if ((pkt[0] & 0x0F) != 0) { dbglog_byte('>'); dbglog_bytes(pkt, 4); }
        if (korg_emu_feed(pkt)) continue;
        if (translate_mixpre_to_icon(pkt)) {
          uint32_t pp = midi_pack(pkt);   // LED note to the Icon
          queue_try_add(&q_mixpre_to_icon, &pp);
        }
      }
#endif
    }
  }

  return 0;
}
