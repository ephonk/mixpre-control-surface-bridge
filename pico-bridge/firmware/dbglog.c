/*
 * Debug log persisted in the last flash sector.
 *
 * Records every USB-MIDI packet received from the MixPre so we can inspect,
 * on the Mac, exactly what the MixPre sends during its (failing) handshake.
 *
 * Layout of the reserved sector:
 *   [0x000] magic "MPLG" + uint32 length (LE), rest 0xFF
 *   [0x100] payload bytes
 */

#include <string.h>
#include "pico/stdlib.h"
#include "pico/flash.h"
#include "hardware/flash.h"
#include "dbglog.h"

#define DBG_SECTORS 2
#define DBG_SIZE   (DBG_SECTORS * FLASH_SECTOR_SIZE)
// Debug logging to flash is DISABLED for releases: flash_safe_execute()
// disables interrupts + locks core1 for tens of ms, which made the USB device
// miss transfers and the MixPre drop the controller calibration. Flip this to 1
// locally while diagnosing.
#define DBGLOG_ENABLE 0

#define DBG_OFFSET (PICO_FLASH_SIZE_BYTES - DBG_SIZE)
#define DBG_XIP    (XIP_BASE + DBG_OFFSET)
#define DBG_MAX    (DBG_SIZE - FLASH_PAGE_SIZE)
#define DBG_MAGIC0 'M'
#define DBG_MAGIC1 'P'
#define DBG_MAGIC2 'L'
#define DBG_MAGIC3 'G'

static uint8_t  ram[DBG_MAX];
static uint32_t ram_len     = 0;
static uint32_t flushed_len = 0;
static uint32_t last_ms     = 0;
static bool     disabled    = false;
static bool     enabled     = false;

void dbglog_init(void) {
  ram_len = 0;
  flushed_len = 0;
  disabled = !DBGLOG_ENABLE;
  enabled = DBGLOG_ENABLE;
}

void dbglog_arm(void) {
  if (!DBGLOG_ENABLE) return;   // release build: arm must not switch logging on
  ram_len = 0;
  flushed_len = 0;
  disabled = false;
  enabled = true;
}

void dbglog_byte(uint8_t b) {
  if (!enabled || disabled) return;
  if (ram_len < DBG_MAX) ram[ram_len++] = b;
}

void dbglog_bytes(const uint8_t *p, uint32_t n) {
  for (uint32_t i = 0; i < n; i++) dbglog_byte(p[i]);
}

void dbglog_disable(void) { disabled = true; }

static void __no_inline_not_in_flash_func(do_flash)(void *arg) {
  (void) arg;
  static uint8_t page[FLASH_PAGE_SIZE];
  uint8_t hdr[8] = {DBG_MAGIC0, DBG_MAGIC1, DBG_MAGIC2, DBG_MAGIC3,
                    (uint8_t) ram_len, (uint8_t) (ram_len >> 8),
                    (uint8_t) (ram_len >> 16), (uint8_t) (ram_len >> 24)};

  flash_range_erase(DBG_OFFSET, DBG_SIZE);

  memset(page, 0xFF, FLASH_PAGE_SIZE);
  memcpy(page, hdr, sizeof(hdr));
  flash_range_program(DBG_OFFSET, page, FLASH_PAGE_SIZE);

  for (uint32_t off = 0; off < ram_len; off += FLASH_PAGE_SIZE) {
    uint32_t n = ram_len - off;
    if (n > FLASH_PAGE_SIZE) n = FLASH_PAGE_SIZE;
    memset(page, 0xFF, FLASH_PAGE_SIZE);
    memcpy(page, ram + off, n);
    flash_range_program(DBG_OFFSET + FLASH_PAGE_SIZE + off, page, FLASH_PAGE_SIZE);
  }
}

void dbglog_tick(uint32_t now_ms) {
  if (!enabled || disabled) return;
  if (ram_len == flushed_len) return;
  if (ram_len < 64) return;               // only substantial traffic flushes (MixPre session)
  if (now_ms < 2500) return;               // don't disturb early handshake
  if (now_ms - last_ms < 500) return;
  last_ms = now_ms;
  if (flash_safe_execute(do_flash, NULL, 2000) == PICO_OK) {
    flushed_len = ram_len;
  }
}

static void __no_inline_not_in_flash_func(do_clear)(void *arg) {
  (void) arg;
  static uint8_t page[FLASH_PAGE_SIZE];
  uint8_t hdr[8] = {DBG_MAGIC0, DBG_MAGIC1, DBG_MAGIC2, DBG_MAGIC3, 0, 0, 0, 0};
  flash_range_erase(DBG_OFFSET, DBG_SIZE);
  memset(page, 0xFF, FLASH_PAGE_SIZE);
  memcpy(page, hdr, sizeof(hdr));
  flash_range_program(DBG_OFFSET, page, FLASH_PAGE_SIZE);
}

void dbglog_clear(void) {
  ram_len = 0;
  flushed_len = 0;
  disabled = false;
  enabled = true;
  flash_safe_execute(do_clear, NULL, 2000);
}

void dbglog_restart(void) {
  ram_len = 0;
  flushed_len = 0;
}

uint32_t dbglog_read(uint8_t *out, uint32_t max) {
  const uint8_t *f = (const uint8_t *) DBG_XIP;
  if (f[0] != DBG_MAGIC0 || f[1] != DBG_MAGIC1 ||
      f[2] != DBG_MAGIC2 || f[3] != DBG_MAGIC3) {
    return 0;
  }
  uint32_t n = (uint32_t) f[4] | ((uint32_t) f[5] << 8) |
               ((uint32_t) f[6] << 16) | ((uint32_t) f[7] << 24);
  if (n > DBG_MAX) n = DBG_MAX;
  if (n > max) n = max;
  memcpy(out, f + FLASH_PAGE_SIZE, n);
  return n;
}
