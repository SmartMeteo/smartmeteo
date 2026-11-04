# Security

## Reporting a vulnerability

Please write to **security@smartmeteo.org**. Do not open a public issue for it.

Say what you found, how to reproduce it and which firmware version you saw it on (the console
shows it under System). You will get a reply within 7 days. Once a fix is released we will credit
you in the release notes, unless you would rather not be named.

## Supported versions

Only the latest released firmware gets fixes. Devices update themselves over the air, signed,
unless the owner has turned automatic updates off.

## What protects a device today

- The web console asks for a password. Each device draws its own random one, shown on its
  screen; there is no shared default.
- The console is not running most of the time: it starts after a long press of the device's
  button (or during first setup) and closes by itself.
- Firmware updates are signed (ECDSA) and checked on the device before they are installed.
- Matter runs on test credentials and is not certified.
- Flash and NVS are not encrypted on development units: someone with the device and a USB cable
  can read its settings, including the Wi-Fi password.
