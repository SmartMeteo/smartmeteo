// The gas manager for a build WITHOUT Bosch's BSEC — what the public source compiles to.
//
// Bosch's licence (December 2024) lets the compiled library ship inside a product with a BME690
// (§2.1) but forbids handing the library, its headers or its configuration to anyone else (§3.1),
// so the public repository cannot carry components/bsec2. main/CMakeLists.txt compiles this file
// instead of bsec_manager.cpp when those files are absent. The official image always has them.
//
// What the device does without it (bench_screens.cpp, read_sensors): it still reads the BME690 —
// pressure, its own temperature and humidity — with the heater off, so the barometer and the rain
// forecast work as before. What it cannot do is turn gas resistance into an air-quality index; that
// IS the closed part. So there is no index, and every surface says so in words instead of showing
// «Warming up» for ever (BSECManager::builtIn()).
//
// Every method answers «nothing here»: init() fails, so the cycle never enters BSEC's loop, and the
// state persistence has no state to keep.
#include "bsec_manager.h"

bool BSECManager::builtIn() { return false; }

BSECManager& BSECManager::getInstance() {
    static BSECManager instance;
    return instance;
}

bool BSECManager::init() { return false; }

bool BSECManager::getSensorControl(int64_t, BSECSensorRequest &req) { req = {}; return false; }

bool BSECManager::processData(int64_t, const BSECSensorRequest &, float, float, float, float, bool,
                              BSECOutputs &) {
    return false;
}

bool BSECManager::loadState() { return false; }

const char* BSECManager::lastLoadSource() const { return "not in this build"; }
void        BSECManager::armLoadProbe() {}
const char* BSECManager::gateSource() const { return "not in this build"; }
uint32_t    BSECManager::gateBlobLen() const { return 0; }

bool BSECManager::saveState()    { return false; }
bool BSECManager::saveStateNVS() { return false; }
void BSECManager::invalidateRtcState() {}
