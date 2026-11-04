// Https:: without a network. The body comes from HttpStub, so a captured answer can be replayed
// byte for byte and the firmware cannot tell the difference.
#include "https_client.h"
#include <string.h>
#include <stdio.h>
#include <strings.h>

namespace HttpStub {
std::string body;
std::string expires;
int    status = 200;
size_t chunk  = 0;
int    reads  = 0;
static size_t pos = 0;
void reset() { body.clear(); expires.clear(); status = 200; chunk = 0; reads = 0; pos = 0; }
}

int esp_http_client_read(esp_http_client_handle_t, char *buf, int len)
{
    using namespace HttpStub;
    if (pos >= body.size()) return 0;
    size_t n = body.size() - pos;
    if (chunk && n > chunk) n = chunk;
    if (n > (size_t)len) n = (size_t)len;
    memcpy(buf, body.data() + pos, n);
    pos += n;
    reads++;
    return (int)n;
}

namespace Https {

static void capture_header(const Options &opt)
{
    if (!opt.capture || !opt.capture->value || !opt.capture->cap) return;
    opt.capture->value[0] = 0;
    if (strcasecmp(opt.capture->name, "Expires") == 0 && !HttpStub::expires.empty())
        snprintf(opt.capture->value, opt.capture->cap, "%s", HttpStub::expires.c_str());
}

esp_http_client_handle_t open(const char *, int &status, const Options &opt, esp_err_t *err)
{
    HttpStub::pos = 0; HttpStub::reads = 0;
    status = HttpStub::status;
    if (err) *err = HttpStub::status > 0 ? ESP_OK : ESP_FAIL;
    if (HttpStub::status <= 0) return nullptr;
    capture_header(opt);
    return (esp_http_client_handle_t)1;
}

void close(esp_http_client_handle_t) {}

Body get(const char *, char *buf, int cap, const Options &opt)
{
    Body b;
    HttpStub::pos = 0; HttpStub::reads = 0;
    b.status = HttpStub::status;
    if (HttpStub::status <= 0) { b.err = ESP_FAIL; if (cap) buf[0] = 0; return b; }
    capture_header(opt);
    b.err = ESP_OK;
    int n = (int)HttpStub::body.size();
    b.complete = n < cap;                    // a full buffer means it was cut short
    if (n > cap - 1) n = cap - 1;
    memcpy(buf, HttpStub::body.data(), n);
    buf[n] = 0;
    b.len = n;
    return b;
}

esp_err_t postJson(const char *, const std::string &, int, int &status)
{
    status = HttpStub::status;
    return status > 0 ? ESP_OK : ESP_FAIL;
}

}  // namespace Https
