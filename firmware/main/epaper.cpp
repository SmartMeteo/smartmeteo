#include "esp_rom_sys.h"
#include "epaper.h"
#include "rle.h"
#include "esp_log.h"
#include "rom/ets_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>

#define PROGMEM
#include "glcdfont.c"

static const char* TAG = "EPAPER";

// The vendor's own border value. Only the border bench changes it — see epaper.h.
int EPaper::part_border = 0x80;
int EPaper::init_border = 0x05;
int EPaper::gray_border = -1;   // -1 = GRAY_BORDER_HIZ; only the border bench overrides it

static uint8_t s_frame_buffer[EPD_BUFFER_SIZE];

// At least `ms`: pdMS_TO_TICKS rounds down and the first tick of a delay is partial (100 Hz tick).
static void wait_ms(uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms + portTICK_PERIOD_MS - 1) + 1); }

#define EPD_MOSI    GPIO_NUM_23
#define EPD_SCK     GPIO_NUM_22
#define EPD_CS      GPIO_NUM_21
#define EPD_DC      GPIO_NUM_20
#define EPD_RST     GPIO_NUM_19
#define EPD_BUSY    GPIO_NUM_18
#define EPD_PWR_EN  GPIO_NUM_10

EPaper::EPaper() : 
    spi_handle(nullptr), 
    frame_buffer(nullptr), 
    initialized(false),
    part_armed(false),
    hibernating(false),
    busy_stuck(false),
    spi_failed(false)
{
}

EPaper::~EPaper() {
    // Nothing to release: the frame buffer has static storage (see above). Note what this
    // destructor still does NOT do -- it does not remove the SPI device, which is why constructing
    // an EPaper per iteration once exhausted the bus and hung the device silently (4.15).
}

// The vendor never sends a byte while BUSY is high: Epaper_READBUSY() loops until it falls, and every
// command after an operation follows that wait. waitBusy() gives up at a limit instead, so the rule is kept
// here: nothing goes to a controller that still reports busy. Nor after a byte the SPI driver failed to
// send — the update command that ends each vendor sequence would drive a frame that never fully arrived.
// Both clear at the hardware reset each vendor sequence begins with.
bool EPaper::linkUsable() {
    if (!spi_handle || spi_failed) return false;
    if (busy_stuck) {
        if (gpio_get_level(EPD_BUSY) == 1) return false;
        busy_stuck = false;             // the operation finished after all
    }
    return true;
}

void EPaper::writeCommand(uint8_t cmd) {
    if (!linkUsable()) return;
    gpio_set_level(EPD_DC, 0);
    spi_transaction_t t = {};
    t.length = 8;
    t.tx_buffer = &cmd;
    const esp_err_t e = spi_device_polling_transmit(spi_handle, &t);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "SPI command 0x%02X failed: %s", cmd, esp_err_to_name(e));
        spi_failed = true;
    }
}

void EPaper::writeData(const uint8_t *data, size_t len) {
    if (len == 0 || !linkUsable()) return;
    gpio_set_level(EPD_DC, 1);
    // SPI DMA can only read from internal RAM. The 4-gray planes and the waveform LUT live in
    // FLASH, and a 15 KB transaction also needs a DMA descriptor chain that may not be available
    // once Wi-Fi and the web server have taken their share of the heap. Both failures are silent:
    // the transfer simply does not happen, the panel receives nothing, BUSY never rises, and the
    // whole refresh "succeeds" in a few hundred milliseconds. That is exactly what happened after
    // a 3-minute portal session (heap 226 KB) while the same code worked at boot (heap 316 KB).
    // So copy through a small RAM bounce buffer in chunks: no large allocation, no flash source.
    static uint8_t bounce[1024];
    size_t sent = 0;
    while (sent < len) {
        const size_t n = (len - sent > sizeof(bounce)) ? sizeof(bounce) : (len - sent);
        memcpy(bounce, data + sent, n);
        spi_transaction_t t = {};
        t.length = n * 8;
        t.tx_buffer = bounce;
        esp_err_t e = spi_device_polling_transmit(spi_handle, &t);
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "SPI write failed at %u/%u: %s",
                     (unsigned)sent, (unsigned)len, esp_err_to_name(e));
            spi_failed = true;
            return;
        }
        sent += n;
    }
}

void EPaper::writeDataByte(uint8_t val) {
    writeData(&val, 1);
}

void EPaper::waitBusy(uint32_t max_delay_ms) {
    if (!linkUsable()) return;          // nothing went out that BUSY could be answering
    uint32_t elapsed = 0;
    // The controller does not raise BUSY the instant the command lands — it takes a few ms to
    // start the waveform. Polling straight away therefore sees 0 ("idle"), returns immediately,
    // and the caller believes a 7-second refresh already finished. That was invisible while the
    // device stayed awake afterwards (the panel finished on its own), but fatal before sleep:
    // the logo was written and then the chip powered down mid-waveform, leaving the previous
    // image on the glass. Measured: display4Gray "completed" in 251 ms. So first give BUSY a
    // moment to ASSERT, then wait for it to clear.
    // pdMS_TO_TICKS(2) is ZERO ticks at the configured 100 Hz tick rate, so this loop used to spin
    // thirty times without yielding and cover no time at all — the grace period it exists to
    // provide did not exist. A busy-wait in microseconds is the honest way to wait for something
    // that asserts in tens of them, and it does not depend on the tick rate at all.
    for (uint32_t t = 0; t < 60000 && gpio_get_level(EPD_BUSY) == 0; t += 500) {
        esp_rom_delay_us(500);
    }
    // SSD1683 busy pin is active high (1 = busy, 0 = idle)
    while (gpio_get_level(EPD_BUSY) == 1) {
        vTaskDelay(pdMS_TO_TICKS(10));
        elapsed += 10;
        if (elapsed >= max_delay_ms) {
            ESP_LOGE(TAG, "BUSY still high after %u ms — no further commands until the next reset",
                     (unsigned)max_delay_ms);
            busy_stuck = true;
            break;
        }
    }
}

void EPaper::reset() {
    busy_stuck = false;
    spi_failed = false;
    gpio_set_level(EPD_RST, 1);
    wait_ms(20);
    gpio_set_level(EPD_RST, 0);
    wait_ms(10);
    gpio_set_level(EPD_RST, 1);
    wait_ms(20);
}

bool EPaper::init() {
    if (initialized) {
        return true;
    }

    ESP_LOGI(TAG, "Initializing E-Paper driver (SSD1683)...");

    // 1. Frame buffer: static storage, cleared to white the first time only. An init() after hibernate()
    // in the same wake must not wipe a frame that has been drawn and not yet sent.
    if (!frame_buffer) {
        frame_buffer = s_frame_buffer;
        memset(frame_buffer, 0xFF, EPD_BUFFER_SIZE);
    }

    // 2. Configure GPIO pins
    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = (1ULL << EPD_DC) | (1ULL << EPD_RST) | (1ULL << EPD_PWR_EN);
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    gpio_config(&io_conf);

    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pin_bit_mask = (1ULL << EPD_BUSY);
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&io_conf);

    // Turn E-Paper power ON (Active LOW PMOS)
    gpio_set_level(EPD_PWR_EN, 0);
    wait_ms(10);

    // 3. Configure SPI Bus
    spi_bus_config_t buscfg = {};
    buscfg.mosi_io_num = EPD_MOSI;
    buscfg.miso_io_num = -1;
    buscfg.sclk_io_num = EPD_SCK;
    buscfg.quadwp_io_num = -1;
    buscfg.quadhd_io_num = -1;
    buscfg.max_transfer_sz = EPD_BUFFER_SIZE;

    esp_err_t err = spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "SPI bus init failed: %s", esp_err_to_name(err));
        return false;
    }

    // Add EPD to SPI bus — once per instance. init() runs again after hibernate(), and a second
    // spi_bus_add_device() would take another chip-select slot on the bus (4.15).
    if (!spi_handle) {
        spi_device_interface_config_t devcfg = {};
        devcfg.clock_speed_hz = 4000000; // 4 MHz
        devcfg.mode = 0;                 // Mode 0
        devcfg.spics_io_num = EPD_CS;    // Hardware CS
        devcfg.queue_size = 7;

        err = spi_bus_add_device(SPI2_HOST, &devcfg, &spi_handle);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to add SPI device: %s", esp_err_to_name(err));
            spi_handle = nullptr;
            return false;
        }
    }

    // No register programming here on purpose. Every vendor entry point — EPD_HW_Init,
    // EPD_HW_Init_4G, EPD_Dis_PartAll — begins with its own hardware reset and sets up exactly
    // the registers its waveform needs. Configuring them once at startup, as we used to, meant a
    // partial and a full frame ran against the same half-init state and neither got what it
    // wanted. init() now brings up pins, power and SPI, and stops there.
    reset();

    hibernating = false;
    initialized = true;
    ESP_LOGI(TAG, "E-Paper SSD1683 driver initialized successfully!");
    return true;
}

void EPaper::clear(uint8_t value) {
    if (frame_buffer) {
        memset(frame_buffer, value, EPD_BUFFER_SIZE);
    }
}

void EPaper::drawPixel(int16_t x, int16_t y, uint8_t color) {
    if (x < 0 || x >= EPD_WIDTH || y < 0 || y >= EPD_HEIGHT) return;
    
    uint16_t byte_idx = (y * (EPD_WIDTH / 8)) + (x / 8);
    uint8_t bit_mask = 0x80 >> (x % 8);
    
    if (color == 0) { // Black
        frame_buffer[byte_idx] &= ~bit_mask;
    } else { // White
        frame_buffer[byte_idx] |= bit_mask;
    }
}

void EPaper::drawChar(int16_t x, int16_t y, char c, uint8_t color, uint8_t size) {
    uint16_t font_idx = (uint8_t)c * 5;  // unsigned: extended chars (e.g. degree 0xF8) index correctly
    for (int8_t i = 0; i < 5; i++) {
        uint8_t line = font[font_idx + i];
        for (int8_t j = 0; j < 8; j++) {
            if (line & 0x01) {
                if (size == 1) {
                    drawPixel(x + i, y + j, color);
                } else {
                    for (uint8_t sx = 0; sx < size; sx++) {
                        for (uint8_t sy = 0; sy < size; sy++) {
                            drawPixel(x + i * size + sx, y + j * size + sy, color);
                        }
                    }
                }
            }
            line >>= 1;
        }
    }
}

uint16_t EPaper::drawString(int16_t x, int16_t y, const char* str, uint8_t color, uint8_t size) {
    int16_t cx = x;
    while (*str) {
        drawChar(cx, y, *str, color, size);
        cx += 6 * size;
        str++;
    }
    return cx;
}

uint16_t EPaper::drawGFXChar(int16_t x, int16_t y, char c, const GFXfont* font,
                             uint8_t color, uint8_t size) {
    uint8_t cc = (uint8_t)c;
    if (cc < font->first || cc > font->last) return 6 * size;   // matches mockup Oc fallback
    const GFXglyph* g = &font->glyph[cc - font->first];
    const uint8_t* bm = font->bitmap;
    uint16_t bo = g->bitmapOffset;
    uint8_t w = g->width, h = g->height;
    int8_t xo = g->xOffset, yo = g->yOffset;
    uint8_t bits = 0, bit = 0;
    for (uint8_t yy = 0; yy < h; yy++) {
        for (uint8_t xx = 0; xx < w; xx++) {
            if (!(bit & 7)) bits = bm[bo++];
            bit++;
            if (bits & 0x80) {
                if (size == 1) drawPixel(x + xo + xx, y + yo + yy, color);
                else fillRect(x + (xo + xx) * size, y + (yo + yy) * size, size, size, color);
            }
            bits <<= 1;
        }
    }
    return g->xAdvance * size;
}

uint16_t EPaper::drawGFXString(int16_t x, int16_t y, const char* str, const GFXfont* font,
                               uint8_t color, uint8_t size) {
    int16_t cx = x;
    while (*str) { cx += drawGFXChar(cx, y, *str, font, color, size); str++; }
    return cx;
}

uint16_t EPaper::gfxStrWidth(const char* str, const GFXfont* font, uint8_t size) {
    uint16_t w = 0;
    while (*str) {
        uint8_t cc = (uint8_t)*str;
        w += (cc < font->first || cc > font->last) ? 6 * size
                                                   : font->glyph[cc - font->first].xAdvance * size;
        str++;
    }
    return w;
}

void EPaper::drawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint8_t color) {
    int16_t dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int16_t dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int16_t err = dx + dy, e2;

    while (true) {
        drawPixel(x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void EPaper::drawRect(int16_t x, int16_t y, int16_t w, int16_t h, uint8_t color) {
    // A rectangle with no width or height must draw nothing. Without this the corner arithmetic
    // (y + h - 1) runs backwards past the origin, so a zero-height rect paints the row ABOVE it --
    // a wrong pixel outside the caller's own box, from a caller that asked for an empty one. No
    // layout produces that today; layouts change, and this is one comparison in a primitive.
    if (w <= 0 || h <= 0) return;
    drawLine(x, y, x + w - 1, y, color);
    drawLine(x + w - 1, y, x + w - 1, y + h - 1, color);
    drawLine(x + w - 1, y + h - 1, x, y + h - 1, color);
    drawLine(x, y + h - 1, x, y, color);
}

// Adafruit-GFX-style quarter-circle helper for rounded rects. corners: 1=TL 2=TR 4=BR 8=BL.
static void circleHelper(EPaper* e, int16_t x0, int16_t y0, int16_t r, uint8_t corner, uint8_t color,
                         void (EPaper::*dp)(int16_t,int16_t,uint8_t)) {
    int16_t f = 1 - r, ddF_x = 1, ddF_y = -2 * r, x = 0, y = r;
    while (x < y) {
        if (f >= 0) { y--; ddF_y += 2; f += ddF_y; }
        x++; ddF_x += 2; f += ddF_x;
        if (corner & 0x4) { (e->*dp)(x0 + x, y0 + y, color); (e->*dp)(x0 + y, y0 + x, color); }
        if (corner & 0x2) { (e->*dp)(x0 + x, y0 - y, color); (e->*dp)(x0 + y, y0 - x, color); }
        if (corner & 0x8) { (e->*dp)(x0 - y, y0 + x, color); (e->*dp)(x0 - x, y0 + y, color); }
        if (corner & 0x1) { (e->*dp)(x0 - y, y0 - x, color); (e->*dp)(x0 - x, y0 - y, color); }
    }
}

void EPaper::drawRoundRect(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r, uint8_t color) {
    int16_t max_r = ((w < h) ? w : h) / 2; if (r > max_r) r = max_r;
    drawLine(x + r, y, x + w - r - 1, y, color);             // top
    drawLine(x + r, y + h - 1, x + w - r - 1, y + h - 1, color); // bottom
    drawLine(x, y + r, x, y + h - r - 1, color);             // left
    drawLine(x + w - 1, y + r, x + w - 1, y + h - r - 1, color); // right
    circleHelper(this, x + r,         y + r,         r, 1, color, &EPaper::drawPixel);
    circleHelper(this, x + w - r - 1, y + r,         r, 2, color, &EPaper::drawPixel);
    circleHelper(this, x + w - r - 1, y + h - r - 1, r, 4, color, &EPaper::drawPixel);
    circleHelper(this, x + r,         y + h - r - 1, r, 8, color, &EPaper::drawPixel);
}

// Adafruit-style filled quarter-circle helper (vertical spans). corners: 1=right 2=left.
static void fillCircleHelper(EPaper* e, int16_t x0, int16_t y0, int16_t r, uint8_t corners,
                             int16_t delta, uint8_t color) {
    int16_t f = 1 - r, ddF_x = 1, ddF_y = -2 * r, x = 0, y = r, px = x, py = y;
    delta++;
    while (x < y) {
        if (f >= 0) { y--; ddF_y += 2; f += ddF_y; }
        x++; ddF_x += 2; f += ddF_x;
        if (x < (y + 1)) {
            if (corners & 1) e->fillRect(x0 + x, y0 - y, 1, 2 * y + delta, color);
            if (corners & 2) e->fillRect(x0 - x, y0 - y, 1, 2 * y + delta, color);
        }
        if (y != py) {
            if (corners & 1) e->fillRect(x0 + py, y0 - px, 1, 2 * px + delta, color);
            if (corners & 2) e->fillRect(x0 - py, y0 - px, 1, 2 * px + delta, color);
            py = y;
        }
        px = x;
    }
}

void EPaper::fillRoundRect(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r, uint8_t color) {
    int16_t max_r = ((w < h) ? w : h) / 2; if (r > max_r) r = max_r;
    fillRect(x + r, y, w - 2 * r, h, color);
    fillCircleHelper(this, x + w - r - 1, y + r, r, 1, h - 2 * r - 1, color);
    fillCircleHelper(this, x + r,         y + r, r, 2, h - 2 * r - 1, color);
}

void EPaper::fillCircle(int16_t x0, int16_t y0, int16_t r, uint8_t color) {
    fillRect(x0, y0 - r, 1, 2 * r + 1, color);
    fillCircleHelper(this, x0, y0, r, 3, 0, color);
}

void EPaper::fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint8_t color) {
    if (w <= 0 || h <= 0) return;          // see drawRect
    // Clipped to the panel, then filled row by row: whole bytes in one store, the partial bytes at
    // either end through a mask. The same pixels as one drawLine per column through drawPixel(),
    // checked bit for bit on the host over ~98 000 rectangles, clipped and not, at every byte edge.
    int x0 = x, y0 = y, x1 = (int)x + w, y1 = (int)y + h;     // half-open
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > EPD_WIDTH)  x1 = EPD_WIDTH;
    if (y1 > EPD_HEIGHT) y1 = EPD_HEIGHT;
    if (x0 >= x1 || y0 >= y1) return;
    const int b0 = x0 / 8, b1 = (x1 - 1) / 8;                  // first and last byte touched
    const uint8_t m0 = (uint8_t)(0xFF >> (x0 % 8));            // x0 .. end of byte b0
    const uint8_t m1 = (uint8_t)(0xFF << (7 - (x1 - 1) % 8));  // start of byte b1 .. x1-1
    const uint8_t fill = color ? 0xFF : 0x00;
    for (int row = y0; row < y1; row++) {
        uint8_t *p = frame_buffer + row * (EPD_WIDTH / 8);
        if (b0 == b1) {
            const uint8_t m = m0 & m1;
            p[b0] = color ? (uint8_t)(p[b0] | m) : (uint8_t)(p[b0] & ~m);
        } else {
            p[b0] = color ? (uint8_t)(p[b0] | m0) : (uint8_t)(p[b0] & ~m0);
            if (b1 > b0 + 1) memset(p + b0 + 1, fill, (size_t)(b1 - b0 - 1));
            p[b1] = color ? (uint8_t)(p[b1] | m1) : (uint8_t)(p[b1] & ~m1);
        }
    }
}

void EPaper::fillRectDither(int16_t x, int16_t y, int16_t w, int16_t h, uint8_t level, uint8_t color) {
    if (level == 0) return;
    if (level >= 4) { fillRect(x, y, w, h, color); return; }
    for (int16_t j = 0; j < h; j++) {
        for (int16_t i = 0; i < w; i++) {
            int16_t px = x + i, py = y + j;
            bool on;
            if (level == 1)      on = ((px & 1) == 0 && (py & 1) == 0);   // ~25%
            else if (level == 2) on = (((px + py) & 1) == 0);            // ~50% checker
            else                 on = !((px & 1) == 0 && (py & 1) == 0);  // ~75%
            if (on) drawPixel(px, py, color);
        }
    }
}

void EPaper::drawBitmap(int16_t x, int16_t y, const uint8_t *bitmap, int16_t w, int16_t h, uint8_t color) {
    int16_t byteWidth = (w + 7) / 8;
    for (int16_t j = 0; j < h; j++) {
        for (int16_t i = 0; i < w; i++) {
            if (bitmap[j * byteWidth + i / 8] & (128 >> (i & 7))) {
                drawPixel(x + i, y + j, color);
            }
        }
    }
}

// ================================================================================================
// PANEL CONTROL. Everything here is a thin call into epd_vendor.cpp; no command bytes live in this
// file any more. If a frame comes out wrong, the question is what we ask the vendor's code to do,
// not what we send the panel.
// ================================================================================================

// The hardware reset every vendor sequence opens with also ends whatever went wrong in the last one.
void EPaper::rstLow()  { busy_stuck = false; spi_failed = false; gpio_set_level(EPD_RST, 0); }
void EPaper::rstHigh() { gpio_set_level(EPD_RST, 1); }

void EPaper::writeBaseline() {
    if (hibernating) powerUp();
    // Open the vendor's partial sequence early — reset, border, update control — so that the
    // reference we are about to load sits AFTER the hardware reset. Loading it before would put
    // it in front of the reset that starts every partial, where it does not survive.
    EPD_Dis_PartAll_Begin();
    writeCommand(0x24);
    writeImageRows(frame_buffer);
    writeCommand(0x26);
    writeImageRows(frame_buffer);
    part_armed = true;
}

void EPaper::refresh(Update how) {
    if (hibernating) powerUp();

    if (how == Update::Fast) {
        // A complete sequence of its own — its LUT is loaded inside the init, which is why the
        // two halves may never be separated. Both RAMs are written, same as the full path.
        part_armed = false;
        EPD_HW_Init_Fast();
        EPD_WhiteScreen_ALL_Fast(frame_buffer);
        return;
    }

    if (how == Update::Full) {
        part_armed = false;
        // "Re-initialization is required for every full screen update" — the vendor's own tips,
        // point 5. EPD_WhiteScreen_ALL then writes the image to BOTH RAMs, so the partial that
        // follows has a reference matching the glass without anyone reconstructing anything.
        EPD_HW_Init();
        EPD_WhiteScreen_ALL(frame_buffer);
        return;
    }

    if (part_armed) {
        // writeBaseline() already ran the first half of EPD_Dis_PartAll; finish it.
        part_armed = false;
        EPD_Dis_PartAll_Image(frame_buffer);
    } else {
        EPD_Dis_PartAll(frame_buffer);
    }
}

void EPaper::display4Gray(const uint8_t *plane24, const uint8_t *plane26) {
    if (hibernating) powerUp();
    part_armed = false;

    // Clear to white FIRST, with an ordinary full refresh, and only then paint the grayscale.
    //
    // The grayscale waveform has no erase phase — it drives towards four levels from wherever the
    // ink already is. Without this the previous picture stays underneath, and on a mostly white
    // logo what shows through is the dashboard's own outer frame, drawn at 0,0,400,300 every
    // minute for months: a black ring around the glass with a clean white middle.
    //
    // This existed and was lost. The old driver did it (4.19 recorded why, in these words: the
    // grayscale LUT has no erase phase, and without it the logo printed OVER the dashboard), and
    // it did not survive transcribing the vendor's thirty-five commands in 4.71. Their demo loop
    // also opens with a white screen — EPD_HW_Init(); EPD_WhiteScreen_White(); — before its first
    // image, though it does not repeat the clear before each one.
    //
    // It matters most exactly where it was noticed. These screens are the ones the device keeps
    // for months with the supply cut, and the vendor asks for a WHITE picture in storage: a dark
    // ring baked in around the edge is the one thing they warn about.
    EPD_HW_Init();
    EPD_WhiteScreen_White();
    // The vendor demo between two frames (GDEY042T81_Arduino.ino, loop()): the sleep command, two seconds,
    // then the next initialisation, whose hardware reset brings the controller out of deep sleep.
    EPD_DeepSleep();
    wait_ms(2000);

    EPD_HW_Init_4G();
    EPD_WhiteScreen_ALL_4G(plane24, plane26);
}

void EPaper::display4GrayRle(const uint8_t *rle24, size_t n24, const uint8_t *rle26, size_t n26) {
    if (hibernating) powerUp();
    part_armed = false;

    // Same opening as display4Gray, and for the same reason recorded there: the grayscale waveform
    // has no erase phase, so the glass is cleared white first.
    EPD_HW_Init();
    EPD_WhiteScreen_White();
    EPD_DeepSleep();
    wait_ms(2000);
    EPD_HW_Init_4G();

    // One plane at a time through the frame buffer. A stream that does not expand to exactly a
    // plane means the array and its length disagree — a build fault, not a smudged picture — so it
    // is said plainly and nothing is sent.
    if (!frame_buffer) { ESP_LOGE(TAG, "4-gray: no frame buffer"); return; }
    if (!rle_expand(frame_buffer, EPD_BUFFER_SIZE, rle24, n24)) {
        ESP_LOGE(TAG, "4-gray: plane24 stream does not expand to %d bytes", EPD_BUFFER_SIZE); return;
    }
    writeCommand(0x24);
    writeImageRows(frame_buffer);
    if (!rle_expand(frame_buffer, EPD_BUFFER_SIZE, rle26, n26)) {
        ESP_LOGE(TAG, "4-gray: plane26 stream does not expand to %d bytes", EPD_BUFFER_SIZE); return;
    }
    writeCommand(0x26);
    writeImageRows(frame_buffer);
    EPD_Update_4G();
}

void EPaper::hibernate() {
    // Nothing to put to sleep if the driver never came up, and the SPI handle would be null.
    // Reachable: the frozen-ink guard returns from draw_dashboard before init() is ever called.
    if (!initialized) return;
    part_armed = false;
    // A command of its own: an SPI error earlier in the frame does not stop the sleep command being tried.
    // A controller still reporting busy cannot take it (linkUsable), and that is said.
    spi_failed = false;
    if (busy_stuck && gpio_get_level(EPD_BUSY) == 1)
        ESP_LOGE(TAG, "controller still busy — the sleep command cannot be sent");
    EPD_DeepSleep();
    hibernating = true;
    initialized = false;
}

void EPaper::powerDownAndIsolate() {
    hibernate();
    
    // Turn E-Paper power OFF (Active LOW PMOS -> set HIGH)
    gpio_set_level(EPD_PWR_EN, 1);
    
    // Isolate the signal lines. Direction alone is not isolation: pull-up and pull-down live in a
    // different register, and init() enables a pull-up on BUSY, so setting it to INPUT left ~3.3 V
    // feeding an unpowered panel through 45k. This project measured what that does -- mode 9 in the
    // panel bench, a rail held in half-power through a 45k pull-up, greyed the glass slowly and drew
    // tens of microamps (4.19). The production sleep path already floats every line explicitly;
    // this one is used only by the bench, which is exactly why it has to match. An instrument that
    // isolates differently from the thing it measures gives answers about itself.
    const gpio_num_t sig[] = { EPD_MOSI, EPD_SCK, EPD_CS, EPD_DC, EPD_RST, EPD_BUSY };
    for (gpio_num_t p : sig) {
        gpio_set_direction(p, GPIO_MODE_INPUT);
        gpio_set_pull_mode(p, GPIO_FLOATING);
    }
}

// In deep sleep the digital pads stop being driven unless explicitly held. EPD_PWR_EN
// drives a P-FET gate: a floating gate is NOT "off" — it can settle in the linear
// region and burn current. Holding it LOW keeps the panel powered through the sleep,
// which is what lets the controller keep its RAM (the partial-refresh baseline).
// EPD_RST is held HIGH so a floating reset line can't wake/reset the sleeping
// controller and wipe that baseline. C6 supports single-pad hold in deep sleep
// (SOC_GPIO_SUPPORT_HOLD_SINGLE_IO_IN_DSLP).
// NB: no gpio_deep_sleep_hold_en() here — IDF only declares it for chips that CANNOT
// hold an individual pad in deep sleep (ESP32/S2/C3/S3/C2). The C6 can
// (SOC_GPIO_SUPPORT_HOLD_SINGLE_IO_IN_DSLP=1), so gpio_hold_en alone is the whole job.
void EPaper::holdForDeepSleep() {
    // The panel keeps its supply through the sleep. Measured, repeatedly: cutting it while the
    // chip is AWAKE does nothing to the image, but cutting it and then entering deep sleep greys
    // the glass immediately — regardless of whether the last refresh was partial or full. So the
    // supply stays, and every line to the panel is latched at the level it has right now.
    //
    // ALL of them, not just the power pin. In deep sleep an unheld pad stops being driven and
    // floats; floating clock and chip-select at a live controller is not a state anyone designed
    // for, and a floating reset can wipe its RAM. Holding two pins and leaving five to chance was
    // half a measure.
    // The gate is DRIVEN low here, not merely latched at whatever it happened to be. gpio_hold_en
    // freezes the pad in its present state, so latching alone assumes someone already put it there
    // — and if init() failed earlier in the cycle the pad may never have been made an output at
    // all. A latched floating gate is the worst of the three possible states: not on, not off, and
    // quite possibly sitting in the transistor's linear region for the whole sleep.
    gpio_set_direction(EPD_PWR_EN, GPIO_MODE_OUTPUT);
    gpio_set_level(EPD_PWR_EN, 0);   // P-FET gate LOW = panel powered, under every circumstance
    gpio_hold_en(EPD_PWR_EN);
    gpio_hold_en(EPD_RST);      // idle HIGH: a floating reset must not wipe the RAM
    gpio_hold_en(EPD_CS);       // idle HIGH: no transfer is in progress
    gpio_hold_en(EPD_SCK);
    gpio_hold_en(EPD_MOSI);
    gpio_hold_en(EPD_DC);
}

// The opposite case: the panel is to stay OFF through the sleep (a static screen we never mean
// to update again — logo, low battery). Cutting power is not enough on its own. In deep sleep an
// undriven pad stops being driven, and the P-FET gate then floats: not "off", but somewhere in
// the linear region, trickling current into the panel. On hardware that showed as the logo
// drawn cleanly and then washing out to grey while the device slept. Latch the gate HIGH so the
// FET is genuinely off for the whole sleep.
void EPaper::holdOffForDeepSleep() {
    // Used ONLY when the device is parked — a final picture and a sleep it does not mean to come
    // out of on its own: the logo after a factory reset, the low-battery notice. The running cycle
    // keeps the supply through every sleep; see PanelSleep in bench_screens.cpp for why.
    //
    // Cutting the supply is a sanctioned step: the panel datasheet's own reference sequence ends
    // with 0x10/0x01 followed by POWER OFF. What this board lacks is the other half of it. There is
    // no bleed resistor and no quick output discharge on 3.3V_ePaper, so once Q4 opens, the ~5-6 uF
    // on the rail drains only through the sleeping panel's own 2-5 uA: about two seconds to fall,
    // longer to finish, spent in the region where the controller is neither running nor off. Once is
    // harmless — that is what pulling the battery does, and the image survives it. Fourteen hundred
    // times a day is not, and one night of it greyed the background.
    //
    // For this device the switch is not worth having at all: it saves about a microamp out of the
    // three hundred and thirty this board actually draws. Two references for exactly our job agree —
    // GxEPD2 has no concept of a panel power switch, and Watchy redraws e-paper every sixty seconds
    // on a battery without one.
    //
    // Lines isolated FIRST, supply cut SECOND — never the other way round. Between the two steps the
    // panel is dead while CS/RST are still driven high, which is phantom feed through its ESD diodes.
    // The window is tens of microseconds against seconds of sag, so it was never the main cause, but
    // the order was wrong and costs nothing to get right.
    //
    // This is what the controller's datasheet prescribes, and it took a day of bisection to arrive
    // at it. The earlier code did the opposite: it drove all six lines to ground, on the theory
    // that floating pins would feed the unpowered panel through its protection diodes. The theory
    // had the sign backwards. Grounding every line left the panel's rail sagging slowly through
    // the region where its controller is still alive but no longer sane, and in that state it
    // disturbs the pixels — the image drifted grey within one sleep.
    //
    // The evidence, from panel_bench.cpp (one variable per run, confirmed in both directions):
    //   all lines grounded ................. greys
    //   any single line grounded ........... clean   (something was still holding the rail up)
    //   all grounded except reset .......... clean   (one high line was enough to hold it)
    //   reset via a 45k pull-up ............ greys SLOWLY, exactly like a slowly sagging rail
    //   every line high-Z .................. CLEAN   <- this, and it is what the vendor says
    //
    // High impedance is better than holding one line high: nothing feeds the panel, so it is
    // genuinely off rather than half-powered, and its rail collapses quickly instead of lingering.
    // e-ink holds a completed image at zero power — that is what shelf labels do for years — so a
    // panel that is properly off keeps its picture, and one that is half-powered does not.
    const gpio_num_t sig[] = { EPD_MOSI, EPD_SCK, EPD_CS, EPD_DC, EPD_RST, EPD_BUSY };
    for (gpio_num_t p : sig) {
        gpio_hold_dis(p);               // an old latch would quietly override everything below
        gpio_set_direction(p, GPIO_MODE_INPUT);
        gpio_set_pull_mode(p, GPIO_FLOATING);
    }
    // Deliberately not latched: a hold on an input pad is about its pull, and no pull is the point.
    // An undriven digital pad is high-Z in deep sleep anyway, which is exactly what is wanted.

    // Only now, with nothing driven towards it, does the panel lose its rail.
    gpio_set_direction(EPD_PWR_EN, GPIO_MODE_OUTPUT);
    gpio_set_level(EPD_PWR_EN, 1);      // P-FET gate HIGH = panel unpowered
    gpio_hold_en(EPD_PWR_EN);           // the gate is the one pad that must NOT float
}

// The PM_SLP_DISABLE_GPIO help text names the remedy for pads that must keep working through an
// automatic light sleep: gpio_sleep_sel_dis() on each of them (esp_pm/Kconfig).
void EPaper::keepPadsThroughLightSleep() {
    static const gpio_num_t pads[] = { EPD_MOSI, EPD_SCK, EPD_CS, EPD_DC, EPD_RST, EPD_BUSY, EPD_PWR_EN };
    for (gpio_num_t p : pads) gpio_sleep_sel_dis(p);
}

// Order matters and is a trap. A held pad ignores writes, and gpio_hold_dis() drops the
// pad back to its DEFAULT (input) state — which would float the P-FET gate at the exact
// moment we are trying to keep the panel alive. So restore the levels we want first,
// then release, so the pad never passes through an undriven state.
void EPaper::releaseDeepSleepHold(bool repower) {
    gpio_config_t io = {};
    io.intr_type = GPIO_INTR_DISABLE;
    io.mode = GPIO_MODE_OUTPUT;
    io.pin_bit_mask = (1ULL << EPD_PWR_EN);
    io.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io.pull_up_en = GPIO_PULLUP_DISABLE;
    gpio_config(&io);
    // The level comes first and the release second, in both directions, for the reason above.
    gpio_set_level(EPD_PWR_EN, repower ? 0 : 1);
    gpio_hold_dis(EPD_PWR_EN);

    // Every latch needs its release whichever way this goes: a held pad silently ignores writes,
    // so leaving one on means the panel never responds again while the driver still reports
    // success. holdForDeepSleep latches all six; holdOffForDeepSleep latches only the gate, and
    // releasing a pad that was never held is free.
    const gpio_num_t sig[] = { EPD_MOSI, EPD_SCK, EPD_CS, EPD_DC, EPD_RST, EPD_BUSY };
    // gpio_hold_dis() (driver/gpio.h): after a deep-sleep wake a released pad falls back to its default,
    // input, mode; a pad that must keep its level is configured as an output at that level BEFORE the
    // release. Reset idles high (init()'s reset() pulses it later) and chip select idles high.
    if (repower) {
        gpio_set_direction(EPD_RST, GPIO_MODE_OUTPUT);
        gpio_set_level(EPD_RST, 1);
        gpio_set_direction(EPD_CS, GPIO_MODE_OUTPUT);
        gpio_set_level(EPD_CS, 1);
    }
    for (gpio_num_t p : sig) gpio_hold_dis(p);

    if (repower) {
        gpio_set_pull_mode(EPD_BUSY, GPIO_FLOATING);
        return;
    }

    // Staying off. Drive NOTHING towards an unpowered panel — a high reset or chip select feeds it
    // through its ESD diodes, which is the one thing the datasheet's "no input load" forbids. The
    // pads are already inputs after the release; make the absence of pulls explicit, because
    // direction is not isolation and BUSY carries a pull-up from init().
    for (gpio_num_t p : sig) {
        gpio_set_direction(p, GPIO_MODE_INPUT);
        gpio_set_pull_mode(p, GPIO_FLOATING);
    }
}

void EPaper::powerUp() {
    if (!hibernating) return;
    
    // MOSI, SCK and CS belong to the SPI driver and are left alone: gpio_set_direction() to output routes
    // a pad back to plain GPIO (gpio_output_enable in gpio.c), after which SPI no longer reaches the panel.
    gpio_set_direction(EPD_DC, GPIO_MODE_OUTPUT);
    gpio_set_direction(EPD_RST, GPIO_MODE_OUTPUT);
    gpio_set_direction(EPD_BUSY, GPIO_MODE_INPUT);
    
    // Turn E-Paper power ON (Active LOW PMOS -> set LOW)
    gpio_set_level(EPD_PWR_EN, 0);
    wait_ms(10);
    
    // No reset and no register setup: whichever vendor entry point runs next opens with its own
    // hardware reset, which is also the documented way out of the controller's deep sleep.
    hibernating = false;
    initialized = true;
}
