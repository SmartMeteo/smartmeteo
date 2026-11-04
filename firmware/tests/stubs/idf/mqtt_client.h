#pragma once
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "esp_event.h"

#ifdef __cplusplus
extern "C" {
#endif
struct esp_mqtt_client { int dummy; };
typedef struct esp_mqtt_client *esp_mqtt_client_handle_t;
typedef struct { int error_type, connect_return_code, esp_tls_last_esp_err, esp_tls_stack_err,
                 esp_tls_cert_verify_flags, esp_transport_sock_errno; } esp_mqtt_error_codes_t;
typedef enum { MQTT_EVENT_ANY = -1, MQTT_EVENT_ERROR, MQTT_EVENT_CONNECTED, MQTT_EVENT_DISCONNECTED,
               MQTT_EVENT_SUBSCRIBED, MQTT_EVENT_UNSUBSCRIBED, MQTT_EVENT_PUBLISHED,
               MQTT_EVENT_DATA, MQTT_EVENT_BEFORE_CONNECT } esp_mqtt_event_id_t;
typedef enum { MQTT_TRANSPORT_UNKNOWN, MQTT_TRANSPORT_OVER_TCP, MQTT_TRANSPORT_OVER_SSL } esp_mqtt_transport_t;
typedef struct { int msg_id; char *topic; int topic_len; char *data; int data_len; int total_data_len;
                 int current_data_offset; esp_mqtt_error_codes_t *error_handle; bool retain; int qos; bool dup;
                 esp_mqtt_client_handle_t client; } esp_mqtt_event_t;
typedef esp_mqtt_event_t *esp_mqtt_event_handle_t;
typedef struct {
    struct { struct { const char *uri, *hostname, *path; uint32_t port; esp_mqtt_transport_t transport; } address;
             struct { const char *username, *password; const void *certificate; esp_err_t (*crt_bundle_attach)(void *); bool skip_cert_common_name_check; } verification; } broker;
    struct { const char *username, *password, *client_id; struct { const char *data; int len; int qos; bool retain; } lwt; } credentials;
    struct { int keepalive; bool disable_clean_session; int network_timeout_ms, refresh_connection_after_ms,
                 reconnect_timeout_ms, disable_auto_reconnect, task_stack, buffer_size; } session;
    struct { int task_stack, buffer_size; } network;
    struct { int out_buffer_size, in_buffer_size; } buffer;
} esp_mqtt_client_config_t;
static inline esp_mqtt_client_handle_t esp_mqtt_client_init(const esp_mqtt_client_config_t *c){ (void)c; return nullptr; }
static inline esp_err_t esp_mqtt_client_start(esp_mqtt_client_handle_t c){ (void)c; return ESP_FAIL; }
static inline esp_err_t esp_mqtt_client_stop(esp_mqtt_client_handle_t c){ (void)c; return ESP_OK; }
static inline esp_err_t esp_mqtt_client_destroy(esp_mqtt_client_handle_t c){ (void)c; return ESP_OK; }
static inline int esp_mqtt_client_publish(esp_mqtt_client_handle_t c, const char *t, const char *d, int l, int q, int r){ (void)c;(void)t;(void)d;(void)l;(void)q;(void)r; return -1; }
static inline int esp_mqtt_client_subscribe(esp_mqtt_client_handle_t c, const char *t, int q){ (void)c;(void)t;(void)q; return -1; }
static inline esp_err_t esp_mqtt_client_register_event(esp_mqtt_client_handle_t c, esp_mqtt_event_id_t id, esp_event_handler_t h, void *a){ (void)c;(void)id;(void)h;(void)a; return ESP_OK; }

#ifdef __cplusplus
}
#endif
