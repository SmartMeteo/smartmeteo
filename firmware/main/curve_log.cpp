#include "curve_log.h"
#include "esp_partition.h"
#include <string.h>

namespace CurveLog {

uint32_t crc32(const uint8_t *p, size_t n)
{
    // Bitwise, reflected, polynomial 0xEDB88320, initial and final 0xFFFFFFFF — the CRC-32 that
    // Python's zlib.crc32 computes, which is the point: the tool checks it without a table of its own.
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
    return c ^ 0xFFFFFFFFu;
}

void seal(Rec &r)
{
    r.magic = MAGIC;
    r.crc = crc32(reinterpret_cast<const uint8_t *>(&r), REC - 4);
}

bool valid(const Rec &r)
{
    return r.magic == MAGIC && r.crc == crc32(reinterpret_cast<const uint8_t *>(&r), REC - 4);
}

static const esp_partition_t *part()
{
    static const esp_partition_t *p = nullptr;
    if (!p) p = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, nullptr);
    return p;
}

int capacity()
{
    const esp_partition_t *p = part();
    return p ? (int)(p->size / REC) : -1;
}

bool read(int i, Rec &r)
{
    const esp_partition_t *p = part();
    if (!p || i < 0 || i >= capacity()) return false;
    return esp_partition_read(p, (size_t)i * REC, &r, REC) == ESP_OK;
}

static bool erased(const Rec &r)
{
    const uint8_t *b = reinterpret_cast<const uint8_t *>(&r);
    for (int i = 0; i < REC; i++) if (b[i] != 0xFF) return false;
    return true;
}

int next_free()
{
    const int cap = capacity();
    if (cap < 0) return -1;
    // From the end backwards: the log is append-only, so the first erased slot after the last
    // written one is the answer, and a torn record in the middle does not stop the search early.
    int i = cap;
    Rec r;
    while (i > 0 && read(i - 1, r) && erased(r)) i--;
    return i;
}

bool append(Rec &r)
{
    const esp_partition_t *p = part();
    const int i = next_free();
    if (!p || i < 0 || i >= capacity()) return false;
    seal(r);
    return esp_partition_write(p, (size_t)i * REC, &r, REC) == ESP_OK;
}

bool erase_all()
{
    const esp_partition_t *p = part();
    return p && esp_partition_erase_range(p, 0, p->size) == ESP_OK;
}

}  // namespace CurveLog
