#include "https_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include <strings.h>

static const char *TAG = "HTTPS";

namespace Https {

// Names the host and nothing after it: the rest of a URL can carry an API key or a bot token.
static void log_failure(const char *what, const char *url)
{
    const char *host = strstr(url, "://");
    host = host ? host + 3 : url;
    const char *end = strchr(host, '/');
    ESP_LOGW(TAG, "%s failed for %.*s", what, end ? (int)(end - host) : (int)strlen(host), host);
}

// Response headers arrive as events while esp_http_client_fetch_headers() runs (esp_http_client.h,
// HTTP_EVENT_ON_HEADER); this keeps the one the caller asked for.
static esp_err_t on_event(esp_http_client_event_t *evt)
{
    if (evt->event_id != HTTP_EVENT_ON_HEADER || !evt->user_data || !evt->header_key) return ESP_OK;
    HeaderCapture *c = (HeaderCapture *)evt->user_data;
    if (strcasecmp(evt->header_key, c->name) == 0 && c->value && c->cap)
        snprintf(c->value, c->cap, "%s", evt->header_value ? evt->header_value : "");
    return ESP_OK;
}

esp_http_client_handle_t open(const char *url, int &status, const Options &opt, esp_err_t *err)
{
    status = -1;
    if (err) *err = ESP_ERR_NO_MEM;
    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.timeout_ms = opt.timeout_ms;
    cfg.buffer_size = opt.rx_buffer;
    cfg.buffer_size_tx = opt.tx_buffer;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;   // TLS: verify the server certificate
    cfg.transport_type = HTTP_TRANSPORT_OVER_SSL;
    if (opt.user_agent) cfg.user_agent = opt.user_agent;
    if (opt.capture) {
        if (opt.capture->value && opt.capture->cap) opt.capture->value[0] = '\0';
        cfg.event_handler = on_event;
        cfg.user_data = opt.capture;
    }

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return nullptr;
    if (err) *err = ESP_FAIL;
    if (esp_http_client_open(client, 0) != ESP_OK) {
        log_failure("open", url);
        esp_http_client_cleanup(client);
        return nullptr;
    }
    if (err) *err = ESP_OK;
    esp_http_client_fetch_headers(client);
    status = esp_http_client_get_status_code(client);
    return client;
}

void close(esp_http_client_handle_t client)
{
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
}

Body get(const char *url, char *buf, int cap, const Options &opt)
{
    Body b;
    buf[0] = '\0';
    esp_http_client_handle_t client = open(url, b.status, opt, &b.err);
    if (!client) return b;
    const int64_t deadline = esp_timer_get_time() + opt.transfer_us;
    bool eof = false;
    while (b.len < cap - 1 && esp_timer_get_time() < deadline) {
        const int r = esp_http_client_read(client, buf + b.len, cap - 1 - b.len);
        if (r <= 0) { eof = (r == 0); break; }
        b.len += r;
    }
    buf[b.len] = '\0';
    // Espressif's native_ota_example checks esp_http_client_is_complete_data_received() after its read
    // loop. That check compares with the declared length, so it can never pass for a body sent with
    // neither Content-Length nor chunking. Such a body ends where the connection closes (RFC 9112
    // section 6.3), and reading to that end with room left in the buffer is what complete means for it.
    const bool undeclared = esp_http_client_get_content_length(client) < 0 &&
                            !esp_http_client_is_chunked_response(client);
    b.complete = esp_http_client_is_complete_data_received(client) ||
                 (undeclared && eof && b.len < cap - 1);
    close(client);
    return b;
}

esp_err_t postJson(const char *url, const std::string &body, int timeout_ms, int &status)
{
    status = -1;
    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.timeout_ms = timeout_ms;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;   // TLS: verify the server certificate
    cfg.transport_type = HTTP_TRANSPORT_OVER_SSL;
    cfg.method = HTTP_METHOD_POST;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return ESP_ERR_NO_MEM;
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, body.c_str(), (int)body.size());
    const esp_err_t err = esp_http_client_perform(client);
    status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    return err;
}

}  // namespace Https
