# Bosch BSEC — the air-quality index

Smart Meteo turns the BME690's gas resistance into an indoor-air-quality index with **Bosch
Sensortec Environmental Cluster (BSEC) 3.x**. BSEC is closed: Bosch's licence lets the compiled
library ship inside a product that uses a Bosch sensor, but does not allow passing the library, its
headers or its configuration files on to anyone else. So this repository does not contain them.

## Without BSEC

The firmware builds and runs without it. `main/CMakeLists.txt` notices that the files below are
missing and compiles `main/bsec_manager_none.cpp` instead of `main/bsec_manager.cpp`. The device then
has everything except the index:

- CO₂, temperature and humidity (SCD41), the **Stuffy** and **Damp** warnings;
- pressure and the barometer forecast — the BME690 is still read, with its heater off;
- the screen, console, Telegram, MQTT / Home Assistant and Matter.

Where the index would be, the screen says **No IAQ**, and the console, Telegram and Home Assistant say
**No air-quality index**. Nothing pretends to be warming up.

The official firmware images (over-the-air updates and releases) are built with BSEC.

## Adding BSEC

1. Download **BSEC 3.3.x** from Bosch Sensortec
   (<https://www.bosch-sensortec.com/software/bme/bsec/>) and accept the licence there.
2. Copy four files out of the package into this folder:

   | From the BSEC package | To |
   |---|---|
   | `release_bin/IAQ/bin/esp/esp32_c2c3/libalgobsec.a` | `lib/libalgobsec.a` |
   | `release_bin/IAQ/inc/bsec_interface.h` | `include/bsec_interface.h` |
   | `release_bin/IAQ/inc/bsec_datatypes.h` | `include/bsec_datatypes.h` |
   | `release_bin/IAQ/config/bme690/bme690_iaq_33v_300s_4d/bsec_iaq.config` | `config/bsec_iaq.config` |

   Paths are as in BSEC 3.3.0.0. The library is Bosch's RISC-V build, which they label
   `esp32_c2c3`; it is the one that runs on the ESP32-C6. Use the `IAQ` folder, not `Sel_IAQ`. The
   configuration must be the **3.3 V, 300 s, 4-day** one: the firmware's gas cadence is built around
   300 s (ultra-low power) and the stored calibration is not interchangeable between cadences.
3. Build again (`idf.py fullclean build`). The build log no longer says
   «building WITHOUT the air-quality index».

All four or none: with only some of them the build stops and names what is missing, because a
half-added BSEC that quietly built without the index would be worse than an error.

Keep them out of any public copy of this repository: the licence does not allow redistributing
them, even unchanged.
