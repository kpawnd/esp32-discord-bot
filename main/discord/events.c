#include "events.h"
#include "mod/filter.h"
#include "commands/dispatcher.h"
#include "esp_log.h"

#define TAG "events"

void events_dispatch_message(const discord_message_t *msg) {
    /* Run filter synchronously in the gateway callback.
     * filter_check_message is fast (string scan) and enqueues REST actions. */
    if (msg->author_id[0] == '\0') return; /* skip system messages */
    filter_check_message(msg);
}

void events_dispatch_interaction(const discord_interaction_t *interaction) {
    /* Discord invalidates interactions unless the first ACK arrives quickly. */
    esp_err_t err = commands_ack_interaction(interaction);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "failed to queue interaction ACK: %s", esp_err_to_name(err));
    }
}
