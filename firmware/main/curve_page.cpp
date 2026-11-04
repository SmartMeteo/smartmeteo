#include "curve_page.h"
#include "nvs_manager.h"
#include "telegram.h"
#include "wifi_manager.h"

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>

static const char *TAG = "CURVE-PAGE";

namespace CurvePage {

static httpd_handle_t s_httpd = nullptr;
static char s_url[40] = "";
static int s_pin = 0;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static char s_status[1600] = "{}";
static volatile bool s_have_reading = false;
static float s_reading = 0;
static int s_bad_codes = 0;
static int64_t s_locked_until = 0;
static int64_t s_last_join = 0;

// ---- the page ---------------------------------------------------------------------------------
// Numbers only ever reach innerHTML: nothing typed by anyone is echoed into the document.
static const char PAGE[] = R"HTML(<!doctype html><html lang="ru"><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>Ток заряда</title>
<style>body{font:16px system-ui,sans-serif;margin:16px;max-width:560px;background:#fff;color:#111}
#st{padding:12px;border-radius:8px;background:#e8f5e9}#st.due{background:#c62828;color:#fff;font-weight:700;font-size:20px}
input{font-size:22px;padding:8px;width:8em;max-width:100%;box-sizing:border-box}button{font-size:20px;padding:8px 16px}
table{border-collapse:collapse;margin-top:12px}td,th{padding:4px 10px;border-bottom:1px solid #ddd;text-align:right}
small{color:#555}</style>
<h2 id="h">Ток заряда, R7</h2><div id="st">Связываюсь со стендом…</div>
<p>Мультиметр в режиме <b>вольт</b> (V⎓), щупы на два конца <b>R7</b>. 1,00 В = 1 А.</p>
<p><input id="v" inputmode="decimal" placeholder="0.870" autocomplete="off"> В</p>
<p><input id="pin" inputmode="numeric" maxlength="4" placeholder="код с экрана" autocomplete="off"></p>
<p><button id="b">Записать</button> <span id="m"></span></p>
<table id="t"></table>
<details style="margin-top:24px"><summary>Telegram: <span id="tm">…</span></summary>
<p><small>Стенд пришлёт «замерь сейчас» со ссылкой сюда. Токен бота — от @BotFather (можно тот же,
что у основного прибора: стенд только отправляет и ничего не читает), чат — твой chat id.</small></p>
<p><input id="tok" placeholder="токен бота" style="width:100%" autocomplete="off"></p>
<p><input id="chat" placeholder="chat id" autocomplete="off"></p>
<p><button id="tb">Сохранить</button> <span id="tr"></span></p></details>
<script>
let ac=null,wasDue=false;
function $(i){return document.getElementById(i)}
function beep(){try{ac=ac||new(window.AudioContext||window.webkitAudioContext)();const o=ac.createOscillator(),g=ac.createGain();
o.frequency.value=880;g.gain.value=0.25;o.connect(g);g.connect(ac.destination);o.start();setTimeout(function(){o.stop()},500)}catch(e){}
try{navigator.vibrate&&navigator.vibrate([300,150,300])}catch(e){}}
function hm(s){s=Math.max(0,s|0);return(s/3600|0)+':'+String((s/60|0)%60).padStart(2,'0')}
function render(s){const st=$('st');
 if(s.board){document.title='R7 · плата '+s.board;$('h').textContent='Ток заряда, R7 · плата '+s.board}
if(s.done){st.className='';st.textContent='Заряд C'+s.cycle+' закончен за '+hm(s.tdone)+'.'+(s.mah>0?' По замерам зарядилось '+Math.round(s.mah)+' мА·ч.':'')}
 else if(!s.charging){st.className='';st.textContent='Сейчас не заряд — замеры не нужны.'}
 else if(s.due){st.className='due';st.textContent='Замерь сейчас: №'+(s.n+1)+' · заряд C'+s.cycle+' идёт '+hm(s.t)+' · ячейка '+s.vcell.toFixed(3)+' В';if(!wasDue)beep()}
 else{st.className='';st.textContent='Следующий замер через '+Math.max(1,Math.ceil(s.next/60))+' мин · заряд C'+s.cycle+' идёт '+hm(s.t)+' · ячейка '+s.vcell.toFixed(3)+' В'}
 wasDue=!!(s.due&&!s.done&&s.charging);
 let h='<tr><th>№</th><th>время</th><th>В на R7</th><th>ток</th></tr>';
 (s.r||[]).forEach(function(x,i){h+='<tr><td>'+(i+1)+'</td><td>'+hm(x[0])+'</td><td>'+Number(x[1]).toFixed(3)+'</td><td>'+Math.round(x[1]*1000)+' мА</td></tr>'});
 $('t').innerHTML=h;$('tm').textContent=s.tg?'настроен':'не настроен'}
async function poll(){try{const r=await fetch('/s',{cache:'no-store'});render(await r.json())}catch(e){$('st').className='';$('st').textContent='Нет связи со стендом…'}setTimeout(poll,5000)}
async function post(path,body){const r=await fetch(path,{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:body});return[r.ok,await r.text()]}
try{$('pin').value=localStorage.getItem('pin')||''}catch(e){}
$('b').onclick=async function(){try{ac=ac||new(window.AudioContext||window.webkitAudioContext)();ac.resume()}catch(e){}
 const v=parseFloat($('v').value.replace(',','.')),pin=$('pin').value.trim();
 if(!(v>=0&&v<=1.5)){$('m').textContent='от 0 до 1,5 В';return}
 try{localStorage.setItem('pin',pin)}catch(e){}
 const[ok,t]=await post('/r','pin='+encodeURIComponent(pin)+'&v='+v);$('m').textContent=ok?'записано: '+Math.round(v*1000)+' мА':t;if(ok)$('v').value=''};
$('tb').onclick=async function(){const[ok,t]=await post('/tg','pin='+encodeURIComponent($('pin').value.trim())+'&token='+encodeURIComponent($('tok').value.trim())+'&chat='+encodeURIComponent($('chat').value.trim()));
 $('tr').textContent=ok?'сохранено':t;if(ok){$('tok').value=''}};
poll();
</script></html>)HTML";

// ---- helpers ----------------------------------------------------------------------------------

static void url_decode(char *s)
{
    char *o = s;
    for (char *p = s; *p; p++) {
        if (*p == '+') *o++ = ' ';
        else if (*p == '%' && p[1] && p[2]) {
            char h[3] = {p[1], p[2], 0};
            *o++ = (char)strtol(h, nullptr, 16);
            p += 2;
        } else *o++ = *p;
    }
    *o = 0;
}

static int recv_body(httpd_req_t *req, char *buf, size_t cap)
{
    if (req->content_len == 0 || req->content_len >= cap) return -1;
    size_t got = 0;
    while (got < req->content_len) {
        const int r = httpd_req_recv(req, buf + got, req->content_len - got);
        if (r <= 0) return -1;
        got += (size_t)r;
    }
    buf[got] = 0;
    return (int)got;
}

// The code gate. Wrong codes count; five lock writes for a minute.
static bool code_ok(httpd_req_t *req, const char *body)
{
    const int64_t now = esp_timer_get_time();
    if (now < s_locked_until) {
        httpd_resp_set_status(req, "429 Too Many Requests");
        httpd_resp_sendstr(req, "подожди минуту: пять неверных кодов");
        return false;
    }
    char code[8] = "";
    httpd_query_key_value(body, "pin", code, sizeof(code));
    if (atoi(code) == s_pin && strlen(code) == 4) { s_bad_codes = 0; return true; }
    if (++s_bad_codes >= 5) { s_locked_until = now + 60LL * 1000000; s_bad_codes = 0; }
    httpd_resp_set_status(req, "403 Forbidden");
    httpd_resp_sendstr(req, "неверный код — он на экране прибора");
    return false;
}

// ---- handlers ---------------------------------------------------------------------------------

static esp_err_t get_page(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, PAGE, sizeof(PAGE) - 1);
}

static esp_err_t get_status(httpd_req_t *req)
{
    static char copy[sizeof(s_status)];
    portENTER_CRITICAL(&s_mux);
    memcpy(copy, s_status, sizeof(copy));
    portEXIT_CRITICAL(&s_mux);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, copy);
}

static esp_err_t post_reading(httpd_req_t *req)
{
    char body[128];
    if (recv_body(req, body, sizeof(body)) < 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body"); return ESP_OK; }
    if (!code_ok(req, body)) return ESP_OK;
    char vs[16] = "";
    httpd_query_key_value(body, "v", vs, sizeof(vs));
    url_decode(vs);
    for (char *p = vs; *p; p++) if (*p == ',') *p = '.';
    char *end = nullptr;
    const float v = strtof(vs, &end);
    if (end == vs || !(v >= 0.0f && v <= 1.5f)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "от 0 до 1,5 В");
        return ESP_OK;
    }
    portENTER_CRITICAL(&s_mux);
    s_reading = v;
    s_have_reading = true;
    portEXIT_CRITICAL(&s_mux);
    ESP_LOGW(TAG, "reading entered: %.3f V on R7", v);
    return httpd_resp_sendstr(req, "ok");
}

static esp_err_t post_telegram(httpd_req_t *req)
{
    char body[256];
    if (recv_body(req, body, sizeof(body)) < 0) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body"); return ESP_OK; }
    if (!code_ok(req, body)) return ESP_OK;
    char tok[96] = "", chat[32] = "";
    httpd_query_key_value(body, "token", tok, sizeof(tok));
    httpd_query_key_value(body, "chat", chat, sizeof(chat));
    url_decode(tok);
    url_decode(chat);
    // A bot token is digits, a colon and a base64-ish tail; a chat id is an optionally negative number.
    size_t n = strlen(tok);
    bool good = n >= 30 && n <= 64 && strchr(tok, ':');
    for (size_t i = 0; good && i < n; i++)
        good = (tok[i] >= '0' && tok[i] <= '9') || (tok[i] >= 'a' && tok[i] <= 'z') || (tok[i] >= 'A' && tok[i] <= 'Z') ||
               tok[i] == ':' || tok[i] == '_' || tok[i] == '-';
    n = strlen(chat);
    good = good && n >= 1 && n <= 20;
    for (size_t i = 0; good && i < n; i++) good = (chat[i] >= '0' && chat[i] <= '9') || (i == 0 && chat[i] == '-');
    if (!good) { httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "токен или chat id не похожи на настоящие"); return ESP_OK; }
    NVSManager::getInstance().setString("bn_tok", tok);
    NVSManager::getInstance().setString("bn_chat", chat);
    memset(tok, 0, sizeof(tok));
    memset(body, 0, sizeof(body));
    ESP_LOGW(TAG, "telegram bot saved for the bench (token not logged)");
    return httpd_resp_sendstr(req, "ok");
}

// ---- the interface ------------------------------------------------------------------------------

static void refresh_url()
{
    const std::string ip = WiFiManager::getInstance().getIPAddress();
    if (!ip.empty() && ip != "0.0.0.0") snprintf(s_url, sizeof(s_url), "http://%s/", ip.c_str());
    else s_url[0] = 0;
}

static bool join()
{
    std::string ssid, pass;
    NVSManager::getInstance().getString("ssid", ssid, "");
    NVSManager::getInstance().getString("pass", pass, "");
    s_last_join = esp_timer_get_time();
    if (ssid.empty()) return false;
    if (!WiFiManager::getInstance().connectSTA(ssid, pass)) return false;
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);       // in reach of a phone, without keeping the receiver on
    refresh_url();
    return true;
}

bool start()
{
    if (!s_pin) s_pin = 1000 + (int)(esp_random() % 9000);
    if (!join()) { ESP_LOGW(TAG, "no network: the charge page is off"); return false; }
    if (!s_httpd) {
        httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
        cfg.stack_size = 6144;
        cfg.max_uri_handlers = 6;
        cfg.max_open_sockets = 4;
        cfg.lru_purge_enable = true;
        if (httpd_start(&s_httpd, &cfg) != ESP_OK) { s_httpd = nullptr; return false; }
        const httpd_uri_t uris[] = {
            {.uri = "/", .method = HTTP_GET, .handler = get_page, .user_ctx = nullptr},
            {.uri = "/s", .method = HTTP_GET, .handler = get_status, .user_ctx = nullptr},
            {.uri = "/r", .method = HTTP_POST, .handler = post_reading, .user_ctx = nullptr},
            {.uri = "/tg", .method = HTTP_POST, .handler = post_telegram, .user_ctx = nullptr},
        };
        for (const auto &u : uris) httpd_register_uri_handler(s_httpd, &u);
    }
    ESP_LOGW(TAG, "charge page at %s", s_url);
    return true;
}

void stop()
{
    if (s_httpd) { httpd_stop(s_httpd); s_httpd = nullptr; }
    WiFiManager::getInstance().shutdown();
    s_url[0] = 0;
}

bool up() { return s_httpd != nullptr && s_url[0]; }
const char *url() { return s_url; }
int pin() { return s_pin; }

void publish(const char *json)
{
    portENTER_CRITICAL(&s_mux);
    strncpy(s_status, json, sizeof(s_status) - 1);
    s_status[sizeof(s_status) - 1] = 0;
    portEXIT_CRITICAL(&s_mux);
}

bool take_reading(float &volts)
{
    bool got = false;
    portENTER_CRITICAL(&s_mux);
    if (s_have_reading) { volts = s_reading; s_have_reading = false; got = true; }
    portEXIT_CRITICAL(&s_mux);
    return got;
}

void keep_alive()
{
    if (!s_httpd) return;
    if (WiFiManager::getInstance().isConnected()) { refresh_url(); return; }
    // connectSTA() gives up for good on failure (a battery rule of the product); on USB, try again
    // once a minute.
    if (esp_timer_get_time() - s_last_join < 60LL * 1000000) return;
    ESP_LOGW(TAG, "network dropped: rejoining");
    join();
}

bool telegram_configured()
{
    std::string tok, chat;
    NVSManager::getInstance().getString("bn_tok", tok, "");
    NVSManager::getInstance().getString("bn_chat", chat, "");
    return !tok.empty() && !chat.empty();
}

bool telegram(const char *text)
{
    if (!WiFiManager::getInstance().isConnected()) return false;
    std::string tok, chat;
    NVSManager::getInstance().getString("bn_tok", tok, "");
    NVSManager::getInstance().getString("bn_chat", chat, "");
    if (tok.empty() || chat.empty()) return false;
    return Telegram::send(tok, chat, text);
}

}  // namespace CurvePage
