# Changelog

The version number lives in `pico-bridge/firmware/version.h`. Every release bumps
it, the `.uf2` file carries the number in its name, and the firmware can be asked
over MIDI which version it is: send `F0 7D 61 F7`.

## 1.0.0

First public release.

- Presents a Korg nanoKONTROL Studio to the MixPre, so the recorder accepts the
  bridge at all.
- Translates Mackie Control from the surface: faders, knobs with three functions,
  mute, solo, rec arm, select, transport, jog wheel, slate, tone and L/R mute.
- Status LED on GP2: steady when the surface is talking, blinking when it is not.
- Keeps the controller configuration in flash, including the three scenes that the
  MixPre writes.
- Built and tested with the iCON Platform M+, on MixPre-6 and MixPre-10.
