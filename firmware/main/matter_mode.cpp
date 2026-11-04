// Matter over Thread for Smart Meteo — see matter_mode.h.
//
// The endpoint set and the way each cluster is written are the ones proven on the bench
// (Software/matter_bench, CLAUDE.md journal 2026-09-16). The pieces that are new here are marked.
#include "matter_mode.h"

#include <atomic>
#include <cmath>
#include <cstring>
#include <cstdio>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "bootloader_random.h"
#include "nvs.h"
#include "sdkconfig.h"
#include "nvs_manager.h"
#include "qrcodegen.h"

#include <esp_matter.h>
#include <esp_matter_core.h>
#include <esp_matter_providers.h>
#include <data_model_provider/esp_matter_data_model_provider.h>
#include <app/ConcreteClusterPath.h>
#include <app/clusters/air-quality-server/AirQualityCluster.h>
#include <app/clusters/temperature-measurement-server/TemperatureMeasurementCluster.h>
#include <clusters/relative_humidity_measurement/integration.h>
#include <app/server/CommissioningWindowManager.h>
#include <app/server/Server.h>
#include <crypto/CHIPCryptoPAL.h>
#include <platform/CommissionableDataProvider.h>
#include <lib/support/Span.h>
#include <platform/ConnectivityManager.h>
#include <platform/ESP32/OpenthreadLauncher.h>
#include <setup_payload/OnboardingCodesUtil.h>
#include <setup_payload/SetupPayload.h>

static const char *TAG = "MATTER";

using namespace esp_matter;
using namespace chip::app::Clusters;

namespace {

// ---- per-device setup codes --------------------------------------------------------------------
// The example provider esp-matter selects without a factory partition hands every unit the same
// passcode (20202021) and discriminator (3840). Anybody within Bluetooth range of any Smart Meteo
// with an open window could then pair it. So each device draws its own, once.
//
// The PASE verifier is derived from the passcode the way CHIP's own Linux provider does it
// (examples/platform/linux/LinuxCommissionableDataProvider.cpp): Spake2pVerifier::Generate over a
// random salt. 10000 PBKDF2 iterations — the figure Espressif's manufacturing tool uses; the spec
// minimum is 1000.
constexpr uint32_t kIterations = 10000;
constexpr size_t   kSaltLen    = 32;

struct Commissionable {
    bool     loaded = false;
    uint32_t pin = 0;
    uint16_t disc = 0;
    uint8_t  salt[kSaltLen] = {};
};
Commissionable s_cd;

// A random number the RNG can vouch for. With no radio running, ESP-IDF's generator is only
// pseudo-random unless the internal entropy source is switched on around the call; with a radio on,
// that source must NOT be switched on (it shares the SAR ADC with the RF calibration).
void fill_random(uint8_t *buf, size_t len, bool rf_active)
{
    if (!rf_active) bootloader_random_enable();
    esp_fill_random(buf, len);
    if (!rf_active) bootloader_random_disable();
}

bool load_commissionable(bool rf_active)
{
    if (s_cd.loaded) return true;
    NVSManager &n = NVSManager::getInstance();
    if (!n.init()) return false;
    uint32_t pin = 0, disc = 0;
    uint8_t salt[kSaltLen] = {};
    size_t salt_len = sizeof(salt);
    const bool have = n.getU32("mt_pin", pin, 0) && n.getU32("mt_disc", disc, 0xFFFFFFFFu) &&
                      n.getBlob("mt_salt", salt, salt_len) && salt_len == kSaltLen &&
                      chip::PayloadContents::IsValidSetupPIN(pin) && disc <= 0xFFF;
    if (!have) {
        // A passcode the spec allows: 1..99999998, and none of the trivial ones IsValidSetupPIN
        // refuses (00000000, 11111111, ..., 12345678, 87654321).
        do {
            uint8_t r[4];
            fill_random(r, sizeof(r), rf_active);
            pin = ((uint32_t)r[0] << 24 | (uint32_t)r[1] << 16 | (uint32_t)r[2] << 8 | r[3]) % 99999998u + 1u;
        } while (!chip::PayloadContents::IsValidSetupPIN(pin));
        uint8_t d[2];
        fill_random(d, sizeof(d), rf_active);
        disc = ((uint32_t)d[0] << 8 | d[1]) & 0xFFFu;
        fill_random(salt, sizeof(salt), rf_active);
        if (!(n.setU32("mt_pin", pin) && n.setU32("mt_disc", disc) && n.setBlob("mt_salt", salt, sizeof(salt)))) {
            ESP_LOGE(TAG, "could not store the setup code — not using a code that would change next boot");
            return false;
        }
        ESP_LOGW(TAG, "setup code generated for this device (discriminator %u)", (unsigned)disc);
    }
    s_cd.pin = pin;
    s_cd.disc = (uint16_t)disc;
    memcpy(s_cd.salt, salt, kSaltLen);
    s_cd.loaded = true;
    return true;
}

class DeviceCommissionableData : public chip::DeviceLayer::CommissionableDataProvider {
public:
    CHIP_ERROR GetSetupDiscriminator(uint16_t &v) override
    {
        VerifyOrReturnError(load_commissionable(true), CHIP_ERROR_INCORRECT_STATE);
        v = s_cd.disc;
        return CHIP_NO_ERROR;
    }
    CHIP_ERROR SetSetupDiscriminator(uint16_t) override { return CHIP_ERROR_NOT_IMPLEMENTED; }
    CHIP_ERROR GetSpake2pIterationCount(uint32_t &v) override { v = kIterations; return CHIP_NO_ERROR; }
    CHIP_ERROR GetSpake2pSalt(chip::MutableByteSpan &buf) override
    {
        VerifyOrReturnError(load_commissionable(true), CHIP_ERROR_INCORRECT_STATE);
        return chip::CopySpanToMutableSpan(chip::ByteSpan(s_cd.salt, kSaltLen), buf);
    }
    CHIP_ERROR GetSpake2pVerifier(chip::MutableByteSpan &buf, size_t &out_len) override
    {
        VerifyOrReturnError(load_commissionable(true), CHIP_ERROR_INCORRECT_STATE);
        out_len = chip::Crypto::kSpake2p_VerifierSerialized_Length;
        VerifyOrReturnError(buf.size() >= out_len, CHIP_ERROR_BUFFER_TOO_SMALL);
        if (!mHaveVerifier) {
            // Computed once per boot, on the Matter thread, the first time a window needs it.
            const int64_t t0 = esp_timer_get_time();
            chip::Crypto::Spake2pVerifier v;
            ReturnErrorOnFailure(v.Generate(kIterations, chip::ByteSpan(s_cd.salt, kSaltLen), s_cd.pin));
            chip::MutableByteSpan ser(mVerifier, sizeof(mVerifier));
            ReturnErrorOnFailure(v.Serialize(ser));
            mHaveVerifier = true;
            ESP_LOGW(TAG, "PASE verifier derived in %lld ms", (long long)((esp_timer_get_time() - t0) / 1000));
        }
        return chip::CopySpanToMutableSpan(chip::ByteSpan(mVerifier, out_len), buf);
    }
    CHIP_ERROR GetSetupPasscode(uint32_t &v) override
    {
        VerifyOrReturnError(load_commissionable(true), CHIP_ERROR_INCORRECT_STATE);
        v = s_cd.pin;
        return CHIP_NO_ERROR;
    }
    CHIP_ERROR SetSetupPasscode(uint32_t) override { return CHIP_ERROR_NOT_IMPLEMENTED; }

private:
    uint8_t mVerifier[chip::Crypto::kSpake2p_VerifierSerialized_Length] = {};
    bool    mHaveVerifier = false;
};
DeviceCommissionableData s_provider;

// ---- stack state ---------------------------------------------------------------------------------
bool     s_running = false;
uint16_t s_air_ep = 0, s_temp_ep = 0, s_hum_ep = 0;
std::atomic<uint32_t> s_events{0};
std::atomic<bool>     s_session{false};
void (*s_hook)(void) = nullptr;

void raise(uint32_t ev)
{
    s_events.fetch_or(ev);
    if (s_hook) s_hook();
}

void store_fabric_count(uint8_t n)
{
    uint8_t old = 0xFF;
    NVSManager::getInstance().getU8("mt_fab", old, 0xFF);
    if (old != n) NVSManager::getInstance().setU8("mt_fab", n);
}

// Runs on the Matter thread with the stack lock held: nothing here may block or take that lock.
void on_event(const chip::DeviceLayer::ChipDeviceEvent *event, intptr_t)
{
    namespace T = chip::DeviceLayer::DeviceEventType;   // CHIP's public events and esp_matter.h's own
    switch (event->Type) {
    case T::kCommissioningComplete:
        ESP_LOGW(TAG, "commissioning complete — the device is in a smart home now");
        s_session = false;
        store_fabric_count(chip::Server::GetInstance().GetFabricTable().FabricCount());
        raise(MatterMode::EV_PAIRED);
        break;
    case T::kFailSafeTimerExpired:
        ESP_LOGW(TAG, "commissioning abandoned: fail-safe timer expired");
        s_session = false;
        raise(MatterMode::EV_FAILSAFE);
        break;
    case T::kCommissioningSessionStarted:
        ESP_LOGW(TAG, "a commissioner connected");
        s_session = true;
        raise(MatterMode::EV_SESSION);
        break;
    case T::kCommissioningSessionStopped:
        ESP_LOGW(TAG, "commissioning session stopped");
        s_session = false;
        raise(MatterMode::EV_SESSION);
        break;
    case T::kCommissioningWindowOpened:
        ESP_LOGW(TAG, "commissioning window opened");
        raise(MatterMode::EV_WINDOW);
        break;
    case T::kCommissioningWindowClosed:
        ESP_LOGW(TAG, "commissioning window closed");
        raise(MatterMode::EV_WINDOW);
        break;
    case T::kFabricRemoved: {
        // CHIP reports every deleted fabric, including the half-added one it reverts when a commissioning
        // attempt fails after AddNOC (FabricTable::Delete). Only losing a fabric that was committed means
        // the device has left its smart home; the stored count says whether there was one.
        uint8_t before = 0;
        NVSManager::getInstance().getU8("mt_fab", before, 0);
        const uint8_t n = chip::Server::GetInstance().GetFabricTable().FabricCount();
        ESP_LOGW(TAG, "a fabric was removed — %u left (%u before)", (unsigned)n, (unsigned)before);
        store_fabric_count(n);
        if (n == 0 && before > 0) raise(MatterMode::EV_LAST_FABRIC);
        break;
    }
    case T::kFabricCommitted:
        store_fabric_count(chip::Server::GetInstance().GetFabricTable().FabricCount());
        break;
    case T::kThreadConnectivityChange:
        ESP_LOGW(TAG, "Thread %s", event->ThreadConnectivityChange.Result == chip::DeviceLayer::kConnectivity_Established
                                       ? "attached" : "detached");
        break;
    default:
        break;
    }
}

esp_err_t on_identify(identification::callback_type_t type, uint16_t endpoint_id, uint8_t, uint8_t, void *)
{
    ESP_LOGW(TAG, "identify request (type %u) on endpoint %u", (unsigned)type, endpoint_id);
    return ESP_OK;
}

// The endpoints are read-only sensors. attribute::update() runs this for our own CO2 publish as well
// (esp_matter_data_model.cpp), so it cannot tell a write from the fabric apart — debug level only.
esp_err_t on_attribute(attribute::callback_type_t type, uint16_t endpoint_id, uint32_t cluster_id,
                       uint32_t attribute_id, esp_matter_attr_val_t *, void *)
{
    if (type == attribute::PRE_UPDATE)
        ESP_LOGD(TAG, "attribute update: ep %u cluster 0x%04lx attr 0x%04lx", endpoint_id,
                 (unsigned long)cluster_id, (unsigned long)attribute_id);
    return ESP_OK;
}

// The CHIP cluster object serving (endpoint, cluster) — the same registry esp_matter's set_val()
// consults when it refuses to write such a cluster (bench finding, 2026-09-16).
template <typename T>
T *sci_cluster(uint16_t endpoint_id, uint32_t cluster_id)
{
    auto *c = esp_matter::data_model::provider::get_instance().registry().Get(
        chip::app::ConcreteClusterPath(endpoint_id, cluster_id));
    return static_cast<T *>(c);
}

} // namespace

// ================================================================================================
namespace MatterMode {

bool selected()
{
    // The console's connectivity choice: 0 Wi-Fi, 1 Wi-Fi + MQTT hub, 2 Matter over Thread.
    uint8_t v = 0;
    NVSManager::getInstance().getU8("wl_mode", v, 0);
    return v == 2;
}

bool codes(Codes &out, bool rf_active)
{
    memset(&out, 0, sizeof(out));
    if (!load_commissionable(rf_active)) return false;
    chip::PayloadContents p;
    p.version           = 0;
    p.vendorID          = CONFIG_DEVICE_VENDOR_ID;
    p.productID         = CONFIG_DEVICE_PRODUCT_ID;
    p.commissioningFlow = chip::CommissioningFlow::kStandard;
    // Bluetooth is how a device that has never seen the home network is found; it is also what
    // the stack advertises whenever the window is open and nothing is paired.
    p.rendezvousInformation.SetValue(chip::RendezvousInformationFlags(chip::RendezvousInformationFlag::kBLE));
    p.discriminator.SetLongValue(s_cd.disc);
    p.setUpPINCode = s_cd.pin;

    chip::MutableCharSpan qr(out.qr, sizeof(out.qr));
    if (GetQRCode(qr, p) != CHIP_NO_ERROR) { ESP_LOGE(TAG, "QR payload refused"); return false; }
    chip::MutableCharSpan man(out.manual, sizeof(out.manual));
    if (GetManualPairingCode(man, p) != CHIP_NO_ERROR) { ESP_LOGE(TAG, "manual code refused"); return false; }
    // 11 digits, grouped 4-3-4 as the Matter onboarding guidelines print them.
    const size_t n = strnlen(out.manual, sizeof(out.manual));
    if (n == 11) snprintf(out.manual_fmt, sizeof(out.manual_fmt), "%.4s-%.3s-%.4s", out.manual, out.manual + 4, out.manual + 7);
    else         snprintf(out.manual_fmt, sizeof(out.manual_fmt), "%s", out.manual);
    return true;
}

int qr_grid(const char *text, uint8_t *grid, size_t grid_len)
{
    uint8_t qr[qrcodegen_BUFFER_LEN_FOR_VERSION(10)];
    uint8_t tmp[qrcodegen_BUFFER_LEN_FOR_VERSION(10)];
    // Medium error correction, raised further when it still fits: e-ink is sharp, but a phone held
    // at an angle to a matte panel is not.
    if (!qrcodegen_encodeText(text, tmp, qr, qrcodegen_Ecc_MEDIUM, 1, 10, qrcodegen_Mask_AUTO, true))
        return 0;
    const int size = qrcodegen_getSize(qr);
    if ((size_t)(size * size) > grid_len) return 0;
    for (int y = 0; y < size; y++)
        for (int x = 0; x < size; x++)
            grid[y * size + x] = qrcodegen_getModule(qr, x, y) ? 1 : 0;
    return size;
}

bool qr_svg(std::string &out, bool rf_active)
{
    Codes c;
    if (!codes(c, rf_active)) return false;
    static uint8_t grid[29 * 29 * 4];      // one caller at a time: the web server's task
    const int size = qr_grid(c.qr, grid, sizeof(grid));
    if (!size) return false;
    const int q = 4, full = size + 2 * q;     // quiet zone of four modules, as the standard asks
    char head[200];
    snprintf(head, sizeof(head),
             "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 %d %d\" shape-rendering=\"crispEdges\">"
             "<rect width=\"%d\" height=\"%d\" fill=\"#fff\"/><path fill=\"#000\" d=\"", full, full, full, full);
    out = head;
    char cell[40];     // two ints of any size: the compiler checks the worst case
    for (int y = 0; y < size; y++)
        for (int x = 0; x < size; x++)
            if (grid[y * size + x]) {
                snprintf(cell, sizeof(cell), "M%d %dh1v1h-1z", x + q, y + q);
                out += cell;
            }
    out += "\"/></svg>";
    return true;
}

bool start()
{
    if (s_running) return true;
    const int64_t t0 = esp_timer_get_time();
    // Codes before the radio comes up: the internal entropy source may only be used while no radio
    // runs, and the stack asks for them as soon as it opens the window.
    if (!load_commissionable(false)) return false;
    esp_matter::set_custom_commissionable_data_provider(&s_provider);

    node::config_t node_config;
    node_t *node = node::create(&node_config, on_attribute, on_identify);
    if (!node) { ESP_LOGE(TAG, "could not create the node"); return false; }

    // New in the product: Time Synchronization on the root endpoint. A commissioner that supports it
    // sets UTC time during commissioning, and CHIP writes that straight into the system clock
    // (TimeSynchronizationCluster.cpp, UpdateUTCTime -> SetClock_RealTime). Whether Google Home does
    // is exactly what the log line in the cycle ("clock set while in Matter mode") will tell.
    {
        endpoint_t *root = endpoint::get(node, 0);
        cluster::time_synchronization::config_t ts;
        if (!root || !cluster::time_synchronization::create(root, &ts, CLUSTER_FLAG_SERVER))
            ESP_LOGE(TAG, "no Time Synchronization cluster — the clock will not come from the hub");
    }

    // Air quality with the CO2 concentration beside it (bench, stage 1).
    endpoint::air_quality_sensor::config_t air_config;
    air_config.air_quality.air_quality = 0;          // unknown until the first publish
    endpoint_t *air_ep = endpoint::air_quality_sensor::create(node, &air_config, ENDPOINT_FLAG_NONE, nullptr);
    if (!air_ep) { ESP_LOGE(TAG, "could not create the air quality endpoint"); return false; }
    {
        // A zero feature map allows only Unknown, Good and Poor; the other four levels each need their bit.
        cluster_t *aq = cluster::get(air_ep, AirQuality::Id);
        if (!aq) { ESP_LOGE(TAG, "no air quality cluster"); return false; }
        cluster::air_quality::feature::fair::add(aq);
        cluster::air_quality::feature::moderate::add(aq);
        cluster::air_quality::feature::very_poor::add(aq);
        cluster::air_quality::feature::extremely_poor::add(aq);

        // Declared in feature_flags BEFORE create(): created without it, esp-matter asserts.
        cluster::carbon_dioxide_concentration_measurement::config_t co2_config;
        co2_config.feature_flags = cluster::concentration_measurement::feature::numeric_measurement::get_id();
        if (!cluster::carbon_dioxide_concentration_measurement::create(air_ep, &co2_config, CLUSTER_FLAG_SERVER)) {
            ESP_LOGE(TAG, "could not create the CO2 cluster"); return false;
        }
    }
    endpoint::temperature_sensor::config_t temp_config;
    endpoint_t *temp_ep = endpoint::temperature_sensor::create(node, &temp_config, ENDPOINT_FLAG_NONE, nullptr);
    endpoint::humidity_sensor::config_t hum_config;
    endpoint_t *hum_ep = endpoint::humidity_sensor::create(node, &hum_config, ENDPOINT_FLAG_NONE, nullptr);
    if (!temp_ep || !hum_ep) { ESP_LOGE(TAG, "could not create the climate endpoints"); return false; }

    s_air_ep  = endpoint::get_id(air_ep);
    s_temp_ep = endpoint::get_id(temp_ep);
    s_hum_ep  = endpoint::get_id(hum_ep);

    // OpenThread on the C6's own radio, settings in our NVS partition (icd_app's platform config).
    esp_openthread_platform_config_t ot = {};
    ot.radio_config.radio_mode = RADIO_MODE_NATIVE;
    ot.host_config.host_connection_mode = HOST_CONNECTION_MODE_NONE;
    ot.port_config.storage_partition_name = "nvs";
    ot.port_config.netif_queue_size = 10;
    ot.port_config.task_queue_size = 10;
    set_openthread_platform_config(&ot);

    const esp_err_t e = esp_matter::start(on_event);
    if (e != ESP_OK) { ESP_LOGE(TAG, "esp_matter::start failed: %s", esp_err_to_name(e)); return false; }
    s_running = true;

    uint8_t fab = 0;
    {
        esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
        fab = chip::Server::GetInstance().GetFabricTable().FabricCount();
    }
    store_fabric_count(fab);
    ESP_LOGW(TAG, "stack up in %lld ms | endpoints air %u, temperature %u, humidity %u | %u fabric(s)",
             (long long)((esp_timer_get_time() - t0) / 1000), s_air_ep, s_temp_ep, s_hum_ep, (unsigned)fab);
    return true;
}

bool running() { return s_running; }

void publish(const Readings &r)
{
    if (!s_running) return;
    // CO2 lives in esp_matter's attribute store (no CHIP server object of its own), and update()
    // takes the stack lock itself — so it is written before, not inside, the lock below.
    {
        esp_matter_attr_val_t v = r.co2_valid ? esp_matter_nullable_float(nullable<float>(r.co2))
                                              : esp_matter_nullable_float(nullable<float>());
        const esp_err_t e = attribute::update(s_air_ep, CarbonDioxideConcentrationMeasurement::Id,
                                              CarbonDioxideConcentrationMeasurement::Attributes::MeasuredValue::Id, &v);
        if (e != ESP_OK) ESP_LOGE(TAG, "CO2 publish failed: %s", esp_err_to_name(e));
    }
    esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
    if (auto *c = sci_cluster<AirQualityCluster>(s_air_ep, AirQuality::Id)) {
        int aq = r.air_quality;
        if (aq < 0 || aq > 6) aq = 0;
        const auto st = c->SetAirQuality(static_cast<AirQuality::AirQualityEnum>(aq));
        if (st != chip::Protocols::InteractionModel::Status::Success)
            ESP_LOGE(TAG, "air quality %d refused (status %u)", aq, static_cast<unsigned>(st));
    } else {
        ESP_LOGE(TAG, "no air quality cluster object on endpoint %u", s_air_ep);
    }
    if (auto *c = sci_cluster<TemperatureMeasurementCluster>(s_temp_ep, TemperatureMeasurement::Id)) {
        const CHIP_ERROR e = r.climate_valid
            ? c->SetMeasuredValue(chip::app::DataModel::MakeNullable((int16_t)lroundf(r.temp_c * 100.0f)))
            : c->SetMeasuredValue(chip::app::DataModel::Nullable<int16_t>());
        if (e != CHIP_NO_ERROR) ESP_LOGE(TAG, "temperature publish: %" CHIP_ERROR_FORMAT, e.Format());
    } else {
        ESP_LOGE(TAG, "no temperature cluster object on endpoint %u", s_temp_ep);
    }
    const CHIP_ERROR he = r.climate_valid
        ? RelativeHumidityMeasurement::SetMeasuredValue(s_hum_ep,
              chip::app::DataModel::MakeNullable((uint16_t)lroundf(r.hum * 100.0f)))
        : RelativeHumidityMeasurement::SetMeasuredValue(s_hum_ep, chip::app::DataModel::Nullable<uint16_t>());
    if (he != CHIP_NO_ERROR) ESP_LOGE(TAG, "humidity publish: %" CHIP_ERROR_FORMAT, he.Format());
    ESP_LOGW(TAG, "published: CO2 %s%.0f, %.1f C, %.0f %%, air quality %d",
             r.co2_valid ? "" : "(none) ", r.co2, r.temp_c, r.hum, r.air_quality);
}

uint8_t fabrics()
{
    if (s_running) {
        esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
        return chip::Server::GetInstance().GetFabricTable().FabricCount();
    }
    uint8_t n = 0;
    NVSManager::getInstance().getU8("mt_fab", n, 0);
    return n;
}

bool window_open()
{
    if (!s_running) return false;
    esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
    return chip::Server::GetInstance().GetCommissioningWindowManager().IsCommissioningWindowOpen();
}

bool session_active() { return s_session.load(); }

bool open_window()
{
    if (!s_running) return false;
    esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
    auto &mgr = chip::Server::GetInstance().GetCommissioningWindowManager();
    if (mgr.IsCommissioningWindowOpen()) return true;
    const CHIP_ERROR e = mgr.OpenBasicCommissioningWindow();   // CHIP_DEVICE_CONFIG_DISCOVERY_TIMEOUT_SECS, BLE + DNS-SD
    if (e != CHIP_NO_ERROR) {
        ESP_LOGE(TAG, "could not reopen the commissioning window: %" CHIP_ERROR_FORMAT, e.Format());
        return false;
    }
    return true;
}

bool thread_attached()
{
    if (!s_running) return false;
    esp_matter::lock::ScopedChipStackLock lock(portMAX_DELAY);
    return chip::DeviceLayer::ConnectivityMgr().IsThreadAttached();
}

uint32_t take_events() { return s_events.exchange(0); }

void set_event_hook(void (*hook)(void)) { s_hook = hook; }

void forget_and_restart()
{
    ESP_LOGW(TAG, "leaving the smart home: erasing fabrics and the Thread network, then restarting");
    NVSManager::getInstance().setU8("mt_fab", 0);
    esp_matter::factory_reset();          // erases and restarts from the Matter thread
    for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
}

void request_forget(bool on) { NVSManager::getInstance().setU8("mt_forget", on ? 1 : 0); }

bool forget_requested()
{
    uint8_t v = 0;
    NVSManager::getInstance().getU8("mt_forget", v, 0);
    return v != 0;
}

void clear_forget() { NVSManager::getInstance().setU8("mt_forget", 0); }

} // namespace MatterMode
