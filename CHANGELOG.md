# Changelog

The version number lives in `pico-bridge/firmware/version.h`. Every release bumps
it, the `.uf2` file carries the number in its name, and the firmware can be asked
over MIDI which version it is: send `F0 7D 61 F7`.

## 1.2.2

First public release. The numbers below it, 1.0.0 to 1.2.1, were development
builds and were never published.

- Presents a Korg nanoKONTROL Studio to the MixPre, so the recorder accepts the
  bridge at all.
- Translates Mackie Control from the surface: faders, knobs with three functions,
  mute, solo, rec arm, select, transport, jog wheel, slate, tone and L/R mute.
- Fader pairing: up to four groups, so two or more faders move as one. Set with
  Mixer, knob 1 to 4 and the Select buttons, while Cycle is off.
- Knob function indicator on the Write LED: off for Trim, steady for LowCut,
  blinking for Pan.
- Status LED on GP2: steady when the surface is talking, blinking when it is not.
- Keeps the controller configuration in flash, including the three scenes that the
  MixPre writes.
- Built and tested with the iCON Platform M+ on MixPre-6 and MixPre-10, and used
  over many days on a film production.
