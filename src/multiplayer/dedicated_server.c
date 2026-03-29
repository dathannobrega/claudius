#include "dedicated_server.h"

#ifdef ENABLE_MULTIPLAYER

#include "mp_autosave.h"
#include "player_registry.h"
#include "network/session.h"

#include <string.h>
#include <stdio.h>
#include <ctype.h>

static mp_dedicated_server_options options;
static int shutdown_requested;

static char *trim_banlist_text(char *text)
{
    char *end;

    while (*text && isspace((unsigned char)*text)) {
        text++;
    }
    end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1])) {
        *--end = '\0';
    }
    return text;
}

static int ascii_case_equals(const char *a, const char *b)
{
    unsigned char ca;
    unsigned char cb;

    if (!a || !b) {
        return 0;
    }

    while (*a && *b) {
        ca = (unsigned char)*a++;
        cb = (unsigned char)*b++;
        if (tolower(ca) != tolower(cb)) {
            return 0;
        }
    }

    return *a == '\0' && *b == '\0';
}

void mp_dedicated_server_reset_options(void)
{
    memset(&options, 0, sizeof(options));
    shutdown_requested = 0;
    options.port = NET_DEFAULT_PORT;
    options.max_players = 8;
    options.dynamic_city_pool = 8;
    options.autosave_interval_sec = MP_AUTOSAVE_DEFAULT_INTERVAL;
    options.advertise_lan = 1;
    options.log_format = MP_DEDICATED_LOG_TEXT;
    options.reconnect_timeout_ticks = 5000;
    options.command_rate_limit_per_sec = 30;
    options.queue_limit_per_peer = 16;
    options.handshake_attempts_per_window = 8;
    options.handshake_window_ms = 10000;
    options.handshake_cooldown_ms = 10000;
    options.pending_connections_per_ip = 2;
    strncpy(options.server_name, "Claudius Dedicated", sizeof(options.server_name) - 1);
    strncpy(options.listen_address, "0.0.0.0", sizeof(options.listen_address) - 1);
}

void mp_dedicated_server_set_options(const mp_dedicated_server_options *new_options)
{
    mp_dedicated_server_reset_options();
    if (!new_options) {
        return;
    }
    options = *new_options;
    if (!options.server_name[0]) {
        strncpy(options.server_name, "Claudius Dedicated",
                sizeof(options.server_name) - 1);
    }
    if (!options.listen_address[0]) {
        strncpy(options.listen_address, "0.0.0.0",
                sizeof(options.listen_address) - 1);
    }
    if (options.max_players < 1) {
        options.max_players = 1;
    } else if (options.max_players > NET_MAX_PLAYERS) {
        options.max_players = NET_MAX_PLAYERS;
    }
    if (options.dynamic_city_pool < 1) {
        options.dynamic_city_pool = options.max_players;
    } else if (options.dynamic_city_pool > NET_MAX_PLAYERS) {
        options.dynamic_city_pool = NET_MAX_PLAYERS;
    }
    if (options.autosave_interval_sec <= 0) {
        options.autosave_interval_sec = MP_AUTOSAVE_DEFAULT_INTERVAL;
    }
    if (options.handshake_attempts_per_window == 0) {
        options.handshake_attempts_per_window = 8;
    }
    if (options.handshake_window_ms == 0) {
        options.handshake_window_ms = 10000;
    }
    if (options.handshake_cooldown_ms == 0) {
        options.handshake_cooldown_ms = 10000;
    }
    if (options.pending_connections_per_ip == 0) {
        options.pending_connections_per_ip = 2;
    }
}

const mp_dedicated_server_options *mp_dedicated_server_get_options(void)
{
    return &options;
}

int mp_dedicated_server_is_enabled(void)
{
    return options.enabled;
}

const char *mp_dedicated_server_get_save_dir(void)
{
    return options.save_dir;
}

const char *mp_dedicated_server_get_listen_address(void)
{
    return options.listen_address;
}

int mp_dedicated_server_is_banned(const uint8_t *player_uuid, const char *remote_address)
{
    FILE *fp;
    char uuid_string[40];
    char line[256];

    if (!options.enabled || !options.banlist_path[0]) {
        return 0;
    }

    fp = fopen(options.banlist_path, "rt");
    if (!fp) {
        return 0;
    }

    uuid_string[0] = '\0';
    if (player_uuid) {
        mp_player_registry_uuid_to_string(player_uuid, uuid_string, sizeof(uuid_string));
    }

    while (fgets(line, sizeof(line), fp)) {
        char *cursor = trim_banlist_text(line);
        char *comment = strchr(cursor, '#');
        char *semicolon = strchr(cursor, ';');
        char *value = cursor;
        int match = 0;

        if (semicolon && (!comment || semicolon < comment)) {
            comment = semicolon;
        }
        if (comment) {
            *comment = '\0';
            cursor = trim_banlist_text(cursor);
        }
        if (!cursor[0]) {
            continue;
        }

        if (strncmp(cursor, "uuid=", 5) == 0 || strncmp(cursor, "uuid:", 5) == 0) {
            value = trim_banlist_text(cursor + 5);
            match = uuid_string[0] && ascii_case_equals(value, uuid_string);
        } else if (strncmp(cursor, "ip=", 3) == 0 || strncmp(cursor, "ip:", 3) == 0) {
            value = trim_banlist_text(cursor + 3);
            match = remote_address && remote_address[0] && strcmp(value, remote_address) == 0;
        } else if (strchr(cursor, '.')) {
            match = remote_address && remote_address[0] && strcmp(cursor, remote_address) == 0;
        } else {
            match = uuid_string[0] && ascii_case_equals(cursor, uuid_string);
        }

        if (match) {
            fclose(fp);
            return 1;
        }
    }

    fclose(fp);
    return 0;
}

void mp_dedicated_server_request_shutdown(void)
{
    shutdown_requested = 1;
}

int mp_dedicated_server_consume_shutdown_request(void)
{
    int requested = shutdown_requested;
    shutdown_requested = 0;
    return requested;
}

#endif /* ENABLE_MULTIPLAYER */
