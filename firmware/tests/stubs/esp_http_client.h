// The transport under Https::, replaced by a string the test hands over. Everything above it —
// Https::Reader, the streaming parse, the day arithmetic — is the firmware's own code, unchanged.
#pragma once
#include <stddef.h>
#include <string>
#include "esp_err.h"

typedef void *esp_http_client_handle_t;

// What the next request will answer with. `chunk` makes the body arrive in pieces, the way a real
// socket delivers it, so a parser that assumes one read fails here rather than on the device.
namespace HttpStub {
extern std::string body;
extern std::string expires;   // the one header the MET path captures
extern int         status;
extern size_t      chunk;     // 0: the whole body in one read
extern int         reads;     // how many reads the last transfer took
void reset();
}

int esp_http_client_read(esp_http_client_handle_t c, char *buf, int len);
