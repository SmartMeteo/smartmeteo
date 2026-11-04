// What the device's own access-point password may contain, and how long it is.
//
// Its own file because four places must agree on it and none of them wants the Wi-Fi driver:
// wifi_manager.cpp draws and validates, portal_screen.cpp draws the glyphs, the console repeats the
// rule to the owner, and Software/mockups/portal_art.py renders a glyph for every character listed
// here and refuses to generate art that would not fit the line.
#pragma once

// EVERY printable character, at a FIXED eight (owner's decision, 2026-09-19: banning characters is
// the wrong trade — shorten the password and widen the alphabet instead).
//
// The length falls out of the glass. The password is printed on the setup screen in Montserrat
// Bold, which is proportional; that line is 202 px and the word "Password:" spends 76 px of it, so
// eight characters may be at most 15.75 px each. Every printable character measures under that
// except capital "W" at 16.3 px — the one exclusion here, and its lowercase twin is allowed. Space
// is left out for a different reason: it is invisible on the glass, so "ab cd" could not be told
// from "abcd" by the person retyping it.
//
// 93^8 is 52.3 bits, against 58.3 for the twelve narrow characters this replaces. The gap was
// measured before the change, not found after it, and Docs/compliance_2026-09-18.md carries the
// same number for AUM-5. Online guessing is bounded anyway (five wrong answers lock the console for
// a minute); the number that matters is an offline attack on a captured WPA2 handshake.
//
// MIN equals LEN because the length is fixed: exactly eight. Software/mockups/portal_art.py reads
// both from here, draws a glyph for every character, and refuses to generate art that would not fit.
#define PORTAL_AP_PASS_ALPHABET \
    "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVXYZ0123456789" \
    "!\"#$%&'()*+,-./:;<=>?@[\\]^_`{|}~"
#define PORTAL_AP_PASS_LEN 8
#define PORTAL_AP_PASS_MIN 8
// What the DEVICE draws for itself — the same alphabet without the characters a person cannot tell
// apart at 14 px on e-ink: I l 1 | , O 0 , and the quote-like and low punctuation. The owner may
// still type any of them; the machine has no reason to choose one, because the whole point of
// printing the password is that it gets read off the glass and typed into a phone. 79^8 = 50.4 bits.
#define PORTAL_AP_PASS_GEN_ALPHABET \
    "abcdefghijkmnopqrstuvwxyzABCDEFGHJKLMNPQRSTUVXYZ23456789" \
    "!#$%&()*+-/<=>?@[]^_{}~"
