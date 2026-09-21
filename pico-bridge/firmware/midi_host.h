#ifndef _TUSB_MIDI_HOST_H_
#define _TUSB_MIDI_HOST_H_

#include "common/tusb_common.h"

#ifdef __cplusplus
extern "C" {
#endif

//--------------------------------------------------------------------+
// Application callbacks (implemented in main.c if needed)
//--------------------------------------------------------------------+
TU_ATTR_WEAK void tuh_midi_mount_cb(uint8_t dev_addr, uint8_t in_ep, uint8_t out_ep,
                                    uint8_t num_cables_rx, uint16_t num_cables_tx);
TU_ATTR_WEAK void tuh_midi_umount_cb(uint8_t dev_addr);
TU_ATTR_WEAK void tuh_midi_rx_cb(uint8_t dev_addr, uint32_t num_packets);
TU_ATTR_WEAK void tuh_midi_tx_cb(uint8_t dev_addr);

//--------------------------------------------------------------------+
// Application API
//--------------------------------------------------------------------+
bool     tuh_midi_mounted(uint8_t dev_addr);
uint32_t tuh_midi_available(uint8_t dev_addr);
uint32_t tuh_midi_stream_read(uint8_t dev_addr, void *buffer, uint32_t bufsize);
uint32_t tuh_midi_stream_write(uint8_t dev_addr, void const *buffer, uint32_t bufsize);
bool     tuh_midi_packet_read(uint8_t dev_addr, uint8_t packet[4]);
bool     tuh_midi_packet_write(uint8_t dev_addr, uint8_t const packet[4]);

//--------------------------------------------------------------------+
// Internal (called by TinyUSB host stack through usbh_app_driver_get_cb)
//--------------------------------------------------------------------+
bool midih_init(void);
bool midih_deinit(void);
bool midih_open(uint8_t rhport, uint8_t dev_addr, tusb_desc_interface_t const *itf_desc, uint16_t max_len);
bool midih_set_config(uint8_t dev_addr, uint8_t itf_num);
bool midih_xfer_cb(uint8_t dev_addr, uint8_t ep_addr, xfer_result_t result, uint32_t xferred_bytes);
void midih_close(uint8_t dev_addr);

#ifdef __cplusplus
}
#endif

#endif /* _TUSB_MIDI_HOST_H_ */
