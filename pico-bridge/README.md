# MixPre bridge: engineering notes

Detailed notes for the bridge firmware. For the short version see the top level
`README.md`.

## Architecture

Two USB roles on one Pico:

- **Core 0, native USB port, device side.** Presents itself to the MixPre as a
  Korg nanoKONTROL Studio and runs the Korg SysEx handshake, the scene and
  global data, and the CC translation towards the recorder.
- **Core 1, PIO USB on GP0/GP1, host side.** Talks to the control surface
  (Mackie Control / MCU) and translates its messages towards the recorder.

MIDI packets travel between the cores in two queues.

`system clock 120 MHz`. Settings written by the MixPre are persisted in the
Pico's flash so the device looks known on later connections.

## Hardware

| Pico 2 W | part | cable wire |
|---|---|---|
| GP0 (pin 1) | 22 Ω in series | D+ |
| GP1 (pin 2) | 22 Ω in series | D− |
| GND (pin 38) | | GND |
| VBUS (pin 40) | | VBUS, needed so the iCON's USB interface stays alive |
| GP2 (pin 4) | 6.8 kΩ to LED anode | status LED |
| GND (pin 3) | | LED cathode |

Pins 3 and 38 are both ground. The LED uses pin 3 because the LED sits on that
side of the board, and the cable's ground uses pin 38 next to VBUS.

The 22 Ω resistors belong at the Pico side, right at the pins: they are series
termination for the host driver.

Keep D+ and D− as a twisted pair and untwist them only over the last 2 to 3 cm
where the resistors sit. That pigtail is the electrically worst part of the
build, which is exactly why it should be short.

**Cable shield: leave it disconnected.** Measured on the iCON, the USB shell is
not tied to its signal ground nor to its DC input, so there is no ground loop to
avoid, and at Full Speed the twisted pair does the work. If interference ever
appears, connect the shield to Pico ground on one end only, never both.

### Status LED

- surface answering the liveness probe: steady
- surface absent, powered off, or silent: blinking at about 1.25 Hz

The LED is driven from a 100 ms timer interrupt, not from the main loop, so it
keeps working even if a core stalls. PWM is used purely as an on/off switch.

## Build

```bash
cd pico-bridge/firmware
export PICO_SDK_PATH=$PWD/../pico-sdk
export PICO_TOOLCHAIN_PATH=/path/to/arm-gnu-toolchain
export PICO_BOARD=pico2_w
cmake -B build -G "Unix Makefiles"
cmake --build build
```

Result: `build/mixpre_bridge_<version>.uf2`, where the version comes from
`version.h`. Hold BOOTSEL, plug the Pico in, copy the file to the RP2350 drive.

`pico-sdk` and `Pico-PIO-USB` are vendored but not committed. Clone them into
`pico-bridge/` first.

## MixPre scene, confirmed from the device

This is what the MixPre actually writes back to the controller. It is identical
on MixPre-6 and MixPre-10.

- Faders: CC 82 to 89, channel 0
- Knobs: CC 13 to 20, **channel 2**
- Solo: CC 33 to 40
- Mute: CC 21 to 28
- Rec arm: CC 41 to 48
- Select: CC 49 to 56
- Transport: Play 79, Stop 78, Record 80, Rewind 75, Fast Forward 76,
  Cycle 57, Return to Zero 77, Marker Set 58, Previous Marker 59,
  Next Marker 60, Previous Track 66, Next Track 67
- Jog wheel: value type Sign Magnitude, CC 81, channel 0

Two facts that took measuring rather than guessing:

- **The knob function is the MIDI channel of the knob CCs.** Channel 0 gives
  Trim, 1 gives LowCut, 2 gives Pan. The MixPre writes channel 2 into its scene,
  which is why the knobs are stuck on Pan unless the bridge overrides the
  channel. The firmware defaults to Trim and switches on the knob push buttons
  and on `|<` / `|>`.
- **The MixPre reports button states only.** It never sends fader positions,
  never knob values, and never the Slate state. Verified on both models by
  logging every packet during a fader sweep, a knob turn and a Slate toggle.

## Target protocol: Korg nanoKONTROL Studio, Native Mode

The MixPre accepts a short list of controllers (Akai MIDIMix, Korg nanoKONTROL 2,
Korg nanoKONTROL Studio, Novation LaunchControl XL) and nothing else, so the
bridge has to reproduce one of them exactly.

### USB descriptors

Reproduced byte for byte: VID `0x0944`, PID `0x0134`, `bcdUSB 0x0110`,
`bMaxPacketSize0 = 16`, `bcdDevice 0x0100`, one interface (Audio /
MIDIStreaming), endpoints `0x01` and `0x82`, string indices 1 to 5
(`KORG INC.`, `nanoKONTROL Studio`, `nanoKONTROL Studio _ CTRL` twice, `EXT0`),
`iSerialNumber = 0`. Config descriptor total 83 bytes. See
`docs/korg-usb-descriptors.md`.

### SysEx handshake

| Step | Message |
|---|---|
| Search device | `F0 42 50 00 dd F7` |
| Global data dump request | `0E` |
| Scene data dump request | `10` |
| Scene write | `40` |
| Write request | `11`, answered with ACK `21` / `23` |
| Identity request | `F0 7E gg 06 01 F7` |

Escaping: 7-bit packing, one MSB byte per group of seven data bytes.

Device to host scene change: `F0 42 40 00 01 37 02 00 00 4F ss F7`.
Host to device scene change request: same with `14 ss`. The real Korg answers
with an ACK only.

Note: a Scene Change resets the MixPre's internal "values matched" state, so the
bridge never sends one.

## Source protocol: Icon Platform M+, Mackie Control

Faders are Pitch Bend on channels 1 to 8. Knobs are CC 16 to 23, relative.
Knob push is notes 32 to 39, Rec notes 0 to 7, Solo 8 to 15, Mute 16 to 23,
Select 24 to 31, transport notes 86 and 91 to 95, jog CC 60.

Mode change: set it with the iCON software or at start-up, see the iCON manual.

## Translation

### Surface to MixPre

| Surface (MCU) | Result |
|---|---|
| Pitch Bend channels 1 to 8 | fader CCs 82 to 89, 14-bit reduced to the top 7 bits |
| CC 16 to 23, relative | knob CCs 13 to 20, accumulated to absolute, channel set by the knob function |
| Notes 0 to 7 / 8 to 15 / 16 to 23 / 24 to 31 | Rec / Solo / Mute / Select |
| Notes 91 to 95 and 86 | Rewind, Fast Forward, Stop, Play, Record, Cycle |
| Notes 75 and 76 | Marker Set, Next Marker |
| CC 60 | jog wheel, same Sign Magnitude value |
| Notes 32 to 39, 48, 49 | switch the knob function, not forwarded |
| Notes 46 and 47 | deliberately unhandled, reserved for a channel bank jump (measured: Bank `<` and Bank `>`) |
| Notes 74 / 75 / 84 | Read / Write / Mixer, see the shortcuts below |

### MixPre to surface

| MixPre CC | Surface |
|---|---|
| 21 to 28 / 33 to 40 / 41 to 48 | Mute / Solo / Rec LEDs, notes 16 to 23 / 8 to 15 / 0 to 7 |
| 57 and 75 to 80 | Cycle and transport LEDs, notes 86 and 91 to 95 |
| 49 to 56 | ignored, the MixPre never reports Select, so the SEL LED is local |
| Pitch Bend | echoed back so the motorized faders hold their position |

### User assignable shortcuts

Tone, L/R mute, Slate and Shuttle exist on the MixPre only as assignable
shortcuts. Assign them in its menu:

| MixPre shortcut | Assign | Driven by |
|---|---|---|
| Marker Set | Tone | Read, note 74 |
| Marker Left | Slate | Write, note 75 |
| Marker Right | L/R Mute | Mixer, note 84 |
| Fast Fwd | Shuttle | jog push |

Note 84 is the button the iCON panel labels Mixer; Mackie Control calls it Mute.
All four numbers here were measured with a MIDI monitor on a Platform M+
(firmware V2.19).

The mechanism, established by measurement:

- **Cycle acts as a toggle that enables the shortcut mode.** The MixPre toggles
  it on the `Cycle 127` edge; the release does not toggle.
- While the mode is on, a single click of the assigned button runs the function.
- **Read and Mixer (note 84) only work while Cycle is on.** That is deliberate: the Mute
  button sits next to the faders, and an accidental mute during a take would
  kill an ISO track. Unmuting always works, so a muted channel can always be
  rescued. The local LED follows the MixPre's reported state.
- **Write (Slate) works regardless of Cycle.** It is momentary, so it is not
  destructive. When Cycle is off, the click is wrapped in a full Cycle click on
  each side, which returns the mode to its previous state. When Cycle is on,
  only the click is sent. The wrapper steps run at 15 ms so it feels as fast as
  the plain click.
- After a wrapped sequence the firmware checks the reported mode state and
  closes it if a sequence was interrupted, for example by rapid presses
  overflowing the request queue.

### Fader pairing (bridge only)

Four groups, tied in the bridge and never written into a scene. Mixer (84) is free
while Cycle is off, so it doubles as the modifier:

| Step | Effect |
|---|---|
| Mixer pressed, Cycle off | `g_pair_edit` set, Select LEDs blanked |
| knob 1 to 4 while held | pick group 1 to 4 into `g_pair_target` |
| Select click | toggle that channel in the target group |
| Mixer released | drop any group left with fewer than two channels |

While `g_pair_edit` is set, Select is consumed locally: the note never reaches
`SC_OFF_SELECT`, because the MixPre screen would otherwise jump on every click and
the release edge would clear the selection again. The knob branch returns before
the scene change for the same reason.

A fader move puts the same value on every member's channel and queues the
matching echo for each motor, so the group travels as one block. The members'
`g_fader_last` is updated too, so a copy is never sent twice.

Echoes have their own queue, drained before the LED traffic. Sharing the main
queue put an echo behind as many as 255 button-state packets, and the motors then
followed the hand late. When it fills, the oldest value goes: a motor only cares
where the fader is now.

Groups live in RAM, not in flash, and the recorder is told nothing. The bridge has
no knowledge of the channel linking in the MixPre menu, so the two must never be
applied to the same pair.

### Jog and shuttle

Turning the jog sends CC 81, the recorder's jog CC, with the same value. On the
MixPre, scrubbing needs Shuttle mode active, hence the jog push driving the
Shuttle shortcut.

### SysEx filter

Bytes inside a SysEx can look like any MIDI message. The iCON sends the middle
packet of its reply with CIN `0x0B`, which reads as Control Change, and the
translator took its payload for a knob CC and slowly drifted the Pan of channel
5 once per probe. Everything between `F0` and `F7` is therefore dropped.

**This filter must time out.** The iCON does not always complete its reply, and
a stuck "inside SysEx" flag silently discarded everything that followed: the
Slate, the faders, the knobs. The flag clears after 200 ms.

### Start-up fader gate

After the bridge starts, the surface runs its own calibration and drives the
faders to zero. Forwarding that would move the MixPre's mix. A 5 s window after
the surface appears therefore drops Pitch Bend only. Buttons, knobs and the jog
pass at all times.

An earlier version also tried to detect a multi-channel sweep, treating three or
more channels within 300 ms as calibration. That blocked normal playing: moving
several faders at once is not a calibration. It was removed.

### Controller presence and the MixPre panel

A connected controller locks the MixPre's own panel, so the bridge occupies the
MixPre only while the surface actually answers.

| Situation | Behaviour |
|---|---|
| surface answering | MixPre occupied, `tud_connect` |
| surface missing at start | bridge stays attached at first, so it can be flashed and debugged |
| surface silent | after 20 s, `tud_disconnect`, which frees the MixPre panel |
| surface back | reconnects automatically |

The surface is probed every 2 s with the Mackie Host Connection Query
(`F0 00 00 66 14 00 7F F7`). A live surface answers
`F0 00 00 66 14 01 F7`. The probe is sent directly from core 1, not through the
queue, because the MixPre floods that queue with LED updates and the probe used
to get dropped when it filled.

No reply for 12 s blinks the LED, no reply for 20 s detaches from the MixPre. A
MIDI Identity Request is useless here, the iCON does not implement it.

Side effect: with no surface attached the bridge is invisible to the host, so
the debug commands only work while a surface is connected.

## Debug commands

Send as SysEx over MIDI.

| Command | Effect |
|---|---|
| `F0 7D 67 F7` | status: mount state, VID/PID, counters, boot count, reset reason |
| `F0 7D 65 F7` | CFSR, HFSR, BFAR, core, PC and LR of the last hard fault |
| `F0 7D 64 vv F7` | force the status LED to level vv, `FF` returns to automatic |
| `F0 7D 63 F7` | counters: packets in, sent, rejected, dropped, worst loop gaps |
| `F0 7D 62 F7` | histogram of packet types read from the surface |
| `F0 7D 6A F7` | return the stored scene, 560 bytes as hex |
| `F0 7D 7C F7` | return the debug log |
| `F0 7D 7B F7` | clear and arm the debug log |
| `F0 7D 7A F7` | reboot |
| `F0 7D 7E F7` | reboot into BOOTSEL |

PC and LR from `F0 7D 65 F7` resolve with
`arm-none-eabi-addr2line -e build/mixpre_bridge.elf`, which is how the hard
fault inside TinyUSB was found.

## Code layout

```
firmware/
  main.c             bridge, both cores, queues, translation, safety gates
  korg_emu.c/h       Korg handshake, scene and global data, settings persistence
  midi_host.c/h      USB host MIDI class driver (TinyUSB has no MIDI host)
  usb_descriptors.c  Korg descriptors
  tusb_config.h      TinyUSB configuration
  dbglog.c/h         debug log in flash
  korg_defaults.h    factory scene and global data
```

## Bugs found the hard way

Kept here so nobody "simplifies" them back.

- **`midih_close` must not close the endpoint streams while the host is still
  finishing aborted transfers.** Doing so corrupted a FIFO buffer pointer and
  the next pull hard faulted inside TinyUSB's `_ff_pull_n`. The streams are left
  alone and re-initialised on the next mount instead.
- **`tud_midi_packet_write` returns false when TinyUSB's MIDI FIFO is full**,
  which happens in bursts. The packet used to be lost silently and the MixPre
  never saw that movement. There is now a retry queue.
- **The start-up gate applies to faders only.** Applying it to everything made
  every button need two presses.
- **No watchdog.** When the surface disappears the PIO USB host stalls core 1
  for several seconds and then recovers by itself. A watchdog turned that into
  a reset in the middle of a session, which made the MixPre see a brand new
  controller. A stalled core 1 only means no input from the surface, which is
  safe.
- **A stuck SysEx filter discards everything.** See above. Any state machine on
  a MIDI path needs a timeout.
- **The debug log writes to flash and stalls both cores.** Useful for
  diagnosing, harmful in use. It is off in normal builds.

## Status

Working and field tested in production.

- Faders, knobs with three functions, Rec / Solo / Mute / Select with LEDs,
  transport with LEDs, jog and shuttle, Slate, Tone, L/R mute
- Settings persistence, liveness probe, status LED, panel handover
- Safety: start-up fader gate, mute gated behind Cycle, shortcuts gated behind
  Cycle

Known limits, both on the MixPre side: no fader position feedback, no knob value
feedback. Details in the top level `README.md`.
