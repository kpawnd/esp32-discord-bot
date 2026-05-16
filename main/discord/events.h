#pragma once
#include "types.h"

/* Called synchronously from the gateway WS event callback */
void events_dispatch_message(const discord_message_t *msg);
void events_dispatch_interaction(const discord_interaction_t *interaction);
