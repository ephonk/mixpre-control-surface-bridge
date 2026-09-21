# USB descriptors: Korg nanoKONTROL Studio reference

Read from the real device with libusb (`GET_DESCRIPTOR`), with the bridge plugged
in next to it for comparison.

## Device descriptor

Identical on both, which is the point: the MixPre matches controllers against a
whitelist, so the bridge has to be indistinguishable from one of them.

| Field | Value |
|---|---|
| idVendor | `0x0944` |
| idProduct | `0x0134` |
| bcdDevice | `0x0100` |
| bcdUSB | `0x0110` |
| bDeviceClass | `0x00` |
| bDeviceSubClass | `0x00` |
| bDeviceProtocol | `0x00` |
| bMaxPacketSize0 | 64 |
| iManufacturer | 1 |
| iProduct | 2 |
| iSerialNumber | 0 |
| bNumConfigurations | 1 |

## String descriptors

| Index | String |
|---|---|
| 1 | `KORG INC.` |
| 2 | `nanoKONTROL Studio` |
| 3 | `nanoKONTROL Studio _ CTRL` |
| 4 | `nanoKONTROL Studio _ CTRL` |
| 5 | `EXT0` |

String 3 and 4 belong to the MIDI jacks (embedded and external), string 5 names
the external jack. That is why macOS shows the real Korg as
`nanoKONTROL Studio _ CTRL`, taking the jack name, while a device without jack
strings shows as plain `nanoKONTROL Studio`.

## Configuration descriptor, Korg (83 bytes, one interface)

```
09 02 53 00 01 01 00 80 64   config: total 0x53, 1 interface, 0x80, 200 mA
09 04 00 00 02 01 03 00 00   itf0: Audio(1) / MIDIStreaming(3), 2 endpoints
07 24 01 00 01 25 00         CS MS header
06 24 02 01 10 03            IN jack embedded  id 0x10, iJack 3
09 24 03 02 40 01 10 01 05   OUT jack external id 0x40, source 0x10, iJack 5
09 24 03 01 30 01 20 01 04   OUT jack embedded id 0x30, source 0x20, iJack 4
06 24 02 02 20 05            IN jack external  id 0x20, iJack 5
09 05 01 02 40 00 00 00 00   EP 0x01 OUT bulk 64
05 25 01 01 10               CS endpoint, jack 0x10
09 05 82 02 40 00 00 00 00   EP 0x82 IN bulk 64
05 25 01 01 30               CS endpoint, jack 0x30
```

The bridge reproduces this byte for byte. No AudioControl interface, endpoint
`0x82` for IN, jack strings present.

## Configuration descriptor, stock TinyUSB (101 bytes, two interfaces)

This is what TinyUSB's MIDI device class produces by default. Kept here to
document why the class driver needs a patch.

```
09 02 65 00 02 01 00 80 32   config: total 0x65, 2 interfaces, 100 mA
09 04 00 00 00 01 01 00 00   itf0: Audio(1) / AudioControl(1), 0 endpoints
09 24 01 00 01 09 00 01 01   CS AC header
09 04 01 00 02 01 03 00 00   itf1: Audio(1) / MIDIStreaming(3), 2 endpoints
07 24 01 00 01 41 00         CS MS header
06 24 02 01 01 00            IN jack embedded  id 1, iJack 0
06 24 02 02 02 00            IN jack external  id 2, iJack 0
09 24 03 01 03 01 02 01 00   OUT jack embedded id 3, source 2, iJack 0
09 24 03 02 04 01 01 01 00   OUT jack external id 4, source 1, iJack 0
09 05 01 02 40 00 00 00 00   EP 0x01 OUT bulk 64
05 25 01 01 01               CS endpoint, jack 1
09 05 81 02 40 00 00 00 00   EP 0x81 IN bulk 64
05 25 01 01 03               CS endpoint, jack 3
```

## Differences

| Element | Korg | Stock TinyUSB |
|---|---|---|
| Interfaces | 1, MIDIStreaming only | 2, AudioControl plus MIDIStreaming |
| IN endpoint | `0x82` | `0x81` |
| Jack strings | 3, 4, 5 | none |
| Port name | `nanoKONTROL Studio _ CTRL` | `nanoKONTROL Studio` |

Note on `bMaxPacketSize0`: the stock TinyUSB default and the real nanoKONTROL
Studio both use 64, so `CFG_TUD_ENDPOINT0_SIZE` is set to 64 and this is no
longer a difference. An earlier revision of this table listed 16 for the Korg,
but that value was an assumption carried in our own source, not a measurement of
the device: `ioreg` reports 64 for the live unit, and the official firmware
declares `0x40` in both the normal (PID `0x0134`) and bootloader (PID `0x1001`)
descriptors.

## Why the MIDI device class is patched

TinyUSB's `midid_open()` requires AudioControl as the first interface
(`src/class/midi/midi_device.c`) and rejects a layout that is MIDIStreaming
alone. The Korg has no AudioControl interface at all, so the descriptor cannot
be cloned without patching the class.

The patch has two parts:

1. `midid_open()` accepts an interface that is MIDIStreaming only, in addition
   to the original AudioControl path.
2. `midid_control_xfer_cb()` records control requests and returns true for
   standard requests, which turned out to matter during bring-up.

The patch lives in the vendored SDK, which is not committed. It has to be
re-applied after cloning `pico-sdk`.

Our layout with AudioControl plus MIDIStreaming is the one that follows the USB
MIDI specification. The Korg's is the non-standard one, and it is one of the
handful of controllers the MixPre accepts, which is the entire reason for this
document.
