#pragma once

// Captive-portal DNS server after Espressif's captive_portal example: every type-A query is answered
// with the AP's own IP (192.168.4.1), which makes phones pop the "sign in to network" sheet and open
// the config page automatically after joining the setup Wi-Fi.

class CaptiveDNS {
public:
    static CaptiveDNS& getInstance();
    void start();  // spawns the UDP:53 responder task (idempotent)
    void stop();

private:
    CaptiveDNS() = default;
    static void serverTask(void *arg);
    volatile bool running = false;
    volatile bool stop_requested = false;
};
