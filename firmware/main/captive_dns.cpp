#include "captive_dns.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include <string.h>

static const char* TAG = "CAPTIVE_DNS";

// Modelled on Espressif's captive portal example (examples/protocols/http_server/captive_portal,
// components/dns_server/dns_server.c): the reply is the query with QR set, one answer per type-A question
// carrying the soft-AP's address from the "WIFI_AP_DEF" netif, TTL 300 s; other questions get no answer.
// Two things differ from that file, both packet layout (RFC 1035 section 4.1):
//   - the reply ends after the question section. The example appends its answers behind the whole query,
//     so a query carrying an additional record — the EDNS OPT record resolvers commonly add — had its
//     answer placed after that record, where no parser reads it as an answer;
//   - ANCOUNT counts the answers written; the example counts every question.
#define DNS_HDR_LEN   12
#define DNS_MAX_QD    4
#define ANS_TTL_SEC   300
#define QD_TYPE_A     0x0001
#define QD_CLASS_IN   0x0001

// 192.168.4.1 in network byte order, the esp_netif soft-AP default; used if the netif cannot be read.
static const uint32_t AP_IP_BE = 0x0104A8C0;

CaptiveDNS& CaptiveDNS::getInstance() {
    static CaptiveDNS instance;
    return instance;
}

void CaptiveDNS::start() {
    if (running) return;
    stop_requested = false;
    running = true;
    xTaskCreate(serverTask, "captive_dns", 3072, this, 5, NULL);
}

void CaptiveDNS::stop() {
    stop_requested = true;
}

// Offset just past the name starting at `pos`, or -1 if it runs off the packet. A question name in a query
// is a plain sequence of labels; a compression pointer there is treated as malformed.
static int skip_name(const uint8_t *p, int len, int pos)
{
    while (pos < len) {
        const uint8_t l = p[pos];
        if (l == 0) return pos + 1;
        if (l & 0xC0) return -1;
        pos += 1 + l;
    }
    return -1;
}

void CaptiveDNS::serverTask(void *arg) {
    CaptiveDNS *self = (CaptiveDNS*)arg;

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "socket() failed");
        self->running = false;
        vTaskDelete(NULL);
        return;
    }

    // The access point's own address, needed twice: as the answer, and as the address to listen on.
    uint32_t ap_ip = AP_IP_BE;
    esp_netif_ip_info_t ip_info = {};
    esp_netif_t *ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (ap && esp_netif_get_ip_info(ap, &ip_info) == ESP_OK && ip_info.ip.addr != 0) ap_ip = ip_info.ip.addr;

    // Bound to the access point, not to every interface. This exists for one job: answering the
    // phone that has just joined the device's own Wi-Fi, so that its captive-portal check lands on
    // the console. Bound to INADDR_ANY — as it was until 2026-09-18 — it also served UDP/53 on the
    // HOME network for the life of every setup window: an open resolver that answers any source
    // address with a reply larger than the question, which is the shape of a reflector. Nothing on
    // the home network ever wanted it: there the console is reached by its address or by mDNS.
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(53);
    addr.sin_addr.s_addr = ap_ip;
    if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "bind(53) failed");
        close(sock);
        self->running = false;
        vTaskDelete(NULL);
        return;
    }

    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ESP_LOGI(TAG, "Captive DNS up: type-A queries answered with %u.%u.%u.%u",
             (unsigned)(ap_ip & 0xFF), (unsigned)((ap_ip >> 8) & 0xFF),
             (unsigned)((ap_ip >> 16) & 0xFF), (unsigned)((ap_ip >> 24) & 0xFF));

    uint8_t buf[512];
    while (!self->stop_requested) {
        struct sockaddr_in src;
        socklen_t src_len = sizeof(src);
        const int len = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr*)&src, &src_len);
        if (len < DNS_HDR_LEN) continue; // timeout or malformed

        // Queries only (QR = 0), standard queries only (OPCODE = 0). Nothing here answers a response, so
        // two such servers — or one and a spoofed pair of addresses — cannot keep each other going.
        const bool is_query  = (buf[2] & 0x80) == 0;
        const bool std_query = (buf[2] & 0x78) == 0;
        const uint16_t qdcount = (uint16_t)((buf[4] << 8) | buf[5]);
        if (!is_query || !std_query || qdcount == 0 || qdcount > DNS_MAX_QD) continue;

        // The question section: where each name starts, and whether it asks for an IPv4 address.
        uint16_t q_off[DNS_MAX_QD] = {};
        bool     q_a[DNS_MAX_QD]   = {};
        int pos = DNS_HDR_LEN;
        for (int i = 0; i < qdcount; i++) {
            const int end = skip_name(buf, len, pos);
            if (end < 0 || end + 4 > len) { pos = -1; break; }
            const uint16_t type  = (uint16_t)((buf[end] << 8) | buf[end + 1]);
            const uint16_t klass = (uint16_t)((buf[end + 2] << 8) | buf[end + 3]);
            q_off[i] = (uint16_t)pos;
            q_a[i]   = (type == QD_TYPE_A && klass == QD_CLASS_IN);
            pos = end + 4;
        }
        if (pos < 0) continue;

        // Reply = header + questions, then the answers.
        int out = pos;
        uint16_t ancount = 0;
        for (int i = 0; i < qdcount; i++) {
            if (!q_a[i] || out + 16 > (int)sizeof(buf)) continue;
            buf[out++] = (uint8_t)(0xC0 | (q_off[i] >> 8));   // name: pointer to the question
            buf[out++] = (uint8_t)(q_off[i] & 0xFF);
            buf[out++] = 0x00; buf[out++] = 0x01;             // type A
            buf[out++] = 0x00; buf[out++] = 0x01;             // class IN
            buf[out++] = (uint8_t)((ANS_TTL_SEC >> 24) & 0xFF); buf[out++] = (uint8_t)((ANS_TTL_SEC >> 16) & 0xFF);
            buf[out++] = (uint8_t)((ANS_TTL_SEC >> 8) & 0xFF);  buf[out++] = (uint8_t)(ANS_TTL_SEC & 0xFF);
            buf[out++] = 0x00; buf[out++] = 0x04;             // RDLENGTH
            memcpy(&buf[out], &ap_ip, 4); out += 4;           // already in network byte order
            ancount++;
        }
        buf[2] |= 0x80;                                       // QR = 1; the other flags as asked
        buf[6]  = (uint8_t)(ancount >> 8); buf[7] = (uint8_t)(ancount & 0xFF);
        buf[8]  = buf[9] = buf[10] = buf[11] = 0x00;          // NSCOUNT, ARCOUNT

        sendto(sock, buf, out, 0, (struct sockaddr*)&src, src_len);
    }

    close(sock);
    ESP_LOGI(TAG, "Captive DNS stopped");
    self->running = false;
    vTaskDelete(NULL);
}
