#include "ota_install.h"
#include "evt_log.h"
#include "ota_check.h"
#include "fw_version.h"
#include "nvs_manager.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_timer.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_task_wdt.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"
#include <string.h>
#include <vector>

static const char *TAG = "OTA_INST";

// The host is compiled in here too, and for the same reason it is compiled into the check: the
// manifest may only ever hand us a PATH. Even with the server fully taken over, the bytes have to
// come from this name and still have to carry our signature.
#define OTA_HOST "ota.smartmeteo.org"

// Anything outside this band is not our firmware and is refused before a single byte is written, so
// a wrong or hostile Content-Length cannot start an erase. The ceiling is the slot itself, read from
// the partition table: it was a hard-coded 2560K, and the table grew to 3072K slots (2026-09-16) —
// an image between the two would have been refused by a device that had room for it.
#define IMG_MIN   (256u * 1024u)

// The whole transfer, not one read. A server answering a byte at a time stays inside every
// individual timeout while running for minutes, and we are inside a wake budget with a watchdog
// underneath. Two minutes is generous for 1.7 MB on any working link and hopeless on a broken one,
// which is exactly the discrimination wanted.
#define XFER_DEADLINE_S  120

// Installing costs a long radio session and ends in a reboot. On a cell too low to be sure of
// finishing, the update is not the emergency — the charge is. The write itself is safe to lose
// (nothing boots the new slot until it is armed), so this threshold is about not spending the last
// of the charge on something that can simply wait for the next day.
#define MIN_SOC_PCT  40.0f

namespace OtaInstall {

char     why[32]  = {0};
volatile uint32_t fetched = 0;
volatile uint32_t total   = 0;
volatile State    state   = StIdle;
volatile bool local_upload = false;
char              phase[24] = {0};

static void set_phase(const char *p) { snprintf(phase, sizeof(phase), "%s", p); }

// Provided by the cycle: the fuel gauge reading it already took this wake, and whether the cable
// is in. Reading them here would mean opening I2C a second time in the same wake.
extern "C" float smart_meteo_soc(void);
extern "C" bool  smart_meteo_on_usb(void);

// The version of the last app partition marked invalid or aborted — by the bootloader, for an image that
// never confirmed itself, or by this firmware when it rolled back — or nullptr. Espressif's
// native_ota_example reads it the same way (esp_ota_get_last_invalid_partition,
// esp_ota_get_partition_description) and will not install that version again.
static const char *last_invalid_version(void)
{
    static esp_app_desc_t d;
    const esp_partition_t *p = esp_ota_get_last_invalid_partition();
    if (!p || esp_ota_get_partition_description(p, &d) != ESP_OK) return nullptr;
    d.version[sizeof(d.version) - 1] = '\0';
    return d.version;
}

const char *blockedReason(bool automatic)
{
    OtaCheck::restore();
    if (OtaCheck::available_seq <= (uint32_t)FW_SEQ) return "nothing newer is on offer";
    if (OtaCheck::available_path[0] == '\0')         return "the offer names no file";

    // The version that already failed here is never fetched again. settle_pending_image() writes
    // this after an image fails its first cycle, and without the check the device would download,
    // install, revert and re-download the same broken build every day for ever.
    uint32_t bad = 0;
    NVSManager::getInstance().getU32("ota_bad", bad, 0);
    if (bad == OtaCheck::available_seq) return "this build already failed on this device";
    // The same for an image that never ran long enough to write ota_bad: the bootloader marked it
    // aborted. run() checks the image's own header too; this answers the console without a download.
    if (const char *iv = last_invalid_version();
        iv && OtaCheck::available_ver[0] && strcmp(iv, OtaCheck::available_ver) == 0)
        return "this build already failed on this device";

    // Only the scheduled path. Pressing Install is the consent this switch exists to give, so
    // requiring the switch as well made the button refuse with the switch off — which is exactly
    // what happened, and the refusal said only "not allowed now".
    if (automatic) {
        uint8_t athro = 1;
        NVSManager::getInstance().getU8("ota_auto", athro, 1);
        if (!athro) return "automatic updates are off";
    }

    if (!smart_meteo_on_usb() && smart_meteo_soc() < MIN_SOC_PCT)
        return "battery too low — connect the cable";
    return nullptr;
}

bool due(bool automatic) { return blockedReason(automatic) == nullptr; }

// Implemented by the cycle: writing the gas calibration where the next image will look for it.
// A hook rather than a call, because the state that must survive an update lives in the cycle and
// this file has no business knowing about BSEC. Safe from either caller — the cycle's own version
// declines when the algorithm did not run this wake, which is exactly the console's case.
extern "C" void smart_meteo_ota_handoff(void);

Result run(bool automatic)
{
    // The automatic path must not start while a pressed install is downloading (its task owns the
    // slot) or has finished and is about to restart into it: esp_ota_begin() has no guard against a
    // second writer, and erasing the slot just made bootable would send the restart back to the old
    // image. The pressed path comes through start(), which already refuses a second press.
    if (automatic && (state == StRunning || state == StDone)) return Result::Idle;
    // Whoever asked — a press or the schedule — there is one slot and something else is writing it.
    if (local_upload) {
        ESP_LOGW(TAG, "an image is being uploaded over the local network — not installing on top of it");
        return Result::Idle;
    }
    why[0] = '\0';
    fetched = 0;
    total   = 0;
    // The caller has to say which path this is. start() has already set StRunning for a press, and
    // asking due() with its default (automatic) refused that press again whenever the owner had
    // automatic updates switched off. That early return, and the two below it, left the state at
    // StRunning for good: every later press was turned away as "already running", and the setup
    // window, which will not close under a running install, stayed open with the radio up.
    if (const char *r = blockedReason(automatic)) {
        if (state == StRunning) {
            snprintf(why, sizeof(why), "%s", r);
            set_phase(why);
            state = StFailed;
        }
        return Result::Idle;
    }
    state = StRunning;
    set_phase("Connecting");

    const esp_partition_t *dst = esp_ota_get_next_update_partition(NULL);
    if (!dst) {
        snprintf(why, sizeof(why), "no slot");
        set_phase(why);
        state = StFailed;
        return Result::Failed;
    }

    char url[192];
    snprintf(url, sizeof(url), "https://%s%s", OTA_HOST, OtaCheck::available_path);
    ESP_LOGW(TAG, "installing seq %u from %s into %s",
             (unsigned)OtaCheck::available_seq, OtaCheck::available_path, dst->label);

    esp_http_client_config_t cfg = {};
    cfg.url               = url;
    cfg.timeout_ms        = 10000;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;   // verified TLS, like every outbound call here
    cfg.transport_type    = HTTP_TRANSPORT_OVER_SSL;
    cfg.keep_alive_enable = true;
    esp_http_client_handle_t cl = esp_http_client_init(&cfg);
    if (!cl) {
        snprintf(why, sizeof(why), "http init");
        set_phase(why);
        state = StFailed;
        return Result::Failed;
    }

    esp_ota_handle_t h = 0;
    bool   ota_open = false;
    Result out      = Result::Failed;

    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);

    do {
        if (esp_http_client_open(cl, 0) != ESP_OK) { snprintf(why, sizeof(why), "connect"); break; }
        const int len = esp_http_client_fetch_headers(cl);
        const int st  = esp_http_client_get_status_code(cl);
        if (st != 200) { snprintf(why, sizeof(why), "http %d", st); break; }

        // Checked BEFORE esp_ota_begin, because that call erases. A length outside the band this
        // firmware can possibly be is refused while the other slot is still intact.
        if (len < (int)IMG_MIN || (size_t)len > dst->size) {
            snprintf(why, sizeof(why), "size %d", len);
            break;
        }

        total = (uint32_t)len;
        std::vector<uint8_t> buf(4096);
        const int64_t deadline = esp_timer_get_time() + (int64_t)XFER_DEADLINE_S * 1000000;

        // The image describes itself in its first bytes: esp_app_desc_t right after the image header and
        // the first segment header. Read before anything is erased, and refused when that version is the
        // one that last failed to boot here — native_ota_example's check, in its order.
        const size_t desc_at  = sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t);
        const int    head_len = (int)(desc_at + sizeof(esp_app_desc_t));
        int head = 0;
        while (head < head_len && esp_timer_get_time() <= deadline) {
            const int n = esp_http_client_read(cl, (char *)buf.data() + head, buf.size() - head);
            if (n <= 0) break;
            head += n;
        }
        if (head < head_len) { snprintf(why, sizeof(why), "short %d/%d", head, len); break; }
        esp_app_desc_t new_desc;
        memcpy(&new_desc, buf.data() + desc_at, sizeof(new_desc));
        new_desc.version[sizeof(new_desc.version) - 1] = '\0';
        ESP_LOGW(TAG, "image version %s", new_desc.version);
        if (const char *iv = last_invalid_version(); iv && strcmp(iv, new_desc.version) == 0) {
            ESP_LOGW(TAG, "version %s already failed to boot on this device — not installing it again",
                     new_desc.version);
            NVSManager::getInstance().setU32("ota_bad", OtaCheck::available_seq);
            snprintf(why, sizeof(why), "failed here before");
            break;
        }

        // The sequence number has to come out of the IMAGE, not out of the manifest.
        //
        // Signing covers the image bytes and nothing else: the manifest is plain JSON on the server.
        // Its `seq` was the only freshness test, and it was never checked against what actually
        // arrived — so a manifest saying {"seq": 999, "path": "…seq42.bin"} made the device fetch a
        // genuinely signed OLD release, verify its real signature, and install it happily. Every
        // past release is still served, at a predictable URL. Nothing beneath this catches it either:
        // CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK is off, which is a factory decision for later.
        //
        // So the version string carries the sequence after a '+' ("0.10.3+68"), the image declares
        // it, and both of these must hold: it is what the manifest promised, and it is newer than
        // what is running. A lying manifest now fails on the first, a replayed old image on the second.
        const char *plus = strrchr(new_desc.version, '+');
        const uint32_t img_seq = plus ? (uint32_t)strtoul(plus + 1, nullptr, 10) : 0u;
        if (!plus || img_seq == 0) {
            ESP_LOGW(TAG, "image version '%s' carries no sequence number", new_desc.version);
            snprintf(why, sizeof(why), "image not sequenced");
            break;
        }
        if (img_seq != OtaCheck::available_seq) {
            ESP_LOGW(TAG, "manifest offered seq %u, image is seq %u — refusing",
                     (unsigned)OtaCheck::available_seq, (unsigned)img_seq);
            snprintf(why, sizeof(why), "seq mismatch");
            break;
        }
        if (img_seq <= (uint32_t)FW_SEQ) {
            ESP_LOGW(TAG, "image seq %u is not newer than the running %u — refusing",
                     (unsigned)img_seq, (unsigned)FW_SEQ);
            snprintf(why, sizeof(why), "not newer");
            break;
        }

        // Erasing the far slot takes seconds and moves no counter; say so, or it reads as a hang.
        set_phase("Preparing flash");
        if (esp_ota_begin(dst, (size_t)len, &h) != ESP_OK) {
            snprintf(why, sizeof(why), "begin");
            break;
        }
        ota_open = true;

        set_phase("Downloading");
        // The bytes already read for the description are the start of the image.
        if (esp_ota_write(h, buf.data(), head) != ESP_OK) { snprintf(why, sizeof(why), "write @0"); break; }
        mbedtls_sha256_update(&sha, buf.data(), head);
        int got = head;
        fetched = (uint32_t)got;
        while (got < len) {
            if (esp_timer_get_time() > deadline) { snprintf(why, sizeof(why), "timeout"); break; }
            const int n = esp_http_client_read(cl, (char *)buf.data(), buf.size());
            if (n < 0) { snprintf(why, sizeof(why), "read"); break; }
            if (n == 0) { snprintf(why, sizeof(why), "short %d/%d", got, len); break; }
            if (esp_ota_write(h, buf.data(), n) != ESP_OK) {
                snprintf(why, sizeof(why), "write @%d", got);
                break;
            }
            mbedtls_sha256_update(&sha, buf.data(), n);
            got += n;
            fetched = (uint32_t)got;      // published as it goes, for the bar in the console
            // Minutes of transfer inside one wake, and the watchdog is subscribed for the whole of
            // it. Fed every chunk rather than every Nth: the cost is nothing and the alternative is
            // a reset in the middle of a flash write, which is the one moment worth protecting.
            esp_task_wdt_reset();
        }
        fetched = (uint32_t)got;
        if (got != len) { if (!why[0]) snprintf(why, sizeof(why), "short"); break; }

        // The manifest's digest is a cheap early check, not the security boundary — esp_ota_end()
        // verifies the ECDSA signature, and that is what actually decides whether these bytes may
        // ever run. This catches corruption before we spend the rest of the sequence on it, and it
        // catches a mismatch between what the manifest advertised and what was served.
        uint8_t digest[32];
        set_phase("Checking image");
        mbedtls_sha256_finish(&sha, digest);
        std::string want;
        NVSManager::getInstance().getString("ota_sha", want, "");
        // Required, not optional. A manifest that simply omitted the digest — or sent a short one —
        // used to skip this check entirely, which is a check that disappears exactly when somebody
        // controls the manifest and wants it gone.
        if (want.size() != 64) {
            ESP_LOGW(TAG, "manifest carries no usable sha256 (%u chars) — refusing",
                     (unsigned)want.size());
            snprintf(why, sizeof(why), "no digest");
            break;
        }
        {
            char hex[65];
            for (int i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", digest[i]);
            if (strcasecmp(hex, want.c_str()) != 0) {
                snprintf(why, sizeof(why), "sha mismatch");
                break;
            }
        }

        // Signature check happens in here. A build signed by anybody else — including one served by
        // a server that is entirely under someone else's control — is refused at this line.
        // The signature check walks the whole image: the longest silent stretch of the install.
        set_phase("Verifying signature");
        const esp_err_t e = esp_ota_end(h);
        ota_open = false;
        if (e != ESP_OK) {
            snprintf(why, sizeof(why), e == ESP_ERR_OTA_VALIDATE_FAILED ? "bad signature" : "end");
            break;
        }

        set_phase("Arming");
        if (esp_ota_set_boot_partition(dst) != ESP_OK) {
            snprintf(why, sizeof(why), "arm");
            break;
        }

        ESP_LOGW(TAG, "seq %u written and armed (%d bytes) — reboot to take it",
                 (unsigned)OtaCheck::available_seq, got);
        out = Result::Installed;
        // Which version went in, in the ring that survives the restart it is about to cause.
        EvtLog::getInstance().event(EVT_SEC, SEC_OTA_INSTALL | (OtaCheck::available_seq << 8));
    } while (0);

    if (ota_open) esp_ota_abort(h);
    mbedtls_sha256_free(&sha);
    esp_http_client_close(cl);
    esp_http_client_cleanup(cl);

    if (out != Result::Installed)
        ESP_LOGE(TAG, "install failed: %s (%u bytes fetched)", why, (unsigned)fetched);
    if (out != Result::Installed) set_phase(why[0] ? why : "Failed");
    state = (out == Result::Installed) ? StDone : StFailed;
    return out;
}

// The worker behind start(). Ends the same way the scheduled path does — hand the calibration over,
// then restart — because an update that lands without it arrives at a new image with no idea what
// this room's air is normally like, and spends a day saying "Warming up".
static void install_task(void *)
{
    // Subscribed for the same reason the transfer feeds the watchdog every chunk: this task now
    // owns minutes of downloading, and an unsubscribed task calling esp_task_wdt_reset() would log
    // an error on every one of four hundred chunks.
    esp_task_wdt_add(NULL);
    const Result r = run(false);   // manual: the press is the consent, exactly as in start()
    esp_task_wdt_delete(NULL);

    if (r == Result::Installed) {
        set_phase("Saving calibration");
        smart_meteo_ota_handoff();
        set_phase("Restarting");
        ESP_LOGW(TAG, "restarting into the new image");
        // Long enough for the console to poll one more time and show the bar full, and for the
        // reply that started this to have left the socket. The owner watching a progress bar
        // vanish at 98% cannot tell success from a device that fell over.
        // Long enough that a page polling once a second is certain to catch the final state
        // before the socket dies. Vanishing at 100 %% with nothing said is what got reported.
        vTaskDelay(pdMS_TO_TICKS(2500));
        esp_restart();
    }
    vTaskDelete(NULL);
}

bool start(void)
{
    if (state == StRunning) return false;   // one at a time; the second press is a no-op
    if (!due(false))        return false;   // manual: the press is the consent
    fetched = 0;
    total   = 0;
    why[0]  = '\0';
    set_phase("Starting");
    state   = StRunning;
    // 8 KB: TLS needs most of it, and the 4 KB transfer buffer is on the heap, not here.
    if (xTaskCreate(install_task, "ota_install", 8192, NULL, 4, NULL) != pdPASS) {
        snprintf(why, sizeof(why), "no task");
        state = StFailed;
        return false;
    }
    return true;
}

}  // namespace OtaInstall
