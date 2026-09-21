/*
 * USB device descriptors — spoof Korg nanoKONTROL Studio.
 *
 * !!! USTAWIĆ WG system_profiler SPUSBDataType (sekcja Korg) !!!
 * MixPre może weryfikować VID/PID, stringi lub nazwy jacków MIDI.
 * Klonujemy wszystko 1:1.
 */

#include "tusb.h"

//--------------------------------------------------------------------+
// KORG nanoKONTROL Studio — dane potwierdzone z system_profiler/ioreg
//--------------------------------------------------------------------+
#define KORG_VID             0x0944   // Korg Inc.
#define KORG_NANOKONTROL_PID 0x0134   // nanoKONTROL Studio (potwierdzone)
#define KORG_BCD_DEVICE      0x0100   // Version 1.00
#define KORG_BCD_USB         0x0110   // USB 1.1

#define STR_MANUFACTURER     "KORG INC."
#define STR_PRODUCT          "nanoKONTROL Studio"

//--------------------------------------------------------------------+
// Device Descriptor
//--------------------------------------------------------------------+
tusb_desc_device_t const desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = KORG_BCD_USB,
    .bDeviceClass       = 0x00,
    .bDeviceSubClass    = 0x00,
    .bDeviceProtocol    = 0x00,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,

    .idVendor           = KORG_VID,
    .idProduct          = KORG_NANOKONTROL_PID,
    .bcdDevice          = KORG_BCD_DEVICE,

    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x00,   // real nanoKONTROL has NO serial string (see docs).
                                  // It was set to 0x06 as an experiment "to force
                                  // re-provisioning" and never reverted - and the
                                  // MixPre then treats every connection as a new
                                  // device: it asks to save its configuration every
                                  // time instead of once, and loads its own default
                                  // scene (channels at 0/off).

    .bNumConfigurations = 0x01
};

uint8_t const * tud_descriptor_device_cb(void) {
  return (uint8_t const *) &desc_device;
}

//--------------------------------------------------------------------+
// Configuration Descriptor (class-compliant USB MIDI)
//--------------------------------------------------------------------+
enum {
  ITF_NUM_MIDI = 0,
  ITF_NUM_TOTAL
};

#define EPNUM_MIDI_OUT  0x01
#define EPNUM_MIDI_IN   0x82

// Exact copy of the real nanoKONTROL Studio configuration descriptor
// (single MIDIStreaming interface, no AudioControl, jack names in str 3/4/5).
#define CONFIG_TOTAL_LEN  83

uint8_t const desc_fs_configuration[] = {
  0x09, 0x02, 0x53, 0x00, 0x01, 0x01, 0x00, 0x80, 0x64,  // config (1 itf, 100mA)
  0x09, 0x04, 0x00, 0x00, 0x02, 0x01, 0x03, 0x00, 0x00,  // itf0 Audio/MIDIStreaming
  0x07, 0x24, 0x01, 0x00, 0x01, 0x25, 0x00,              // CS MS header
  0x06, 0x24, 0x02, 0x01, 0x10, 0x03,                    // IN jack embedded  (str 3)
  0x09, 0x24, 0x03, 0x02, 0x40, 0x01, 0x10, 0x01, 0x05,  // OUT jack external (str 5)
  0x09, 0x24, 0x03, 0x01, 0x30, 0x01, 0x20, 0x01, 0x04,  // OUT jack embedded (str 4)
  0x06, 0x24, 0x02, 0x02, 0x20, 0x05,                    // IN jack external  (str 5)
  0x09, 0x05, 0x01, 0x02, 0x40, 0x00, 0x00, 0x00, 0x00,  // EP 0x01 OUT bulk 64
  0x05, 0x25, 0x01, 0x01, 0x10,                          // CS endpoint (jack 0x10)
  0x09, 0x05, 0x82, 0x02, 0x40, 0x00, 0x00, 0x00, 0x00,  // EP 0x82 IN bulk 64
  0x05, 0x25, 0x01, 0x01, 0x30,                          // CS endpoint (jack 0x30)
};

uint8_t const * tud_descriptor_configuration_cb(uint8_t index) {
  (void) index;
  return desc_fs_configuration;
}

//--------------------------------------------------------------------+
// String Descriptors
//--------------------------------------------------------------------+
enum {
  STRID_LANGID = 0,
  STRID_MANUFACTURER,
  STRID_PRODUCT,
  STRID_JACK_A,
  STRID_JACK_B,
  STRID_EXT0,
};

static char const *string_desc_arr[] = {
  (const char[]) { 0x09, 0x04 }, // English (0x0409)
  STR_MANUFACTURER,
  STR_PRODUCT,
  "nanoKONTROL Studio _ CTRL",   // 3
  "nanoKONTROL Studio _ CTRL",   // 4
  "EXT0",                        // 5
  "MPBRIDGE0001",                // 6: unused (real Korg sends no serial; leave it here so
                                 // the string indices 1-5 keep their numbers)
};

static uint16_t _desc_str[32 + 1];

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
  (void) langid;
  size_t chr_count;

  switch (index) {
    case STRID_LANGID:
      memcpy(&_desc_str[1], string_desc_arr[0], 2);
      chr_count = 1;
      break;

    default:
      if (index >= sizeof(string_desc_arr) / sizeof(string_desc_arr[0])) {
        return NULL;
      }

      const char *str = string_desc_arr[index];
      chr_count = strlen(str);
      const size_t max_count = sizeof(_desc_str) / sizeof(_desc_str[0]) - 1;
      if (chr_count > max_count) chr_count = max_count;

      for (size_t i = 0; i < chr_count; i++) {
        _desc_str[1 + i] = str[i];
      }
      break;
  }

  _desc_str[0] = (uint16_t) ((TUSB_DESC_STRING << 8) | (2 * chr_count + 2));
  return _desc_str;
}
