#pragma once

#include <stdint.h>
#include <string>

class NVSManager {
public:
    static NVSManager& getInstance();
    
    bool init();
    
    // Get/Set integer values
    bool getU8(const char* key, uint8_t &value, uint8_t default_val = 0);
    bool setU8(const char* key, uint8_t value);
    
    bool getU32(const char* key, uint32_t &value, uint32_t default_val = 0);
    bool setU32(const char* key, uint32_t value);
    
    bool getFloat(const char* key, float &value, float default_val = 0.0f);
    bool setFloat(const char* key, float value);
    
    // Get/Set strings (like Wi-Fi SSID, token etc)
    bool getString(const char* key, std::string &value, const char* default_val = "");
    bool setString(const char* key, const std::string &value);

    // Get/Set binary blobs
    bool getBlob(const char* key, uint8_t *dest, size_t &length);
    bool setBlob(const char* key, const uint8_t *src, size_t length);

private:
    NVSManager() : initialized(false) {}
    ~NVSManager() = default;
    NVSManager(const NVSManager&) = delete;
    NVSManager& operator=(const NVSManager&) = delete;
    
    bool initialized;
};
