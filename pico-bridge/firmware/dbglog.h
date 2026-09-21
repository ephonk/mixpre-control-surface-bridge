#ifndef _DBGLOG_H_
#define _DBGLOG_H_

#include <stdint.h>

void     dbglog_init(void);
void     dbglog_arm(void);               // enable + reset (session start)
void     dbglog_byte(uint8_t b);
void     dbglog_bytes(const uint8_t *p, uint32_t n);
void     dbglog_tick(uint32_t now_ms);   // periodic flush to flash
void     dbglog_disable(void);           // stop logging/flushing (during read-out)
void     dbglog_clear(void);             // erase persisted log
void     dbglog_restart(void);           // discard current buffer (keep flash)
uint32_t dbglog_read(uint8_t *out, uint32_t max); // read persisted log from flash

#endif
