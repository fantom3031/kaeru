//
// SPDX-FileCopyrightText: 2026 fantom3031 <mpiven69@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//

#include <board_ops.h>
#include <lib/environment.h>
#include <lib/framebuffer.h>
#include <lib/common.h>
#include <lib/thread.h>
#include <lib/bootmode.h>

#define MDELAY_ADDR                      0x48004cac
#define SNPRINTF_ADDR                    0x48032e70
#define STRCMP_ADDR                      0x480332a4
#define SBC_STATUS_ADDB                  0x11c50060
#define MT_SCRATCH_FB_ADDR               0x48b00000
#define LOGO_BIN_ADDR                    0x7a890000
#define FB_VAR_LIST_ADDR                 0x4831b048
#define G_EMMC_SIZE_ADDR                 0x483085E8
#define OEM_CMDS_ADDR                    0x4831b048
#define GET_UNLOCKED_STATUS_ADDR         0x48063b34
#define PHYSICAL_MEMORY_SIZE_ADDR        0x480027a0
#define MT_DISP_UPDATE_ADDR              0x480010fc
#define CLEAN_SCREEN                     0x48032038
#define MT_DECOMPRESS_LOGO_ADDR          0x480351e0
#define MT_DISP_SHOW_BOOT_LOGO_ADDR      0x4801ad28
#define CONTINUE_ADDR                    0x48027578
#define REBOOT_ADDR                      0x48027b94
#define REBOOT_BOOTLOADER_ADDR           0x48027bc4
#define REBOOT_RECOVERY_ADDR             0x480277b4
#define REBOOT_FASTBOOT_ADDR             0x480277f0
#define MT_POWER_OFF_ADDR                0x48006b34
#define SECCFG_SET_LOCK_STATE_ADDR       0x4806353c
#define MTK_ARCH_RESET_ADDR              0x4800716c
#define GET_SBOOT_STATE_ADDR             0x480628b4
#define PARTITION_WRITE_ADDR             0x4804bb3c

#define LOGO_STRIDE_PX \
    (((CONFIG_FRAMEBUFFER_WIDTH * CONFIG_FRAMEBUFFER_BYTES_PER_PIXEL + \
       CONFIG_FRAMEBUFFER_ALIGNMENT - 1) & ~(CONFIG_FRAMEBUFFER_ALIGNMENT - 1)) \
     / CONFIG_FRAMEBUFFER_BYTES_PER_PIXEL)

#define LOGO_ORIGINAL_FASTBOOT 88
#define LOGO_CUSTOM_FASTBOOT    88
#define LOGO_CONFIRM            89
#define FONT_MEDIUM_LOGO_INDEX  90
#define FONT_SEMIBOLD_LOGO_INDEX 91

typedef int (*mt_decompress_logo_t)(void *, void *, int, int);
typedef void (*mt_disp_update_t)(uint32_t, uint32_t, uint32_t, uint32_t);
typedef void (*mdelay_t)(uint32_t msec);
typedef long ssize_t;

static mt_decompress_logo_t mt_decompress_logo = (mt_decompress_logo_t) (MT_DECOMPRESS_LOGO_ADDR | 1);
static mt_disp_update_t mt_disp_update = (mt_disp_update_t) (MT_DISP_UPDATE_ADDR | 1);
static mdelay_t lk_mdelay = (mdelay_t) (MDELAY_ADDR | 1);

static void video_clean_screen(void) {
    ((void (*)(void))(CLEAN_SCREEN | 1))();
}

long partition_read(const char* part_name, long long offset, uint8_t* data, size_t size) {
    return ((long (*)(const char*, long long, uint8_t*, size_t))(CONFIG_PARTITION_READ_ADDRESS | 1))(
            part_name, offset, data, size);
}

static int logo_parse(void *logo_base, unsigned int index,
                      void **out_addr, unsigned int *out_len) {
    uint32_t *p = (uint32_t *)logo_base;

    uint32_t logonum = p[0];
    if (index >= logonum)
        return -1;

    *out_len  = (index < logonum - 1) ? p[3 + index] - p[2 + index]
                                      : p[1] - p[2 + index];
    *out_addr = (void *)((uint8_t *)logo_base + p[2 + index]);
    return 0;
}

static void logo_blit(void *dst, void *src, int w, int h,
                      int stride, int rotation, int bpp) {
    uint32_t *d = (uint32_t *)dst;

    if (rotation == 0 && bpp == 32 && stride == w) {
        memcpy(dst, src, (size_t)(w * h * 4));
        return;
    }

    if (rotation == 0 && bpp == 32) {
        uint32_t *s = (uint32_t *)src;
        for (int i = 0; i < h; i++)
            memcpy(d + i * stride, s + i * w, (size_t)(w * 4));
        return;
    }

    if (rotation == 0 && bpp == 16) {
        uint16_t *s = (uint16_t *)src;
        for (int i = 0; i < h; i++) {
            uint32_t *row = d + i * stride;
            for (int j = 0; j < w; j++) {
                uint16_t px = s[i * w + j];
                uint8_t r   = (px >> 8) & 0xF8;
                uint8_t g   = (px >> 3) & 0xFC;
                uint8_t b   = (px << 3) & 0xF8;
                row[j] = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
            }
        }
        return;
    }

    // Generic rotation path — pixel by pixel.
    for (int i = 0; i < h; i++) {
        for (int j = 0; j < w; j++) {
            uint32_t px;
            if (bpp == 32) {
                px = ((uint32_t *)src)[i * w + j];
            } else {
                uint16_t p16 = ((uint16_t *)src)[i * w + j];
                uint8_t r    = (p16 >> 8) & 0xF8;
                uint8_t g    = (p16 >> 3) & 0xFC;
                uint8_t b    = (p16 << 3) & 0xF8;
                px = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
            }

            uint32_t dst_idx;
            switch (rotation) {
                case 90:  dst_idx = (uint32_t)(stride * j + (stride - i - 1));             break;
                case 180: dst_idx = (uint32_t)(stride * (h - i) - j - 1 - (stride - w));   break;
                case 270: dst_idx = (uint32_t)(stride * (h - j - 1) + i);                  break;
                default:  dst_idx = (uint32_t)(stride * i + j);                            break;
            }
            d[dst_idx] = px;
        }
    }
}

static void logo_show(unsigned int index, void *fb_addr, uint32_t fb_stride,
                      int width, int height, int rotation, int do_update) {
    if (!fb_addr)
        return;

    void        *comp_data;
    unsigned int comp_len;

    if (logo_parse((void *)LOGO_BIN_ADDR, index, &comp_data, &comp_len) < 0)
        return;

    void *scratch = (void *)(uintptr_t)MT_SCRATCH_FB_ADDR;

    int raw_size = mt_decompress_logo(comp_data, scratch, (int)comp_len,
                                      width * height * 4);
    if (raw_size <= 0)
        return;

    int bpp = (raw_size == width * height * 2) ? 16 : 32;

    logo_blit(fb_addr, scratch, width, height, (int)fb_stride, rotation, bpp);

    if (do_update)
        mt_disp_update(0, 0, (uint32_t)width, (uint32_t)height);
}

typedef struct {
    uint8_t       *data;         // pointer to decompressed spritesheet (ARGB8888)
    const uint8_t *advances;     // advance-width table; index = char_code - 32
    uint16_t       sheet_stride; // sheet_w * 4 (bytes per row)
    uint8_t        cell_size;    // cell size in pixels (equal in X and Y)
    uint8_t        cols;         // columns in the spritesheet (typically 12)
} font_t;

#define FONT_MEDIUM_CHAR_SIZE    56
#define FONT_MEDIUM_COLS         12
#define FONT_MEDIUM_ROWS          8
#define FONT_MEDIUM_SHEET_W      (FONT_MEDIUM_COLS * FONT_MEDIUM_CHAR_SIZE)   // 672 px
#define FONT_MEDIUM_SHEET_H      (FONT_MEDIUM_ROWS * FONT_MEDIUM_CHAR_SIZE)   // 448 px
#define FONT_MEDIUM_SHEET_STRIDE (FONT_MEDIUM_SHEET_W * 4)                    // 2688 bytes/row
#define FONT_SEMIBOLD_CHAR_SIZE    56
#define FONT_SEMIBOLD_COLS         12
#define FONT_SEMIBOLD_ROWS          8
#define FONT_SEMIBOLD_SHEET_W      (FONT_SEMIBOLD_COLS * FONT_SEMIBOLD_CHAR_SIZE)   // 672 px
#define FONT_SEMIBOLD_SHEET_H      (FONT_SEMIBOLD_ROWS * FONT_SEMIBOLD_CHAR_SIZE)   // 448 px
#define FONT_SEMIBOLD_SHEET_STRIDE (FONT_SEMIBOLD_SHEET_W * 4)                      // 2688 bytes/row

static const uint8_t g_font_medium_advances[] = {
      6,   6,  10,  13,  14,  19,  14,   6,   7,   7,  11,  14,   6,  10,   6,   8,
     14,   9,  13,  13,  14,  13,  13,  12,  13,  13,   6,   6,  14,  14,  14,  12,
     21,  15,  14,  16,  15,  13,  12,  16,  15,   6,  12,  14,  12,  19,  15,  16,
     13,  16,  14,  14,  14,  15,  15,  20,  14,  14,  13,   7,   8,   7,  10,  10,
      7,  12,  13,  12,  13,  12,   8,  13,  12,   6,   6,  12,   6,  18,  12,  12,
     13,  13,   8,  11,   8,  12,  12,  17,  12,  12,  11,   9,   7,   9,  14,
};

static const uint8_t g_font_semibold_advances[] = {
      6,   6,  10,  13,  14,  20,  14,   7,   8,   8,  11,  14,   6,  10,   6,   8,
     14,   9,  13,  13,  14,  13,  13,  12,  13,  13,   6,   6,  14,  14,  14,  12,
     21,  15,  14,  16,  15,  13,  13,  16,  15,   6,  12,  15,  12,  19,  16,  16,
     14,  16,  14,  14,  14,  15,  15,  21,  15,  15,  14,   8,   8,   8,  10,  10,
      7,  12,  13,  12,  13,  12,   8,  13,  13,   6,   6,  12,   6,  19,  13,  13,
     13,  13,   8,  11,   8,  13,  12,  17,  12,  12,  11,  10,   8,  10,  14,
};

static font_t g_font_medium;
static font_t g_font_semibold;

static int font_load_entry(font_t *font, int logo_index,
                           const uint8_t *advances, uint8_t cell_size,
                           uint8_t cols, uint8_t *buf, int buf_size) {
    font->data = NULL;

    void        *comp_data;
    unsigned int comp_len;

    if (logo_parse((void *)LOGO_BIN_ADDR, (unsigned int)logo_index,
                   &comp_data, &comp_len) != 0)
        return 0;

    int raw_size = mt_decompress_logo(comp_data, buf, (int)comp_len, buf_size);
    if (raw_size <= 0 || raw_size > buf_size)
        return 0;

    font->data         = buf;
    font->advances     = advances;
    font->cell_size    = cell_size;
    font->cols         = cols;
    font->sheet_stride = (uint16_t)((int)cols * (int)cell_size * 4);
    return 1;
}

static void font_load_all(void) {
    uint8_t *scratch = (uint8_t *)(uintptr_t)MT_SCRATCH_FB_ADDR;

    const int medium_slot   = FONT_MEDIUM_COLS   * FONT_MEDIUM_ROWS
                            * FONT_MEDIUM_CHAR_SIZE   * FONT_MEDIUM_CHAR_SIZE   * 4; // 1,204,224
    const int semibold_slot = FONT_SEMIBOLD_COLS * FONT_SEMIBOLD_ROWS
                            * FONT_SEMIBOLD_CHAR_SIZE * FONT_SEMIBOLD_CHAR_SIZE * 4; // 1,204,224

    font_load_entry(&g_font_medium,
                    FONT_MEDIUM_LOGO_INDEX,
                    g_font_medium_advances,
                    FONT_MEDIUM_CHAR_SIZE, FONT_MEDIUM_COLS,
                    scratch, medium_slot);

    font_load_entry(&g_font_semibold,
                    FONT_SEMIBOLD_LOGO_INDEX,
                    g_font_semibold_advances,
                    FONT_SEMIBOLD_CHAR_SIZE, FONT_SEMIBOLD_COLS,
                    scratch + medium_slot, semibold_slot);
}

static int test_fonts_availablity(void) {
    void *addr;
    unsigned int len;

    if (logo_parse((void *)LOGO_BIN_ADDR, FONT_MEDIUM_LOGO_INDEX,
                   &addr, &len) != 0 || len == 0)
        return 0;

    if (logo_parse((void *)LOGO_BIN_ADDR, FONT_SEMIBOLD_LOGO_INDEX,
                   &addr, &len) != 0 || len == 0)
        return 0;

    return 1;
}

#define UI_LINE_HEIGHT  24

static void font_draw_str(int x, int y, const char *str,
                          uint32_t color, const font_t *font) {
    // if (!font || !font->data ||
    //     (uintptr_t)font->data < 0x7a000000u ||
    //     (uintptr_t)font->data > 0x7fffffffu) {
    //     fb_text((uint32_t)x, (uint32_t)y, str, color);
    //     return;
    // }

    int origin_x = x;

    uint8_t fg_r = (color >> 16) & 0xFF;
    uint8_t fg_g = (color >>  8) & 0xFF;
    uint8_t fg_b =  color        & 0xFF;

    uint32_t *fb     = (uint32_t *)(uintptr_t)CONFIG_FRAMEBUFFER_ADDRESS;
    int       stride = LOGO_STRIDE_PX;
    uint8_t   cs     = font->cell_size;
    uint8_t   cols   = font->cols;
    uint16_t  ss     = font->sheet_stride;

    while (*str) {
        if (*str == '\n') {
            x  = origin_x;
            y += UI_LINE_HEIGHT;
            str++;
            continue;
        }

        int ch = (int)(unsigned char)*str;
        if (ch < 32 || ch > 126) ch = 32;

        int idx       = ch - 32;
        int glyph_col = idx % cols;
        int glyph_row = idx / cols;

        uint8_t *glyph_base = font->data
            + (glyph_row * cs) * ss
            + (glyph_col * cs) * 4;

        for (int gy = 0; gy < cs; gy++) {
            uint8_t *row_ptr = glyph_base + gy * ss;
            for (int gx = 0; gx < cs; gx++) {
                uint8_t alpha = row_ptr[gx * 4 + 3];
                if (alpha == 0)
                    continue;

                int px = x + gx;
                int py = y + gy;
                if (px < 0 || px >= CONFIG_FRAMEBUFFER_WIDTH  ||
                    py < 0 || py >= CONFIG_FRAMEBUFFER_HEIGHT)
                    continue;

                uint32_t out;
                if (alpha == 0xFF) {
                    out = 0xFF000000u | color;
                } else {
                    uint8_t r = (uint8_t)((fg_r * alpha) / 255u);
                    uint8_t g = (uint8_t)((fg_g * alpha) / 255u);
                    uint8_t b = (uint8_t)((fg_b * alpha) / 255u);
                    out = 0xFF000000u | ((uint32_t)r << 16)
                                     | ((uint32_t)g <<  8) | b;
                }

                fb[py * stride + px] = out;
            }
        }

        x += (idx >= 0 && idx < 95) ? font->advances[idx] : cs;
        str++;
    }
}

struct fastboot_var {
    uint32_t next;
    uint32_t name;
    uint32_t value;
};

static const char *fastboot_get_var(const char *name) {
    struct fastboot_var **head = (struct fastboot_var **)FB_VAR_LIST_ADDR;
    if (!*head)
        return NULL;
    for (struct fastboot_var *v = *head; v; v = (struct fastboot_var *)v->next) {
        if (((int (*)(const char *, const char *))(STRCMP_ADDR | 1))((const char *)v->name, name) == 0)//
            return (const char *)v->value;
    }
    return NULL;
}

static const char *get_boot_reason(void) {
    uint32_t wdt_status = 0;
    uint32_t exp_type = 0;
    
    int (*read_wdt)(uint32_t*) = (int (*)(uint32_t*))(0x4802bed4 | 1);
    int (*read_exp)(uint32_t*) = (int (*)(uint32_t*))(0x4802befc | 1);
    
    if (read_wdt(&wdt_status) && read_exp(&exp_type)) {
        if (exp_type == 1) return "Watchdog";
        if (exp_type == 2 || exp_type == 3) return "kernel_panic";
        if (exp_type == 4) return "mrdump";
        if (exp_type == 5) return "hang_detect";
        if (exp_type == 6) return "lk_crash";
        
        if (wdt_status & 1) return "HW_reboot";
        if (wdt_status & 8) return "SPM_Thermal";
        if (wdt_status & 0x10) return "SPM_reboot";
        if (wdt_status & 0x20) return "Thermal_reboot";
        if (wdt_status & 0x80) return "security_reboot";
    }
    
    return "normal";
}

static const char *get_sbc_status(void) {
    uint32_t (*get_sboot)(uint32_t*) = (uint32_t (*)(uint32_t*))(GET_SBOOT_STATE_ADDR | 1);
    uint32_t sboot_state = 1;
    get_sboot(&sboot_state);
    
    if (sboot_state) {
        return "enabled";
    } else {
        return "disabled";
    }
}

typedef enum {
    FB_OPTION_CONTINUE = 0,
    FB_OPTION_REBOOT,
    FB_OPTION_RESTART_BOOTLOADER,
    FB_OPTION_RECOVERY_MODE,
    FB_OPTION_FASTBOOT_MODE,
    FB_OPTION_POWEROFF,
    FB_OPTION_BAD_APPLE,
    FB_OPTION_COUNT,
} fb_option_t;

static char ui_dram_str[16];
static char ui_emmc_str[16];

#define UI_COLOR_HEADER 0xFFFFa500u // orange
#define UI_COLOR_TEXT 0xFF999999u // light gray
#define KEY_VOLUME_UP 17
#define KEY_VOLUME_DOWN 1
#define KEY_POWER 8

typedef int (*get_unlocked_status_t)(void);
static get_unlocked_status_t get_unlocked_status = (get_unlocked_status_t) (GET_UNLOCKED_STATUS_ADDR | 1);

static void fastboot_ui_render_info(void) {
    video_clean_screen();
    int w = CONFIG_FRAMEBUFFER_WIDTH;
    int h = CONFIG_FRAMEBUFFER_HEIGHT;

    int x = 50;
    int y = 1100;
    char buf[128];

    // fb_fill_rect(0, (uint32_t)(y - 10), (uint32_t)w,
    //              (uint32_t)(11 * UI_LINE_HEIGHT + 40), 0xAA000000u);

    font_draw_str(x, y, "Fastboot Mode", UI_COLOR_HEADER, &g_font_medium);
    y += UI_LINE_HEIGHT * 2;

    npf_snprintf(buf, sizeof(buf), "Product revision: %s", fastboot_get_var("product"));
    font_draw_str(x, y, buf, UI_COLOR_TEXT, &g_font_medium);
    y += UI_LINE_HEIGHT;

    npf_snprintf(buf, sizeof(buf), "Kaeru version: %s %s", fastboot_get_var("kaeru-version"), __TIMESTAMP__);
    font_draw_str(x, y, buf, UI_COLOR_TEXT, &g_font_medium);
    y += UI_LINE_HEIGHT;

    npf_snprintf(buf, sizeof(buf), "Bootloader version: %s", fastboot_get_var("version-bootloader"));
    font_draw_str(x, y, buf, UI_COLOR_TEXT, &g_font_medium);
    y += UI_LINE_HEIGHT;

    npf_snprintf(buf, sizeof(buf), "Baseband version: %s", fastboot_get_var("version-baseband"));
    font_draw_str(x, y, buf, UI_COLOR_TEXT, &g_font_medium);
    y += UI_LINE_HEIGHT;

    npf_snprintf(buf, sizeof(buf), "Serial number: %s", fastboot_get_var("serialno"));
    font_draw_str(x, y, buf, UI_COLOR_TEXT, &g_font_medium);
    y += UI_LINE_HEIGHT;

    npf_snprintf(buf, sizeof(buf), "Secure boot: %s", get_sbc_status());
    font_draw_str(x, y, buf, UI_COLOR_TEXT, &g_font_medium);
    y += UI_LINE_HEIGHT;
    
    npf_snprintf(buf, sizeof(buf), "DRAM: %s", ui_dram_str[0] ? ui_dram_str : "N/A");
    font_draw_str(x, y, buf, UI_COLOR_TEXT, &g_font_medium);
    y += UI_LINE_HEIGHT;

    npf_snprintf(buf, sizeof(buf), "EMMC: %s", ui_emmc_str[0] ? ui_emmc_str : "N/A");
    font_draw_str(x, y, buf, UI_COLOR_TEXT, &g_font_medium);
    y += UI_LINE_HEIGHT;

    npf_snprintf(buf, sizeof(buf), "Device state: %s", get_unlocked_status() ? "unlocked" : "locked");
    font_draw_str(x, y, buf, UI_COLOR_TEXT, &g_font_medium);
    y += UI_LINE_HEIGHT;

    npf_snprintf(buf, sizeof(buf), "Boot slot: %s", "device is not A/B");
    font_draw_str(x, y, buf, UI_COLOR_TEXT, &g_font_medium);
    y += UI_LINE_HEIGHT;

    npf_snprintf(buf, sizeof(buf), "Enter reason: %s", get_boot_reason());
    font_draw_str(x, y, buf, UI_COLOR_TEXT, &g_font_medium);

    mt_disp_update(0, 0, (uint32_t)w, (uint32_t)h);
}

static void fastboot_ui_render_mode(int sel) {
    const char *label;
    int x, y = 670;

    fb_fill_rect(400, 660, 260, 90, 0xFF000000u);

    switch (sel) {
        case FB_OPTION_CONTINUE:           label = "Boot";       x = 580; break;
        case FB_OPTION_REBOOT:             label = "Reboot";     x = 550; break;
        case FB_OPTION_RESTART_BOOTLOADER: label = "Restart BL"; x = 530; break;
        case FB_OPTION_RECOVERY_MODE:      label = "Recovery";   x = 545; break;
        case FB_OPTION_FASTBOOT_MODE:      label = "Fastboot";   x = 540; break;
        case FB_OPTION_POWEROFF:           label = "Power Off";  x = 535; break;
        case FB_OPTION_BAD_APPLE:          label = "Bad Apple";  x = 545; break;
        
        default: label = "unknown"; x = 600; break;
    }

    font_draw_str(x, y, label, UI_COLOR_TEXT, &g_font_semibold);
    mt_disp_update(0, 0, CONFIG_FRAMEBUFFER_WIDTH, CONFIG_FRAMEBUFFER_HEIGHT);
}


void play_simple_video(void) {
    #define PALETTE_SIZE (256 * 4)
    uint8_t *palette = (uint8_t*)(uintptr_t)MT_SCRATCH_FB_ADDR;
    // palette + sizes: 1024 + 4 = 1028 bytes
    uint8_t *frame_buffer = palette + 1024 + 4;
    
    long long offset = 0;
    int frame_num = 0;
    
    fb_config_t *fb = fb_get_config();
    if (!fb || !fb->buffer) return;
    
    long bytes = partition_read("gz2", offset, palette, PALETTE_SIZE);
    if (bytes < PALETTE_SIZE) {
        video_printf("Failed to read palette!\n");
        return;
    }
    offset += PALETTE_SIZE;
    
    uint16_t video_width, video_height;
    bytes = partition_read("gz2", offset, (uint8_t*)&video_width, 2);
    bytes += partition_read("gz2", offset + 2, (uint8_t*)&video_height, 2);
    if (bytes < 4 || video_width == 0 || video_height == 0) {
        video_printf("Invalid video size!\n");
        return;
    }
    offset += 4;
    
    video_printf("Video: %dx%d\n", video_width, video_height);
    
    uint32_t max_frame_size = video_width * video_height;
    
    uint8_t *rle_buffer = frame_buffer + max_frame_size;
    
    int scale_x = fb->width / video_width;
    int scale_y = fb->height / video_height;
    int scale = (scale_x < scale_y) ? scale_x : scale_y;
    if (scale < 1) scale = 1;
    
    int start_x = (fb->width - video_width * scale) / 2;
    int start_y = (fb->height - video_height * scale) / 2;
    video_clean_screen();
    
    while (1) {
        uint16_t rle_size;
        bytes = partition_read("gz2", offset, (uint8_t*)&rle_size, 2);
        if (bytes < 2 || rle_size == 0 || rle_size > 32768) break;
        offset += 2;
        
        bytes = partition_read("gz2", offset, rle_buffer, rle_size);
        if (bytes < rle_size) break;
        offset += rle_size;
        
        int out_pos = 0, in_pos = 0;
        while (in_pos < rle_size && out_pos < max_frame_size) {
            if (in_pos + 1 > rle_size) break;
            uint8_t count = rle_buffer[in_pos++];
            uint8_t value = rle_buffer[in_pos++];
            
            if (out_pos + count > max_frame_size)
                count = max_frame_size - out_pos;
            
            for (int i = 0; i < count && out_pos < max_frame_size; i++)
                frame_buffer[out_pos++] = value;
        }
        
        int idx = 0;
        for (int y = 0; y < video_height; y++) {
            for (int x = 0; x < video_width; x++) {
                uint8_t color_idx = frame_buffer[idx++];
                uint8_t *pal = palette + (color_idx * 4);
                uint32_t color = (pal[2] << 16) | (pal[1] << 8) | pal[0] | (pal[3] << 24);
                
                for (int dy = 0; dy < scale; dy++) {
                    for (int dx = 0; dx < scale; dx++) {
                        int fb_x = start_x + x * scale + dx;
                        int fb_y = start_y + y * scale + dy;
                        if (fb_x >= 0 && fb_x < fb->width && fb_y >= 0 && fb_y < fb->height) {
                            fb_pixel(fb_x, fb_y, color);
                        }
                    }
                }
            }
        }
        mt_disp_update(0, 0, fb->width, fb->height);
        if (mtk_detect_key(KEY_POWER)) break;
        frame_num++;
        lk_mdelay(40);
    }
}


static int fastboot_ui_thread(void *arg) {
    void *fb = (void *)(uintptr_t)CONFIG_FRAMEBUFFER_ADDRESS;
    int fonts_ok = test_fonts_availablity();

    if (!fonts_ok) {
        lk_mdelay(1);
        fb_clear(FB_BLACK);
        logo_show(LOGO_ORIGINAL_FASTBOOT, fb, LOGO_STRIDE_PX,
                  CONFIG_FRAMEBUFFER_WIDTH, CONFIG_FRAMEBUFFER_HEIGHT,
                  0 /* rotation */, 1 /* do_update */);
        return 1;
    }

    ui_dram_str[0] = '\0';
    uint64_t dram = ((unsigned long long (*)(void))(PHYSICAL_MEMORY_SIZE_ADDR | 1))(); // physical_memory_size
    if (dram > 0) {
        unsigned int gb = (unsigned int)(dram / (1024ULL * 1024ULL * 1024ULL));
        if (gb > 0 && gb < 32)
            npf_snprintf(ui_dram_str, sizeof(ui_dram_str), "%u GB", gb);
    }

       ui_emmc_str[0] = '\0';
    uint64_t emmc_bytes = *(volatile uint64_t*)(G_EMMC_SIZE_ADDR);
    if (emmc_bytes > 0) {
        unsigned int gib = (unsigned int)(emmc_bytes >> 30);
        unsigned int gb = (gib * 1024u + 500u) / 1000u;
        static const unsigned int std_sizes[] = { 32, 64, 128, 256 };
        for (int i = 0; i < 4; i++) { if (gb <= std_sizes[i]) { gb = std_sizes[i]; break; } }
        if (gb > 0) npf_snprintf(ui_emmc_str, sizeof(ui_emmc_str), "%u GB", gb);
    }

    int sel = 0;

    // Get all variables before rendering fastboot logo
    for (int i = 0; i < 100; i++) {
        if (fastboot_get_var("serialno"))
            break;
        lk_mdelay(10);
    }

    logo_show(LOGO_CUSTOM_FASTBOOT, fb, LOGO_STRIDE_PX,
              CONFIG_FRAMEBUFFER_WIDTH, CONFIG_FRAMEBUFFER_HEIGHT,
              0 /* rotation */, 0 /* do_update */);
    font_load_all();
    fastboot_ui_render_info();
    fastboot_ui_render_mode(sel);

    for (;;) {
        if (mtk_detect_key(KEY_VOLUME_UP)) {
            sel = (sel + FB_OPTION_COUNT - 1) % FB_OPTION_COUNT;
            fastboot_ui_render_mode(sel);
            lk_mdelay(150);
        } else if (mtk_detect_key(KEY_VOLUME_DOWN)) {
            sel = (sel + 1) % FB_OPTION_COUNT;
            fastboot_ui_render_mode(sel);
            lk_mdelay(150);
        } else if (mtk_detect_key(KEY_POWER)) {
            lk_mdelay(200);
            switch (sel) {
                case FB_OPTION_CONTINUE:
                    fb_clear(FB_BLACK);
                    mt_disp_update(0, 0, CONFIG_FRAMEBUFFER_WIDTH, CONFIG_FRAMEBUFFER_HEIGHT);
                    lk_mdelay(100);
                    ((void (*)(void))(MT_DISP_SHOW_BOOT_LOGO_ADDR | 1))(); // mt_disp_show_boot_logo
                    ((void (*)(const char *, void *, unsigned))(CONTINUE_ADDR | 1))("", NULL, 0); // cmd_continue
                    return 0;
                case FB_OPTION_REBOOT:
                    ((void (*)(void))(REBOOT_ADDR | 1))(); // cmd_reboot
                    return 0;
                case FB_OPTION_RESTART_BOOTLOADER:
                    ((void (*)(void))(REBOOT_BOOTLOADER_ADDR | 1))(); // cmd_reboot_bootloader
                    return 0;
                case FB_OPTION_RECOVERY_MODE:
                    ((void (*)(const char *, void *, unsigned))(REBOOT_RECOVERY_ADDR | 1))("", NULL, 0); // cmd_reboot_recovery
                    return 0;
                case FB_OPTION_FASTBOOT_MODE:
                    ((void (*)(const char *, void *, unsigned))(REBOOT_FASTBOOT_ADDR | 1))("", NULL, 0); // cmd_reboot_fastboot
                    return 0;
                case FB_OPTION_POWEROFF:
                    ((void (*)(void))(MT_POWER_OFF_ADDR | 1))(); // mt_power_off
                    return 0;
                 case FB_OPTION_BAD_APPLE:
                    play_simple_video();
                    font_load_all();
                    fastboot_ui_render_info();
                    fastboot_ui_render_mode(sel);
                    break;
                
            }
        }
        lk_mdelay(100);
    }
}

#define CONFIRM_LOCK_TEXT \
    "Relocking the bootloader restores the restriction on flashing\n" \
    "partitions. The device will no longer allow flashing custom\n" \
    "firmware through fastboot.\n" \
    "\n" \
    "If you have installed a custom ROM, modified the kernel, or\n" \
    "changed any AVB-verified partition your device may fail to\n" \
    "boot after relocking and may enter a permanent bootloop or\n" \
    "become unusable until the bootloader is unlocked again."

#define CONFIRM_UNLOCK_TEXT \
    "Unlocking the bootloader grants full access to device partitions\n" \
    "and allows flashing custom firmware, kernels, or recoveries.\n" \
    "\n" \
    "Ensure you only flash trusted images, as incorrect firmware\n" \
    "can lead to system instability or make the device fail to boot."

typedef int (*seccfg_set_lock_state_t)(int lock_state);
typedef void (*mtk_arch_reset_t)(int);
static seccfg_set_lock_state_t seccfg_set_lock_state = (seccfg_set_lock_state_t) (SECCFG_SET_LOCK_STATE_ADDR | 1);
static mtk_arch_reset_t mtk_arch_reset = (mtk_arch_reset_t) (MTK_ARCH_RESET_ADDR | 1);

static int show_confirm_prompt(const char *label) {
    if (!test_fonts_availablity()) {
        return 1;
    }

    void *fb = (void *)(uintptr_t)CONFIG_FRAMEBUFFER_ADDRESS;
    logo_show(LOGO_CONFIRM, fb, LOGO_STRIDE_PX,
              CONFIG_FRAMEBUFFER_WIDTH, CONFIG_FRAMEBUFFER_HEIGHT,
              0 /* rotation */, 0 /* do_update */);

    font_load_all();
    fb_fill_rect(0, 1000, (uint32_t)CONFIG_FRAMEBUFFER_WIDTH, 65, 0xAA000000u);
    font_draw_str(80, 1120, label, UI_COLOR_TEXT, &g_font_medium);
    mt_disp_update(0, 0, CONFIG_FRAMEBUFFER_WIDTH, CONFIG_FRAMEBUFFER_HEIGHT);
    lk_mdelay(500);

    while (1) {
        if (mtk_detect_key(KEY_VOLUME_UP)) {
            lk_mdelay(150);
            return 1;
        }
        if (mtk_detect_key(KEY_VOLUME_DOWN)) {
            lk_mdelay(150);
            return 0;
        }
        lk_mdelay(100);
    }
}

static void cmd_flashing_unlock(const char *arg, void *data, unsigned sz) {
    if (get_unlocked_status()) {
        fastboot_fail("Device is already unlocked");
        return;
    }
    fastboot_okay("");

    if (show_confirm_prompt(CONFIRM_UNLOCK_TEXT))
        seccfg_set_lock_state(3);
    lk_mdelay(400);
    mtk_arch_reset(1);
}

static void cmd_flashing_lock(const char *arg, void *data, unsigned sz) {
    if (!get_unlocked_status()) {
        fastboot_fail("Device is already locked");
        return;
    }
    fastboot_okay("");

    if (show_confirm_prompt(CONFIRM_LOCK_TEXT))
        seccfg_set_lock_state(1);
    lk_mdelay(400);
    mtk_arch_reset(1);
}

void cmd_help(const char *arg, void *data, unsigned sz) {
    struct fastboot_cmd *cmd = (struct fastboot_cmd *)OEM_CMDS_ADDR;//

    if (!cmd) {
        fastboot_fail("No commands found!");
        return;
    }

    fastboot_info("Available oem commands:");//nothing find, but wrote this
    while (cmd) {
        if (cmd->prefix) {
            if (strncmp(cmd->prefix, "oem", 3) == 0) {
                fastboot_info(cmd->prefix);
            }
        }
        cmd = cmd->next;
    }
    fastboot_okay("");
}

void board_early_init(void) {
    printf("Entering early init for Realme c15\n");
    uint32_t addr = 0;
 

    addr = SEARCH_PATTERN(LK_START, LK_END, 0xB508, 0x4B0E, 0x447B, 0x681B);
    if (addr) {
        printf("Found orange_state_warning at 0x%08X\n", addr);
        FORCE_RETURN(addr, 0);
    }
   addr = SEARCH_PATTERN(LK_START, LK_END, 0xB530, 0xB083, 0xAB02, 0x2200, 0x4604);
    if (addr) {
        printf("Found dm_verity_corruption at 0x%08X\n", addr);
        FORCE_RETURN(addr, 0);
    }
    addr = SEARCH_PATTERN(LK_START, LK_END, 0xE92D, 0x4FF0, 0xB0A9, 0xF101);
    if (addr) {
        printf("Found AVB cmdline function at 0x%08X\n", addr);
        NOP(addr + 0x9C, 4);
    }
    // LK has two security gates in the fastboot command processor that
    // reject commands with "not support on security" and "not allowed
    // in locked state" errors. When spoofing lock state, these would
    // block all fastboot operations despite the device being actually
    // unlocked underneath.
    //
    // Even without spoofing, we patch these out as a safety measure
    // since OEM-specific checks could still interfere with fastboot
    // commands in unexpected ways.
    addr = SEARCH_PATTERN(LK_START, LK_END, 0xE92D, 0x4880, 0xB087, 0x4D5A);
    if (addr) {
        printf("Found fastboot command processor at 0x%08X\n", addr);
    
        // "not support on security" call
        NOP(addr + 0x15A, 2);

        // "not allowed in locked state" call
        NOP(addr + 0x166, 2);
        // Jump directly to command handler
        PATCH_MEM(addr + 0xF0, 0xE006);
    }
    // BBK added a verification check to ensure the device was officially unlocked.
    // If the check fails, the bootloader exits fastboot mode and reboots.
    //
    // This is unnecessary, seccfg-based unlocks are already valid, so we patch
    // the check to always return true, ensuring fastboot remains accessible.
    addr = SEARCH_PATTERN(LK_START, LK_END, 0xB508, 0xF7C9, 0xF851);
    if (addr) {
        printf("Found fastboot_unlock_verify at 0x%08X\n", addr);
        FORCE_RETURN(addr, 0);
    }

    // In cmd_flash function, there's a check for local_6c == 0 that rejects
    // flashing certain partitions with "download for partition is not allowed".
    // This is a software restriction, not a hardware/fuse limitation.
    //
    // We patch the conditional branch (beq) to an unconditional branch (b),
    // skipping the failure path and allowing the flash to proceed.
    addr = SEARCH_PATTERN(LK_START, LK_END, 0x9b07, 0x2b00, 0xf000, 0x8091);
    if (addr) {
        printf("Found download restriction at 0x%08X\n", addr);
        PATCH_MEM(addr + 2, 0xe000);  // b instead of beq
    }
    // The stock cmd_erase function has a check "if (local_64 == 0)" that
    // rejects erasing partitions with "format for partition is not allowed".
    //
    // We NOP out the cbz instruction, so the check never branches to fail.
    addr = SEARCH_PATTERN(LK_START, LK_END, 0x9b03, 0xb323);  // ldr r3, [sp, #0xc]; cbz r3
    if (addr) {
        printf("Found erase restriction at 0x%08X\n", addr);
        NOP(addr + 2, 2);
    }

    addr = SEARCH_PATTERN(LK_START, LK_END, 0xE92D, 0x41F0, 0x460A, 0x4604);
    if (addr) {
        printf("Found ccci_ld_md_sec_ptr_hdr_verify at 0x%08X\n", addr);
        FORCE_RETURN(addr, 0);
    }
      // This function handles certificate chain and hash verification for
    // modem-related images (md1rom, md3rom, etc.) during the modem loading
    // process. Same idea as above — force it to return 0 so modem images
    // can be loaded without passing signature verification.
    addr = SEARCH_PATTERN(LK_START, LK_END, 0xE92D, 0x43F0, 0x460C, 0x4601);
   if (addr) {
        printf("Found ccci_ld_md_sec_image_verify at 0x%08X\n", addr);
       FORCE_RETURN(addr, 0);
    }

    PATCH_MEM_ARM(0x4827b608, (uint32_t)cmd_flashing_unlock | 1);
    PATCH_MEM_ARM(0x4827b78c, (uint32_t)cmd_flashing_lock   | 1);

    // Register help command with all available commands
    fastboot_register("oem help", cmd_help, 1);
}

void board_late_init(void) {
    printf("Entering late init for Realme c15\n");
    if (get_bootmode()) {    
        show_bootmode(get_bootmode());
    }
    if (mtk_detect_key(KEY_VOLUME_UP)) {
        set_bootmode(BOOTMODE_RECOVERY);
        show_bootmode(get_bootmode());
    } else if (mtk_detect_key(KEY_VOLUME_DOWN)) {
        set_bootmode(BOOTMODE_FASTBOOT);
    }
    // On locked Realme devices, volume key detection is broken, making it
    // difficult to enter fastboot or recovery mode through key combos.
    //
    if (mtk_detect_key(KEY_VOLUME_DOWN)) {
        ((void (*)(void))(REBOOT_BOOTLOADER_ADDR | 1))();
    }
    // Run custom  fastboot UI
    if (get_bootmode() == BOOTMODE_FASTBOOT) {
        thread_t *thr = thread_create("fastboot_ui", fastboot_ui_thread, NULL, LOW_PRIORITY, DEFAULT_STACK_SIZE);
        if (thr)
            thread_resume(thr);
    }
}
