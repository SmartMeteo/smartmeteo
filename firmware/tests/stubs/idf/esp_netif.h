#pragma once
#include <stdint.h>
#include "esp_err.h"
#include "esp_event.h"

#ifdef __cplusplus
extern "C" {
#endif
struct esp_netif_obj { int dummy; };
typedef struct esp_netif_obj esp_netif_t;
extern esp_netif_t host_netif;
typedef struct { uint32_t addr; } esp_ip4_addr_t;
typedef struct { esp_ip4_addr_t ip, netmask, gw; } esp_netif_ip_info_t;
typedef struct { int if_index; esp_netif_t *esp_netif; bool ip_changed; esp_netif_ip_info_t ip_info; } ip_event_got_ip_t;
typedef enum { IP_EVENT_STA_GOT_IP, IP_EVENT_STA_LOST_IP, IP_EVENT_AP_STAIPASSIGNED } ip_event_t;
extern esp_event_base_t IP_EVENT;
#define IP2STR(a) 0,0,0,0
#define IPSTR "%d.%d.%d.%d"
#define ESP_IP4_ADDR_INIT(a,b,c,d) { 0 }
typedef enum { ESP_NETIF_DHCP_INIT, ESP_NETIF_DHCP_STARTED, ESP_NETIF_DHCP_STOPPED } esp_netif_dhcp_status_t;
typedef enum { ESP_NETIF_DOMAIN_NAME_SERVER = 6, ESP_NETIF_CAPTIVEPORTAL_URI = 114 } esp_netif_dhcp_option_id_t;
typedef enum { ESP_NETIF_OP_SET, ESP_NETIF_OP_GET } esp_netif_dhcp_option_mode_t;
static inline esp_err_t esp_netif_init(void){ return ESP_OK; }
static inline esp_netif_t *esp_netif_create_default_wifi_sta(void){ return &host_netif; }
static inline esp_netif_t *esp_netif_create_default_wifi_ap(void){ return &host_netif; }
static inline esp_netif_t *esp_netif_get_handle_from_ifkey(const char *k){ (void)k; return &host_netif; }
static inline esp_err_t esp_netif_get_ip_info(esp_netif_t *n, esp_netif_ip_info_t *i){ (void)n; if(i){ i->ip.addr=0; i->netmask.addr=0; i->gw.addr=0; } return ESP_OK; }
static inline esp_err_t esp_netif_set_ip_info(esp_netif_t *n, const esp_netif_ip_info_t *i){ (void)n;(void)i; return ESP_OK; }
static inline esp_err_t esp_netif_dhcps_stop(esp_netif_t *n){ (void)n; return ESP_OK; }
static inline esp_err_t esp_netif_dhcps_start(esp_netif_t *n){ (void)n; return ESP_OK; }
static inline esp_err_t esp_netif_dhcps_option(esp_netif_t *n, esp_netif_dhcp_option_mode_t m, esp_netif_dhcp_option_id_t o, void *v, uint32_t l){ (void)n;(void)m;(void)o;(void)v;(void)l; return ESP_OK; }
static inline esp_err_t esp_netif_set_hostname(esp_netif_t *n, const char *h){ (void)n;(void)h; return ESP_OK; }
static inline char *esp_ip4addr_ntoa(const esp_ip4_addr_t *a, char *b, int l){ (void)a; if(b&&l>0) b[0]=0; return b; }
static inline uint32_t esp_ip4addr_aton(const char *s){ (void)s; return 0; }

typedef struct { struct { uint32_t type; union { esp_ip4_addr_t ip4; uint8_t ip6[16]; } u_addr; } ip; } esp_netif_dns_info_t;
typedef enum { ESP_NETIF_DNS_MAIN, ESP_NETIF_DNS_BACKUP, ESP_NETIF_DNS_FALLBACK } esp_netif_dns_type_t;
static inline esp_err_t esp_netif_get_dns_info(esp_netif_t *n, esp_netif_dns_type_t t, esp_netif_dns_info_t *d){
    (void)n;(void)t; if(d) d->ip.u_addr.ip4.addr = 0; return ESP_OK; }
static inline esp_err_t esp_netif_set_dns_info(esp_netif_t *n, esp_netif_dns_type_t t, esp_netif_dns_info_t *d){ (void)n;(void)t;(void)d; return ESP_OK; }
static inline esp_err_t esp_netif_dhcpc_stop(esp_netif_t *n){ (void)n; return ESP_OK; }
static inline esp_err_t esp_netif_dhcpc_start(esp_netif_t *n){ (void)n; return ESP_OK; }
static inline esp_err_t esp_netif_dhcpc_get_status(esp_netif_t *n, esp_netif_dhcp_status_t *s){ (void)n; if(s)*s=ESP_NETIF_DHCP_INIT; return ESP_OK; }
#define ESP_IPADDR_TYPE_V4 0

#ifdef __cplusplus
}
#endif
