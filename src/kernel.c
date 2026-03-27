#include "shell.h"
#include "console.h"

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

    puts("Welcome to mljOS (UEFI Mode via Multiboot 2)\n");
    shell_run();
}
