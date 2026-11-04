#pragma once
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif
// Сеть в этой оснастке не работает и работать не должна: окно связи меряется на приборе. Здесь
// каждый запрос честно отказывает, и это ПРОВЕРЯЕМАЯ ветка — расписание обязано пережить отсутствие
// погоды, а не только её наличие.
typedef void *esp_http_client_handle_t;
typedef enum { HTTP_METHOD_GET, HTTP_METHOD_POST, HTTP_METHOD_PUT, HTTP_METHOD_DELETE, HTTP_METHOD_HEAD } esp_http_client_method_t;
typedef enum { HTTP_TRANSPORT_UNKNOWN, HTTP_TRANSPORT_OVER_TCP, HTTP_TRANSPORT_OVER_SSL } esp_http_client_transport_t;
typedef enum { HTTP_EVENT_ERROR, HTTP_EVENT_ON_CONNECTED, HTTP_EVENT_HEADERS_SENT,
               HTTP_EVENT_ON_HEADER, HTTP_EVENT_ON_DATA, HTTP_EVENT_ON_FINISH,
               HTTP_EVENT_DISCONNECTED, HTTP_EVENT_REDIRECT } esp_http_client_event_id_t;
typedef struct esp_http_client_event { esp_http_client_event_id_t event_id; esp_http_client_handle_t client;
    void *data; int data_len; void *user_data; char *header_key, *header_value; } esp_http_client_event_t;
typedef esp_http_client_event_t *esp_http_client_event_handle_t;
typedef esp_err_t (*http_event_handle_cb)(esp_http_client_event_t *);
typedef struct { const char *url, *host, *path, *query, *username, *password, *cert_pem;
    esp_http_client_method_t method; int timeout_ms; bool disable_auto_redirect; int max_redirection_count;
    http_event_handle_cb event_handler; esp_http_client_transport_t transport_type;
    int buffer_size, buffer_size_tx; void *user_data; bool is_async, use_global_ca_store, skip_cert_common_name_check;
    esp_err_t (*crt_bundle_attach)(void *); bool keep_alive_enable; const char *user_agent; } esp_http_client_config_t;
static inline esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *c){ (void)c; return nullptr; }
static inline esp_err_t esp_http_client_open(esp_http_client_handle_t c, int len){ (void)c;(void)len; return ESP_FAIL; }
static inline esp_err_t esp_http_client_perform(esp_http_client_handle_t c){ (void)c; return ESP_FAIL; }
static inline int64_t esp_http_client_fetch_headers(esp_http_client_handle_t c){ (void)c; return -1; }
static inline int  esp_http_client_get_status_code(esp_http_client_handle_t c){ (void)c; return -1; }
static inline int64_t esp_http_client_get_content_length(esp_http_client_handle_t c){ (void)c; return -1; }
static inline bool esp_http_client_is_chunked_response(esp_http_client_handle_t c){ (void)c; return false; }
static inline bool esp_http_client_is_complete_data_received(esp_http_client_handle_t c){ (void)c; return false; }
static inline int  esp_http_client_read(esp_http_client_handle_t c, char *b, int n){ (void)c;(void)b;(void)n; return -1; }
static inline int  esp_http_client_read_response(esp_http_client_handle_t c, char *b, int n){ (void)c;(void)b;(void)n; return -1; }
static inline int  esp_http_client_write(esp_http_client_handle_t c, const char *b, int n){ (void)c;(void)b;(void)n; return -1; }
static inline esp_err_t esp_http_client_close(esp_http_client_handle_t c){ (void)c; return ESP_OK; }
static inline esp_err_t esp_http_client_cleanup(esp_http_client_handle_t c){ (void)c; return ESP_OK; }
static inline esp_err_t esp_http_client_set_header(esp_http_client_handle_t c, const char *k, const char *v){ (void)c;(void)k;(void)v; return ESP_OK; }
static inline esp_err_t esp_http_client_set_method(esp_http_client_handle_t c, esp_http_client_method_t m){ (void)c;(void)m; return ESP_OK; }
static inline esp_err_t esp_http_client_set_post_field(esp_http_client_handle_t c, const char *d, int n){ (void)c;(void)d;(void)n; return ESP_OK; }
static inline esp_err_t esp_http_client_set_url(esp_http_client_handle_t c, const char *u){ (void)c;(void)u; return ESP_OK; }
static inline esp_err_t esp_http_client_set_timeout_ms(esp_http_client_handle_t c, int t){ (void)c;(void)t; return ESP_OK; }
static inline esp_err_t esp_http_client_get_header(esp_http_client_handle_t c, const char *k, char **v){ (void)c;(void)k; if(v)*v=nullptr; return ESP_OK; }

#ifdef __cplusplus
}
#endif
