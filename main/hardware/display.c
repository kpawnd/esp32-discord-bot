#include "display.h"
#include "buttons.h"
#include "buzzer.h"
#include "config/web_server.h"
#include "net/wifi.h"
#include "discord/gateway.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>

#define TAG "display"

/* M5StickC PLUS2 ST7789V2 pin map */
#define LCD_SCLK  13
#define LCD_MOSI  15
#define LCD_DC    14
#define LCD_CS     5
#define LCD_RST   12
#define LCD_BL    27
#define LCD_W    135
#define LCD_H    240
#define LCD_SPI  SPI2_HOST

static spi_device_handle_t s_spi;
static bool s_dirty = true;
static int64_t s_last_cfg_toggle_us = 0;

/* ---- Status state -------------------------------------------------------- */

static struct {
    bool    gw_connected;
    char    gw_state[16];
    char    last_event[32];
    uint32_t heap_kb;
    int8_t   rssi;
} s_status;

void display_set_gateway_status(bool c, const char *st) {
    s_status.gw_connected = c;
    strlcpy(s_status.gw_state, st, sizeof(s_status.gw_state));
    s_dirty = true;
}
void display_set_last_event(const char *e) {
    strlcpy(s_status.last_event, e, sizeof(s_status.last_event));
    s_dirty = true;
}
void display_set_heap_kb(uint32_t kb)  { s_status.heap_kb = kb; s_dirty = true; }
void display_set_rssi(int8_t r)        { s_status.rssi = r;     s_dirty = true; }

/* ---- Minimal 5×7 font (ASCII 0x20–0x7E) --------------------------------- */
/* Each char: 5 bytes = 5 columns of 7 bits (bit0 = top row) */
static const uint8_t s_font5x7[][5] = {
    {0x00,0x00,0x00,0x00,0x00}, /* ' ' */
    {0x00,0x00,0x5F,0x00,0x00}, /* '!' */
    {0x00,0x07,0x00,0x07,0x00}, /* '"' */
    {0x14,0x7F,0x14,0x7F,0x14}, /* '#' */
    {0x24,0x2A,0x7F,0x2A,0x12}, /* '$' */
    {0x23,0x13,0x08,0x64,0x62}, /* '%' */
    {0x36,0x49,0x55,0x22,0x50}, /* '&' */
    {0x00,0x05,0x03,0x00,0x00}, /* ''' */
    {0x00,0x1C,0x22,0x41,0x00}, /* '(' */
    {0x00,0x41,0x22,0x1C,0x00}, /* ')' */
    {0x14,0x08,0x3E,0x08,0x14}, /* '*' */
    {0x08,0x08,0x3E,0x08,0x08}, /* '+' */
    {0x00,0x50,0x30,0x00,0x00}, /* ',' */
    {0x08,0x08,0x08,0x08,0x08}, /* '-' */
    {0x00,0x60,0x60,0x00,0x00}, /* '.' */
    {0x20,0x10,0x08,0x04,0x02}, /* '/' */
    {0x3E,0x51,0x49,0x45,0x3E}, /* '0' */
    {0x00,0x42,0x7F,0x40,0x00}, /* '1' */
    {0x42,0x61,0x51,0x49,0x46}, /* '2' */
    {0x21,0x41,0x45,0x4B,0x31}, /* '3' */
    {0x18,0x14,0x12,0x7F,0x10}, /* '4' */
    {0x27,0x45,0x45,0x45,0x39}, /* '5' */
    {0x3C,0x4A,0x49,0x49,0x30}, /* '6' */
    {0x01,0x71,0x09,0x05,0x03}, /* '7' */
    {0x36,0x49,0x49,0x49,0x36}, /* '8' */
    {0x06,0x49,0x49,0x29,0x1E}, /* '9' */
    {0x00,0x36,0x36,0x00,0x00}, /* ':' */
    {0x00,0x56,0x36,0x00,0x00}, /* ';' */
    {0x08,0x14,0x22,0x41,0x00}, /* '<' */
    {0x14,0x14,0x14,0x14,0x14}, /* '=' */
    {0x00,0x41,0x22,0x14,0x08}, /* '>' */
    {0x02,0x01,0x51,0x09,0x06}, /* '?' */
    {0x32,0x49,0x79,0x41,0x3E}, /* '@' */
    {0x7E,0x11,0x11,0x11,0x7E}, /* 'A' */
    {0x7F,0x49,0x49,0x49,0x36}, /* 'B' */
    {0x3E,0x41,0x41,0x41,0x22}, /* 'C' */
    {0x7F,0x41,0x41,0x22,0x1C}, /* 'D' */
    {0x7F,0x49,0x49,0x49,0x41}, /* 'E' */
    {0x7F,0x09,0x09,0x09,0x01}, /* 'F' */
    {0x3E,0x41,0x49,0x49,0x7A}, /* 'G' */
    {0x7F,0x08,0x08,0x08,0x7F}, /* 'H' */
    {0x00,0x41,0x7F,0x41,0x00}, /* 'I' */
    {0x20,0x40,0x41,0x3F,0x01}, /* 'J' */
    {0x7F,0x08,0x14,0x22,0x41}, /* 'K' */
    {0x7F,0x40,0x40,0x40,0x40}, /* 'L' */
    {0x7F,0x02,0x0C,0x02,0x7F}, /* 'M' */
    {0x7F,0x04,0x08,0x10,0x7F}, /* 'N' */
    {0x3E,0x41,0x41,0x41,0x3E}, /* 'O' */
    {0x7F,0x09,0x09,0x09,0x06}, /* 'P' */
    {0x3E,0x41,0x51,0x21,0x5E}, /* 'Q' */
    {0x7F,0x09,0x19,0x29,0x46}, /* 'R' */
    {0x46,0x49,0x49,0x49,0x31}, /* 'S' */
    {0x01,0x01,0x7F,0x01,0x01}, /* 'T' */
    {0x3F,0x40,0x40,0x40,0x3F}, /* 'U' */
    {0x1F,0x20,0x40,0x20,0x1F}, /* 'V' */
    {0x3F,0x40,0x38,0x40,0x3F}, /* 'W' */
    {0x63,0x14,0x08,0x14,0x63}, /* 'X' */
    {0x07,0x08,0x70,0x08,0x07}, /* 'Y' */
    {0x61,0x51,0x49,0x45,0x43}, /* 'Z' */
    {0x00,0x7F,0x41,0x41,0x00}, /* '[' */
    {0x02,0x04,0x08,0x10,0x20}, /* '\' */
    {0x00,0x41,0x41,0x7F,0x00}, /* ']' */
    {0x04,0x02,0x01,0x02,0x04}, /* '^' */
    {0x40,0x40,0x40,0x40,0x40}, /* '_' */
    {0x00,0x01,0x02,0x04,0x00}, /* '`' */
    {0x20,0x54,0x54,0x54,0x78}, /* 'a' */
    {0x7F,0x48,0x44,0x44,0x38}, /* 'b' */
    {0x38,0x44,0x44,0x44,0x20}, /* 'c' */
    {0x38,0x44,0x44,0x48,0x7F}, /* 'd' */
    {0x38,0x54,0x54,0x54,0x18}, /* 'e' */
    {0x08,0x7E,0x09,0x01,0x02}, /* 'f' */
    {0x0C,0x52,0x52,0x52,0x3E}, /* 'g' */
    {0x7F,0x08,0x04,0x04,0x78}, /* 'h' */
    {0x00,0x44,0x7D,0x40,0x00}, /* 'i' */
    {0x20,0x40,0x44,0x3D,0x00}, /* 'j' */
    {0x7F,0x10,0x28,0x44,0x00}, /* 'k' */
    {0x00,0x41,0x7F,0x40,0x00}, /* 'l' */
    {0x7C,0x04,0x18,0x04,0x78}, /* 'm' */
    {0x7C,0x08,0x04,0x04,0x78}, /* 'n' */
    {0x38,0x44,0x44,0x44,0x38}, /* 'o' */
    {0x7C,0x14,0x14,0x14,0x08}, /* 'p' */
    {0x08,0x14,0x14,0x18,0x7C}, /* 'q' */
    {0x7C,0x08,0x04,0x04,0x08}, /* 'r' */
    {0x48,0x54,0x54,0x54,0x20}, /* 's' */
    {0x04,0x3F,0x44,0x40,0x20}, /* 't' */
    {0x3C,0x40,0x40,0x20,0x7C}, /* 'u' */
    {0x1C,0x20,0x40,0x20,0x1C}, /* 'v' */
    {0x3C,0x40,0x30,0x40,0x3C}, /* 'w' */
    {0x44,0x28,0x10,0x28,0x44}, /* 'x' */
    {0x0C,0x50,0x50,0x50,0x3C}, /* 'y' */
    {0x44,0x64,0x54,0x4C,0x44}, /* 'z' */
    {0x00,0x08,0x36,0x41,0x00}, /* '{' */
    {0x00,0x00,0x7F,0x00,0x00}, /* '|' */
    {0x00,0x41,0x36,0x08,0x00}, /* '}' */
    {0x10,0x08,0x08,0x10,0x08}, /* '~' */
};

/* ---- ST7789 low-level --------------------------------------------------- */

static void lcd_cmd(uint8_t cmd) {
    gpio_set_level(LCD_DC, 0);
    spi_transaction_t t = { .length = 8, .tx_buffer = &cmd };
    spi_device_polling_transmit(s_spi, &t);
}

static void lcd_data(const uint8_t *data, int len) {
    if (!len) return;
    gpio_set_level(LCD_DC, 1);
    spi_transaction_t t = { .length = (size_t)len * 8, .tx_buffer = data };
    spi_device_polling_transmit(s_spi, &t);
}

static void lcd_u8(uint8_t v) { lcd_data(&v, 1); }

static void lcd_u16(uint16_t v) {
    uint8_t b[2] = { v >> 8, v & 0xFF };
    lcd_data(b, 2);
}

static void lcd_set_window(int x0, int y0, int x1, int y1) {
    /* PLUS2: x offset=52, y offset=40 for portrait */
    x0 += 52; x1 += 52;
    y0 += 40; y1 += 40;
    lcd_cmd(0x2A); lcd_u16(x0); lcd_u16(x1);
    lcd_cmd(0x2B); lcd_u16(y0); lcd_u16(y1);
    lcd_cmd(0x2C);
}

static void lcd_fill(uint16_t color, int w, int h) {
    /* Send 128-pixel row batches to avoid large stack allocs */
    static uint8_t row[128 * 2];
    for (int i = 0; i < 128; i++) { row[i*2] = color >> 8; row[i*2+1] = color & 0xFF; }
    int total = w * h;
    gpio_set_level(LCD_DC, 1);
    while (total > 0) {
        int batch = total < 128 ? total : 128;
        spi_transaction_t t = { .length = (size_t)batch * 16, .tx_buffer = row };
        spi_device_polling_transmit(s_spi, &t);
        total -= batch;
    }
}

/* Draw character at pixel position (x, y), scaled up 2x */
static void lcd_char(int x, int y, char ch, uint16_t fg, uint16_t bg) {
    if (ch < 0x20 || ch > 0x7E) ch = '?';
    const uint8_t *glyph = s_font5x7[ch - 0x20];
    static uint8_t buf[5 * 2 * 7 * 2 * 2]; /* 5cols*2scale * 7rows*2scale * 2bytes */
    int idx = 0;
    for (int row = 0; row < 7; row++) {
        for (int rep_r = 0; rep_r < 2; rep_r++) {
            for (int col = 0; col < 5; col++) {
                uint16_t px = (glyph[col] >> row) & 1 ? fg : bg;
                for (int rep_c = 0; rep_c < 2; rep_c++) {
                    buf[idx++] = px >> 8;
                    buf[idx++] = px & 0xFF;
                }
            }
        }
    }
    lcd_set_window(x, y, x + 9, y + 13); /* 5*2 wide, 7*2 tall */
    lcd_data(buf, idx);
}

static void lcd_str(int x, int y, const char *str, uint16_t fg, uint16_t bg) {
    while (*str && x < LCD_W) {
        lcd_char(x, y, *str++, fg, bg);
        x += 11; /* 10px char + 1px gap */
    }
}

static void lcd_hline(int y, uint16_t color) {
    lcd_set_window(0, y, LCD_W - 1, y);
    lcd_fill(color, LCD_W, 1);
}

/* RGB565 color helpers */
#define RGB565(r,g,b) ((uint16_t)(((r) & 0xF8) << 8 | ((g) & 0xFC) << 3 | ((b) >> 3)))
#define C_BLACK   0x0000
#define C_WHITE   0xFFFF
#define C_GREEN   RGB565(0, 200, 80)
#define C_RED     RGB565(220, 40, 40)
#define C_BLUE    RGB565(40, 100, 220)
#define C_YELLOW  RGB565(220, 200, 0)
#define C_DGRAY   RGB565(30, 30, 30)
#define C_LGRAY   RGB565(160, 160, 160)

/* ---- Init & draw --------------------------------------------------------- */

esp_err_t display_init(void) {
    ESP_LOGI(TAG, "display_init: BL gpio");
    gpio_config_t bl = {
        .pin_bit_mask = BIT64(LCD_BL),
        .mode         = GPIO_MODE_OUTPUT,
        .intr_type    = GPIO_INTR_DISABLE,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&bl));
    gpio_set_level(LCD_BL, 1);

    ESP_LOGI(TAG, "display_init: SPI bus");
    spi_bus_config_t bus = {
        .miso_io_num   = -1,
        .mosi_io_num   = LCD_MOSI,
        .sclk_io_num   = LCD_SCLK,
        .quadwp_io_num = -1, .quadhd_io_num = -1,
        .max_transfer_sz = LCD_W * LCD_H * 2,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_SPI, &bus, SPI_DMA_CH_AUTO));

    ESP_LOGI(TAG, "display_init: SPI device");
    spi_device_interface_config_t dev = {
        .clock_speed_hz = 20 * 1000 * 1000,
        .mode           = 0,
        .spics_io_num   = LCD_CS,
        .queue_size     = 7,
        .pre_cb         = NULL,
    };
    ESP_ERROR_CHECK(spi_bus_add_device(LCD_SPI, &dev, &s_spi));

    ESP_LOGI(TAG, "display_init: DC/RST gpio");
    gpio_config_t io = {
        .pin_bit_mask = BIT64(LCD_DC) | BIT64(LCD_RST),
        .mode         = GPIO_MODE_OUTPUT,
        .intr_type    = GPIO_INTR_DISABLE,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));

    ESP_LOGI(TAG, "display_init: HW reset");
    gpio_set_level(LCD_RST, 0); vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(LCD_RST, 1); vTaskDelay(pdMS_TO_TICKS(120));

    ESP_LOGI(TAG, "display_init: LCD init sequence");
    lcd_cmd(0x01); vTaskDelay(pdMS_TO_TICKS(150)); /* SWRESET */
    lcd_cmd(0x11); vTaskDelay(pdMS_TO_TICKS(120)); /* SLPOUT */
    lcd_cmd(0x3A); lcd_u8(0x05);                   /* COLMOD: 16-bit */
    lcd_cmd(0x36); lcd_u8(0x00);                   /* MADCTL: portrait */
    lcd_cmd(0xB2); lcd_u8(0x0C); lcd_u8(0x0C); lcd_u8(0x00); lcd_u8(0x33); lcd_u8(0x33);
    lcd_cmd(0xB7); lcd_u8(0x35);
    lcd_cmd(0xBB); lcd_u8(0x19);
    lcd_cmd(0xC0); lcd_u8(0x2C);
    lcd_cmd(0xC2); lcd_u8(0x01);
    lcd_cmd(0xC3); lcd_u8(0x12);
    lcd_cmd(0xC4); lcd_u8(0x20);
    lcd_cmd(0xC6); lcd_u8(0x0F);
    lcd_cmd(0xD0); lcd_u8(0xA4); lcd_u8(0xA1);
    lcd_cmd(0x29); vTaskDelay(pdMS_TO_TICKS(10)); /* DISPON */

    ESP_LOGI(TAG, "display_init: clear screen");
    lcd_set_window(0, 0, LCD_W - 1, LCD_H - 1);
    lcd_fill(C_BLACK, LCD_W, LCD_H);

    ESP_LOGI(TAG, "ST7789V2 initialized (%dx%d)", LCD_W, LCD_H);
    return ESP_OK;
}

void display_update(void) {
    if (!s_dirty) return;
    s_dirty = false;

    /* Header */
    lcd_set_window(0, 0, LCD_W - 1, 15);
    lcd_fill(C_BLUE, LCD_W, 16);
    lcd_str(2, 1, "Discord Bot", C_WHITE, C_BLUE);
    lcd_hline(16, C_LGRAY);

    /* Gateway status */
    uint16_t gw_col = s_status.gw_connected ? C_GREEN : C_RED;
    lcd_set_window(0, 18, LCD_W - 1, 33);
    lcd_fill(C_BLACK, LCD_W, 16);
    char line[24];
    snprintf(line, sizeof(line), "GW: %s", s_status.gw_state);
    lcd_str(2, 18, line, gw_col, C_BLACK);

    /* Latency */
    extern int32_t gateway_latency_ms(void);
    int32_t lat = gateway_latency_ms();
    char lat_str[16];
    snprintf(lat_str, sizeof(lat_str), "%dms", (int)lat);
    lcd_str(LCD_W - 40, 18, lat_str, C_LGRAY, C_BLACK);

    lcd_hline(34, C_DGRAY);

    /* Last event */
    lcd_set_window(0, 36, LCD_W - 1, 51);
    lcd_fill(C_BLACK, LCD_W, 16);
    snprintf(line, sizeof(line), "%.22s", s_status.last_event);
    lcd_str(2, 36, line, C_WHITE, C_BLACK);

    lcd_hline(52, C_DGRAY);

    /* Heap */
    lcd_set_window(0, 54, LCD_W - 1, 69);
    lcd_fill(C_BLACK, LCD_W, 16);
    snprintf(line, sizeof(line), "Heap: %luKB", (unsigned long)s_status.heap_kb);
    lcd_str(2, 54, line, C_LGRAY, C_BLACK);

    /* WiFi RSSI */
    lcd_set_window(0, 70, LCD_W - 1, 85);
    lcd_fill(C_BLACK, LCD_W, 16);
    snprintf(line, sizeof(line), "WiFi: %ddBm", s_status.rssi);
    lcd_str(2, 70, line, C_LGRAY, C_BLACK);
}

void display_show_config_mode(const char *ip_addr) {
    lcd_set_window(0, 0, LCD_W - 1, LCD_H - 1);
    lcd_fill(C_BLACK, LCD_W, LCD_H);
    lcd_str(2,  2, "Config Mode", C_YELLOW, C_BLACK);
    lcd_str(2, 20, "Open browser:", C_WHITE,  C_BLACK);
    lcd_str(2, 36, "http://", C_LGRAY, C_BLACK);
    if (ip_addr) lcd_str(2, 52, ip_addr, C_GREEN, C_BLACK);
}

/* ---- Hardware task ------------------------------------------------------- */

void hardware_task(void *arg) {
    (void)arg;
    button_event_t evt;
    static bool mod_muted = false;
    static int page = 0;

    for (;;) {
        /* Poll display update at ~5 Hz */
        s_status.heap_kb = (uint32_t)(esp_get_free_heap_size() / 1024);
        s_status.rssi    = wifi_rssi();

        const char *gw_states[] = { "DISC", "CONN", "IDENT", "RESUME", "READY" };
        gateway_state_t gst = gateway_state();
        if (gst <= GW_STATE_READY)
            display_set_gateway_status(gateway_is_connected(), gw_states[gst]);

        display_update();

        /* Handle button events (non-blocking) */
        if (buttons_get_event(&evt, 0)) {
            if (evt.id == BTN_A) {
                if (evt.type == BTN_SHORT_PRESS) {
                    page = (page + 1) % 2;
                    s_dirty = true;
                } else if (evt.type == BTN_LONG_PRESS) {
                    mod_muted = !mod_muted;
                    display_set_last_event(mod_muted ? "Mod: MUTED" : "Mod: ACTIVE");
                    buzzer_beep(mod_muted ? 3 : 1);
                }
            } else if (evt.id == BTN_B) {
                if (evt.type == BTN_SHORT_PRESS) {
                    display_set_last_event("Status posted");
                } else if (evt.type == BTN_LONG_PRESS) {
                    int64_t now = esp_timer_get_time();
                    if (now - s_last_cfg_toggle_us < 1500000) continue;
                    s_last_cfg_toggle_us = now;
                    if (!web_server_is_running()) {
                        web_server_start();
                        display_set_last_event("Config server ON");
                    } else {
                        web_server_stop();
                        display_set_last_event("Config server OFF");
                    }
                    buzzer_beep(1);
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(200));
    }
}
