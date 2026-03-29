#include "SDL.h"

#include "core/config.h"
#include "core/time.h"
#include "game/file.h"
#include "game/game.h"
#include "game/settings.h"
#include "game/state.h"
#include "game/tick.h"
#include "multiplayer/bootstrap.h"
#include "multiplayer/dedicated_server.h"
#include "multiplayer/game_manifest.h"
#include "multiplayer/mp_autosave.h"
#include "multiplayer/player_registry.h"
#include "multiplayer/runtime.h"
#include "multiplayer/server_rules.h"
#include "multiplayer/worldgen.h"
#include "network/session.h"
#include "platform/file_manager.h"
#include "platform/user_path.h"

#include <ctype.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SERVER_STATUS_INTERVAL_MS 1000u
#define SERVER_DEFAULT_CONFIG     "claudius-server.ini"

typedef struct {
    char config_path[MP_DEDICATED_PATH_MAX];
    char data_dir[MP_DEDICATED_PATH_MAX];
} dedicated_bootstrap_paths;

static volatile sig_atomic_t server_shutdown_requested;
static volatile sig_atomic_t server_reload_requested;

static void handle_signal(int sig)
{
    if (sig == SIGINT || sig == SIGTERM) {
        server_shutdown_requested = 1;
    }
#ifndef _WIN32
    if (sig == SIGHUP) {
        server_reload_requested = 1;
    }
#endif
}

static char *trim_whitespace(char *text)
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

static int parse_bool_value(const char *value)
{
    if (!value) {
        return 0;
    }
    return strcmp(value, "1") == 0 ||
           strcmp(value, "true") == 0 ||
           strcmp(value, "yes") == 0 ||
           strcmp(value, "on") == 0;
}

static void apply_server_config_kv(mp_dedicated_server_options *options,
                                   const char *section,
                                   const char *key,
                                   const char *value)
{
    if (!options || !section || !key || !value) {
        return;
    }

    if (strcmp(section, "server") == 0) {
        if (strcmp(key, "name") == 0) {
            snprintf(options->server_name, sizeof(options->server_name), "%s", value);
        } else if (strcmp(key, "listen_address") == 0) {
            snprintf(options->listen_address, sizeof(options->listen_address), "%s", value);
        } else if (strcmp(key, "port") == 0) {
            options->port = (uint16_t)atoi(value);
        } else if (strcmp(key, "scenario") == 0) {
            snprintf(options->scenario_name, sizeof(options->scenario_name), "%s", value);
        } else if (strcmp(key, "resume_save") == 0) {
            snprintf(options->resume_save, sizeof(options->resume_save), "%s", value);
        } else if (strcmp(key, "max_players") == 0) {
            options->max_players = (uint8_t)atoi(value);
        } else if (strcmp(key, "dynamic_city_pool") == 0) {
            options->dynamic_city_pool = (uint8_t)atoi(value);
        } else if (strcmp(key, "autosave_interval_sec") == 0) {
            options->autosave_interval_sec = atoi(value);
        } else if (strcmp(key, "save_dir") == 0) {
            snprintf(options->save_dir, sizeof(options->save_dir), "%s", value);
        } else if (strcmp(key, "status_file") == 0) {
            snprintf(options->status_file, sizeof(options->status_file), "%s", value);
        } else if (strcmp(key, "advertise_lan") == 0) {
            options->advertise_lan = parse_bool_value(value);
        } else if (strcmp(key, "log_format") == 0) {
            options->log_format = strcmp(value, "json") == 0
                ? MP_DEDICATED_LOG_JSON : MP_DEDICATED_LOG_TEXT;
        }
    } else if (strcmp(section, "security") == 0) {
        if (strcmp(key, "reconnect_timeout_ticks") == 0) {
            options->reconnect_timeout_ticks = (uint32_t)strtoul(value, NULL, 10);
        } else if (strcmp(key, "rate_limit_per_sec") == 0) {
            options->command_rate_limit_per_sec = (uint32_t)strtoul(value, NULL, 10);
        } else if (strcmp(key, "queue_limit_per_peer") == 0) {
            options->queue_limit_per_peer = (uint32_t)strtoul(value, NULL, 10);
        } else if (strcmp(key, "handshake_attempts_per_window") == 0) {
            options->handshake_attempts_per_window = (uint32_t)strtoul(value, NULL, 10);
        } else if (strcmp(key, "handshake_window_ms") == 0) {
            options->handshake_window_ms = (uint32_t)strtoul(value, NULL, 10);
        } else if (strcmp(key, "handshake_cooldown_ms") == 0) {
            options->handshake_cooldown_ms = (uint32_t)strtoul(value, NULL, 10);
        } else if (strcmp(key, "pending_connections_per_ip") == 0) {
            options->pending_connections_per_ip = (uint32_t)strtoul(value, NULL, 10);
        } else if (strcmp(key, "banlist_path") == 0) {
            snprintf(options->banlist_path, sizeof(options->banlist_path), "%s", value);
        }
    } else if (strcmp(section, "rules") == 0) {
        /* Rules are applied after config_load() so the dedicated server becomes authoritative. */
        (void)key;
        (void)value;
    }
}

static void load_server_ini(const char *path, mp_dedicated_server_options *options)
{
    FILE *fp;
    char line[512];
    char section[64];

    if (!path || !path[0] || !options) {
        return;
    }

    fp = fopen(path, "rt");
    if (!fp) {
        return;
    }

    section[0] = '\0';
    while (fgets(line, sizeof(line), fp)) {
        char *cursor = trim_whitespace(line);
        char *equals;

        if (!cursor[0] || cursor[0] == '#' || cursor[0] == ';') {
            continue;
        }
        if (cursor[0] == '[') {
            char *end = strchr(cursor, ']');
            if (end) {
                *end = '\0';
                snprintf(section, sizeof(section), "%s", trim_whitespace(cursor + 1));
            }
            continue;
        }

        equals = strchr(cursor, '=');
        if (!equals) {
            continue;
        }
        *equals = '\0';
        apply_server_config_kv(options, section,
                               trim_whitespace(cursor),
                               trim_whitespace(equals + 1));
    }

    fclose(fp);
}

static void load_server_rules_ini(const char *path)
{
    FILE *fp;
    char line[512];
    char section[64];

    if (!path || !path[0]) {
        return;
    }

    fp = fopen(path, "rt");
    if (!fp) {
        return;
    }

    section[0] = '\0';
    while (fgets(line, sizeof(line), fp)) {
        char *cursor = trim_whitespace(line);
        char *equals;

        if (!cursor[0] || cursor[0] == '#' || cursor[0] == ';') {
            continue;
        }
        if (cursor[0] == '[') {
            char *end = strchr(cursor, ']');
            if (end) {
                *end = '\0';
                snprintf(section, sizeof(section), "%s", trim_whitespace(cursor + 1));
            }
            continue;
        }
        if (strcmp(section, "rules") != 0) {
            continue;
        }

        equals = strchr(cursor, '=');
        if (!equals) {
            continue;
        }
        *equals = '\0';
        mp_server_rules_apply_named_rule(trim_whitespace(cursor),
                                         trim_whitespace(equals + 1));
    }

    fclose(fp);
}

static void parse_cli_paths(int argc, char **argv, dedicated_bootstrap_paths *paths)
{
    snprintf(paths->config_path, sizeof(paths->config_path), "%s", SERVER_DEFAULT_CONFIG);
    snprintf(paths->data_dir, sizeof(paths->data_dir), ".");

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
            snprintf(paths->config_path, sizeof(paths->config_path), "%s", argv[++i]);
        } else if (strcmp(argv[i], "--data-dir") == 0 && i + 1 < argc) {
            snprintf(paths->data_dir, sizeof(paths->data_dir), "%s", argv[++i]);
        }
    }
}

static int parse_cli_options(int argc, char **argv, mp_dedicated_server_options *options)
{
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--config") == 0 || strcmp(argv[i], "--data-dir") == 0) {
            i++;
        } else if (strcmp(argv[i], "--listen-address") == 0 && i + 1 < argc) {
            snprintf(options->listen_address, sizeof(options->listen_address), "%s", argv[++i]);
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            options->port = (uint16_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--server-name") == 0 && i + 1 < argc) {
            snprintf(options->server_name, sizeof(options->server_name), "%s", argv[++i]);
        } else if (strcmp(argv[i], "--scenario") == 0 && i + 1 < argc) {
            snprintf(options->scenario_name, sizeof(options->scenario_name), "%s", argv[++i]);
        } else if (strcmp(argv[i], "--resume-save") == 0 && i + 1 < argc) {
            snprintf(options->resume_save, sizeof(options->resume_save), "%s", argv[++i]);
        } else if (strcmp(argv[i], "--max-players") == 0 && i + 1 < argc) {
            options->max_players = (uint8_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--dynamic-city-pool") == 0 && i + 1 < argc) {
            options->dynamic_city_pool = (uint8_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--autosave-interval-sec") == 0 && i + 1 < argc) {
            options->autosave_interval_sec = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--save-dir") == 0 && i + 1 < argc) {
            snprintf(options->save_dir, sizeof(options->save_dir), "%s", argv[++i]);
        } else if (strcmp(argv[i], "--status-file") == 0 && i + 1 < argc) {
            snprintf(options->status_file, sizeof(options->status_file), "%s", argv[++i]);
        } else if (strcmp(argv[i], "--advertise-lan") == 0 && i + 1 < argc) {
            options->advertise_lan = parse_bool_value(argv[++i]);
        } else if (strcmp(argv[i], "--log-format") == 0 && i + 1 < argc) {
            options->log_format = strcmp(argv[++i], "json") == 0
                ? MP_DEDICATED_LOG_JSON : MP_DEDICATED_LOG_TEXT;
        } else if (strcmp(argv[i], "--help") == 0) {
            return 0;
        }
    }

    return 1;
}

static void print_usage(void)
{
    printf("Usage: claudius-server [options]\n");
    printf("  --config <path>\n");
    printf("  --data-dir <path>\n");
    printf("  --listen-address <ip>\n");
    printf("  --port <n>\n");
    printf("  --server-name <name>\n");
    printf("  --scenario <scenario>\n");
    printf("  --resume-save <path>\n");
    printf("  --max-players <2..8>\n");
    printf("  --dynamic-city-pool <n>\n");
    printf("  --autosave-interval-sec <n>\n");
    printf("  --save-dir <path>\n");
    printf("  --status-file <path>\n");
    printf("  --advertise-lan <0|1>\n");
    printf("  --log-format <text|json>\n");
}

static int millis_per_tick_for_speed(int speed)
{
    static const int millis_per_tick_per_speed[] = {
        702, 502, 352, 242, 162, 112, 82, 57, 37, 22, 16
    };
    static const int millis_per_hyper_speed[] = {
        702, 16, 8, 5, 3, 2
    };

    if (speed < 10) {
        return 1000;
    }
    if (speed <= 100) {
        return millis_per_tick_per_speed[speed / 10];
    }
    if (speed > 500) {
        speed = 500;
    }
    return millis_per_hyper_speed[speed / 100];
}

static void write_status_file_if_needed(uint32_t now_ms, uint32_t *last_status_ms)
{
    const mp_dedicated_server_options *options = mp_dedicated_server_get_options();
    const mp_game_manifest *manifest;
    FILE *fp;
    int connected_players = 0;
    int active_players = 0;

    if (!options->status_file[0] || (now_ms - *last_status_ms) < SERVER_STATUS_INTERVAL_MS) {
        return;
    }

    *last_status_ms = now_ms;
    manifest = mp_game_manifest_get();
    for (int i = 0; i < MP_MAX_PLAYERS; i++) {
        mp_player *player = mp_player_registry_get((uint8_t)i);
        if (!player || !player->active) {
            continue;
        }
        active_players++;
        if (player->connection_state == MP_CONNECTION_CONNECTED) {
            connected_players++;
        }
    }

    fp = fopen(options->status_file, "wt");
    if (!fp) {
        return;
    }

    fprintf(fp,
            "{"
            "\"phase\":\"%s\","
            "\"tick\":%u,"
            "\"session_id\":%u,"
            "\"players_connected\":%d,"
            "\"players_active\":%d,"
            "\"max_players\":%u,"
            "\"dynamic_pool_remaining\":%d,"
            "\"dirty\":%d,"
            "\"last_save_tick\":%u,"
            "\"world_uuid_set\":%d,"
            "\"uptime_ms\":%u"
            "}\n",
            net_session_state_name(net_session_get_state()),
            net_session_get_authoritative_tick(),
            net_session_get()->session_id,
            connected_players,
            active_players,
            options->max_players,
            mp_worldgen_get_dynamic_city_pool_remaining(),
            mp_autosave_is_dirty(),
            mp_autosave_get_last_save_tick(),
            manifest && manifest->valid,
            now_ms);
    fclose(fp);
}

static int start_dedicated_session(void)
{
    const mp_dedicated_server_options *options = mp_dedicated_server_get_options();

    if (!net_session_host(options->server_name, options->port)) {
        return 0;
    }

    mp_autosave_set_interval(options->autosave_interval_sec);

    if (options->resume_save[0]) {
        mp_bootstrap_set_save(options->resume_save);
        if (!mp_bootstrap_host_resume_game()) {
            return 0;
        }
        if (!mp_bootstrap_host_launch_resumed_game()) {
            return 0;
        }
        return 1;
    }

    if (!options->scenario_name[0]) {
        fprintf(stderr, "Dedicated server requires --scenario or --resume-save.\n");
        return 0;
    }

    mp_bootstrap_set_scenario(options->scenario_name);
    return mp_bootstrap_host_start_game();
}

int main(int argc, char **argv)
{
    dedicated_bootstrap_paths paths;
    mp_dedicated_server_options options;
    uint32_t last_status_ms = 0;
    uint32_t last_loop_ms;
    uint32_t accumulated_ms = 0;

    parse_cli_paths(argc, argv, &paths);

    mp_dedicated_server_reset_options();
    options = *mp_dedicated_server_get_options();
    options.enabled = 1;

    load_server_ini(paths.config_path, &options);
    if (!parse_cli_options(argc, argv, &options)) {
        print_usage();
        return 1;
    }

    mp_dedicated_server_set_options(&options);

    if (SDL_Init(0) != 0) {
        fprintf(stderr, "SDL init failed: %s\n", SDL_GetError());
        return 1;
    }

    if (!platform_file_manager_set_base_path(paths.data_dir)) {
        fprintf(stderr, "Invalid data dir: %s\n", paths.data_dir);
        SDL_Quit();
        return 1;
    }

    if (!game_pre_init()) {
        fprintf(stderr, "game_pre_init failed\n");
        SDL_Quit();
        return 1;
    }

    load_server_rules_ini(paths.config_path);
    mp_server_rules_capture_from_config();
    setting_set_default_game_speed();
    platform_user_path_create_subdirectories();

    if (!game_init_headless_server()) {
        fprintf(stderr, "game_init_headless_server failed\n");
        SDL_Quit();
        return 1;
    }

    if (!net_session_init()) {
        fprintf(stderr, "net_session_init failed\n");
        game_exit_headless_server();
        SDL_Quit();
        return 1;
    }

    multiplayer_runtime_init();

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
#ifndef _WIN32
    signal(SIGHUP, handle_signal);
#endif

    if (!start_dedicated_session()) {
        fprintf(stderr, "Failed to start dedicated session\n");
        net_session_shutdown();
        game_exit_headless_server();
        SDL_Quit();
        return 1;
    }

    last_loop_ms = SDL_GetTicks();
    while (!server_shutdown_requested) {
        uint32_t now_ms = SDL_GetTicks();
        uint32_t delta_ms = now_ms - last_loop_ms;
        int millis_per_tick;

        last_loop_ms = now_ms;
        if (mp_dedicated_server_consume_shutdown_request()) {
            server_shutdown_requested = 1;
            break;
        }
        time_set_millis(now_ms);
        multiplayer_runtime_update();

        millis_per_tick = millis_per_tick_for_speed(setting_game_speed());
        accumulated_ms += delta_ms;
        while (!game_state_is_paused() && accumulated_ms >= (uint32_t)millis_per_tick) {
            game_tick_run();
            game_file_write_mission_saved_game();
            accumulated_ms -= (uint32_t)millis_per_tick;
        }

        write_status_file_if_needed(now_ms, &last_status_ms);

        if (server_reload_requested) {
            server_reload_requested = 0;
            load_server_ini(paths.config_path, &options);
            mp_dedicated_server_set_options(&options);
            mp_autosave_set_interval(options.autosave_interval_sec);
            net_session_refresh_discovery_announcement();
        }

        SDL_Delay(10);
    }

    mp_autosave_final_save();
    net_session_shutdown();
    game_exit_headless_server();
    SDL_Quit();
    return 0;
}
