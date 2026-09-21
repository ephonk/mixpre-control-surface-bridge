/*
 * Minimal USB MIDI Host class driver for TinyUSB.
 *
 * USB MIDI devices use bulk IN + bulk OUT endpoints carrying 4-byte MIDI
 * packets. There is no control transfer beyond standard enumeration, so the
 * driver is very small. Registered as an application driver through
 * usbh_app_driver_get_cb().
 *
 * Supports up to CFG_TUH_MIDI MIDI streaming interfaces (cables). For this
 * project: 1 interface on the Icon Platform M+.
 */

#include "tusb_option.h"

#if (CFG_TUH_ENABLED && CFG_TUH_MIDI)

#include "host/usbh.h"
#include "host/usbh_pvt.h"
#include "class/midi/midi.h"
#include "midi_host.h"

#ifndef CFG_TUH_MIDI_EP_BUFSIZE
#define CFG_TUH_MIDI_EP_BUFSIZE 64
#endif
#ifndef CFG_TUH_MIDI_RX_BUFSIZE
#define CFG_TUH_MIDI_RX_BUFSIZE 512
#endif
#ifndef CFG_TUH_MIDI_TX_BUFSIZE
#define CFG_TUH_MIDI_TX_BUFSIZE 512
#endif

#define AUDIO_SUBCLASS_MIDISTREAMING 0x03

typedef struct {
  uint8_t  daddr;
  uint8_t  itf_num;
  uint8_t  in_ep;
  uint8_t  out_ep;
  uint8_t  num_cables_rx;
  uint16_t num_cables_tx;
  bool     mounted;

  tu_edpt_stream_t rx;
  tu_edpt_stream_t tx;

  uint8_t rx_ff_buf[CFG_TUH_MIDI_RX_BUFSIZE];
  uint8_t tx_ff_buf[CFG_TUH_MIDI_TX_BUFSIZE];

  CFG_TUSB_MEM_ALIGN uint8_t rx_ep_buf[CFG_TUH_MIDI_EP_BUFSIZE];
  CFG_TUSB_MEM_ALIGN uint8_t tx_ep_buf[CFG_TUH_MIDI_EP_BUFSIZE];
} midih_interface_t;

static midih_interface_t _midih[CFG_TUH_MIDI];

// Debug (read by core0 and written to the log)
volatile uint32_t midih_dbg_n = 0;
volatile uint8_t  midih_dbg_cls = 0, midih_dbg_sub = 0;
volatile uint8_t  midih_dbg_in = 0, midih_dbg_out = 0, midih_dbg_res = 0;

static midih_interface_t* find_by_daddr(uint8_t daddr) {
  for (uint8_t i = 0; i < CFG_TUH_MIDI; i++) {
    if (_midih[i].daddr == daddr) return &_midih[i];
  }
  return NULL;
}

static midih_interface_t* find_by_daddr_itf(uint8_t daddr, uint8_t itf_num) {
  midih_interface_t* p = find_by_daddr(daddr);
  return (p && p->itf_num == itf_num) ? p : NULL;
}

static midih_interface_t* find_free(void) {
  for (uint8_t i = 0; i < CFG_TUH_MIDI; i++) {
    if (_midih[i].daddr == 0) return &_midih[i];
  }
  return NULL;
}

//--------------------------------------------------------------------+
// USBH class driver API
//--------------------------------------------------------------------+
bool midih_init(void) {
  tu_memclr(_midih, sizeof(_midih));
  return true;
}

bool midih_deinit(void) {
  tu_memclr(_midih, sizeof(_midih));
  return true;
}

bool midih_open(uint8_t rhport, uint8_t daddr, tusb_desc_interface_t const *itf_desc, uint16_t max_len) {
  (void) rhport;

  uint8_t const* desc     = (uint8_t const*) itf_desc;
  uint8_t const* desc_end = desc + max_len;

  midih_dbg_n++;
  midih_dbg_cls = itf_desc->bInterfaceClass;
  midih_dbg_sub = itf_desc->bInterfaceSubClass;

  if (itf_desc->bInterfaceClass != TUSB_CLASS_AUDIO) return false;

  // With CFG_TUH_MIDI the stack groups AudioControl + MIDIStreaming and calls
  // open() once with the AudioControl descriptor. Skip past it (and its
  // class-specific descriptors) to reach the MIDIStreaming interface.
  if (itf_desc->bInterfaceSubClass == 0x01 /* AUDIO_SUBCLASS_CONTROL */) {
    desc = tu_desc_next(desc);
    while (desc < desc_end && tu_desc_type(desc) == TUSB_DESC_CS_INTERFACE) {
      desc = tu_desc_next(desc);
    }
  }

  if (desc >= desc_end) return false;
  tusb_desc_interface_t const* midi_itf = (tusb_desc_interface_t const*) desc;
  if (midi_itf->bInterfaceClass    != TUSB_CLASS_AUDIO ||
      midi_itf->bInterfaceSubClass != AUDIO_SUBCLASS_MIDISTREAMING) {
    return false;
  }

  midih_interface_t* p = find_free();
  TU_VERIFY(p);
  p->daddr   = daddr;
  p->itf_num = midi_itf->bInterfaceNumber;
  p->mounted = false;          // set_config() flips this once both itfs report

  tu_edpt_stream_init(&p->rx, true, false, false,
                      p->rx_ff_buf, CFG_TUH_MIDI_RX_BUFSIZE,
                      p->rx_ep_buf, CFG_TUH_MIDI_EP_BUFSIZE);
  tu_edpt_stream_init(&p->tx, true, true, false,
                      p->tx_ff_buf, CFG_TUH_MIDI_TX_BUFSIZE,
                      p->tx_ep_buf, CFG_TUH_MIDI_EP_BUFSIZE);

  desc = tu_desc_next(desc);   // past the MIDIStreaming interface descriptor

  // Walk all sub-descriptors of the MIDIStreaming interface.
  // Note: endpoint descriptors are interleaved with CS_ENDPOINT (0x25)
  // descriptors (one per endpoint), so we must not stop at those.
  while (desc < desc_end) {
    uint8_t const dt = tu_desc_type(desc);

    if (dt == TUSB_DESC_INTERFACE) break;              // next interface

    if (dt == TUSB_DESC_CS_INTERFACE) {
      if ((midi_cs_interface_subtype_t) desc[2] == MIDI_CS_INTERFACE_HEADER) {
        p->num_cables_rx = desc[3];                    // bNumEmbMIDIJack
      }
    } else if (dt == TUSB_DESC_ENDPOINT) {
      tusb_desc_endpoint_t const* ep = (tusb_desc_endpoint_t const*) desc;
      TU_ASSERT(tuh_edpt_open(daddr, ep));
      if (tu_edpt_dir(ep->bEndpointAddress) == TUSB_DIR_IN) {
        p->in_ep = ep->bEndpointAddress;
        tu_edpt_stream_open(&p->rx, ep);
      } else {
        p->out_ep = ep->bEndpointAddress;
        tu_edpt_stream_open(&p->tx, ep);
      }
    }

    desc = tu_desc_next(desc);
  }

  TU_VERIFY(p->in_ep && p->out_ep);
  midih_dbg_in = p->in_ep;
  midih_dbg_out = p->out_ep;
  midih_dbg_res = 1;   // ok
  return true;
}

bool midih_set_config(uint8_t daddr, uint8_t itf_num) {
  midih_interface_t* p = find_by_daddr(daddr);
  TU_VERIFY(p);

  if (p->mounted) {                     // both grouped interfaces report in
    usbh_driver_set_config_complete(daddr, itf_num);
    return true;
  }

  p->mounted = true;

  if (tuh_midi_mount_cb) {
    tuh_midi_mount_cb(p->daddr, p->in_ep, p->out_ep,
                      p->num_cables_rx, p->num_cables_tx);
  }

  // Arm first bulk IN transfer
  tu_edpt_stream_read_xfer(p->daddr, &p->rx);

  usbh_driver_set_config_complete(daddr, itf_num);
  return true;
}

bool midih_xfer_cb(uint8_t daddr, uint8_t ep_addr, xfer_result_t result, uint32_t xferred_bytes) {
  midih_interface_t* p = find_by_daddr(daddr);
  TU_VERIFY(p);

  if (ep_addr == p->rx.ep_addr) {
    // An aborted transfer (controller unplugged) can report a bogus byte
    // count. Feeding that to the FIFO made _ff_pull_n() take the wrap-around
    // branch with a nonsense length and memcpy past the buffer - that was the
    // hard fault (PC = memcpy, LR = _ff_pull_n, tusb_fifo.c:260).
    if (xferred_bytes > CFG_TUH_MIDI_EP_BUFSIZE) xferred_bytes = CFG_TUH_MIDI_EP_BUFSIZE;
    tu_edpt_stream_read_xfer_complete(&p->rx, xferred_bytes);
    if (xferred_bytes >= 4 && tuh_midi_rx_cb) tuh_midi_rx_cb(daddr, xferred_bytes / 4);
    if (result == XFER_RESULT_SUCCESS) tu_edpt_stream_read_xfer(daddr, &p->rx);
  } else if (ep_addr == p->tx.ep_addr) {
    if (tuh_midi_tx_cb) tuh_midi_tx_cb(daddr);
    if (result == XFER_RESULT_SUCCESS) tu_edpt_stream_write_xfer(daddr, &p->tx);
  }

  return true;
}

void midih_close(uint8_t daddr) {
  midih_interface_t* p = find_by_daddr(daddr);
  if (!p) return;

  if (tuh_midi_umount_cb) tuh_midi_umount_cb(daddr);

  // Abort what is still in flight BEFORE closing the streams. Closing them
  // while the PIO-USB host was still completing an aborted transfer corrupted
  // the FIFO buffer pointer - the next pull then hard-faulted inside TinyUSB's
  // _ff_pull_n (memcpy). Aborting first means the transfer is finished (and its
  // callback done) before the FIFO is torn down.
  if (p->in_ep)  tuh_edpt_abort_xfer(daddr, p->in_ep);
  if (p->out_ep) tuh_edpt_abort_xfer(daddr, p->out_ep);

  tu_edpt_stream_close(&p->rx);
  tu_edpt_stream_close(&p->tx);

  // Mark the slot free WITHOUT wiping the struct.
  //
  // A transfer completion can still arrive after the unmount. With the struct
  // intact, find_by_daddr() returns NULL for that late callback (daddr no
  // longer matches) and midih_xfer_cb() bails out. The previous
  // tu_memclr(p, ...) zeroed the FIFO buffer pointers, so such a late
  // completion pulled from a cleared FIFO and hard-faulted - the crash was
  // memcpy inside TinyUSB's _ff_pull_n (tusb_fifo.c), triggered exactly when
  // the controller was unplugged.
  p->daddr   = 0;
  p->mounted = false;
}

//--------------------------------------------------------------------+
// Application API
//--------------------------------------------------------------------+
bool tuh_midi_mounted(uint8_t daddr) {
  midih_interface_t* p = find_by_daddr(daddr);
  return p && p->mounted;
}

uint32_t tuh_midi_available(uint8_t daddr) {
  midih_interface_t* p = find_by_daddr(daddr);
  TU_VERIFY(p, 0);
  return tu_edpt_stream_read_available(&p->rx);
}

uint32_t tuh_midi_stream_read(uint8_t daddr, void* buffer, uint32_t bufsize) {
  midih_interface_t* p = find_by_daddr(daddr);
  TU_VERIFY(p, 0);
  return tu_edpt_stream_read(daddr, &p->rx, buffer, bufsize);
}

uint32_t tuh_midi_stream_write(uint8_t daddr, void const* buffer, uint32_t bufsize) {
  midih_interface_t* p = find_by_daddr(daddr);
  TU_VERIFY(p, 0);

  uint32_t n = tu_edpt_stream_write(daddr, &p->tx, buffer, bufsize);
  tu_edpt_stream_write_xfer(daddr, &p->tx);
  return n;
}

bool tuh_midi_packet_read(uint8_t daddr, uint8_t packet[4]) {
  return tuh_midi_stream_read(daddr, packet, 4) == 4;
}

bool tuh_midi_packet_write(uint8_t daddr, uint8_t const packet[4]) {
  return tuh_midi_stream_write(daddr, packet, 4) == 4;
}

//--------------------------------------------------------------------+
// Register with TinyUSB host stack
//--------------------------------------------------------------------+
static usbh_class_driver_t const midi_host_driver = {
  .name       = "MIDI",
  .init       = midih_init,
  .deinit     = midih_deinit,
  .open       = midih_open,
  .set_config = midih_set_config,
  .xfer_cb    = midih_xfer_cb,
  .close      = midih_close,
};

usbh_class_driver_t const* usbh_app_driver_get_cb(uint8_t* driver_count) {
  *driver_count = 1;
  return &midi_host_driver;
}

#endif /* CFG_TUH_ENABLED && CFG_TUH_MIDI */
