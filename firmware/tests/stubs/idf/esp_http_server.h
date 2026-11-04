#pragma once
#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif
typedef void *httpd_handle_t;
typedef struct httpd_req { httpd_handle_t handle; int method; const char *uri; size_t content_len; void *user_ctx; void *sess_ctx; } httpd_req_t;
typedef enum { HTTP_GET = 1, HTTP_POST = 3, HTTP_PUT, HTTP_DELETE, HTTP_ANY = 0x7F } httpd_method_t;
typedef enum { HTTPD_404_NOT_FOUND, HTTPD_400_BAD_REQUEST, HTTPD_500_INTERNAL_SERVER_ERROR,
               HTTPD_408_REQ_TIMEOUT, HTTPD_405_METHOD_NOT_ALLOWED, HTTPD_411_LENGTH_REQUIRED,
               HTTPD_413_CONTENT_TOO_LARGE, HTTPD_414_URI_TOO_LONG, HTTPD_431_REQ_HDR_FIELDS_TOO_LARGE } httpd_err_code_t;
typedef esp_err_t (*httpd_err_handler_func_t)(httpd_req_t *r, httpd_err_code_t e);
typedef struct { const char *uri; httpd_method_t method; esp_err_t (*handler)(httpd_req_t *); void *user_ctx; } httpd_uri_t;
typedef struct { unsigned task_priority; size_t stack_size; int core_id; uint16_t server_port, ctrl_port;
                 uint16_t max_open_sockets, max_uri_handlers, max_resp_headers, backlog_conn;
                 bool lru_purge_enable; uint16_t recv_wait_timeout, send_wait_timeout;
                 void *global_user_ctx; void (*global_user_ctx_free_fn)(void*); void *global_transport_ctx;
                 void (*global_transport_ctx_free_fn)(void*); bool enable_so_linger; int linger_timeout;
                 bool keep_alive_enable; int keep_alive_idle, keep_alive_interval, keep_alive_count;
                 bool (*uri_match_fn)(const char*, const char*, size_t); } httpd_config_t;
#define HTTPD_DEFAULT_CONFIG() (httpd_config_t){0}
#define HTTPD_RESP_USE_STRLEN ((ssize_t)-1)
#define HTTPD_SOCK_ERR_FAIL      (-1)
#define HTTPD_SOCK_ERR_INVALID   (-2)
#define HTTPD_SOCK_ERR_TIMEOUT   (-3)
#define ESP_ERR_HTTPD_BASE           0x8000
#define ESP_ERR_HTTPD_HANDLERS_FULL  0x8001
#define ESP_ERR_HTTPD_HANDLER_EXISTS 0x8002
#define ESP_ERR_HTTPD_INVALID_REQ    0x8003
#define ESP_ERR_HTTPD_RESULT_TRUNC   0x8004
#define ESP_ERR_HTTPD_ALLOC_MEM      0x8005
#define ESP_ERR_HTTPD_TASK           0x8006
static inline esp_err_t httpd_start(httpd_handle_t *h, const httpd_config_t *c){ (void)c; if(h)*h=nullptr; return ESP_OK; }
static inline esp_err_t httpd_stop(httpd_handle_t h){ (void)h; return ESP_OK; }
static inline esp_err_t httpd_register_uri_handler(httpd_handle_t h, const httpd_uri_t *u){ (void)h;(void)u; return ESP_OK; }
static inline esp_err_t httpd_register_err_handler(httpd_handle_t h, httpd_err_code_t e, httpd_err_handler_func_t f){ (void)h;(void)e;(void)f; return ESP_OK; }
static inline esp_err_t httpd_resp_send(httpd_req_t *r, const char *b, ssize_t n){ (void)r;(void)b;(void)n; return ESP_OK; }
static inline esp_err_t httpd_resp_send_chunk(httpd_req_t *r, const char *b, ssize_t n){ (void)r;(void)b;(void)n; return ESP_OK; }
static inline esp_err_t httpd_resp_set_type(httpd_req_t *r, const char *t){ (void)r;(void)t; return ESP_OK; }
static inline esp_err_t httpd_resp_set_hdr(httpd_req_t *r, const char *k, const char *v){ (void)r;(void)k;(void)v; return ESP_OK; }
static inline esp_err_t httpd_resp_set_status(httpd_req_t *r, const char *s){ (void)r;(void)s; return ESP_OK; }
static inline esp_err_t httpd_resp_send_err(httpd_req_t *r, httpd_err_code_t e, const char *m){ (void)r;(void)e;(void)m; return ESP_OK; }
static inline int  httpd_req_recv(httpd_req_t *r, char *b, size_t n){ (void)r;(void)b;(void)n; return 0; }
static inline size_t httpd_req_get_hdr_value_len(httpd_req_t *r, const char *f){ (void)r;(void)f; return 0; }
static inline esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *r, const char *f, char *v, size_t n){ (void)r;(void)f;(void)v;(void)n; return ESP_FAIL; }
static inline size_t httpd_req_get_url_query_len(httpd_req_t *r){ (void)r; return 0; }
static inline esp_err_t httpd_req_get_url_query_str(httpd_req_t *r, char *q, size_t n){ (void)r;(void)q;(void)n; return ESP_FAIL; }
static inline esp_err_t httpd_query_key_value(const char *q, const char *k, char *v, size_t n){ (void)q;(void)k;(void)v;(void)n; return ESP_FAIL; }
static inline int httpd_req_to_sockfd(httpd_req_t *r){ (void)r; return -1; }

static inline esp_err_t httpd_resp_sendstr(httpd_req_t *r, const char *s){ return httpd_resp_send(r, s, HTTPD_RESP_USE_STRLEN); }
static inline esp_err_t httpd_resp_sendstr_chunk(httpd_req_t *r, const char *s){ return httpd_resp_send_chunk(r, s, s ? HTTPD_RESP_USE_STRLEN : 0); }
static inline esp_err_t httpd_resp_set_hdr_len(httpd_req_t *r, const char *k, const char *v, size_t n){ (void)n; return httpd_resp_set_hdr(r,k,v); }
static inline esp_err_t httpd_queue_work(httpd_handle_t h, void (*fn)(void*), void *a){ (void)h;(void)fn;(void)a; return ESP_OK; }
static inline esp_err_t httpd_sess_trigger_close(httpd_handle_t h, int fd){ (void)h;(void)fd; return ESP_OK; }
static inline esp_err_t httpd_unregister_uri_handler(httpd_handle_t h, const char *u, httpd_method_t m){ (void)h;(void)u;(void)m; return ESP_OK; }
static inline int httpd_default_recv(httpd_handle_t h, int fd, char *b, size_t n, int f){ (void)h;(void)fd;(void)b;(void)n;(void)f; return 0; }
#define HTTPD_TYPE_JSON "application/json"
#define HTTPD_TYPE_TEXT "text/html"
#define HTTPD_TYPE_OCTET "application/octet-stream"

bool httpd_uri_match_wildcard(const char *tpl, const char *uri, size_t len);

#ifdef __cplusplus
}
#endif
