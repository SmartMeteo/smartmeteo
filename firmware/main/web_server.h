#pragma once

#include "esp_http_server.h"

class WebServer {
public:
    static WebServer& getInstance();
    
    bool start();
    void stop();

    // Is the HTTP server actually listening right now? The on-screen address band is driven by
    // this, not by "USB is plugged in": the band promises a reachable console, so it must appear
    // and disappear with the server itself.
    bool isRunning() const { return server_handle != nullptr; }
    
private:
    WebServer() : server_handle(nullptr) {}
    ~WebServer() = default;
    WebServer(const WebServer&) = delete;
    WebServer& operator=(const WebServer&) = delete;
    
    static esp_err_t getRootHandler(httpd_req_t *req);
    static esp_err_t getCaptiveHandler(httpd_req_t *req);
    static esp_err_t redirect404Handler(httpd_req_t *req, httpd_err_code_t err);
    static esp_err_t getStatusHandler(httpd_req_t *req);
    static esp_err_t getScanHandler(httpd_req_t *req);
    static esp_err_t postSaveHandler(httpd_req_t *req);
    static esp_err_t postRebootHandler(httpd_req_t *req);
    static esp_err_t postPowerOffHandler(httpd_req_t *req);
    static esp_err_t postFactoryHandler(httpd_req_t *req);
    // Firmware handed over the local network inside the setup window — the path that remains when
    // the remote one cannot be used. Refuses anything without a valid signature.
    static esp_err_t postOtaHandler(httpd_req_t *req);
    // Ask the update server now, rather than waiting for this device's daily hour.
    static esp_err_t postOtaCheckHandler(httpd_req_t *req);
    static esp_err_t postOtaInstallHandler(httpd_req_t *req);
    // Go back to the image that was running before. Refuses, rather than reboots, when there is
    // nothing valid to go back to.
    static esp_err_t postOtaRevertHandler(httpd_req_t *req);
    static esp_err_t postPortalHandler(httpd_req_t *req);
    static esp_err_t postCalibrateHandler(httpd_req_t *req);
    static esp_err_t postClockHandler(httpd_req_t *req);
    static esp_err_t postTempCalHandler(httpd_req_t *req);
    static esp_err_t postCloseHandler(httpd_req_t *req);
    static esp_err_t getIconHandler(httpd_req_t *req);
    static esp_err_t getManifestHandler(httpd_req_t *req);
    static esp_err_t getHistoryHandler(httpd_req_t *req);
    static esp_err_t getEventsHandler(httpd_req_t *req);
    static esp_err_t getGeocodeHandler(httpd_req_t *req);
    static esp_err_t postWeatherTestHandler(httpd_req_t *req);
    static esp_err_t postTelegramTestHandler(httpd_req_t *req);
    static esp_err_t postMqttTestHandler(httpd_req_t *req);

    httpd_handle_t server_handle;
};
