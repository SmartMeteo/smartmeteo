#pragma once
// The battery bench's log, in one place: curve_bench.cpp writes it, curve_summary.cpp reads it on
// the device (for the screen and for what to do next), tools/curve_fit.py reads it on the Mac, and
// the host tests check that all three agree (t_cycle writes a test through this code and through
// curve_summary.cpp; t_curve.py reads the same image back through the tool). A format described
// twice drifts; this one is described here and checked from both ends.
//
// TWO GENERATIONS. Version 1 (2026-09-30) logged one discharge: a header, samples, an end. Version 2
// (2026-10-03) logs a whole TEST — charge, discharge, charge, discharge, charge — as a sequence of
// segments. Every v1 kind keeps its meaning inside a v2 discharge segment, so the tool's curve fit
// reads a v2 discharge exactly as it read a v1 run; the v2 kinds are new numbers, never reused ones.
//
// WHERE: the coredump partition, 64 KB = 2048 records. The bench replaces the whole device cycle,
// so nothing else is using it while it runs. A v2 test of two cycles takes about a thousand: some
// 300 per discharge (a sample and a pulse every twenty minutes for a day and a half), some 120 per
// charge. The cost is known: if the bench itself crashes, ESP-IDF writes its dump over the log; and
// with CONFIG_ESP_COREDUMP_CHECK_BOOT the product firmware complains once per boot about a "dump" it
// cannot parse until the partition is erased — which the reading tool offers to do.
//
// HOW: append-only, 32-byte records, each sealed with a CRC. Appends go to the first slot that is
// still erased (all 0xFF), not to the first slot that fails its CRC: a record torn by a reset in the
// middle of a write is neither valid nor erased, and writing over it would corrupt the next one.

#include <stdint.h>
#include <stddef.h>

namespace CurveLog {

static constexpr uint16_t MAGIC   = 0xC7B1;
static constexpr uint8_t  VERSION = 2;
static constexpr int      REC     = 32;

enum Kind : uint8_t {
    // ---- a discharge segment (v1: the whole log) ----
    KindHeader  = 1,   // a discharge begins: t is the absolute clock (s), detail the format version;
                       // v2: spread = the cycle (1 or 2)
    KindSample  = 2,   // one rested reading, and the loaded one just before the rest; v2: detail = temp
    KindEnd     = 3,   // the cell reached the end voltage; the discharge is complete
    KindCharger = 4,   // a cable went in mid-discharge: charge entered, the discharge is void
    KindReset   = 5,   // the chip restarted mid-segment; detail = esp_reset_reason(), | 0x80 when
                       // the clock had been lost and was set forward to the last record
    // ---- v2 only ----
    KindTest        = 6,   // a test begins (the log was erased just before): t absolute, detail VERSION
    KindChargeStart = 7,   // the cable went in: t absolute; spread = the cycle (0, 1, 2); v_rest = the
                           // last rested reading before it (0 if none); v_load = the first reading
                           // on the charger; flags FlagRecharge when it refills a voided discharge
    KindChargeSample= 8,   // t = s since the charge began; load_s = s the charger was seen CHARGING
                           // (STDBY# high); v_rest = cell voltage on the charger; flags Usb|Stdby;
                           // detail = temp
    KindChargeEnd   = 9,   // STDBY# went low after a watched charge: t = the charge time; v_rest = the
                           // voltage then; v_load = the highest voltage seen during the charge
    KindPulse       = 10,  // 10 s into a load phase: v_rest = the rested reading just before it,
                           // v_load = the reading now; the drop over the known load is resistance.
                           // t, load_s as a sample; detail = temp
    KindUnplug      = 11,  // the cable came out before the charge finished; detail 0 = pulled early,
                           // 1 = the charger had been idle (cell above its restart point, run it down)
    KindTestDone    = 12,  // the last charge finished: the test is complete
    KindProg        = 13,  // the charge current, read by hand off TP4056's PROG pin (volts across R7,
                           // entered on the bench's page): t = s since the charge began, when it was
                           // entered; v_load = the meter's volts (1.00 V = 1 A, see CurveSummary);
                           // v_rest = the cell on the charger then, raw; spread = the reading's number
};

enum Flag : uint8_t {
    FlagUsb      = 1,     // cable in
    FlagStdby    = 2,     // STDBY# low: the charger says it has finished
    FlagSta      = 4,     // connected to the home network for the load phase
    FlagAp       = 8,     // own access point up for the load phase
    FlagRecharge = 16,    // ChargeStart: refills a voided discharge; not one of the measured charges
};

// Temperature in `detail`: half degrees from -40 C, 0 = not taken. -39.5 .. +87.5 C.
static inline uint8_t temp_code(float c)
{
    if (!(c > -39.5f && c < 87.5f)) return 0;            // also NaN
    return (uint8_t)((c + 40.0f) * 2.0f + 0.5f);
}
static inline bool temp_of(uint8_t code, float &c)
{
    if (!code) return false;
    c = code / 2.0f - 40.0f;
    return true;
}

#pragma pack(push, 1)
struct Rec {
    uint16_t magic;
    uint8_t  kind;
    uint8_t  flags;
    uint32_t t;          // seconds since the segment began (header, test, charge start: absolute)
    uint32_t load_s;     // discharge: seconds under load since it began; charge sample: seconds charging
    float    v_rest;     // rested cell voltage from the divider, NO gain; 0 = not taken
    float    v_load;     // cell voltage at the end of the load phase, NO gain; 0 = not taken
    float    gain;       // BatterySource::calibration() when written — the tool may override it
    uint16_t seq;        // record number within the run (v2: within the test)
    uint8_t  spread;     // LSB spread of the burst, clamped to 255; header, charge start: the cycle
    uint8_t  detail;     // kind-specific, see Kind
    uint32_t crc;        // CRC-32 (IEEE 802.3, as zlib) of the 28 bytes before it
};
#pragma pack(pop)
static_assert(sizeof(Rec) == REC, "tools/curve_fit.py reads 32-byte records");

uint32_t crc32(const uint8_t *p, size_t n);
void seal(Rec &r);                 // stamps the magic and the CRC
bool valid(const Rec &r);

// The partition. All of these return false (or -1) when it cannot be found or read.
int  capacity();                   // records the partition holds
bool read(int i, Rec &r);          // raw slot i, valid or not
int  next_free();                  // first slot still erased; capacity() when full
bool append(Rec &r);               // seals r and writes it to next_free()
bool erase_all();

}  // namespace CurveLog
