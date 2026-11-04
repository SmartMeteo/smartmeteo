// Три модуля прошивки, заглушённые на границе КЛАССА, а не под ней. У каждого своя причина, и
// причина — не «так проще»:
//
//   WebServer     — выход цикла, а не цикл. Его поведение уже покрыто с двух сторон: t_console
//                   сверяет обработчики с консолью в обе стороны, t_browser гоняет настоящий
//                   Chromium. Поднимать httpd здесь значило бы проверять третий раз то же самое.
//   MqttPublisher — то же: договор тем проверяет t_console, а окно связи меряется на приборе.
//   MatterMode    — за ним стоит CHIP, которого на этой машине нет. И проверять здесь нечего:
//                   режим Matter не уходит в глубокий сон и не зовёт sleep_after_cycle(), то есть
//                   к расписанию Wi-Fi, которое здесь под проверкой, отношения не имеет.
//
// Всё остальное, что цикл зовёт, — НАСТОЯЩИЙ код прошивки: датчики, кольца, аналитика, экран,
// настройки, солнце и луна.
#include "web_server.h"
#include "mqtt_publisher.h"
#include "matter_mode.h"
#include "host.h"
#include "esp_system.h"
#include <string>

WebServer &WebServer::getInstance() { static WebServer s; return s; }
bool WebServer::start() { return true; }
void WebServer::stop()  {}

namespace MqttPublisher {
std::string buildState() { return "{}"; }
Result publishState(const std::string &payload, bool also_discovery, std::string *weather_out,
                    int timeout_ms, const std::string *alert, bool *alert_acked) {
    (void)payload; (void)also_discovery; (void)weather_out; (void)timeout_ms; (void)alert;
    if (alert_acked) *alert_acked = false;
    // По умолчанию отказ: расписание обязано пережить недоступного брокера, и это ветка, которую
    // молчаливый успех навсегда спрятал бы. Погоду этот брокер не публикует даже когда отвечает —
    // так проверяется случай «хаб на месте, но погоды в нём нет», ради которого шесть часов и есть.
    return Result{Host::mqtt_ok, Host::mqtt_ok ? nullptr : "connect"};
}
}  // namespace MqttPublisher

namespace MatterMode {
bool selected()        { return false; }
bool start()           { return false; }
bool running()         { return false; }
uint8_t fabrics()      { return 0; }   // «ни в одном умном доме»: экран подключения, а не дашборд
bool window_open()     { return false; }
bool session_active()  { return false; }
bool open_window()     { return false; }
bool thread_attached() { return false; }
bool codes(Codes &out, bool rf_active) { (void)out; (void)rf_active; return false; }
int  qr_grid(const char *text, uint8_t *grid, size_t grid_len) { (void)text; (void)grid; (void)grid_len; return 0; }
void publish(const Readings &r) { (void)r; }
void set_event_hook(void (*hook)(void)) { (void)hook; }
uint32_t take_events() { return 0; }
bool forget_requested() { return false; }
void clear_forget() {}
[[noreturn]] void forget_and_restart() { esp_restart(); __builtin_unreachable(); }
}  // namespace MatterMode
