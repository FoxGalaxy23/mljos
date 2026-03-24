#ifndef RTC_H
#define RTC_H

#include "common.h"

void get_rtc_time(uint8_t *hh, uint8_t *mm, uint8_t *ss);
void get_rtc_date(uint8_t *day, uint8_t *month, uint16_t *year);

#endif
