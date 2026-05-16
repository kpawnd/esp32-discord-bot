#pragma once
#include <stdint.h>
#include <stdbool.h>

#define DISCORD_ID_LEN      24
#define DISCORD_TOKEN_LEN  128
#define DISCORD_ITOKEN_LEN 256   /* interaction token */
#define MSG_CONTENT_LEN    512
#define REASON_LEN         128
#define CMD_NAME_LEN        32
#define OPTION_LEN          64
#define URL_LEN            256

typedef struct {
    char id[DISCORD_ID_LEN];
    char content[MSG_CONTENT_LEN];
    char author_id[DISCORD_ID_LEN];
    char channel_id[DISCORD_ID_LEN];
    uint8_t mention_count;
    uint8_t url_count;
} discord_message_t;

typedef struct {
    char id[DISCORD_ID_LEN];
    char token[DISCORD_ITOKEN_LEN];
    char command[CMD_NAME_LEN];
    char option_action[OPTION_LEN];
    char option_user_id[DISCORD_ID_LEN];
    char option_reason[REASON_LEN];
    char invoker_id[DISCORD_ID_LEN];
    char guild_id[DISCORD_ID_LEN];
    char channel_id[DISCORD_ID_LEN];
} discord_interaction_t;

typedef enum {
    GW_STATE_DISCONNECTED = 0,
    GW_STATE_CONNECTING,
    GW_STATE_IDENTIFYING,
    GW_STATE_RESUMING,
    GW_STATE_READY,
} gateway_state_t;
