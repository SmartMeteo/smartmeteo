#include "telegram.h"
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "https_client.h"
#include "ArduinoJson.h"
#include <vector>
#include <cstdio>

static const char* TAG = "TELEGRAM";

// Telegram's HTML mode, so that the first line — the device and what happened — is bold. Everything is
// escaped first: the device name is the owner's own text, and a stray "<" or "&" would make the API
// refuse the whole message (Bot API, formatting options: outside tags <, > and & must be entities). If the
// API refuses the markup anyway, the same words go out once more as plain text — a message without bold
// is worth far more than no message. MQTT carries the plain text; only Telegram gets the markup.
static std::string html_escape(const std::string &s)
{
    std::string o;
    o.reserve(s.size() + 16);
    for (char c : s) {
        if (c == '&')      o += "&amp;";
        else if (c == '<') o += "&lt;";
        else if (c == '>') o += "&gt;";
        else               o += c;
    }
    return o;
}

static bool post_message(const std::string &token, const std::string &chat, const std::string &text,
                         bool html, int &status)
{
    std::string url = "https://api.telegram.org/bot" + token + "/sendMessage";
    JsonDocument doc;
    doc["chat_id"] = chat;
    doc["text"] = text;
    if (html) doc["parse_mode"] = "HTML";
    std::string body;
    serializeJson(doc, body);
    status = -1;
    const esp_err_t err = Https::postJson(url.c_str(), body, 10000, status);
    return err == ESP_OK && status == 200;
}

bool Telegram::send(const std::string &token, const std::string &chat,
                    const std::string &text) {
    if (token.empty() || chat.empty()) return false;

    const size_t nl = text.find('\n');
    const std::string html = "<b>" + html_escape(text.substr(0, nl)) + "</b>" +
                             (nl == std::string::npos ? std::string() : html_escape(text.substr(nl)));
    int status = -1;
    if (post_message(token, chat, html, true, status)) {
        ESP_LOGI(TAG, "Message sent");
        return true;
    }
    if (status == 400) {
        ESP_LOGW(TAG, "formatted message refused (HTTP 400) — sending it as plain text");
        if (post_message(token, chat, text, false, status)) {
            ESP_LOGI(TAG, "Message sent as plain text");
            return true;
        }
    }
    ESP_LOGW(TAG, "Send failed: status=%d", status);
    return false;
}

int Telegram::getUpdates(const std::string &token, int32_t offset,
                         TgUpdate *out, int max_out) {
    if (token.empty() || max_out <= 0) return -1;

    char off[16]; snprintf(off, sizeof(off), "%ld", (long)offset);
    std::string url = "https://api.telegram.org/bot" + token +
                      "/getUpdates?timeout=0&limit=" + std::to_string(max_out) + "&offset=" + off;

    Https::Options opt;
    opt.rx_buffer = 2048;
    std::vector<char> buf(4096);
    const Https::Body got = Https::get(url.c_str(), buf.data(), (int)buf.size(), opt);
    if (got.err != ESP_OK) return -1;
    const int status = got.status;
    const int total = got.len;

    int n = -1;
    if (status == 200) {
        // Parse only what we need (update_id, chat id, text) to stay small.
        JsonDocument filter;
        filter["result"][0]["update_id"] = true;
        filter["result"][0]["message"]["chat"]["id"] = true;
        filter["result"][0]["message"]["text"] = true;
        JsonDocument doc;
        if (deserializeJson(doc, buf.data(), DeserializationOption::Filter(filter)) == DeserializationError::Ok) {
            n = 0;
            for (JsonObject u : doc["result"].as<JsonArray>()) {
                if (n >= max_out) break;
                out[n].update_id = u["update_id"] | 0;
                int64_t cid = u["message"]["chat"]["id"] | (int64_t)0;
                char cbuf[24]; snprintf(cbuf, sizeof(cbuf), "%lld", (long long)cid);
                out[n].chat = cbuf;
                out[n].text = (const char*)(u["message"]["text"] | "");
                n++;
            }
        } else if (total >= (int)buf.size() - 1) {
            // The response filled the buffer, so it was cut mid-JSON and cannot be parsed. The
            // caller only advances the offset on a successful parse, which means one oversized
            // message -- a single maximum-length Telegram post is about the size of this buffer
            // -- would leave the same batch being re-fetched every radio window and no command
            // ever answered again. Telegram expires updates after a day, so it does heal on its
            // own, but a day of silence from a feature the owner turned on is not acceptable
            // when the head of the batch is legible right here: update_id appears in the first
            // few dozen bytes, long before the truncation. Report it with no messages attached
            // so the caller steps past it and keeps going.
            const char *k = strstr(buf.data(), "\"update_id\":");
            long long id = k ? atoll(k + 12) : 0;
            if (id > 0) {
                out[0].update_id = (int32_t)id;
                out[0].chat.clear();
                out[0].text.clear();
                n = 1;
                ESP_LOGW(TAG, "getUpdates reply truncated at %d bytes — skipping update %lld",
                         total, id);
            } else {
                ESP_LOGW(TAG, "getUpdates reply truncated at %d bytes and unreadable", total);
            }
        } else {
            ESP_LOGW(TAG, "getUpdates JSON parse failed");
        }
    } else {
        ESP_LOGW(TAG, "getUpdates HTTP %d", status);
    }
    return n;
}

bool Telegram::setCommands(const std::string &token) {
    if (token.empty()) return false;

    // Telegram stores this list against the bot and shows it in every chat with it. Sending it
    // once is enough — repeating it on every window would spend radio to tell Telegram something
    // it already knows.
    std::string url = "https://api.telegram.org/bot" + token + "/setMyCommands";

    JsonDocument doc;
    JsonArray cmds = doc["commands"].to<JsonArray>();
    auto add = [&](const char *c, const char *d) {
        JsonObject o = cmds.add<JsonObject>();
        o["command"] = c;
        o["description"] = d;
    };
    // Wording is what the owner sees in the menu, so it says what they get rather than what the
    // firmware calls it.
    add("status",  "CO\u2082, climate, air quality and battery");
    add("co2",     "CO\u2082 now, against your alert level");
    add("air",     "Air quality class and gas sensor accuracy");
    add("battery", "Charge, voltage and power source");

    std::string body;
    serializeJson(doc, body);

    int status = -1;
    const esp_err_t err = Https::postJson(url.c_str(), body, 10000, status);

    const bool ok = (err == ESP_OK && status == 200);
    ESP_LOGW(TAG, "command menu %s (HTTP %d)", ok ? "registered" : "NOT registered", status);
    return ok;
}
