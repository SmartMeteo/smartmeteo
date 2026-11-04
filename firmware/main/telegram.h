#pragma once

#include <string>
#include <cstdint>

// One inbound Telegram message (from getUpdates).
struct TgUpdate {
    int32_t     update_id = 0;
    std::string chat;   // chat id (as string, to compare with the configured one)
    std::string text;   // message text, e.g. "/status"
};

// Telegram Bot over HTTPS (certificate verified against the mbedTLS bundle —
// unlike the old Arduino build which used setInsecure()). Only usable in Wi-Fi
// standalone mode, where the device itself has internet during a radio window.
class Telegram {
public:
    // POST a text message to `chat` using bot `token`. Returns true on HTTP 200.
    // Call only while Wi-Fi STA is connected.
    static bool send(const std::string &token, const std::string &chat,
                     const std::string &text);

    // Long-poll for inbound commands: GET getUpdates with `offset` (pass -1 to
    // fetch only the latest, or last_id+1 to get everything newer). Writes up to
    // `max_out` messages into `out`, returns the count (or -1 on error). Cheap —
    // one HTTPS GET; call in a radio window (battery) or every ~30 s (USB).
    static int getUpdates(const std::string &token, int32_t offset,
                          TgUpdate *out, int max_out);

    // Register the bot's command list with Telegram, once. After this the chat shows a menu
    // button beside the text field and offers the commands as you type "/", so nobody has to
    // remember or spell them. Costs one request, ever — Telegram keeps the list on its side.
    static bool setCommands(const std::string &token);
};
