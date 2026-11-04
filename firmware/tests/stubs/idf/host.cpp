// Оснастка: ESP-IDF, которого нет, и модули, которые для расписания являются входами.
//
// Разделение сделано по одному правилу: НАСТОЯЩИЙ код прошивки везде, где он собирается на этой
// машине. Заглушки стоят под ним, на границе ESP-IDF, — то же, что t_weather делает с
// esp_http_client.h. Поэтому readPlan(), plan_tick(), time_to_next_wake() и battery_too_low()
// здесь подлинные, и подлинны же nvs_manager.cpp (путь настроек), hist_store.cpp и evt_log.cpp
// (кольца во флеше), analytics.cpp, sun_moon.cpp, screen.cpp с epaper.cpp.
//
// На границе МОДУЛЯ заглушены три класса, и у каждого есть причина: WebServer и MqttPublisher —
// это выходы цикла, а не цикл, и оба уже покрыты t_console и t_browser; MatterMode не уходит в
// глубокий сон и живёт своим циклом, к расписанию Wi-Fi отношения не имея.
//
// Чего эта оснастка НЕ проверяет и проверить не может: сон, радио, шину I2C и панель. Их заменить
// нечем — только прибор. Здесь проверяется РЕШЕНИЕ: что прибор собирался сделать и на какой срок
// собирался уснуть.
#include "host.h"
#include "battery_source.h"   // fresh() изображает новую загрузку: см. reset()
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_err.h"
#include "esp_system.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "esp_pm.h"
#include "esp_partition.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_core_dump.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "esp_netif.h"
#include "esp_flash.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_netif_sntp.h"
#include "nvs.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <map>
#include <vector>
#include <string>
#include <cstring>
#include <cstdio>
#include <cstdarg>

namespace Host {
time_t  wall    = 1758700000;          // 2025-09-24 около полудня UTC — просто известная точка
int64_t mono_us = 0;
bool    usb_in = false, button_down = false;
bool    bus_ok = true,  gauge_ok = true;
bool    mqtt_ok = false;   // брокер по умолчанию недоступен — см. host.h
bool    sensors_present = true, adc_cali_ok = true, stdby_low = false;
int     adc_spike_every = 0, adc_settle_bad = 0, adc_glitch_bursts = 0, adc_bursts = 0;
float   adc_glitch_v = 0.0f, adc_burst_drift_v = 0.0f, adc_mv_per_code = 0.0f, adc_burst_noise_v = 0.0f;
int     i2c_probes = 0;
int     gauge_writes = 0, gauge_reads = 0;
float   vcell = 3.95f, soc = 80.0f, crate = -0.5f;
size_t  free_heap = 180 * 1024;
int      reset_reason = 8 /*ESP_RST_DEEPSLEEP*/;
int      wake_cause   = 4 /*ESP_SLEEP_WAKEUP_TIMER*/;
uint64_t gpio_wake_mask = 0;
uint8_t  elf_sha[32] = {0};
uint64_t armed_timer_us = 0, armed_gpio_mask = 0;
bool     armed_button = false;
int      armed_gpio_level = -1, sources_cleared = 0, restarts = 0, deep_sleeps = 0;
bool     light_sleep_on = false;
int      no_ls_locks = 0, rollbacks = 0, marked_valid = 0;
int      intr_type[31] = {0};
bool     intr_enabled[31] = {false};
std::vector<std::string> log_lines;
}  // namespace Host

// ---- хранилище ключ-значение -------------------------------------------------------------------
// Настоящее, потому что над ним собирается настоящий nvs_manager.cpp: хранение float битовым
// образом U32, отсутствующий ключ против нулевого — всё это исполняется как на приборе.
namespace {
struct Blob { std::vector<uint8_t> b; };
std::map<std::string, std::map<std::string, Blob>> g_nvs;
std::vector<std::string> g_ns;                    // handle - 1 = индекс
bool g_nvs_inited = false;

std::map<std::string, Blob> &store(nvs_handle_t h) {
    static std::map<std::string, Blob> nowhere;
    if (h == 0 || h > g_ns.size() || h > g_ns.size()) return nowhere;
    return g_nvs[g_ns[h - 1]];
}
// «Флеш» в ОЗУ: разделы из main/partitions.csv, чтобы ёмкость кольца истории совпадала с прибором.
struct Part { esp_partition_t p; std::vector<uint8_t> data; };
std::vector<Part> g_parts;
void make_parts() {
    if (!g_parts.empty()) return;
    auto add = [](esp_partition_type_t t, esp_partition_subtype_t s, uint32_t addr, uint32_t size, const char *label) {
        Part q{}; q.p.type = t; q.p.subtype = s; q.p.address = addr; q.p.size = size;
        q.p.erase_size = 4096; q.p.encrypted = false;
        snprintf(q.p.label, sizeof(q.p.label), "%s", label);
        q.data.assign(size, 0xFF);                       // стёртый флеш — единицы, как на приборе
        g_parts.push_back(std::move(q));
    };
    add(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS,   0x620000, 1600 * 1024, "storage");
    add(ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t)0x40,       0x7C0000,  256 * 1024, "events");
    add(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, 0x7B0000,   64 * 1024, "coredump");
    add(ESP_PARTITION_TYPE_APP,  ESP_PARTITION_SUBTYPE_APP_OTA_0,     0x020000, 3072 * 1024, "app0");
    add(ESP_PARTITION_TYPE_APP,  ESP_PARTITION_SUBTYPE_APP_OTA_1,     0x320000, 3072 * 1024, "app1");
}
}  // namespace

extern "C" {

esp_err_t nvs_flash_init(void) { g_nvs_inited = true; return ESP_OK; }
esp_err_t nvs_flash_erase(void) { g_nvs.clear(); return ESP_OK; }

esp_err_t nvs_open(const char *ns, nvs_open_mode_t mode, nvs_handle_t *out) {
    (void)mode;
    if (!g_nvs_inited) return ESP_ERR_NVS_NOT_FOUND;
    g_ns.push_back(ns ? ns : "");
    if (out) *out = (nvs_handle_t)g_ns.size();
    return ESP_OK;
}
void      nvs_close (nvs_handle_t h) { (void)h; }
esp_err_t nvs_commit(nvs_handle_t h) { (void)h; return ESP_OK; }

static esp_err_t get_raw(nvs_handle_t h, const char *key, void *out, size_t want) {
    auto &s = store(h); auto it = s.find(key ? key : "");
    if (it == s.end()) return ESP_ERR_NVS_NOT_FOUND;
    if (it->second.b.size() != want) return ESP_ERR_NVS_INVALID_LENGTH;
    memcpy(out, it->second.b.data(), want); return ESP_OK;
}
static esp_err_t set_raw(nvs_handle_t h, const char *key, const void *v, size_t n) {
    auto &s = store(h); Blob b; b.b.assign((const uint8_t *)v, (const uint8_t *)v + n);
    s[key ? key : ""] = std::move(b); return ESP_OK;
}
esp_err_t nvs_get_u8 (nvs_handle_t h, const char *k, uint8_t  *o) { return get_raw(h, k, o, 1); }
esp_err_t nvs_set_u8 (nvs_handle_t h, const char *k, uint8_t   v) { return set_raw(h, k, &v, 1); }
esp_err_t nvs_get_u16(nvs_handle_t h, const char *k, uint16_t *o) { return get_raw(h, k, o, 2); }
esp_err_t nvs_set_u16(nvs_handle_t h, const char *k, uint16_t  v) { return set_raw(h, k, &v, 2); }
esp_err_t nvs_get_u32(nvs_handle_t h, const char *k, uint32_t *o) { return get_raw(h, k, o, 4); }
esp_err_t nvs_set_u32(nvs_handle_t h, const char *k, uint32_t  v) { return set_raw(h, k, &v, 4); }
esp_err_t nvs_get_i32(nvs_handle_t h, const char *k, int32_t  *o) { return get_raw(h, k, o, 4); }
esp_err_t nvs_set_i32(nvs_handle_t h, const char *k, int32_t   v) { return set_raw(h, k, &v, 4); }

esp_err_t nvs_get_str(nvs_handle_t h, const char *k, char *out, size_t *len) {
    auto &s = store(h); auto it = s.find(k ? k : "");
    if (it == s.end()) return ESP_ERR_NVS_NOT_FOUND;
    const size_t need = it->second.b.size();
    if (!out) { if (len) *len = need; return ESP_OK; }
    if (!len || *len < need) { if (len) *len = need; return ESP_ERR_NVS_INVALID_LENGTH; }
    memcpy(out, it->second.b.data(), need); *len = need; return ESP_OK;
}
esp_err_t nvs_set_str(nvs_handle_t h, const char *k, const char *v) {
    return set_raw(h, k, v ? v : "", (v ? strlen(v) : 0) + 1);
}
esp_err_t nvs_get_blob(nvs_handle_t h, const char *k, void *out, size_t *len) {
    auto &s = store(h); auto it = s.find(k ? k : "");
    if (it == s.end()) return ESP_ERR_NVS_NOT_FOUND;
    const size_t need = it->second.b.size();
    if (!out) { if (len) *len = need; return ESP_OK; }
    if (!len || *len < need) { if (len) *len = need; return ESP_ERR_NVS_INVALID_LENGTH; }
    memcpy(out, it->second.b.data(), need); *len = need; return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t h, const char *k, const void *v, size_t n) { return set_raw(h, k, v, n); }
esp_err_t nvs_erase_key(nvs_handle_t h, const char *k) { store(h).erase(k ? k : ""); return ESP_OK; }
esp_err_t nvs_erase_all(nvs_handle_t h) { store(h).clear(); return ESP_OK; }

// ---- разделы -----------------------------------------------------------------------------------
const esp_partition_t *esp_partition_find_first(esp_partition_type_t t, esp_partition_subtype_t s, const char *label) {
    make_parts();
    for (auto &q : g_parts) {
        if (t != ESP_PARTITION_TYPE_ANY && q.p.type != t) continue;
        if (s != ESP_PARTITION_SUBTYPE_ANY && q.p.subtype != s) continue;
        if (label && strcmp(label, q.p.label) != 0) continue;
        return &q.p;
    }
    return nullptr;
}
static Part *owner(const esp_partition_t *p) {
    make_parts();
    for (auto &q : g_parts) if (&q.p == p) return &q;
    return nullptr;
}
esp_err_t esp_partition_read(const esp_partition_t *p, size_t off, void *dst, size_t n) {
    Part *q = owner(p); if (!q || off + n > q->data.size()) return ESP_ERR_INVALID_ARG;
    memcpy(dst, q->data.data() + off, n); return ESP_OK;
}
esp_err_t esp_partition_write(const esp_partition_t *p, size_t off, const void *src, size_t n) {
    Part *q = owner(p); if (!q || off + n > q->data.size()) return ESP_ERR_INVALID_ARG;
    // Настоящий флеш умеет только сбрасывать биты. Это не придирка: кольцо истории опирается на
    // «стёртое = 0xFF», и заглушка, позволяющая писать поверх, скрыла бы дефект переноса головы.
    for (size_t i = 0; i < n; i++) q->data[off + i] &= ((const uint8_t *)src)[i];
    return ESP_OK;
}
esp_err_t esp_partition_erase_range(const esp_partition_t *p, size_t off, size_t n) {
    Part *q = owner(p); if (!q || off + n > q->data.size()) return ESP_ERR_INVALID_ARG;
    memset(q->data.data() + off, 0xFF, n); return ESP_OK;
}


// ---- шина I2C ----------------------------------------------------------------------------------
// Отвечает только газомер. Это выбор, а не полумера: для расписания напряжение ячейки — вход, и
// настоящий max17048.cpp над этой заглушкой считает его из регистров сам. Для SCD41 и BME690
// пришлось бы эмулировать протокол с CRC и профилем нагревателя, а расписание от их ЗНАЧЕНИЙ не
// зависит — только от того, ответили они или нет. Поэтому они молчат, и это тоже проверяемая ветка.
struct i2c_master_bus_t { int port; };
struct i2c_master_dev_t { uint16_t addr; };

static i2c_master_bus_t g_bus = { 0 };
static i2c_master_dev_t g_devs[8];
static int g_ndev = 0;

esp_err_t i2c_new_master_bus(const i2c_master_bus_config_t *c, i2c_master_bus_handle_t *h) {
    (void)c;
    if (!Host::bus_ok) { if (h) *h = nullptr; return ESP_FAIL; }
    if (h) *h = &g_bus;
    return ESP_OK;
}
esp_err_t i2c_del_master_bus(i2c_master_bus_handle_t h) { (void)h; return ESP_OK; }
esp_err_t i2c_master_bus_reset(i2c_master_bus_handle_t h) { (void)h; return ESP_OK; }
esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t b, const i2c_device_config_t *c,
                                    i2c_master_dev_handle_t *d) {
    (void)b;
    if (!c || g_ndev >= 8) { if (d) *d = nullptr; return ESP_FAIL; }
    g_devs[g_ndev].addr = c->device_address;
    if (d) *d = &g_devs[g_ndev];
    g_ndev++;
    return ESP_OK;
}
esp_err_t i2c_master_bus_rm_device(i2c_master_dev_handle_t d) { (void)d; return ESP_OK; }
esp_err_t i2c_master_probe(i2c_master_bus_handle_t b, uint16_t addr, int t) {
    (void)b; (void)t;
    Host::i2c_probes++;
    if (!Host::bus_ok) return ESP_FAIL;   // мёртвая шина молчит ВСЯ, а не выборочно
    if (addr == 0x36) return Host::gauge_ok ? ESP_OK : ESP_FAIL;
    // Протокол этих двоих оснастка не эмулирует, а вот их ПРИСУТСТВИЕ — эмулирует, и оно важно:
    // именно по нему battery_source отличает плату без газомера от платы с мёртвым газомером.
    if (addr == 0x62 || addr == 0x76) return Host::sensors_present ? ESP_OK : ESP_FAIL;
    return ESP_FAIL;
}
esp_err_t i2c_master_transmit(i2c_master_dev_handle_t d, const uint8_t *w, size_t n, int t) {
    (void)w; (void)n; (void)t;
    if (!d) return ESP_FAIL;
    if (d->addr == 0x36) { Host::gauge_writes++; return Host::gauge_ok ? ESP_OK : ESP_FAIL; }
    return ESP_FAIL;                       // SCD41 и BME690 не отвечают
}
esp_err_t i2c_master_receive(i2c_master_dev_handle_t d, uint8_t *r, size_t n, int t) {
    (void)t;
    if (!d || !r) return ESP_FAIL;
    for (size_t i = 0; i < n; i++) r[i] = 0;
    return (d->addr == 0x36 && Host::gauge_ok) ? ESP_OK : ESP_FAIL;
}
esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t d, const uint8_t *w, size_t wn,
                                      uint8_t *r, size_t rn, int t) {
    (void)t;
    if (!d || !w || wn < 1 || !r) return ESP_FAIL;
    if (d->addr == 0x36) Host::gauge_reads++;
    if (d->addr != 0x36 || !Host::gauge_ok) return ESP_FAIL;
    // Регистры MAX17048, в тех же единицах, что в даташите: VCELL 78,125 мкВ, SOC 1/256 %,
    // CRATE 0,208 %/ч. Слово идёт старшим байтом вперёд, как читает readRegister().
    uint16_t v = 0;
    switch (w[0]) {
        case 0x02: v = (uint16_t)(Host::vcell / 0.000078125f); break;      // VCELL
        case 0x04: v = (uint16_t)(Host::soc * 256.0f);         break;      // SOC
        case 0x16: v = (uint16_t)(int16_t)(Host::crate / 0.208f); break;    // CRATE, со знаком
        case 0x08: v = 0x0012;                                 break;      // VERSION
        case 0x1A: v = 0x0000;                                 break;      // STATUS: флагов нет
        default:   v = 0x0000;                                 break;
    }
    for (size_t i = 0; i < rn; i++) r[i] = 0;
    if (rn >= 2) { r[0] = (uint8_t)(v >> 8); r[1] = (uint8_t)(v & 0xFF); }
    return ESP_OK;
}

// ---- часы --------------------------------------------------------------------------------------
// Перехват libc: проверено на этой машине, определение в объектном файле побеждает libSystem.
// Прошивка не знает об этом ничего — она по-прежнему зовёт time(NULL).
time_t time(time_t *t) { if (t) *t = Host::wall; return Host::wall; }
int gettimeofday(struct timeval *tv, void *tz) {
    (void)tz; if (tv) { tv->tv_sec = Host::wall; tv->tv_usec = (suseconds_t)(Host::mono_us % 1000000); }
    return 0;
}
// Часы, выставленные NTP: прошивка зовёт settimeofday(), и оснастка обязана это принять —
// иначе ветка «часы появились, отметки переносятся с монотонного таймера» недостижима.
int settimeofday(const struct timeval *tv, const struct timezone *tz) {
    (void)tz; if (tv) Host::wall = tv->tv_sec; return 0;
}
int64_t esp_timer_get_time(void) { return Host::mono_us; }
void esp_rom_delay_us(uint32_t us) { Host::advance((double)us / 1e6); }

// ---- FreeRTOS ----------------------------------------------------------------------------------
void vTaskDelay(TickType_t ticks) { Host::advance((double)ticks * portTICK_PERIOD_MS / 1000.0); }
TickType_t xTaskGetTickCount(void) { return (TickType_t)(Host::mono_us / 1000); }

// ---- выводы ------------------------------------------------------------------------------------
int gpio_get_level(gpio_num_t pin) {
    if (pin == 3)  return Host::usb_in     ? 0 : 1;   // USB_DETECT: НИЗКИЙ = кабель вставлен
    if (pin == 2)  return Host::button_down ? 0 : 1;  // кнопка:     НИЗКИЙ = нажата
    // Плата v2: STDBY# зарядника с открытым стоком. НИЗКИЙ = заряд завершён. Внешней подтяжки на
    // плате нет, поэтому отпущенная линия читается как ВЫСОКИЙ через внутреннюю — то есть
    // «ещё заряжает». Смысл обратен имени цепи на схеме (CHRG_DETECT) — см. battery_source.h.
    if (pin == 5)  return Host::stdby_low  ? 0 : 1;
    return 1;
}
esp_err_t gpio_set_level(gpio_num_t p, uint32_t l) { (void)p; (void)l; return ESP_OK; }
esp_err_t gpio_config(const gpio_config_t *c) { (void)c; return ESP_OK; }
esp_err_t gpio_set_direction(gpio_num_t p, gpio_mode_t m) { (void)p; (void)m; return ESP_OK; }
esp_err_t gpio_set_pull_mode(gpio_num_t p, int m) { (void)p; (void)m; return ESP_OK; }
esp_err_t gpio_set_intr_type(gpio_num_t p, gpio_int_type_t t) {
    if (p >= 0 && p < 31) Host::intr_type[p] = (int)t; return ESP_OK;
}
esp_err_t gpio_intr_enable(gpio_num_t p)  { if (p >= 0 && p < 31) Host::intr_enabled[p] = true;  return ESP_OK; }
esp_err_t gpio_intr_disable(gpio_num_t p) { if (p >= 0 && p < 31) Host::intr_enabled[p] = false; return ESP_OK; }
esp_err_t gpio_install_isr_service(int f) { (void)f; return ESP_OK; }
esp_err_t gpio_isr_handler_add(gpio_num_t p, gpio_isr_t fn, void *a) { (void)p; (void)fn; (void)a; return ESP_OK; }
esp_err_t gpio_isr_handler_remove(gpio_num_t p) { (void)p; return ESP_OK; }
esp_err_t gpio_hold_en(gpio_num_t p) { (void)p; return ESP_OK; }
esp_err_t gpio_hold_dis(gpio_num_t p) { (void)p; return ESP_OK; }
esp_err_t gpio_deep_sleep_hold_en(void) { return ESP_OK; }
esp_err_t gpio_deep_sleep_hold_dis(void) { return ESP_OK; }
esp_err_t gpio_wakeup_enable(gpio_num_t p, gpio_int_type_t t) {
    // На приборе этот вызов ПЕРЕПИСЫВАЕТ тип прерывания того же вывода и обратно его не возвращает
    // (gpio.c). Это стоило шторма прерываний на долгом нажатии 2026-09-19, поэтому заглушка ведёт
    // себя так же: иначе тест не смог бы поймать возврат этого дефекта.
    if (p >= 0 && p < 31) Host::intr_type[p] = (int)t;
    return ESP_OK;
}
esp_err_t gpio_wakeup_disable(gpio_num_t p) { (void)p; return ESP_OK; }
esp_err_t gpio_sleep_sel_dis(gpio_num_t p) { (void)p; return ESP_OK; }
esp_err_t gpio_sleep_sel_en(gpio_num_t p)  { (void)p; return ESP_OK; }
esp_err_t gpio_reset_pin(gpio_num_t p) { (void)p; return ESP_OK; }

// ---- сон и пробуждение -------------------------------------------------------------------------
esp_sleep_source_t esp_sleep_get_wakeup_cause(void) { return (esp_sleep_source_t)Host::wake_cause; }
uint64_t esp_sleep_get_gpio_wakeup_status(void) { return Host::gpio_wake_mask; }
esp_err_t esp_sleep_disable_wakeup_source(esp_sleep_source_t s) {
    if (s == ESP_SLEEP_WAKEUP_ALL) {
        Host::sources_cleared++;
        Host::armed_timer_us = 0; Host::armed_button = false;
        Host::armed_gpio_mask = 0; Host::armed_gpio_level = -1;
    }
    return ESP_OK;
}
esp_err_t esp_sleep_enable_timer_wakeup(uint64_t us) { Host::armed_timer_us = us; return ESP_OK; }
esp_err_t esp_deep_sleep_enable_gpio_wakeup(uint64_t mask, int level) {
    Host::armed_gpio_mask |= mask; Host::armed_gpio_level = level;
    if (mask & (1ULL << 2)) Host::armed_button = true;
    return ESP_OK;
}
esp_err_t esp_sleep_enable_gpio_wakeup(void) { return ESP_OK; }
esp_err_t esp_sleep_enable_ext1_wakeup(uint64_t m, int mode) { (void)m; (void)mode; return ESP_OK; }
void esp_deep_sleep_start(void) {
    Host::deep_sleeps++;
    throw HostDeepSleep{ Host::armed_timer_us, Host::armed_button };
}
esp_err_t esp_light_sleep_start(void) { return ESP_OK; }

// ---- система -----------------------------------------------------------------------------------
esp_reset_reason_t esp_reset_reason(void) { return (esp_reset_reason_t)Host::reset_reason; }
void esp_restart(void) { Host::restarts++; throw HostRestart{ Host::restarts }; }
esp_err_t esp_register_shutdown_handler(void (*fn)(void)) { (void)fn; return ESP_OK; }
size_t heap_caps_get_free_size(unsigned c) { (void)c; return Host::free_heap; }
size_t heap_caps_get_largest_free_block(unsigned c) { (void)c; return Host::free_heap / 2; }
size_t heap_caps_get_total_size(unsigned c) { (void)c; return 452 * 1024; }
size_t heap_caps_get_minimum_free_size(unsigned c) { (void)c; return Host::free_heap; }
esp_err_t esp_core_dump_image_erase(void) { return ESP_OK; }

// Детерминированный генератор: пароль точки доступа должен быть воспроизводим в прогоне, иначе
// проверка «ровно 8 знаков из разрешённого алфавита» падала бы через раз.
static uint32_t g_rng = 0x2545F491u;
uint32_t esp_random(void) { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5; return g_rng; }
void esp_fill_random(void *buf, size_t len) {
    uint8_t *b = (uint8_t *)buf;
    for (size_t i = 0; i < len; i++) b[i] = (uint8_t)(esp_random() & 0xFF);
}

// Описатель блокировки несёт свой тип: тест спрашивает, сколько блокировок «не спать» взято и не
// отдано, а это и есть условие, при котором лёгкий сон разрешён только внутри ожидания датчика.
static constexpr intptr_t LOCK_NO_LS = 0x4E4F4C53;   // 'NOLS'
static constexpr intptr_t LOCK_OTHER = 0x4F544852;   // 'OTHR'

// ---- менеджер питания --------------------------------------------------------------------------
esp_err_t esp_pm_configure(const void *cfg) {
    // Раскладка совпадает с esp_pm_config_t из заглушки: три поля, последнее — флаг.
    if (cfg) Host::light_sleep_on = ((const esp_pm_config_t *)cfg)->light_sleep_enable;
    return ESP_OK;
}
esp_err_t esp_pm_lock_create(esp_pm_lock_type_t t, int a, const char *n, esp_pm_lock_handle_t *h) {
    (void)a; (void)n;
    // Тип запоминается в самом описателе: тест спрашивает, сколько блокировок «не спать» взято.
    if (h) *h = (esp_pm_lock_handle_t)(intptr_t)(t == ESP_PM_NO_LIGHT_SLEEP ? LOCK_NO_LS : LOCK_OTHER);
    return ESP_OK;
}
esp_err_t esp_pm_lock_acquire(esp_pm_lock_handle_t h) {
    if ((intptr_t)h == LOCK_NO_LS) Host::no_ls_locks++;
    return ESP_OK;
}
esp_err_t esp_pm_lock_release(esp_pm_lock_handle_t h) {
    if ((intptr_t)h == LOCK_NO_LS) Host::no_ls_locks--;
    return ESP_OK;
}

// ---- OTA ---------------------------------------------------------------------------------------
static esp_partition_t g_running = { ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0,
                                     0x020000, 3072 * 1024, 4096, "app0", false };
const esp_partition_t *esp_ota_get_running_partition(void) { return &g_running; }
const esp_partition_t *esp_ota_get_next_update_partition(const esp_partition_t *p) {
    (void)p; return esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, nullptr);
}
const esp_partition_t *esp_ota_get_last_invalid_partition(void) { return nullptr; }
esp_err_t esp_ota_get_state_partition(const esp_partition_t *p, esp_ota_img_states_t *s) {
    (void)p; if (s) *s = ESP_OTA_IMG_VALID; return ESP_OK;
}
esp_err_t esp_ota_mark_app_valid_cancel_rollback(void) { Host::marked_valid++; return ESP_OK; }
esp_err_t esp_ota_mark_app_invalid_rollback_and_reboot(void) {
    Host::rollbacks++; throw HostRestart{ ++Host::restarts };
}
bool esp_ota_check_rollback_is_possible(void) { return true; }
esp_err_t esp_ota_set_boot_partition(const esp_partition_t *p) { (void)p; return ESP_OK; }
esp_err_t esp_ota_get_partition_description(const esp_partition_t *p, esp_app_desc_t *d) {
    (void)p; if (d) *d = *esp_app_get_description(); return ESP_OK;
}
const esp_app_desc_t *esp_app_get_description(void) {
    static esp_app_desc_t d;
    d.magic_word = 0xABCD5432; snprintf(d.version, sizeof(d.version), "%s", "0.10.16+81");
    snprintf(d.project_name, sizeof(d.project_name), "%s", "smart-meteo");
    memcpy(d.app_elf_sha256, Host::elf_sha, 32);
    return &d;
}

// ---- сеть: ничего не поднимается ---------------------------------------------------------------
esp_event_base_t WIFI_EVENT = "WIFI_EVENT";
esp_event_base_t IP_EVENT   = "IP_EVENT";
esp_netif_t host_netif = { 0 };
esp_flash_t *esp_flash_default_chip = nullptr;
bool httpd_uri_match_wildcard(const char *t, const char *u, size_t n) { (void)t; (void)u; (void)n; return false; }
esp_err_t esp_netif_sntp_init(const esp_sntp_config_t *c) { (void)c; return ESP_OK; }
void      esp_netif_sntp_deinit(void) {}
esp_err_t esp_netif_sntp_sync_wait(uint32_t t) { (void)t; return ESP_ERR_TIMEOUT; }

// ---- лог ---------------------------------------------------------------------------------------
void host_log(char level, const char *tag, const char *fmt, ...) {
    char buf[512];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    char line[600]; snprintf(line, sizeof(line), "%c %s: %s", level, tag ? tag : "?", buf);
    Host::log_lines.push_back(line);
}

}  // extern "C"

namespace Host {
void advance(double secs) {
    if (secs <= 0) return;
    mono_us += (int64_t)(secs * 1e6);
    if (wall > 0) wall += (time_t)secs;              // часов нет — они и не идут
}
bool logged(const char *needle) {
    for (auto &l : log_lines) if (l.find(needle) != std::string::npos) return true;
    return false;
}
int logged_count(const char *needle) {
    int n = 0; for (auto &l : log_lines) if (l.find(needle) != std::string::npos) n++;
    return n;
}
void clear_settings(void) { g_nvs.clear(); }
static std::map<std::string, Blob> &sm(void) { return g_nvs["smart_meteo"]; }
void set_u8 (const char *k, uint8_t  v) { Blob b; b.b.assign(&v, &v + 1); sm()[k] = b; }
void set_u32(const char *k, uint32_t v) { Blob b; b.b.assign((uint8_t*)&v, (uint8_t*)&v + 4); sm()[k] = b; }
void set_float(const char *k, float v) {
    // Как это делает прошивка: float хранится битовым образом U32 (nvs_manager.cpp).
    uint32_t u; memcpy(&u, &v, 4); set_u32(k, u);
}
void set_str(const char *k, const char *v) {
    Blob b; b.b.assign((const uint8_t*)v, (const uint8_t*)v + strlen(v) + 1); sm()[k] = b;
}
bool has_key(const char *k) { return sm().count(k) != 0; }

void reset(void) {
    wall = 1758700000; mono_us = 0;
    usb_in = false; button_down = false;
    mqtt_ok = false;
    bus_ok = true; gauge_ok = true; vcell = 3.95f; soc = 80.0f; crate = -0.5f; free_heap = 180 * 1024;
    sensors_present = true; adc_cali_ok = true; stdby_low = false; i2c_probes = 0; gauge_writes = 0; gauge_reads = 0;
    adc_spike_every = 0; adc_settle_bad = 0; adc_glitch_bursts = 0; adc_bursts = 0;
    adc_glitch_v = 0.0f; adc_burst_drift_v = 0.0f; adc_mv_per_code = 0.0f; adc_burst_noise_v = 0.0f;
    // На приборе каждое пробуждение — новая загрузка, и всё, что прошивка кэширует в статиках,
    // начинается заново. Здесь все случаи идут в одном процессе, поэтому «новую загрузку» надо
    // изобразить явно, иначе ревизия и поправка перетекали бы из проверки в проверку — тот самый
    // механизм ложной зелени, на котором Host::reset() уже попадался 29 сентября.
    BatterySource::forgetBoard();
    // Среднее по чтениям живёт в RTC_DATA: сон оно переживает, а включение — нет. Каждый случай
    // здесь начинается с включения.
    BatterySource::forgetSmoothing();
    reset_reason = 8; wake_cause = 4; gpio_wake_mask = 0;
    memset(elf_sha, 0, sizeof(elf_sha));
    armed_timer_us = 0; armed_gpio_mask = 0; armed_button = false; armed_gpio_level = -1;
    sources_cleared = 0; restarts = 0; deep_sleeps = 0;
    light_sleep_on = false; no_ls_locks = 0; rollbacks = 0; marked_valid = 0;
    memset(intr_type, 0, sizeof(intr_type)); memset(intr_enabled, 0, sizeof(intr_enabled));
    log_lines.clear();
    g_nvs.clear(); g_ns.clear();
    // Проинициализирован и пуст — ровно то, чем кончается factory_wipe():
    // nvs_flash_erase() и сразу nvs_flash_init(). «Не проинициализирован» — другое
    // состояние, и подставлять его под каждый случай значило бы тестировать не то.
    g_nvs_inited = true;
    g_parts.clear();
    g_ndev = 0;
    g_rng = 0x2545F491u;
}
}  // namespace Host

// ---- АЦП: делитель батареи платы v2 ------------------------------------------------------------
// Оснастка моделирует ФИЗИКУ — два резистора со схемы, — а не коэффициент из прошивки. Числа здесь
// и выражение в battery_source.h выведены из одного чертежа независимо, поэтому тест, который
// задаёт Host::vcell и ждёт его обратно, ловит ошибку в любом из двух.
struct adc_oneshot_unit_ctx { int dummy; };
struct adc_cali_ctx { int dummy; };
static adc_oneshot_unit_ctx g_adc_unit;
static adc_cali_ctx         g_adc_cali;

// Одна сессия АЦП = одна «вспышка» прошивки: по открытию блока видно, где кончается одна и
// начинается другая, и именно так провал под нагрузкой ложится ровно на одну вспышку.
static bool  g_adc_open = false;
static int   g_adc_since_cfg = 0;
static float g_adc_burst_v = 0.0f;
esp_err_t adc_oneshot_new_unit(const adc_oneshot_unit_init_cfg_t *cfg, adc_oneshot_unit_handle_t *out) {
    (void)cfg;
    if (g_adc_open) return ESP_ERR_NOT_FOUND;         // как драйвер: у блока один владелец
    g_adc_open = true;
    g_adc_burst_v = Host::vcell + Host::adc_burst_drift_v * (float)Host::adc_bursts
                  + ((Host::adc_bursts & 1) ? Host::adc_burst_noise_v : -Host::adc_burst_noise_v);
    if (Host::adc_glitch_bursts > 0) { g_adc_burst_v = Host::adc_glitch_v; Host::adc_glitch_bursts--; }
    Host::adc_bursts++;
    if (out) *out = &g_adc_unit;
    return ESP_OK;
}
esp_err_t adc_oneshot_config_channel(adc_oneshot_unit_handle_t h, adc_channel_t ch,
                                     const adc_oneshot_chan_cfg_t *cfg) {
    (void)h; (void)ch; (void)cfg; g_adc_since_cfg = 0; return ESP_OK;
}
esp_err_t adc_oneshot_read(adc_oneshot_unit_handle_t h, adc_channel_t ch, int *raw) {
    (void)h;
    if (!raw) return ESP_FAIL;
    if (ch != ADC_CHANNEL_4) return ESP_FAIL;         // на плате делитель только здесь
    const int k = g_adc_since_cfg++;
    if (k < Host::adc_settle_bad) { *raw = 0; return ESP_OK; }
    if (Host::adc_spike_every > 0 && k % Host::adc_spike_every == Host::adc_spike_every - 1) {
        *raw = ((k / Host::adc_spike_every) % 2) ? 0 : 4095;
        return ESP_OK;
    }
    // R23 1 МОм сверху, R12 390 кОм снизу (схема v2 от 2026-09-29). Сырое значение отдаём сразу в
    // милливольтах — пересчёт калибровки ниже тождественный.
    const double top = 1000e3, bot = 390e3;
    const double node_mv = g_adc_burst_v * (bot / (top + bot)) * 1000.0;
    *raw = Host::adc_mv_per_code > 0.0f ? (int)(node_mv / Host::adc_mv_per_code + 0.5)
                                        : (int)(node_mv + 0.5);
    return ESP_OK;
}
esp_err_t adc_oneshot_del_unit(adc_oneshot_unit_handle_t h) { (void)h; g_adc_open = false; return ESP_OK; }

esp_err_t adc_cali_raw_to_voltage(adc_cali_handle_t h, int raw, int *mv) {
    (void)h; if (!mv) return ESP_FAIL;
    *mv = Host::adc_mv_per_code > 0.0f ? (int)(raw * Host::adc_mv_per_code + 0.5) : raw;
    return ESP_OK;
}
esp_err_t adc_cali_create_scheme_curve_fitting(const adc_cali_curve_fitting_config_t *c, adc_cali_handle_t *h) {
    (void)c;
    if (!Host::adc_cali_ok) return ESP_FAIL;
    if (h) *h = &g_adc_cali;
    return ESP_OK;
}
esp_err_t adc_cali_delete_scheme_curve_fitting(adc_cali_handle_t h) { (void)h; return ESP_OK; }
