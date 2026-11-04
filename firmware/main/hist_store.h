#pragma once

#include <stdint.h>

// Compact 32-byte history record, written once per measurement cycle into a
// raw circular log on the "storage" partition (2 MB = 65536 records -> ~228 days at 5-min).
// Capacity is derived from the partition size at runtime (cap_ = size / sizeof(HistRec)), so
// resizing "storage" in partitions.csv is the only knob for retention.
// HistRec::flags bit 6 — see the field. Old records carry 0 here: no firmware before 2026-10-08
// set bits 6 or 7.
constexpr uint8_t HIST_FLAG_CURVE = 0x40;

struct __attribute__((packed)) HistRec {
    uint32_t t;        // unix seconds (UTC); 0xFFFFFFFF = empty slot
    uint16_t co2;      // ppm
    // Room climate, from the SCD41. Written only when that sensor answered for this record:
    // temperature and humidity ride on the same read as the CO2 above, so a tick it stayed
    // silent on logs 0 rather than repeating the previous tick's room. 0 is the same sentinel
    // the fields below use; a genuine 0.0 C indoors would be indistinguishable, and the
    // dashboard already refuses to draw outside 2..50 C.
    int16_t  temp_d;   // °C * 10; 0 = not recorded
    uint8_t  hum;      // %RH (0..100)
    uint16_t pres_d;   // mmHg * 10
    uint16_t iaq;      // 0..500
    uint8_t  gas;      // air purity, gas % (0..100)
    uint8_t  soc;      // battery % (0..100)
    uint8_t  flags;    // bits [0-1] iaq accuracy (0..3), bits [2-4] pollution class (0..5),
                       // bit  [5]   on USB power. Without it a stretch of charging would be read
                       // as the device consuming a negative current.
                       // bit  [6]   the battery percentage was read through THIS cell's measured
                       // curve (HIST_FLAG_CURVE, since 2026-10-08). Before that the v1 board logged
                       // its fuel gauge's model, which stood still on the 103450's plateau; the
                       // two are different instruments and the estimate must not slope across both.
    uint16_t siaq;     // BSEC static IAQ — Bosch: "unscaled indoor-air-quality estimate", from 0 with
                       // no upper limit (AN011). Logged only; no verdict is taken from it.
    // Battery, at full resolution. `soc` above is whole percent, which is useless for measuring
    // consumption: a day at ~300 uA drains 0.36 % of the cell, so a run of integers would be a
    // flat line. The gauge itself resolves 1/256 %, and that is what these keep.
    uint16_t soc_d;    // state of charge * 100 (0..10000); 0 in records written before this field
    uint16_t vcell_mv; // cell voltage in mV; 0 = not recorded
    // The gas verdict's own arithmetic, so that a verdict can still be explained a month later.
    // IAQ is a RELATIVE index (4.4) and the whole cascade rests on z = (iaq - base) / scale — of
    // which only iaq was ever stored. That left "why did it say Poor air at 3 am" unanswerable,
    // and it hid the one failure mode this detector actually has: a baseline that drifts upward
    // into a long event and quietly adopts dirty air as normal. That drift was observed on
    // hardware (4.16, 53 -> 148 while laundry dried) and would have been invisible here.
    int16_t  gas_z_d;      // z * 100, signed
    uint16_t gas_base_d;   // the room's learned norm * 10. 0 = not recorded: records written
                           // before this field existed, and ticks that took no gas sample. The
                           // baseline is an EMA of IAQ seeded only once accuracy >= 1, so a
                           // genuine zero does not occur on a working device.
    uint16_t gas_scale_d;  // the spread it is judged against * 10 (floor 8, ceiling 30)
    // The BME690's own climate. A diagnostic pair, never the room climate — the gas heater sits
    // next to this element, so it reads warm and dry (web_server.cpp). It is logged because the gap
    // between the two temperature sensors is the only evidence for what the device's self-heating
    // actually is, and until now that gap could only be read live: two numbers copied by hand tell
    // you nothing about how it moves with the room, the hour, or a charging cable.
    // Written only on a gas tick. On every other wake the BME690 is not read at all, and storing the
    // previous value again would log a measurement that never happened (see the note above).
    //
    // These take the pad rather than one of the three dead gas_* fields: those were genuinely
    // written until 0.9.55, so records older than that still carry a z-score there, and reusing one
    // would decode old logs as a plausible-looking temperature. The pad has always been zero.
    int16_t  b_temp_d;     // BME690 °C * 10; 0 = not recorded (same sentinel as soc_d, vcell_mv)
    uint8_t  b_hum;        // BME690 %RH (0..100); 0 = not recorded
    uint8_t  reserved[1];    // pad to 32 B (a divisor of the 4096 sector, so records never straddle
                             // a sector boundary and the ring's erase-before-write stays valid)
};
static_assert(sizeof(HistRec) == 32, "HistRec must be 32 bytes (sector-aligned)");

// Raw-partition circular history log. No filesystem: uses esp_partition
// directly (energy-minimal, no mount). Write head is cached in RTC across deep
// sleep and rebuilt by scanning after a full power loss.
class HistStore {
public:
    static HistStore& getInstance();

    bool init();                        // find partition, recover head (idempotent)
    bool append(const HistRec &rec);    // write one record (erases a sector on wrap)
    uint32_t count() const;             // approx. number of stored records
    // Wipe the whole ring. The history lives in its own raw partition, so a factory reset — which
    // only erases NVS — leaves it completely intact; this is what a factory reset must also call
    // for the device to genuinely forget. Erases in chunks and yields between them: 2 MB in one
    // blocking call is long enough to starve the idle task and trip the watchdog.
    void eraseAll();

    // TEST ONLY. Moves the write head so the wrap can be exercised without waiting for the ring to
    // fill — that takes 228 days at the normal rate, and the code that runs at the boundary has
    // therefore never executed on any device. See hist_bench.cpp.
    void debugSetHead(uint32_t slot, uint32_t count);
    uint32_t debugHead() const;
    uint32_t capacity() const { return cap_; }

    // Iterate stored records oldest->newest within [from,to] (0 = unbounded).
    // Calls cb for at most `max_out` records, decimating if more match.
    typedef void (*RecCb)(const HistRec &r, void *ctx);
    // Optional predicate, applied in BOTH passes so the decimation counts only the records that
    // will actually be emitted. Filtering afterwards is not the same thing: the log holds a record
    // every gas tick but a CO2 reading only every profile interval, so a caller that wants the CO2
    // series and filters at the callback still gets a decimation stride computed over records it
    // then throws away — and when the stride shares a factor with the interval, every sample lands
    // on the same phase and the answer is all-or-nothing.
    typedef bool (*RecKeep)(const HistRec &r);
    void query(uint32_t from, uint32_t to, uint32_t max_out, RecCb cb, void *ctx,
               RecKeep keep = nullptr);

    // The newest stored timestamp, read from the slot behind the head rather than by
    // scanning. 0 when the log is empty or the last records are unreadable.
    uint32_t newestTimestamp();

private:
    HistStore() = default;
    void recover();                     // scan flash to find head/count
    const void *part_ = nullptr;        // esp_partition_t*
    uint32_t cap_ = 0;                  // total record slots
};
