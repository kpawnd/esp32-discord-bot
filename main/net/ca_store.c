#include "net/ca_store.h"
#include "esp_spiffs.h"
#include "esp_log.h"
#include <sys/stat.h>
#include <stdlib.h>
#include <stdio.h>

#define TAG "ca_store"
#define CA_PATH "/spiffs/ca.pem"
#define MAX_CA_SIZE (8 * 1024)

static bool   s_spiffs_ready = false;
static char  *s_ca_pem = NULL;
static size_t s_ca_len = 0;

static esp_err_t spiffs_mount(void) {
    if (s_spiffs_ready) return ESP_OK;

    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = "spiffs",
        .max_files = 2,
        .format_if_mount_failed = false,
    };

    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SPIFFS mount failed: %s", esp_err_to_name(err));
        return err;
    }

    s_spiffs_ready = true;
    return ESP_OK;
}

esp_err_t ca_store_init(void) {
    if (s_ca_pem) return ESP_OK;

    esp_err_t err = spiffs_mount();
    if (err != ESP_OK) return err;

    struct stat st;
    if (stat(CA_PATH, &st) != 0) {
        ESP_LOGI(TAG, "No custom CA at %s", CA_PATH);
        return ESP_OK;
    }

    if (st.st_size <= 0 || st.st_size > MAX_CA_SIZE) {
        ESP_LOGW(TAG, "CA size invalid: %ld bytes", (long)st.st_size);
        return ESP_ERR_INVALID_SIZE;
    }

    FILE *f = fopen(CA_PATH, "rb");
    if (!f) {
        ESP_LOGW(TAG, "Failed to open %s", CA_PATH);
        return ESP_FAIL;
    }

    s_ca_pem = (char *)malloc((size_t)st.st_size + 1);
    if (!s_ca_pem) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }

    size_t read = fread(s_ca_pem, 1, (size_t)st.st_size, f);
    fclose(f);
    if (read != (size_t)st.st_size) {
        free(s_ca_pem);
        s_ca_pem = NULL;
        return ESP_FAIL;
    }

    s_ca_pem[read] = '\0';
    s_ca_len = read + 1;
    ESP_LOGI(TAG, "Loaded custom CA (%d bytes)", (int)read);
    return ESP_OK;
}

bool ca_store_has_custom(void) { return s_ca_pem != NULL; }
const char *ca_store_pem(void) { return s_ca_pem; }
size_t ca_store_pem_len(void)  { return s_ca_len; }
