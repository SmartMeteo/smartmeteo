// Good Display's own driver for GDEY042T81, transliterated and nothing else.
//
// SOURCE OF TRUTH: Software/vendor_reference_GDEY042T81/GDEY042T81_Arduino/Display_EPD_W21.cpp
// Every command byte, every argument, every ordering and every delay below is copied from that
// file. The function names are kept in the vendor's C style ON PURPOSE, so this file can be read
// side by side with theirs and any divergence shows up as a diff rather than as an argument.
//
// WHY THIS FILE EXISTS. Between July and September we accumulated our own panel sequences, each
// one a reasonable-looking patch on top of the last: a border register written before every
// partial, a substituted temperature, a hand-copied 233-byte waveform from a DIFFERENT vendor's
// demo, a fast update code taken from a third source. Six explanations of the greying were
// written down with confidence and six were later shown wrong (4.19, 4.29, 4.42, 4.50, 4.67,
// 4.68). The last patch of all — a fast waveform activated without the initialisation it belongs
// to — inverted the screen. So the local sequences are gone and this is what drives the panel now.
//
// THE RULE FOR ANYONE EDITING THIS FILE: don't. Improvements go somewhere else. If the panel
// misbehaves, the first question is "where do we differ from Display_EPD_W21.cpp", and that
// question is only cheap to answer while the answer is "nowhere".
//
// WHAT IS OURS AND WHY (each forced by hardware, none by taste; the row order below is the fourth):
//   1. The four primitives — SPI, DC/RST/BUSY pins, delays — are ESP-IDF instead of Arduino.
//      Unavoidable; they carry no protocol decisions.
//   2. Bulk pixel writes go through a small RAM bounce buffer in 1 KB chunks (see writeData).
//      The vendor writes byte at a time from flash, which we cannot do: our DMA cannot read
//      flash, and a 15 KB transaction silently does nothing when the heap is short — measured,
//      4.19. Same bytes on the wire, in the same order.
//   3. waitBusy() waits for BUSY to RISE before waiting for it to fall; the vendor spins only on
//      the fall. On this board the controller takes a few ms to raise it, so the bare spin
//      returns immediately and the caller believes a 2-second waveform already finished — then
//      cuts the supply mid-frame (measured: a 4-gray frame "completed" in 251 ms, 4.7). Ours is
//      a strict superset: it waits for the same falling edge, just not before there is one.
//   The primitives also send nothing while BUSY is still high after a timed-out wait, or after an
//   SPI error, until the next hardware reset (epaper.cpp, linkUsable). The vendor's READBUSY never
//   times out, so it never sends in that state either.
#include "epaper.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

// EPD_ARRAY in the vendor file: EPD_WIDTH * EPD_HEIGHT / 8 = 15000 for this panel.
#define EPD_ARRAY   EPD_BUFFER_SIZE

// delay_xms()
// The vendor's delays are minimums ("delay_xms(10);//At least 10ms delay"). At this build's 100 Hz tick
// vTaskDelay(pdMS_TO_TICKS(ms)) can return after almost nothing: it rounds down, and the first tick of a
// delay is partial. So round up to whole ticks and add one.
static inline void delay_xms(unsigned int ms) { vTaskDelay(pdMS_TO_TICKS(ms + portTICK_PERIOD_MS - 1) + 1); }

// EPD_W21_RST_0 / EPD_W21_RST_1 are the two halves of the vendor's reset; it appears inline in
// several of their functions rather than as a helper, and is reproduced there the same way.

// ------------------------------------------------------------------------------------------
// Row order. THE FOURTH DEVIATION, and it is on our side of the wire, not theirs.
//
// Their init sets data entry mode 0x11 = 0x01 — Y DECREMENT — and starts the Y counter at
// EPD_HEIGHT-1, so a linear stream fills the panel from the BOTTOM row upwards. Their image
// converter emits arrays in that order, so their demo is self-consistent. Ours is self-consistent
// the other way: drawPixel() puts row 0 at the top, which is what every other part of this
// firmware assumes, from screen.cpp to the 4-gray planes baked on the host.
//
// Streaming our buffer straight into their addressing lands our top row on their bottom one, which
// is a vertical flip — the screen came back upside down the first time this ran on hardware.
//
// So the adaptation lives here, in the streaming, and their register writes stay untouched. The
// alternative was to change 0x11 and 0x4F to our convention, which would have meant editing the
// vendor sequence within a day of adopting it, for a difference that is purely about which end of
// our own buffer we start from.
static const size_t EPD_ROW_BYTES = EPD_WIDTH / 8;   // 50

void EPaper::writeImageRows(const uint8_t *img)
{
    // Last row first: it is the one their counter writes to Y = EPD_HEIGHT-1, the bottom.
    for (int y = EPD_HEIGHT - 1; y >= 0; --y)
        writeData(img + (size_t)y * EPD_ROW_BYTES, EPD_ROW_BYTES);
}

// ------------------------------------------------------------------------------------------
// Full screen refresh initialization  (vendor: EPD_HW_Init)
// ------------------------------------------------------------------------------------------
void EPaper::EPD_HW_Init()
{
    rstLow();
    delay_xms(10);                  // At least 10ms delay
    rstHigh();
    delay_xms(10);                  // At least 10ms delay

    waitBusy();
    writeCommand(0x12);             // SWRESET
    waitBusy();

    writeCommand(0x01);             // Driver output control
    writeDataByte((EPD_HEIGHT - 1) % 256);
    writeDataByte((EPD_HEIGHT - 1) / 256);
    writeDataByte(0x00);

    writeCommand(0x21);             // Display update control
    writeDataByte(0x40);
    writeDataByte(0x00);

    writeCommand(0x3C);             // BorderWavefrom (vendor 0x05)
    writeDataByte((uint8_t)init_border);

    writeCommand(0x11);             // data entry mode
    writeDataByte(0x01);

    writeCommand(0x44);             // set Ram-X address start/end position
    writeDataByte(0x00);
    writeDataByte(EPD_WIDTH / 8 - 1);
    writeCommand(0x45);             // set Ram-Y address start/end position
    writeDataByte((EPD_HEIGHT - 1) % 256);
    writeDataByte((EPD_HEIGHT - 1) / 256);
    writeDataByte(0x00);
    writeDataByte(0x00);

    writeCommand(0x4E);             // set RAM x address count to 0
    writeDataByte(0x00);
    writeCommand(0x4F);             // set RAM y address count to 0x199
    writeDataByte((EPD_HEIGHT - 1) % 256);
    writeDataByte((EPD_HEIGHT - 1) / 256);
    waitBusy();
}

// ------------------------------------------------------------------------------------------
// Fast refresh initialization  (vendor: EPD_HW_Init_Fast)
//
// A COMPLETE initialisation in its own right, not a variant of the one above, and that is the
// whole point of transcribing it rather than reaching for the update code alone. Taking 0x1A=0x6E
// and an update code without this init is exactly what inverted the screen on 4 September: the
// fast waveform is not in the panel's default LUT, so it has to be loaded here, by a standalone
// 0x22=0x91 + 0x20 that waits on BUSY before anything is drawn.
//
// The vendor recommends it as the routine replacement for a full refresh: 1.5 s and ONE flicker,
// against a full refresh's several. That is what makes cleaning often enough to matter bearable.
// A full refresh stays for power-on and for a thorough clear.
// ------------------------------------------------------------------------------------------
void EPaper::EPD_HW_Init_Fast()
{
    rstLow();
    delay_xms(10);                  // At least 10ms delay
    rstHigh();
    delay_xms(10);                  // At least 10ms delay

    writeCommand(0x12);             // SWRESET
    waitBusy();

    writeCommand(0x21);             // Display update control
    writeDataByte(0x40);
    writeDataByte(0x00);

    writeCommand(0x3C);             // BorderWavefrom (vendor 0x05)
    writeDataByte((uint8_t)init_border);

    // 1.5 s waveform: name the temperature the fast LUT lives at, then load it. This activation
    // is a step of its own and MUST complete before the image goes out.
    writeCommand(0x1A);             // Write to temperature register
    writeDataByte(0x6E);

    writeCommand(0x22);             // Load temperature value
    writeDataByte(0x91);
    writeCommand(0x20);
    waitBusy();

    writeCommand(0x11);             // Data entry mode
    writeDataByte(0x01);

    writeCommand(0x44);             // set Ram-X address start/end position
    writeDataByte(0x00);
    writeDataByte(EPD_WIDTH / 8 - 1);
    writeCommand(0x45);             // set Ram-Y address start/end position
    writeDataByte((EPD_HEIGHT - 1) % 256);
    writeDataByte((EPD_HEIGHT - 1) / 256);
    writeDataByte(0x00);
    writeDataByte(0x00);

    writeCommand(0x4E);             // set RAM x address count to 0
    writeDataByte(0x00);
    writeCommand(0x4F);             // set RAM y address count to 0x199
    writeDataByte((EPD_HEIGHT - 1) % 256);
    writeDataByte((EPD_HEIGHT - 1) / 256);
    waitBusy();
}

// ------------------------------------------------------------------------------------------
// Fast refresh update trigger  (vendor: EPD_Update_Fast)
// ------------------------------------------------------------------------------------------
void EPaper::EPD_Update_Fast()
{
    writeCommand(0x22);             // Display Update Control
    writeDataByte(0xC7);
    writeCommand(0x20);             // Activate Display Update Sequence
    waitBusy(10000);
}

// ------------------------------------------------------------------------------------------
// Fast refresh display  (vendor: EPD_WhiteScreen_ALL_Fast)
//
// Like the full version it writes BOTH RAMs, so the partial that follows measures against a
// reference that matches the glass without anyone reconstructing it.
// ------------------------------------------------------------------------------------------
void EPaper::EPD_WhiteScreen_ALL_Fast(const uint8_t *datas)
{
    writeCommand(0x24);             // write RAM for black(0)/white(1)
    writeImageRows(datas);
    writeCommand(0x26);             // write RAM for black(0)/white(1)
    writeImageRows(datas);
    EPD_Update_Fast();
}

// ------------------------------------------------------------------------------------------
// 4 Gray initialization  (vendor: EPD_HW_Init_4G)
//
// NB: no waveform table is uploaded. The vendor selects the grayscale waveform from the panel's
// own OTP by writing a temperature (0x1A = 0x5A) and activating a LUT load (0x22 = 0x91). Our
// previous grayscale path carried a 233-byte table hand-copied from Waveshare's demo for a
// different module; it is deleted. This is the maker of this panel telling us where its waveform
// lives.
// ------------------------------------------------------------------------------------------
void EPaper::EPD_HW_Init_4G()
{
    rstLow();
    delay_xms(10);
    rstHigh();
    delay_xms(10);

    writeCommand(0x12);             // SWRESET
    waitBusy();

    // BORDER WAVEFORM. 0xC0 = HiZ, and this is a DELIBERATE DEPARTURE from the vendor's 0x05.
    // Measured on board f708, 2026-10-15, by border_bench.cpp, one state per flash, each parked by
    // the production sequence and photographed with the supply off:
    //     0x05 (vendor)      ring
    //     0x01 (documented)  ring
    //     0xC0 (HiZ)         no ring
    // The border electrode (VBD) is a single output outside the 400x300 matrix, reachable only
    // through this register. A[7:6] = 00 drives it along a LUT of the waveform currently loaded,
    // A[1:0] picks which LUT; 11 leaves it floating (SSD1683 Rev 1.0 p. 34).
    //
    // Why HiZ and not another LUT: the white pre-clear that display4Gray runs before the planes
    // goes out under the black/white waveform and drives the border WHITE — which is why the
    // dashboard and the power-off screen, the same register value on the same panel, have never
    // shown a ring. The grayscale waveform is the panel's own OTP (0x1A = 0x5A below) and cannot be
    // read or replaced; both of its LUTs that we can select end dark, which is what 0x05 and 0x01
    // measured. So the cure is to stop asking the grayscale waveform about the border at all, and
    // the pre-clear's white is what stays. The cure is the PAIR: the white frame must keep driving
    // the border (init_border = 0x05, where it is a LUT value and does mean white), and this write
    // must keep its hands off. Change either half and the ring comes back.
    //
    // Floating is safe for the border where it would not be for a signal pad: it is an output of
    // the panel's own driver, left undriven between frames, and 0xC0 is the register's own
    // power-on value (SSD1683: "HiZ, [POR]"), i.e. the state the panel sits in before any init.
    //
    // The ring only ever mattered on the grayscale screens (the logo and LOW BATTERY) because only
    // they are parked — panel_to_sleep(Parked) cuts the supply and the frame then sits on the glass
    // for months. The vendor's demo never unpowers the panel, so this condition was never theirs.
    writeCommand(0x3C);             // Border Waveform Control
    writeDataByte((uint8_t)(gray_border >= 0 ? gray_border : GRAY_BORDER_HIZ));

    writeCommand(0x1A);             // Write to temperature register
    writeDataByte(0x5A);

    writeCommand(0x22);             // Load temperature value
    writeDataByte(0x91);
    writeCommand(0x20);
    waitBusy();

    writeCommand(0x11);             // Data entry mode
    writeDataByte(0x01);

    writeCommand(0x44);
    writeDataByte(0x00);
    writeDataByte(EPD_WIDTH / 8 - 1);
    writeCommand(0x45);
    writeDataByte((EPD_HEIGHT - 1) % 256);
    writeDataByte((EPD_HEIGHT - 1) / 256);
    writeDataByte(0x00);
    writeDataByte(0x00);

    writeCommand(0x4E);
    writeDataByte(0x00);
    writeCommand(0x4F);
    writeDataByte((EPD_HEIGHT - 1) % 256);
    writeDataByte((EPD_HEIGHT - 1) / 256);
    waitBusy();
}

// ------------------------------------------------------------------------------------------
// Update triggers  (vendor: EPD_Update / EPD_Update_4G / EPD_Part_Update)
//
// EPD_Update_Fast (0xC7) is above, next to EPD_HW_Init_Fast: it belongs to that initialisation, which
// ends in its own LUT load, and taking the update code without the init is exactly the mistake that
// inverted the screen. The two halves are only used together (refresh(Update::Fast)).
// ------------------------------------------------------------------------------------------
void EPaper::EPD_Update()
{
    writeCommand(0x22);             // Display Update Control
    writeDataByte(0xF7);
    writeCommand(0x20);             // Activate Display Update Sequence
    waitBusy(10000);
}

void EPaper::EPD_Update_4G()
{
    writeCommand(0x22);
    writeDataByte(0xCF);
    writeCommand(0x20);
    waitBusy(10000);
}

void EPaper::EPD_Part_Update()
{
    writeCommand(0x22);
    writeDataByte(0xFF);
    writeCommand(0x20);
    waitBusy(5000);
}

// ------------------------------------------------------------------------------------------
// Full screen refresh display  (vendor: EPD_WhiteScreen_ALL)
//
// The image goes into BOTH RAMs. 0x24 is what to show; 0x26 becomes the reference the next
// partial update measures against, and it has to hold what is physically on the glass. This is
// also the vendor's EPD_SetRAMValue_BaseMap, which is the same function under another name and
// carries the comment "this function is necessary, please do not delete it!!!".
// ------------------------------------------------------------------------------------------
void EPaper::EPD_WhiteScreen_ALL(const uint8_t *datas)
{
    writeCommand(0x24);             // write RAM for black(0)/white(1)
    writeImageRows(datas);
    writeCommand(0x26);             // write RAM for black(0)/white(1)
    writeImageRows(datas);
    EPD_Update();
}

// Clear screen display  (vendor: EPD_WhiteScreen_White)
void EPaper::EPD_WhiteScreen_White()
{
    static uint8_t white[256];
    memset(white, 0xFF, sizeof(white));
    writeCommand(0x24);
    for (size_t i = 0; i < EPD_ARRAY; i += sizeof(white))
        writeData(white, (EPD_ARRAY - i > sizeof(white)) ? sizeof(white) : (EPD_ARRAY - i));
    writeCommand(0x26);
    for (size_t i = 0; i < EPD_ARRAY; i += sizeof(white))
        writeData(white, (EPD_ARRAY - i > sizeof(white)) ? sizeof(white) : (EPD_ARRAY - i));
    EPD_Update();
}

// ------------------------------------------------------------------------------------------
// Full screen partial refresh  (vendor: EPD_Dis_PartAll)
//
// Note what is NOT here: no SWRESET, no register init, no write to 0x26. The vendor hardware-
// resets, sets the border and the update control, writes the new image to 0x24 alone and fires
// the partial waveform — relying on 0x26 still holding the previous frame. That is the whole
// mechanism, and it is why the base map above is mandatory before the first partial.
// ------------------------------------------------------------------------------------------
void EPaper::EPD_Dis_PartAll_Begin()
{
    rstLow();                       // Module reset
    delay_xms(10);
    rstHigh();
    delay_xms(10);

    if (part_border >= 0) {         // vendor writes 0x80 here; see part_border in epaper.h
        writeCommand(0x3C);         // BorderWavefrom
        writeDataByte((uint8_t)part_border);
    }

    writeCommand(0x21);
    writeDataByte(0x00);
    writeDataByte(0x00);
}

void EPaper::EPD_Dis_PartAll_Image(const uint8_t *datas)
{
    writeCommand(0x24);             // Write Black and White image to RAM
    writeImageRows(datas);

    EPD_Part_Update();
}

// The vendor's function, unsplit, for any caller that has the image in hand already.
void EPaper::EPD_Dis_PartAll(const uint8_t *datas)
{
    EPD_Dis_PartAll_Begin();
    EPD_Dis_PartAll_Image(datas);
}

// ------------------------------------------------------------------------------------------
// Deep sleep  (vendor: EPD_DeepSleep)
//
// The vendor's demo calls this after every refresh and says in its own tips: "Please make sure
// that EPD enters sleep mode when refresh is completed and always leave the sleep mode command.
// Otherwise, this may result in a reduced lifespan of EPD."
// ------------------------------------------------------------------------------------------
void EPaper::EPD_DeepSleep()
{
    writeCommand(0x10);             // Enter deep sleep
    writeDataByte(0x01);
    delay_xms(100);
}

// ------------------------------------------------------------------------------------------
// 4 Gray refresh display  (vendor: EPD_WhiteScreen_ALL_4G)
//
// The vendor takes interleaved 2-bit source data and unpacks it with R24_DTM1/R26_DTM2 into the
// two planes as it streams. Ours arrive already separated into those same two planes, packed on
// the host by Software/mockups/gen_4gray.py — the bytes reaching the panel are identical, the
// unpacking simply happened earlier and off the device.
// ------------------------------------------------------------------------------------------
void EPaper::EPD_WhiteScreen_ALL_4G(const uint8_t *plane24, const uint8_t *plane26)
{
    writeCommand(0x24);
    writeImageRows(plane24);
    writeCommand(0x26);
    writeImageRows(plane26);
    EPD_Update_4G();
}
