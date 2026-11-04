#pragma once

// The release identity, and the two forms it takes.
//
// FW_SEQ is a plain monotonic counter and it is what the update manifest compares. Never a parsed
// version string: "1.10.0" against "1.9.0" is a string-comparison trap, and a device that decides
// wrongly here either refuses a fix or installs an older image over a newer one. Bump it by one on
// every release, never reuse a number, and never let two different binaries carry the same one.
//
// FW_VERSION is the human form, set in sdkconfig as CONFIG_APP_PROJECT_VER so that the bootloader,
// the app description and `esptool image_info` all report the same thing. It is what the owner
// reads; nothing decides anything by it.
//
// SINCE 2026-09-18 THAT STRING MUST END WITH "+<FW_SEQ>" — "0.10.3+68".
// The installer reads the sequence back out of the downloaded image's own description and refuses
// anything whose number is not the one the manifest promised, or is not newer than what is running
// (ota_install.cpp). Without it the manifest was the only thing that said how new an image was, and
// the manifest is unsigned JSON on a server: a compromised one could point a high `seq` at a
// genuinely signed old release and roll the whole fleet back. Bump both numbers together, and
// publish with the same full string: tools/vps/publish.sh refuses a mismatch.
#define FW_SEQ 115
