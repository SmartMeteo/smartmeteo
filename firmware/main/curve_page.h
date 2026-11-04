#pragma once
// The battery bench's charge page: while a charge runs, the bench says when to read the volts across
// R7 (TP4056's PROG resistor: 1 V = 1 A) and the owner types the reading on a phone instead of on
// paper. Wi-Fi comes up for the charge only — the board runs from USB then, so the radio costs the
// cell nothing — in modem power save, so it heats the board as little as it can.
//
// The page writes nothing itself: a reading is handed to the bench's loop (take_reading()), which
// is the log's only writer. A four-digit code, drawn per boot and shown only on the e-ink, gates
// every write — whoever types it is standing at the device. Five wrong codes lock writes for a
// minute. Telegram is optional and send-only: the bench pings "measure now" with a link, and never
// reads the bot's updates, so a bot shared with the main device does not collide with it. The token
// is typed on the page by the owner, kept in NVS ("bn_tok", not the product's keys), and never
// shown back or logged.

namespace CurvePage {

bool start();                     // false: no saved network, or it did not join
void stop();
bool up();
const char *url();                // "http://192.168.1.23/", "" while down
int  pin();

void publish(const char *json);   // the status the page polls, built by the loop each second
bool take_reading(float &volts);  // a reading entered on the page since the last call
void keep_alive();                // rejoin if the network dropped; call every few seconds

bool telegram_configured();
bool telegram(const char *text);  // to the configured chat; false if none or it failed

}  // namespace CurvePage
