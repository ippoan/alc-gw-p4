#include <string.h>

#include "nvs.h"
#include "esp_log.h"

#include "credential.h"

static const char *TAG = "credential";

#define NVS_NAMESPACE "hub_link"
#define KEY_DEV_ID "dev_id"
#define KEY_DEV_SECRET "dev_secret"

bool credential_load(char *device_id, size_t device_id_cap, char *device_secret, size_t device_secret_cap) {
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }

    size_t id_len = device_id_cap;
    size_t secret_len = device_secret_cap;
    esp_err_t id_err = nvs_get_str(h, KEY_DEV_ID, device_id, &id_len);
    esp_err_t secret_err = nvs_get_str(h, KEY_DEV_SECRET, device_secret, &secret_len);
    nvs_close(h);

    if (id_err != ESP_OK || secret_err != ESP_OK) {
        return false;
    }
    return device_id[0] != '\0' && device_secret[0] != '\0';
}

esp_err_t credential_save(const char *device_id, const char *device_secret) {
    if (device_id == NULL || device_secret == NULL || device_id[0] == '\0' || device_secret[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(device_id) >= CREDENTIAL_ID_MAX_LEN || strlen(device_secret) >= CREDENTIAL_SECRET_MAX_LEN) {
        return ESP_ERR_INVALID_SIZE;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_str(h, KEY_DEV_ID, device_id);
    if (err == ESP_OK) {
        err = nvs_set_str(h, KEY_DEV_SECRET, device_secret);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "credential saved (device_id=%s)", device_id);
    } else {
        ESP_LOGE(TAG, "credential save failed: %s", esp_err_to_name(err));
    }
    return err;
}
