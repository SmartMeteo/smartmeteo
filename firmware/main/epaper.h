#pragma once

#include <stdint.h>
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "gfxfont.h"

#define EPD_WIDTH       400
#define EPD_HEIGHT      300
#define EPD_BUFFER_SIZE (EPD_WIDTH * EPD_HEIGHT / 8)

class EPaper {
public:
    EPaper();
    ~EPaper();

    bool init();
    void clear(uint8_t value = 0xFF);

    
    // Graphics primitives
    void drawPixel(int16_t x, int16_t y, uint8_t color);
    void drawChar(int16_t x, int16_t y, char c, uint8_t color, uint8_t size);
    uint16_t drawString(int16_t x, int16_t y, const char* str, uint8_t color, uint8_t size); // returns end x

    // GFXfont (Adafruit-format) rendering for large digits (Org_01). (x, y) is the cursor
    // baseline; the glyph is placed at its (xOffset, yOffset) from there, scaled by `size`.
    // Mirrors the mockup's Oc/O/Ow (render_states.py) 1:1 so layouts port pixel-for-pixel.
    uint16_t drawGFXChar(int16_t x, int16_t y, char c, const GFXfont* font, uint8_t color, uint8_t size);
    uint16_t drawGFXString(int16_t x, int16_t y, const char* str, const GFXfont* font, uint8_t color, uint8_t size); // returns end x
    uint16_t gfxStrWidth(const char* str, const GFXfont* font, uint8_t size);
    void drawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint8_t color);
    void drawRect(int16_t x, int16_t y, int16_t w, int16_t h, uint8_t color);
    void drawRoundRect(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r, uint8_t color);
    void fillRoundRect(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r, uint8_t color);
    void fillCircle(int16_t x0, int16_t y0, int16_t r, uint8_t color);
    void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint8_t color);
    // Dithered fill = "gray" in pure 1-bit (density of dots), so it survives partial refresh
    // unlike true 4-gray. level: 0=none 1=~25% 2=~50% checker 3=~75% 4=solid. Pattern is keyed
    // to absolute x,y so adjacent fills tile seamlessly and redraws stay stable.
    void fillRectDither(int16_t x, int16_t y, int16_t w, int16_t h, uint8_t level, uint8_t color);
    void drawBitmap(int16_t x, int16_t y, const uint8_t *bitmap, int16_t w, int16_t h, uint8_t color);

    // Refresh. Every path is Good Display's own sequence — see epd_vendor.cpp, which is a
    // transliteration of their Display_EPD_W21.cpp and the only place panel commands are written.
    // Three ways to put the buffer on the glass, named rather than encoded in a bool:
    //   Full    — EPD_HW_Init() + EPD_WhiteScreen_ALL(). Several flickers, the thorough clear.
    //             Power-on and the rare deep clean; the vendor's own advice is to keep it for that.
    //   Fast    — EPD_HW_Init_Fast() + EPD_WhiteScreen_ALL_Fast(). 1.5 s, ONE flicker, and the
    //             vendor's recommended replacement for a routine cleaning frame. Writes both RAMs,
    //             so the next partial has a true reference for free.
    //   Partial — EPD_Dis_PartAll(): hardware reset, border, image to 0x24, 0xFF.
    enum class Update { Full, Fast, Partial };
    void refresh(Update how = Update::Full);

    // 4-level grayscale full refresh, for static event screens only (splash, lockout, setup QR):
    // the grayscale waveform cannot run as a partial. Takes two pre-packed 1-bit planes (15000 B
    // each), packed on the host by Software/mockups/gen_4gray.py in the vendor's own mapping:
    //   plane24/plane26 bits = {black:0,0  darkgray:1,0  lightgray:0,1  white:1,1}.
    // Drives EPD_HW_Init_4G() + EPD_WhiteScreen_ALL_4G(); the waveform comes from the panel's OTP,
    // selected by the vendor's temperature write, not from a table we carry.
    void display4Gray(const uint8_t *plane24, const uint8_t *plane26);
    // The same, from planes that ship run-length coded (rle.h). Each is expanded into the frame
    // buffer just before it goes out, so no 15 KB has to be found anywhere: the buffer is free here
    // because every screen drawn afterwards starts by clearing it (drawMainScreen, drawPairingScreen).
    // That matters most for LOW BATTERY — the one picture drawn on a device that may have nothing
    // left to allocate.
    void display4GrayRle(const uint8_t *rle24, size_t n24, const uint8_t *rle26, size_t n26);

    // Load the frame currently in the buffer as the PREVIOUS image, without displaying it.
    //
    // This is the one thing we do that the vendor's driver does not, and it is forced by our
    // architecture rather than chosen. Their demo runs a whole series of partial updates inside a
    // single waking session — base map once, then EPD_Dis_PartAll repeatedly — and sleeps only at
    // the end. We deep-sleep between every frame, so the reference RAM cannot be assumed intact by
    // the time the next partial runs; and their own way of restoring it ends in a full refresh,
    // which would flash the whole screen every minute.
    // Call order is therefore: render the PREVIOUS frame -> writeBaseline() -> render the new
    // frame -> refresh(true). The write lands inside the vendor's partial sequence, straight after
    // its hardware reset, because anywhere earlier the reset would wipe it.
    // BORDER WAVEFORM UNDER TEST — default is the vendor's own value and must stay that way.
    //
    // Their partial writes 0x3C = 0x80, driving the border ring to VCOM, and every init writes
    // 0x05. GxEPD2, on this same panel and clean for months, writes 0x3C = 0x01 once in init and
    // never touches it again. Applied every minute for months, 0x80 is the leading explanation for
    // the border going permanently dark — visible on the white logo screen, and not removable by a
    // full white refresh because the ring is outside the pixel array.
    //
    // Set false and the partial leaves 0x3C alone, which is GxEPD2's arrangement. Only the border
    // bench flips it; the shipping cycle stays on the vendor's value until an experiment says
    // otherwise. This is the one variable of that experiment, not an improvement to their file.
    // Value written to 0x3C at the start of every partial, or -1 to write nothing at all.
    // 0x80 is the vendor's. 0x01 is GxEPD2's. 0xC0 is what our own driver used until
    // September, and July's bench recorded it as keeping the edge white where 0x80 darkened
    // it. Only the border bench changes this; shipping stays on the vendor's value.
    static int part_border;
    // Same, for the value every INIT writes. The vendor writes 0x05; GxEPD2 writes 0x01.
    // The border was grey on the very first full refresh, before any partial ran, which is
    // what moved the question from the partial's value to this one.
    static int init_border;
    // The value the FOUR-GRAY init writes, which is a separate question from the one above and has
    // a different answer: production writes GRAY_BORDER_HIZ there, not the vendor's 0x05, because
    // the vendor's value prints a black ring outside the image and HiZ does not. The reasoning and
    // the measurements are at the 0x3C write in EPD_HW_Init_4G (epd_vendor.cpp) — read that before
    // touching either value, because the cure is the pair of them and not this one alone.
    // -1 means "production value"; only the border bench overrides it.
    static int gray_border;
    // 0xC0: A[7:6] = 11, border floating. Also the register's power-on value (SSD1683 Rev 1.0 p. 34).
    static constexpr int GRAY_BORDER_HIZ = 0xC0;

    void writeBaseline();

    // Is the panel up RIGHT NOW — this wake brought it up, or it was never taken down.
    // init() powers the rail as a side effect, so this is the only honest answer to
    // "does the controller have a supply to hear a command with".
    bool isUp() const { return initialized; }
    // The sleep command has gone out this wake and nothing has woken the controller since.
    bool asleep() const { return hibernating; }
    // Did the frame just sent arrive whole: no SPI error, and BUSY fell within its wait. Both clear at
    // the hardware reset every vendor sequence begins with, so read this straight after a refresh.
    bool sessionOk() const { return !spi_failed && !busy_stuck; }

    // End the session. The vendor calls this after every refresh and is emphatic about it:
    // "always leave the sleep mode command. Otherwise, this may result in a reduced lifespan."
    void hibernate();
    void powerDownAndIsolate();
    void powerUp();

    // Deep sleep: keep the panel POWERED (P-FET stays on) so the controller's RAM — our
    // partial-refresh baseline — survives. Call hibernate() first, then this.
    static void holdForDeepSleep();
    // Latch the panel's power FET OFF for the whole deep sleep. Use when the screen shown is
    // final for this sleep (logo, low battery): an unheld gate floats and slowly greys the image.
    static void holdOffForDeepSleep();
    // Call on wake BEFORE init() reconfigures the pads. `repower` says which way the last sleep
    // left the switch: a wake that is not going to draw must not turn the panel back on, or a
    // device parked with a static picture would cross the half-powered region once per wake
    // instead of once per park.
    static void releaseDeepSleepHold(bool repower = true);
    // Automatic light sleep (Matter mode only) isolates every pad for as long as it lasts
    // (ESP_SLEEP_GPIO_RESET_WORKAROUND -> PM_SLP_DISABLE_GPIO). The power gate, RST and CS must not
    // float between frames, so all seven lines keep their working configuration through it.
    static void keepPadsThroughLightSleep();

private:
    // Ours: the four primitives the vendor's code sits on, in ESP-IDF terms.
    void writeCommand(uint8_t cmd);
    void writeData(const uint8_t *data, size_t len);
    void writeDataByte(uint8_t val);
    void waitBusy(uint32_t max_delay_ms = 2000);
    void reset();
    void rstLow();
    void rstHigh();
    bool linkUsable();

    // Good Display's, verbatim — every one of these lives in epd_vendor.cpp and must stay
    // diffable against Software/vendor_reference_GDEY042T81/.../Display_EPD_W21.cpp.
    void EPD_HW_Init();
    void EPD_HW_Init_Fast();
    void EPD_HW_Init_4G();
    void EPD_Update();
    void EPD_Update_Fast();
    void EPD_Update_4G();
    void EPD_Part_Update();
    void writeImageRows(const uint8_t *img);    // our row order -> their Y-decrement addressing
    void EPD_WhiteScreen_ALL(const uint8_t *datas);
    void EPD_WhiteScreen_ALL_Fast(const uint8_t *datas);
    void EPD_WhiteScreen_White();
    void EPD_Dis_PartAll(const uint8_t *datas);
    void EPD_Dis_PartAll_Begin();               // split only so our 0x26 write fits inside it
    void EPD_Dis_PartAll_Image(const uint8_t *datas);
    void EPD_DeepSleep();
    void EPD_WhiteScreen_ALL_4G(const uint8_t *plane24, const uint8_t *plane26);

    spi_device_handle_t spi_handle;
    uint8_t *frame_buffer;
    bool initialized;
    bool part_armed;      // EPD_Dis_PartAll_Begin() has run; the image half still owes a call
    bool hibernating;
    bool busy_stuck;      // BUSY did not fall within waitBusy()'s limit
    bool spi_failed;      // the SPI driver reported an error during this sequence
};
