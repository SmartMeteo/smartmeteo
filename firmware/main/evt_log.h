#pragma once

#include <stdint.h>
#include <string>

// FLIGHT RECORDER — one record every wake, plus a record whenever something happens.
//
// The device is awake for roughly a second at a time and then gone, so the serial port does not
// meaningfully exist while it is doing its job (§4.22: ~1.3 s per wake, less than macOS needs to
// enumerate USB-CDC). Every question about behaviour has therefore had to be answered either by
// staring at the glass or by inference from the measurement history, which records what the air
// did and nothing at all about what the firmware did. This ring records the second half.
//
// ORDERED BY A COUNTER, NOT BY THE CLOCK. The history ring orders by timestamp, and that is how
// its whole contents once became invisible: a single record dated in the future captured the head
// permanently (§4.32). Worse, a wake that happens before the clock is set has no timestamp to
// order by at all — and those wakes are exactly the ones worth studying. `seq` is monotonic,
// survives deep sleep in RTC memory, and after a power loss is restored from the highest value
// found in flash, so ordering never depends on anything outside this file.
//
// COST. A 32-byte page program is well under a millisecond, and a sector erase (every 128 records,
// so roughly every two hours) is tens of milliseconds — together about 15 mA·s per day against a
// budget near 8000. Endurance is not a concern either: each sector is erased about 16 times a year
// against a rated 100000.

enum EvtKind : uint8_t {
    EVT_WAKE   = 0,   // the per-wake summary, written on the way into sleep
    EVT_BOOT   = 1,   // start of a wake that followed an abnormal reset; detail = esp_reset_reason
    EVT_SENSOR = 2,   // a sensor failed, recovered, or was reset
    EVT_GAS    = 3,   // BSEC: no outputs, accuracy moved, state loaded/saved
    EVT_RADIO  = 4,   // Wi-Fi / NTP / MQTT / weather outcome
    EVT_DRAW   = 5,   // panel refresh that was notable (too fast, full, failed)
    EVT_BUTTON = 6,   // a press, and how it was classified
    EVT_ALERT  = 7,   // the alert state machine moved
    EVT_POWER  = 8,   // charge, lockout, low-battery threshold
    EVT_OTA    = 9,   // update check or install
    EVT_NOTE   = 10,  // anything else; detail carries a code from EvtNote below
    EVT_SEC    = 11,  // security-relevant: who got in, what they changed; detail from EvtSec below
};

// EVT_SEC detail. Added 2026-09-18 for EN 18031-2 [LGM], which asks that security- and
// privacy-relevant internal activity be logged, persistently and with a time.
//
// These are the events somebody would want to reconstruct afterwards: whether anyone got into the
// console, whether they failed first, and what they changed once inside. They go in the same ring as
// everything else, which means they survive power loss and a firmware update and are readable with
// tools/read_events.py — and they are erased by a factory reset along with the rest, which is the
// right answer for a device that changes hands.
//
// Deliberately NOT logged: the password, the token, the address of whoever connected. A log that
// carries the secret it is protecting is worse than no log, and an IP on a home network is personal
// data we have no reason to keep.
enum EvtSec : uint32_t {
    SEC_SIGNIN_OK     = 1,   // somebody answered the challenge correctly
    SEC_SIGNIN_FAIL   = 2,   // wrong answer; low byte = how many in a row
    SEC_SIGNIN_LOCKED = 3,   // too many wrong answers; the door is shut for a minute
    SEC_PASSWORD_SET  = 4,   // the device password was replaced from the console
    SEC_FACTORY_REQ   = 5,   // a factory reset was asked for (the wipe itself is NOTE_FACTORY_WIPE)
    SEC_OTA_INSTALL   = 6,   // an update was installed; low 16 bits = its sequence number
    SEC_OTA_REVERT    = 7,   // the running image was marked bad and the device sent back
    SEC_MATTER_FORGET = 8,   // removal from the smart home was requested
    SEC_WINDOW_OPEN   = 9,   // the console window opened; 1 = own access point, 0 = home network
};

// Codes for EVT_NOTE. Kept here rather than at the call sites so the host-side reader can name
// them: a bare number in a log a year old is not evidence of anything.
enum EvtNote : uint32_t {
    NOTE_MQTT_FAIL = 1,   // the broker refused or was unreachable
    NOTE_CAL_SAVED = 2,   // gas calibration written to flash; detail's high byte = accuracy
    NOTE_CAL_WIPED = 3,   // gas calibration deliberately discarded (new room, first setup)
    NOTE_FACTORY_WIPE = 4,  // factory reset carried out: the first record of the fresh log
    // How much of a measuring wake the chip actually spent in light sleep. detail: code in bits
    // 0-7, milliseconds slept in 8-21, number of sleeps in 22-31. It lives here rather than in a
    // log line because it cannot be watched over the cable: with a computer on the USB-Serial/JTAG
    // port IDF holds its own ESP_PM_NO_LIGHT_SLEEP lock, so a device being watched never sleeps.
    NOTE_LIGHT_SLEEP = 5,
    // Сколько пробуждения прошло ДО того, как завёлся сам регистратор. awake_ms считается от
    // EvtLog::init(), а тот вызывается восьмым шагом: загрузчик, инициализация ESP-IDF и первые
    // семь шагов в него не входят, то есть всякая оценка бодрствования по журналу была нижней
    // границей. detail: код в битах 0-7, миллисекунды в 8-31.
    NOTE_WAKE_PRE = 6,
    // Switched off by hand — the console or a ten-second press. The pair to NOTE_FACTORY_WIPE:
    // same road out (sensors parked, panel supply cut, only the button armed), nothing erased.
    NOTE_POWER_OFF = 7,
};

// EVT_SENSOR detail for the CO2 sensor. Bit 16 and up name what happened; the low 16 bits carry its
// number. Records written before 0.9.44 have none of these bits: those were the automatic factory
// reset, attempt number in the low byte and self-test result above it.
enum EvtSensor : uint32_t {
    SENS_SCD_REINIT  = 1u << 16,   // no reading; reinit sent (Sensirion's recovery). Low bit: accepted
    SENS_SCD_FRC_OK  = 1u << 17,   // forced recalibration done; low 16 bits: correction as int16
    SENS_SCD_FRC_BAD = 1u << 18,   // forced recalibration refused by the sensor (0xFFFF)
};

// EVT_GAS detail. 0 is the original record: BSEC returned OK with no outputs. Bit 16 and up name a BSEC
// warning and which call returned it; the low 16 bits carry the warning code as int16.
enum EvtGas : uint32_t {
    GAS_ZERO_OUTPUTS   = 0,
    GAS_WARN_CONTROL   = 1u << 16,   // bsec_sensor_control
    GAS_WARN_STEPS     = 1u << 17,   // bsec_do_steps
    GAS_WARN_SUBSCRIBE = 1u << 18,   // bsec_update_subscription
};

// Wake causes, kept separate from ESP-IDF's enum so the meaning here is ours: which of OUR reasons
// brought the device up, including the one that is not a chip wake at all (a software restart).
enum EvtWake : uint8_t {
    WK_UNKNOWN = 0, WK_COLD = 1, WK_TIMER = 2, WK_BUTTON = 3, WK_USB = 4, WK_RESTART = 5,
};

// EvtRec.flags
enum : uint8_t {
    EF_USB     = 1 << 0,   // cable present
    EF_NIGHT   = 1 << 1,   // night schedule in force
    EF_MEASURE = 1 << 2,   // sensors were read this wake
    EF_RADIO   = 1 << 3,   // radio was brought up
    EF_FORCED  = 1 << 4,   // the button asked for this, not the schedule
    EF_FIRST   = 1 << 5,   // first wake of this image
    EF_DREW    = 1 << 6,   // the panel was refreshed
    EF_FULL    = 1 << 7,   // ...and it was a full refresh
};

// EvtRec.sens — who answered. A zero bit means "did not answer", which is the point: a sensor that
// goes quiet for a week shows up as a column of zeros rather than as a gap someone has to notice.
enum : uint8_t {
    ES_SCD   = 1 << 0, ES_BME  = 1 << 1, ES_BSEC = 1 << 2, ES_GAUGE = 1 << 3,
    ES_NVS   = 1 << 4, ES_HIST = 1 << 5, ES_WIFI = 1 << 6, ES_MQTT  = 1 << 7,
};

struct __attribute__((packed)) EvtRec {
    uint32_t seq;        // monotonic; 0xFFFFFFFF is what erased flash reads as, so it means "empty"
    uint32_t t;          // unix UTC, or 0 when the clock has never been set
    uint16_t awake_ms;   // this wake so far, saturated at 65535 (18 h of console counts as "long")
    uint8_t  kind;       // EvtKind
    uint8_t  wake;       // EvtWake
    uint8_t  flags;      // EF_*
    uint8_t  gas;        // BSEC accuracy in bits 0-1, pollution class in 2-4, outputs seen in bit 5
    uint8_t  sens;       // ES_*
    uint8_t  boot_inc;   // consecutive boots that never reached sleep, as seen on the way in
    uint16_t heap_kb;    // free heap / 1024
    uint16_t draw_ms;    // last panel refresh this wake, 0 if none
    uint16_t radio_ms;   // time the radio was up, 0 if it never came up
    uint16_t vcell_mv;
    uint16_t soc_d;      // state of charge * 100
    int8_t   rssi;       // 0 when not associated
    int8_t   temp_c;     // rounded ambient, for correlating behaviour against temperature
    uint32_t detail;     // kind-specific
};
static_assert(sizeof(EvtRec) == 32, "EvtRec must be 32 bytes (a divisor of the 4096-byte sector, so "
                                    "no record straddles the boundary the ring erases at)");

class EvtLog {
public:
    static EvtLog& getInstance();

    bool init();

    // Момент, от которого считается awake_ms: esp_timer на первом init(). Он же — стоимость всего,
    // что случилось до регистратора, и до 2026-09-20 это число просто выбрасывалось.
    int64_t baseUs() const;                    // find partition, restore seq (idempotent, cheap after the first)

    // Record something that just happened. Fields the caller does not know are filled from whatever
    // the wake has already established, so a call site only has to name its own news.
    void event(EvtKind kind, uint32_t detail = 0);

    // The per-wake summary. Called from the one place every sleep path passes through.
    void wake(uint16_t awake_ms);

    // Facts the wake accumulates. Kept here rather than passed at write time so that a call site
    // that learns something does not also have to know how to write a record.
    void setWake(EvtWake w, bool first);
    void setFlags(uint8_t set, uint8_t clear = 0);
    void setSensor(uint8_t bit, bool ok);
    void setGas(uint8_t accuracy, uint8_t pollution_class, bool outputs);
    void setDraw(uint16_t ms, bool full);
    void setRadio(uint16_t ms, int8_t rssi);
    void setBattery(uint16_t vcell_mv, uint16_t soc_d);
    void setTemp(float c);
    void setBootIncomplete(uint8_t n);
    void setDetail(uint32_t d);      // free field on the wake record; see its call site

    uint32_t count() const;
    uint32_t capacity() const { return cap_; }
    uint32_t head() const;
    uint32_t nextSeq() const;
    uint32_t recordSize() const { return sizeof(EvtRec); }

    // Read one slot. Returns false when the partition is missing or the slot is out of range; an
    // empty slot is returned as-is (seq == 0xFFFFFFFF) so a caller can tell "never written" from
    // "written and unremarkable".
    bool read(uint32_t slot, EvtRec &out) const;

    // Build one framed chunk of records newer than `since`, at most `max_records` of them, and
    // return the highest sequence number it contains (== since when there is nothing new). The
    // frame is self-delimiting — magic, version, record size, count — so a receiver can simply
    // append chunks to a file and a reader can walk them without any external index.
    uint32_t buildChunk(uint32_t since, uint32_t max_records, std::string &out);

    // The high-water mark of what has been handed to the hub. Kept beside the ring rather than in
    // NVS: it changes every radio window, and the ring is the system of record anyway — losing it
    // only means sending some records twice, which the reader drops by sequence number.
    uint32_t lastSent() const;
    void markSent(uint32_t seq);

    // A trailing 24 hours of behaviour, summarised. This is what the owner sees in Home Assistant:
    // not records, but four numbers that say whether the device is behaving. The storm of 2026-08-29
    // was found by a human looking at the glass, not by the device noticing anything about itself.
    struct Health {
        bool     valid;        // false until there is enough span to divide by
        uint32_t wakes;        // in the window
        uint32_t draws;        // panel refreshes, of which
        uint32_t fulls;        //   ...full ones
        uint32_t edges;        // rejected button edges
        uint64_t awake_ms;     // summed
        uint32_t span_s;       // what the numbers above actually cover
    };
    Health health() const;

    void eraseAll();                // factory reset: this is a third store, and it forgets too

private:
    EvtLog() = default;
    void restoreSeq();              // scan for the highest seq after a power loss
    bool append(EvtRec &r);
    const void *part_ = nullptr;
    uint32_t cap_ = 0;
};
