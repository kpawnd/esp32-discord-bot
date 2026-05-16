#include "warnings.h"
#include "config/nvs_config.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_err.h"
#include <string.h>
#include <stdlib.h>

#define TAG "warnings"

typedef struct {
    uint32_t user_id_lo; /* low 32 bits of Discord user ID */
    uint8_t  count;
    uint8_t  valid;
} warn_entry_t;

static warn_entry_t s_table[WARN_TABLE_SIZE];
static nvs_handle_t s_nvs;

esp_err_t warnings_init(void) {
    memset(s_table, 0, sizeof(s_table));
    /* Load existing warnings from NVS into the table */
    esp_err_t err = nvs_open("warnings", NVS_READWRITE, &s_nvs);
    if (err != ESP_OK) ESP_LOGW(TAG, "nvs_open warnings: %s", esp_err_to_name(err));
    return err;
}

static uint32_t parse_user_id_lo(const char *user_id) {
    if (!user_id || user_id[0] == '\0') return 0;
    return (uint32_t)(strtoull(user_id, NULL, 10) & 0xFFFFFFFF);
}

static int find_slot(uint32_t uid_lo) {
    /* Linear probe: find existing entry or first empty slot */
    int empty = -1;
    for (int i = 0; i < WARN_TABLE_SIZE; i++) {
        if (s_table[i].valid && s_table[i].user_id_lo == uid_lo) return i;
        if (!s_table[i].valid && empty < 0) empty = i;
    }
    return empty; /* new slot */
}

static void nvs_flush(int slot, const char *user_id) {
    if (s_nvs == 0) return;
    char key[16];
    snprintf(key, sizeof(key), "w%u", (unsigned)s_table[slot].user_id_lo);
    nvs_set_u8(s_nvs, key, s_table[slot].count);
    nvs_commit(s_nvs);
    ESP_LOGD(TAG, "flush: user %s → %d warnings", user_id, s_table[slot].count);
}

uint8_t warnings_get(const char *user_id) {
    uint32_t uid = parse_user_id_lo(user_id);
    for (int i = 0; i < WARN_TABLE_SIZE; i++) {
        if (s_table[i].valid && s_table[i].user_id_lo == uid)
            return s_table[i].count;
    }
    /* Try NVS */
    if (s_nvs) {
        char key[16];
        snprintf(key, sizeof(key), "w%u", (unsigned)uid);
        uint8_t v = 0;
        nvs_get_u8(s_nvs, key, &v);
        return v;
    }
    return 0;
}

uint8_t warnings_increment(const char *user_id) {
    uint32_t uid = parse_user_id_lo(user_id);
    int slot = find_slot(uid);
    if (slot < 0) {
        /* Table full — evict slot 0 (LRU not tracked; simple eviction) */
        slot = 0;
        ESP_LOGW(TAG, "Warning table full, evicting slot 0");
    }
    if (!s_table[slot].valid) {
        /* New entry — load from NVS first */
        s_table[slot].valid      = 1;
        s_table[slot].user_id_lo = uid;
        s_table[slot].count      = warnings_get(user_id);
    }
    s_table[slot].count++;
    nvs_flush(slot, user_id);
    return s_table[slot].count;
}

void warnings_clear(const char *user_id) {
    uint32_t uid = parse_user_id_lo(user_id);
    for (int i = 0; i < WARN_TABLE_SIZE; i++) {
        if (s_table[i].valid && s_table[i].user_id_lo == uid) {
            s_table[i].count = 0;
            nvs_flush(i, user_id);
            return;
        }
    }
}
