#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include "esp_err.h"
#include "esp_http_client.h"
#include "esp_timer.h"

// Outbound HTTPS for the weather, Telegram and the update check, written once.
//
// Every request is verified against the mbedTLS certificate bundle, so an API key or a bot token never
// leaves the device in clear text and the far end is the genuine endpoint.
//
// A body is read under one deadline for the whole transfer. The client's own timeout bounds a single
// read, and a server that answers a byte at a time keeps every read inside it while the fetch runs for
// minutes. That is hardly an attack surface given the verified certificate, but a congested network
// does the same by accident, and these requests run inside a wake budget with a watchdog underneath.
namespace Https {

// One response header to keep, copied while the headers are read (HTTP_EVENT_ON_HEADER): the client has
// no call that returns a response header afterwards. `value` is left empty when the header is absent.
struct HeaderCapture {
    const char *name;
    char       *value;
    size_t      cap;
};

struct Options {
    int     timeout_ms  = 8000;                  // esp_http_client_config_t::timeout_ms
    int     rx_buffer   = 0;                     // 0: the client's default
    int     tx_buffer   = 0;                     // 0: the client's default
    int64_t transfer_us = 15LL * 1000 * 1000;    // the whole body
    const char    *user_agent = nullptr;         // nullptr: the client's default ("ESP32 HTTP Client/1.0")
    HeaderCapture *capture    = nullptr;         // one response header to keep, or none
};

// A GET with its headers read. nullptr on a transport error; `status` gets the HTTP status, -1 when
// there is none, and `err`, when given, what Body::err says. Finish with close().
esp_http_client_handle_t open(const char *url, int &status, const Options &opt = Options(),
                              esp_err_t *err = nullptr);
void close(esp_http_client_handle_t client);

struct Body {
    esp_err_t err = ESP_FAIL;   // ESP_OK once connected; ESP_ERR_NO_MEM when no client could be created;
                                // ESP_FAIL when the connection was not made
    int  status   = -1;      // HTTP status; -1 when the request got no answer
    int  len      = 0;       // bytes read into the buffer, which is NUL-terminated
    bool complete = false;   // the whole body arrived: not cut short by the connection, the deadline
                             // or a full buffer
};

// A GET into `buf`, `cap` bytes including the terminating NUL.
Body get(const char *url, char *buf, int cap, const Options &opt = Options());

// A POST with a JSON body. The reply is not read; `status` gets the HTTP status, -1 when there is none.
esp_err_t postJson(const char *url, const std::string &body, int timeout_ms, int &status);

// ArduinoJson reads from any object with read() and readBytes(). This one reads straight off an open
// request, so a response is parsed as it arrives and is never held whole in memory. The five-day
// forecast is about 17 KB; buffered, that is one contiguous block asked of a heap that Wi-Fi and TLS
// have already carved up, and a failed allocation there is an abort rather than an error code.
class Reader {
public:
    Reader(esp_http_client_handle_t client, int64_t deadline)
        : client_(client), deadline_(deadline) {}

    int read() {
        if (pos_ >= len_ && !refill()) return -1;
        return (unsigned char)buf_[pos_++];
    }
    size_t readBytes(char *out, size_t n) {
        size_t got = 0;
        while (got < n) {
            if (pos_ >= len_ && !refill()) break;
            size_t k = (size_t)(len_ - pos_);
            if (k > n - got) k = n - got;
            memcpy(out + got, buf_ + pos_, k);
            pos_ += (int)k; got += k;
        }
        return got;
    }

private:
    bool refill() {
        if (done_ || esp_timer_get_time() > deadline_) return false;
        const int r = esp_http_client_read(client_, buf_, sizeof(buf_));
        if (r <= 0) { done_ = true; return false; }
        len_ = r; pos_ = 0;
        return true;
    }
    esp_http_client_handle_t client_;
    int64_t deadline_;
    char buf_[512];
    int  len_ = 0, pos_ = 0;
    bool done_ = false;
};

}  // namespace Https
