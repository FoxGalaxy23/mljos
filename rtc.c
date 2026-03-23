#include "rtc.h"
#include "io.h"

static inline uint8_t bcd_to_bin(uint8_t val) {
    return (val & 0x0F) + ((val >> 4) * 10);
}

static uint8_t read_rtc_register(uint8_t reg) {
    outb(0x70, (uint8_t)(reg | 0x80));
    return inb(0x71);
}

void get_rtc_time(uint8_t *hh, uint8_t *mm, uint8_t *ss) {
    while (read_rtc_register(0x0A) & 0x80) {
    }

    uint8_t s = read_rtc_register(0x00);
    uint8_t m = read_rtc_register(0x02);
    uint8_t h = read_rtc_register(0x04);
    uint8_t status_b = read_rtc_register(0x0B);

    if (!(status_b & 0x04)) {
        s = bcd_to_bin(s);
        m = bcd_to_bin(m);
        h = bcd_to_bin(h);
    }

    *hh = h;
    *mm = m;
    *ss = s;
}

void get_rtc_date(uint8_t *day, uint8_t *month, uint16_t *year) {
    while (read_rtc_register(0x0A) & 0x80) {
    }

    uint8_t d = read_rtc_register(0x07);
    uint8_t mo = read_rtc_register(0x08);
    uint8_t y = read_rtc_register(0x09);
    uint8_t status_b = read_rtc_register(0x0B);

    if (!(status_b & 0x04)) {
        d = bcd_to_bin(d);
        mo = bcd_to_bin(mo);
        y = bcd_to_bin(y);
    }

    *day = d;
    *month = mo;
    *year = 2000 + (uint16_t)y;
}
