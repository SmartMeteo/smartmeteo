#include "nvs_manager.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"

static const char* TAG = "NVS_MANAGER";
static const char* NAMESPACE = "smart_meteo";

NVSManager& NVSManager::getInstance() {
    static NVSManager instance;
    return instance;
}

bool NVSManager::init() {
    if (initialized) {
        return true;
    }
    
    ESP_LOGI(TAG, "Initializing NVS Flash partition...");
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition needs erase, erasing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS flash initialization failed: %s", esp_err_to_name(ret));
        return false;
    }
    
    initialized = true;
    return true;
}

bool NVSManager::getU8(const char* key, uint8_t &value, uint8_t default_val) {
    if (!initialized) return false;
    
    nvs_handle_t my_handle;
    esp_err_t err = nvs_open(NAMESPACE, NVS_READONLY, &my_handle);
    if (err != ESP_OK) {
        value = default_val;
        return false;
    }
    
    err = nvs_get_u8(my_handle, key, &value);
    nvs_close(my_handle);
    
    if (err != ESP_OK) {
        value = default_val;
        return false;
    }
    return true;
}

bool NVSManager::setU8(const char* key, uint8_t value) {
    if (!initialized) return false;
    
    nvs_handle_t my_handle;
    esp_err_t err = nvs_open(NAMESPACE, NVS_READWRITE, &my_handle);
    if (err != ESP_OK) return false;
    
    err = nvs_set_u8(my_handle, key, value);
    if (err == ESP_OK) {
        err = nvs_commit(my_handle);
    }
    nvs_close(my_handle);
    return (err == ESP_OK);
}

bool NVSManager::getU32(const char* key, uint32_t &value, uint32_t default_val) {
    if (!initialized) return false;
    
    nvs_handle_t my_handle;
    esp_err_t err = nvs_open(NAMESPACE, NVS_READONLY, &my_handle);
    if (err != ESP_OK) {
        value = default_val;
        return false;
    }
    
    err = nvs_get_u32(my_handle, key, &value);
    nvs_close(my_handle);
    
    if (err != ESP_OK) {
        value = default_val;
        return false;
    }
    return true;
}

bool NVSManager::setU32(const char* key, uint32_t value) {
    if (!initialized) return false;
    
    nvs_handle_t my_handle;
    esp_err_t err = nvs_open(NAMESPACE, NVS_READWRITE, &my_handle);
    if (err != ESP_OK) return false;
    
    err = nvs_set_u32(my_handle, key, value);
    if (err == ESP_OK) {
        err = nvs_commit(my_handle);
    }
    nvs_close(my_handle);
    return (err == ESP_OK);
}

bool NVSManager::getFloat(const char* key, float &value, float default_val) {
    // Floats are stored as raw 32-bit values
    uint32_t u32_val;
    union {
        uint32_t u;
        float f;
    } convert;
    
    convert.f = default_val;
    if (getU32(key, u32_val, convert.u)) {
        convert.u = u32_val;
        value = convert.f;
        return true;
    }
    value = default_val;
    return false;
}

bool NVSManager::setFloat(const char* key, float value) {
    union {
        uint32_t u;
        float f;
    } convert;
    convert.f = value;
    return setU32(key, convert.u);
}

bool NVSManager::getString(const char* key, std::string &value, const char* default_val) {
    if (!initialized) return false;
    
    nvs_handle_t my_handle;
    esp_err_t err = nvs_open(NAMESPACE, NVS_READONLY, &my_handle);
    if (err != ESP_OK) {
        value = default_val;
        return false;
    }
    
    size_t required_size = 0;
    err = nvs_get_str(my_handle, key, nullptr, &required_size);
    if (err != ESP_OK || required_size == 0) {
        value = default_val;
        nvs_close(my_handle);
        return false;
    }
    
    char* buf = (char*)malloc(required_size);
    if (!buf) {
        value = default_val;
        nvs_close(my_handle);
        return false;
    }
    
    err = nvs_get_str(my_handle, key, buf, &required_size);
    if (err == ESP_OK) {
        value = std::string(buf);
    } else {
        value = default_val;
    }
    
    free(buf);
    nvs_close(my_handle);
    return (err == ESP_OK);
}

bool NVSManager::setString(const char* key, const std::string &value) {
    if (!initialized) return false;
    
    nvs_handle_t my_handle;
    esp_err_t err = nvs_open(NAMESPACE, NVS_READWRITE, &my_handle);
    if (err != ESP_OK) return false;
    
    err = nvs_set_str(my_handle, key, value.c_str());
    if (err == ESP_OK) {
        err = nvs_commit(my_handle);
    }
    nvs_close(my_handle);
    return (err == ESP_OK);
}

bool NVSManager::getBlob(const char* key, uint8_t *dest, size_t &length) {
    if (!initialized) return false;
    
    nvs_handle_t my_handle;
    esp_err_t err = nvs_open(NAMESPACE, NVS_READONLY, &my_handle);
    if (err != ESP_OK) {
        return false;
    }
    
    err = nvs_get_blob(my_handle, key, dest, &length);
    nvs_close(my_handle);
    
    return (err == ESP_OK);
}

bool NVSManager::setBlob(const char* key, const uint8_t *src, size_t length) {
    if (!initialized) return false;
    
    nvs_handle_t my_handle;
    esp_err_t err = nvs_open(NAMESPACE, NVS_READWRITE, &my_handle);
    if (err != ESP_OK) {
        return false;
    }
    
    err = nvs_set_blob(my_handle, key, src, length);
    if (err == ESP_OK) {
        err = nvs_commit(my_handle);
    }
    nvs_close(my_handle);
    
    return (err == ESP_OK);
}
