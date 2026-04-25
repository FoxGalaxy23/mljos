#include "console.h"
#include "launcher.h"
#include "shell.h"
#include "task.h"
#include "wm.h"
#include "net.h"
#include "cpu.h"
#include "sound.h"

struct multiboot_tag {
    uint32_t type;
    uint32_t size;
};

struct multiboot_tag_framebuffer {
    struct multiboot_tag common;
    uint64_t framebuffer_addr;
    uint32_t framebuffer_pitch;
    uint32_t framebuffer_width;
    uint32_t framebuffer_height;
    uint8_t framebuffer_bpp;
    uint8_t framebuffer_type;
    uint8_t reserved;
};

void kernel_main(uintptr_t mbi) {
    uint32_t mbi_size = *(uint32_t *)mbi;
    struct multiboot_tag *tag = (struct multiboot_tag *)(mbi + 8);

    while (tag->type != 0) {
        if (tag->type == 8) { // Framebuffer tag
            struct multiboot_tag_framebuffer *fb_tag = (struct multiboot_tag_framebuffer *)tag;
            console_init((uint32_t *)fb_tag->framebuffer_addr, fb_tag->framebuffer_width, fb_tag->framebuffer_height, fb_tag->framebuffer_pitch);
            break;
        }
        tag = (struct multiboot_tag *)((uintptr_t)tag + ((tag->size + 7) & ~7));
    }

    cpu_init();

    task_init();
    wm_init();
    net_init();
    console_set_visible(NULL, 0);
    
    // Startup beep: 440Hz for 250ms
    sound_beep(440, 250);

    shell_boot();

    // Main WM + cooperative scheduler loop.
    for (;;) {
        wm_pump_input();

        char launch[32];
        if (wm_consume_launch_request(launch, sizeof(launch))) {
            (void)launcher_launch_gui(launch);
        }

        wm_reap_closed_windows();
        wm_compose_if_dirty();
        net_poll();
        task_schedule_once();
    }
}
