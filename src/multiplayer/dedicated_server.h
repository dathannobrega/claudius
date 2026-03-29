#ifndef MULTIPLAYER_DEDICATED_SERVER_H
#define MULTIPLAYER_DEDICATED_SERVER_H

#ifdef ENABLE_MULTIPLAYER

#include <stdint.h>

#define MP_DEDICATED_PATH_MAX 260

typedef enum {
    MP_DEDICATED_LOG_TEXT = 0,
    MP_DEDICATED_LOG_JSON = 1
} mp_dedicated_log_format;

typedef struct {
    int enabled;
    char server_name[64];
    char listen_address[64];
    uint16_t port;
    uint8_t max_players;
    uint8_t dynamic_city_pool;
    int autosave_interval_sec;
    int advertise_lan;
    mp_dedicated_log_format log_format;
    char save_dir[MP_DEDICATED_PATH_MAX];
    char status_file[MP_DEDICATED_PATH_MAX];
    char scenario_name[64];
    char resume_save[MP_DEDICATED_PATH_MAX];
    char banlist_path[MP_DEDICATED_PATH_MAX];
    uint32_t reconnect_timeout_ticks;
    uint32_t command_rate_limit_per_sec;
    uint32_t queue_limit_per_peer;
    uint32_t handshake_attempts_per_window;
    uint32_t handshake_window_ms;
    uint32_t handshake_cooldown_ms;
    uint32_t pending_connections_per_ip;
} mp_dedicated_server_options;

void mp_dedicated_server_reset_options(void);
void mp_dedicated_server_set_options(const mp_dedicated_server_options *options);
const mp_dedicated_server_options *mp_dedicated_server_get_options(void);
int mp_dedicated_server_is_enabled(void);
const char *mp_dedicated_server_get_save_dir(void);
const char *mp_dedicated_server_get_listen_address(void);
int mp_dedicated_server_is_banned(const uint8_t *player_uuid, const char *remote_address);
void mp_dedicated_server_request_shutdown(void);
int mp_dedicated_server_consume_shutdown_request(void);

#endif /* ENABLE_MULTIPLAYER */

#endif /* MULTIPLAYER_DEDICATED_SERVER_H */
