#include "sound.h"
#include "io.h"

static void delay_ms(uint32_t ms) {
    // Increased loop constant for better visibility in VMs.
    // Roughly calibrated for typical modern CPU speed in a VM.
    for (uint32_t i = 0; i < ms; i++) {
        for (volatile uint32_t j = 0; j < 2000000; j++) {
            __asm__ volatile ("nop");
        }
    }
}

void sound_play(uint32_t freq) {
    if (freq == 0) {
        sound_stop();
        return;
    }

    uint32_t div = 1193180 / freq;
    
    // Set PIT Channel 2 to square wave mode
    outb(0x43, 0xB6);
    
    // Send divisor
    outb(0x42, (uint8_t)(div & 0xFF));
    outb(0x42, (uint8_t)((div >> 8) & 0xFF));

    // Enable speaker
    uint8_t tmp = inb(0x61);
    if (tmp != (tmp | 3)) {
        outb(0x61, tmp | 3);
    }
}

void sound_stop(void) {
    uint8_t tmp = inb(0x61) & 0xFC;
    outb(0x61, tmp);
}

void sound_beep(uint32_t freq, uint32_t duration_ms) {
    sound_play(freq);
    delay_ms(duration_ms);
    sound_stop();
}
