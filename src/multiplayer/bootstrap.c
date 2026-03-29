#include "bootstrap.h"

#ifdef ENABLE_MULTIPLAYER

#include "game_manifest.h"
#include "server_rules.h"
#include "dedicated_server.h"
#include "frontend.h"
#include "mp_trade_route.h"
#include "mp_autosave.h"
#include "trade_execution.h"
#include "scenario_selection.h"
#include "mp_debug_log.h"
#include "mp_safe_file.h"
#include "save_transfer.h"
#include "player_registry.h"
#include "ownership.h"
#include "empire_sync.h"
#include "time_sync.h"
#include "command_bus.h"
#include "checksum.h"
#include "resync.h"
#include "snapshot.h"
#include "session_save.h"
#include "trade_sync.h"
#include "worldgen.h"
#include "join_transaction.h"
#include "network/session.h"
#include "network/serialize.h"
#include "network/protocol.h"
#include "network/transport_tcp.h"
#include "network/discovery_lan.h"
#include "scenario/empire.h"
#include "city/finance.h"
#include "city/emperor.h"
#include "empire/city.h"
#include "game/file.h"
#include "game/state.h"
#include "platform/file_manager.h"
#include "core/log.h"
#include "core/random.h"
#include "core/string.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static struct {
    mp_boot_state state;
    char scenario_name[MP_MANIFEST_SCENARIO_NAME_MAX];
    char save_filename[MP_MANIFEST_SCENARIO_NAME_MAX];
    int is_resume;
    struct {
        int active;
        int transfer_in_progress;
        int awaiting_load_complete;
        uint8_t peer_index;
        uint8_t player_id;
    } late_join;
    struct {
        int active;
        int transfer_in_progress;
        int awaiting_load_complete;
        uint8_t peer_index;
        uint8_t player_id;
    } reconnect;
} boot_data;

void mp_bootstrap_init(void)
{
    memset(&boot_data, 0, sizeof(boot_data));
    mp_game_manifest_init();
    mp_join_transaction_init();
}

void mp_bootstrap_reset(void)
{
    memset(&boot_data, 0, sizeof(boot_data));
    mp_game_manifest_clear();
    mp_join_transaction_init();
    mp_worldgen_clear();
    mp_save_transfer_reset();
    scenario_empire_set_multiplayer_mode(0);
    mp_autosave_reset();
}

mp_boot_state mp_bootstrap_get_state(void)
{
    return boot_data.state;
}

void mp_bootstrap_set_scenario(const char *scenario_name)
{
    if (!scenario_name || !scenario_name[0]) {
        return;
    }
    snprintf(boot_data.scenario_name, sizeof(boot_data.scenario_name), "%s",
             scenario_name);
    boot_data.state = MP_BOOT_SCENARIO_SELECTED;

    MP_LOG_INFO("BOOT", "Scenario selected: '%s'", boot_data.scenario_name);
}

const char *mp_bootstrap_get_scenario_name(void)
{
    return boot_data.scenario_name;
}

static uint32_t generate_session_seed(void)
{
    uint32_t value = 0;
    if (random_fill_secure_bytes((uint8_t *)&value, sizeof(value))) {
        return value ? value : 1u;
    }
    srand((unsigned int)time(NULL));
    value = (uint32_t)rand() ^ ((uint32_t)rand() << 16);
    return value ? value : 1u;
}

static int bootstrap_has_local_player(void)
{
    return net_session_has_local_player();
}

static int bootstrap_active_player_count(void)
{
    int count = net_session_get_peer_count();
    if (bootstrap_has_local_player()) {
        count++;
    }
    return count;
}

static int bootstrap_player_id_start(void)
{
    return bootstrap_has_local_player() ? 1 : 0;
}

static uint8_t bootstrap_manifest_max_players(void)
{
    if (mp_dedicated_server_is_enabled()) {
        const mp_dedicated_server_options *options = mp_dedicated_server_get_options();
        if (options && options->max_players > 0) {
            return options->max_players;
        }
    }
    return NET_MAX_PLAYERS;
}

static int bootstrap_reserved_pool_target(int active_player_count)
{
    int max_players = (int)bootstrap_manifest_max_players();
    int available = max_players - active_player_count;

    if (available <= 0) {
        return 0;
    }

    if (mp_dedicated_server_is_enabled()) {
        const mp_dedicated_server_options *options = mp_dedicated_server_get_options();
        int requested = options ? (int)options->dynamic_city_pool : available;
        if (requested < 0) {
            requested = 0;
        }
        return requested < available ? requested : available;
    }

    return available > 4 ? 4 : available;
}

static void finalize_late_join_transfer(void);
static void finalize_reconnect_transfer(void);

static int load_scenario_locally(const char *scenario_name)
{
    /* Convert ASCII scenario name to uint8_t for Claudius API */
    uint8_t name_buf[MP_MANIFEST_SCENARIO_NAME_MAX];
    string_copy(string_from_ascii(scenario_name), name_buf,
                MP_MANIFEST_SCENARIO_NAME_MAX);

    MP_LOG_INFO("BOOT", "Loading scenario locally: '%s'", scenario_name);

    if (!game_file_start_scenario_by_name(name_buf)) {
        MP_LOG_ERROR("BOOT", "Failed to load scenario: '%s'", scenario_name);
        return 0;
    }

    MP_LOG_INFO("BOOT", "Scenario loaded successfully: '%s'", scenario_name);
    return 1;
}

void mp_bootstrap_bind_loaded_scenario(void)
{
    MP_LOG_INFO("BOOT", "Binding multiplayer subsystems to loaded scenario");

    /* 1. Enable multiplayer mode flag */
    scenario_empire_set_multiplayer_mode(1);

    /* 2. Initialize subsystems (but NOT player_registry — preserve lobby roster) */
    mp_ownership_init();
    mp_trade_route_init();
    mp_trade_execution_init();
    mp_empire_sync_init();
    mp_time_sync_init();
    mp_command_bus_init();
    mp_checksum_init();
    mp_resync_init();

    /* 3. Assign empire cities to players using the spawn table.
     *    The spawn table was generated by the host and replicated to clients.
     *    Each spawn entry maps a slot_id to an empire_city_id. */
    const mp_spawn_table *table = mp_worldgen_get_spawn_table();
    uint8_t local_id = net_session_get_local_player_id();

    for (int i = 0; i < table->spawn_count; i++) {
        const mp_spawn_entry *spawn = &table->spawns[i];
        if (!spawn->valid) {
            continue;
        }

        /* Find the player who owns this slot */
        mp_player *player = mp_player_registry_get_by_slot(spawn->slot_id);
        if (!player || !player->active) {
            MP_LOG_WARN("BOOT", "No player for slot %d (city %d) — skipping",
                        (int)spawn->slot_id, spawn->empire_city_id);
            continue;
        }

        int city_id = spawn->empire_city_id;

        if (player->player_id == local_id) {
            /* This is our city */
            empire_city_set_owner(city_id, CITY_OWNER_LOCAL, player->player_id);
            mp_ownership_set_city(city_id, MP_OWNER_LOCAL_PLAYER, player->player_id);
        } else {
            /* Remote player's city */
            empire_city_set_owner(city_id, CITY_OWNER_REMOTE, player->player_id);
            mp_ownership_set_city(city_id, MP_OWNER_REMOTE_PLAYER, player->player_id);
        }

        mp_player_registry_set_city(player->player_id, city_id);
        mp_player_registry_set_assigned_city(player->player_id, city_id);

        /* Register in empire sync for trade view replication */
        if (net_session_is_host()) {
            mp_empire_sync_register_player_city(city_id, player->player_id,
                                                 spawn->empire_object_id);
        }

        MP_LOG_INFO("BOOT", "City %d assigned to player %d (slot %d, %s)",
                    city_id, (int)player->player_id, (int)spawn->slot_id,
                    player->player_id == local_id ? "local" : "remote");
    }

    /* 4. Initialize autosave system on host */
    if (net_session_is_host()) {
        mp_autosave_init();
    }

    MP_LOG_INFO("BOOT", "Multiplayer subsystems bound to scenario");
}

static void broadcast_spawn_table(void)
{
    uint8_t spawn_buf[2048];
    uint32_t spawn_size = 0;
    mp_worldgen_serialize(spawn_buf, &spawn_size);

    /* Send as HOST_EVENT with NET_EVENT_SPAWN_TABLE_UPDATED */
    uint8_t event_buf[2048 + 16];
    net_serializer es;
    net_serializer_init(&es, event_buf, sizeof(event_buf));
    net_write_u16(&es, NET_EVENT_SPAWN_TABLE_UPDATED);
    net_write_u32(&es, 0); /* event tick — game hasn't started yet */
    net_write_u32(&es, spawn_size);
    net_write_raw(&es, spawn_buf, spawn_size);

    net_session_broadcast_in_game(NET_MSG_HOST_EVENT, event_buf,
                                  (uint32_t)net_serializer_position(&es));

    MP_LOG_INFO("BOOT", "Spawn table broadcast: %u bytes, %d spawns",
                spawn_size, (int)mp_worldgen_get_spawn_table()->spawn_count);
}

static void broadcast_join_barrier_event(uint16_t event_type, uint8_t player_id)
{
    if (!net_session_is_host() || !net_session_is_in_game()) {
        return;
    }

    uint8_t event_buf[16];
    net_serializer es;
    net_serializer_init(&es, event_buf, sizeof(event_buf));
    net_write_u16(&es, event_type);
    net_write_u32(&es, mp_time_sync_get_authoritative_tick());
    net_write_u8(&es, player_id);
    net_session_broadcast_in_game(NET_MSG_HOST_EVENT, event_buf,
                                  (uint32_t)net_serializer_position(&es));
}

int mp_bootstrap_host_start_game(void)
{
    uint8_t manifest_max_players;
    int active_player_count;
    int reserved_pool_target;
    int required_capacity;
    int player_count;

    if (!net_session_is_host()) {
        MP_LOG_ERROR("BOOT", "Only host can start the game");
        return 0;
    }

    if (boot_data.scenario_name[0] == '\0') {
        MP_LOG_ERROR("BOOT", "No scenario selected");
        return 0;
    }

    boot_data.state = MP_BOOT_PREPARING;

    /* 1. Build the manifest with real scenario hash */
    uint32_t seed = generate_session_seed();
    active_player_count = bootstrap_active_player_count();
    manifest_max_players = bootstrap_manifest_max_players();
    reserved_pool_target = bootstrap_reserved_pool_target(active_player_count);
    required_capacity = active_player_count + reserved_pool_target;
    player_count = required_capacity;

    uint32_t scenario_hash = 0;
    if (!mp_scenario_compute_file_hash(boot_data.scenario_name, &scenario_hash)) {
        MP_LOG_WARN("BOOT", "Could not compute scenario hash — continuing without");
    }

    mp_game_manifest_set(MP_GAME_MODE_SCENARIO, boot_data.scenario_name,
                         scenario_hash, scenario_hash, 0, seed, manifest_max_players);
    mp_game_manifest_set_player_count((uint8_t)active_player_count);

    /* Generate unique world instance UUID for this session */
    {
        mp_game_manifest *m = mp_game_manifest_get_mutable();
        mp_player_registry_generate_uuid(m->world_instance_uuid);
    }

    /* Store seed in spawn table for worldgen */
    mp_worldgen_get_spawn_table_mutable()->session_seed = seed;

    /* 2. Load and validate locally before telling clients to prepare anything */
    if (!load_scenario_locally(boot_data.scenario_name)) {
        MP_LOG_ERROR("BOOT", "Host failed to load scenario — aborting game start");
        boot_data.state = MP_BOOT_SCENARIO_SELECTED;
        return 0;
    }

    mp_game_manifest_set_starting_finance(city_finance_treasury(),
                                          city_emperor_personal_savings(),
                                          city_finance_tax_percentage());

    /* 3b. Validate scenario has enough eligible cities for player count */
    if (required_capacity > 0 && !mp_scenario_validate_capacity(required_capacity)) {
        MP_LOG_ERROR("BOOT", "Scenario '%s' does not support %d players — aborting",
                     boot_data.scenario_name, player_count);
        boot_data.state = MP_BOOT_SCENARIO_SELECTED;
        return 0;
    }

    /* 4. Generate worldgen spawns */
    mp_worldgen_init();
    if (!mp_worldgen_generate_player_spawns(seed, active_player_count, 1)) {
        MP_LOG_ERROR("BOOT", "Worldgen failed: no valid spawn configuration");
        boot_data.state = MP_BOOT_SCENARIO_SELECTED;
        return 0;
    }
    mp_worldgen_lock();

    /* 4b. Generate reserved spawns for late joiners / dedicated dynamic pool. */
    if (reserved_pool_target > 0) {
        int generated = mp_worldgen_generate_dynamic_city_pool(reserved_pool_target);
        if (generated < reserved_pool_target) {
            MP_LOG_WARN("BOOT", "Generated %d/%d pooled cities for future joins",
                        generated, reserved_pool_target);
        }
    }

    /* 5. Bind multiplayer subsystems to the loaded scenario */
    mp_bootstrap_bind_loaded_scenario();

    /* 6. Apply spawns to empire (create city ownership records) */
    mp_worldgen_apply_spawns();

    /* 7. Only after local preparation succeeds, ask clients to prepare too */
    {
        uint8_t manifest_buf[MP_GAME_MANIFEST_WIRE_MAX];
        uint32_t manifest_size = 0;
        mp_game_manifest_serialize(manifest_buf, &manifest_size);

        for (int i = 0; i < NET_MAX_PEERS; i++) {
            const net_peer *peer = net_session_get_peer(i);
            if (peer && peer->active) {
                net_session_send_to_peer(i, NET_MSG_GAME_PREPARE,
                                         manifest_buf, manifest_size);
            }
        }

        MP_LOG_INFO("BOOT", "GAME_PREPARE sent to %d peers", net_session_get_peer_count());
    }

    /* 8. Transition session to in-game state (sets peers to IN_GAME) */
    net_session_transition_to_game();

    /* 9. Broadcast spawn table to clients */
    broadcast_spawn_table();

    /* 10. Send initial full snapshot so clients have authoritative MP state */
    {
        uint8_t *snap_buf = (uint8_t *)malloc(MP_SNAPSHOT_MAX_SIZE);
        if (snap_buf) {
            uint32_t snap_size = 0;
            if (mp_snapshot_build_full(snap_buf, MP_SNAPSHOT_MAX_SIZE, &snap_size)) {
                net_session_broadcast_in_game(NET_MSG_FULL_SNAPSHOT, snap_buf, snap_size);
                MP_LOG_INFO("BOOT", "Initial snapshot broadcast: %u bytes", snap_size);
            }
            free(snap_buf);
        }
    }

    /* 11. Send GAME_START_FINAL after the authoritative snapshot */
    {
        uint8_t start_buf[8];
        net_serializer ss;
        net_serializer_init(&ss, start_buf, sizeof(start_buf));
        net_write_u32(&ss, 0); /* start tick */
        net_write_u8(&ss, 2);  /* normal speed */
        net_session_broadcast_in_game(NET_MSG_GAME_START_FINAL, start_buf,
                                      (uint32_t)net_serializer_position(&ss));
    }

    /* 12. Transition to WINDOW_CITY */
    boot_data.state = MP_BOOT_IN_GAME;
    mp_frontend_enter_game();

    MP_LOG_INFO("BOOT", "=== Game started successfully ===");
    MP_LOG_INFO("BOOT", "Scenario: '%s', Active players: %d, Pool: %d, Seed: %u",
                boot_data.scenario_name, active_player_count,
                mp_worldgen_get_dynamic_city_pool_remaining(), seed);

    return 1;
}

int mp_bootstrap_client_prepare(const uint8_t *payload, uint32_t size)
{
    MP_LOG_INFO("BOOT", "GAME_PREPARE received from host (%u bytes)", size);

    /* 1. Deserialize manifest */
    if (!mp_game_manifest_deserialize(payload, size)) {
        MP_LOG_ERROR("BOOT", "Failed to deserialize game manifest");
        return 0;
    }

    const mp_game_manifest *manifest = mp_game_manifest_get();

    /* Check if this is a resume from save */
    if (manifest->mode == MP_GAME_MODE_SAVED_GAME) {
        MP_LOG_INFO("BOOT", "Client resume mode: waiting for save transfer from host");
        snprintf(boot_data.save_filename, sizeof(boot_data.save_filename), "%s",
                 manifest->scenario_name);
        boot_data.is_resume = 1;
        boot_data.state = MP_BOOT_SAVE_TRANSFER;

        /* Initialize save transfer to receive the full save file.
         * The host will send the complete save (base game state + MP state)
         * in chunks. The client will write it to a temp file and load it
         * using the standard game_file_load_saved_game() path. */
        mp_save_transfer_init();
        scenario_empire_set_multiplayer_mode(1);

        /* Initialize minimal stateless subsystems for post-load */
        mp_checksum_init();
        mp_resync_init();

        MP_LOG_INFO("BOOT", "Client prepared for resume: waiting for save transfer");
        return 1;
    }

    /* 2. Store session seed for worldgen */
    mp_worldgen_get_spawn_table_mutable()->session_seed = manifest->session_seed;

    /* 3. Copy scenario name to boot data */
    snprintf(boot_data.scenario_name, sizeof(boot_data.scenario_name), "%s",
             manifest->scenario_name);
    boot_data.state = MP_BOOT_PREPARING;

    /* 4. Load the scenario locally */
    if (!load_scenario_locally(manifest->scenario_name)) {
        MP_LOG_ERROR("BOOT", "Client failed to load scenario '%s'",
                     manifest->scenario_name);
        return 0;
    }

    city_finance_apply_starting_state(manifest->initial_treasury,
                                      manifest->starting_tax_percentage);
    city_emperor_set_personal_savings(manifest->starting_personal_savings);

    /* 4b. Validate scenario hash matches host's manifest */
    if (manifest->scenario_hash != 0) {
        uint32_t local_hash = 0;
        if (mp_scenario_compute_file_hash(manifest->scenario_name, &local_hash)) {
            if (!mp_scenario_hashes_match(local_hash, manifest->scenario_hash)) {
                MP_LOG_ERROR("BOOT", "Scenario hash mismatch — client has different "
                             "version of '%s'", manifest->scenario_name);
                return 0;
            }
        } else {
            MP_LOG_WARN("BOOT", "Could not compute local hash for validation");
        }
    }

    /* 5. Initialize worldgen (client doesn't generate, waits for spawn table) */
    mp_worldgen_init();

    boot_data.state = MP_BOOT_LOADED;

    MP_LOG_INFO("BOOT", "Client prepared: scenario '%s' loaded, waiting for spawn table",
                manifest->scenario_name);
    return 1;
}

int mp_bootstrap_client_enter_game(void)
{
    MP_LOG_INFO("BOOT", "GAME_START_FINAL received — entering game");

    if (boot_data.is_resume) {
        /* Resume mode: client did NOT load a save locally.
         * All state comes from the host snapshot that was sent before
         * GAME_START_FINAL. The snapshot apply handler already set up
         * ownership, routes, traders, time sync, etc.
         *
         * We just need to re-apply city ownership flags from the snapshot data
         * so the empire map reflects the correct owners. */
        mp_ownership_reapply_city_owners();
        MP_LOG_INFO("BOOT", "Client entering resumed game (state from host snapshot)");
    } else {
        /* The spawn table should have arrived via HOST_EVENT before this message.
         * Bind subsystems now that we have both scenario and spawn data. */
        const mp_spawn_table *table = mp_worldgen_get_spawn_table();
        if (table->spawn_count == 0) {
            MP_LOG_WARN("BOOT", "Entering game with empty spawn table — "
                        "city assignment may be incomplete");
        }

        /* Lock the spawn table on client side too */
        mp_worldgen_lock();

        /* Bind multiplayer subsystems */
        mp_bootstrap_bind_loaded_scenario();

        /* Apply spawns (client-side ownership records) */
        mp_worldgen_apply_spawns();
    }

    /* Stop LAN discovery listening */
    net_discovery_stop_listening();

    /* Transition to gameplay */
    boot_data.state = MP_BOOT_IN_GAME;
    mp_frontend_enter_game();

    MP_LOG_INFO("BOOT", "=== Client entered game ===");
    return 1;
}

int mp_bootstrap_is_resume(void)
{
    return boot_data.is_resume;
}

int mp_bootstrap_is_late_join_busy(void)
{
    return boot_data.late_join.active;
}

int mp_bootstrap_is_reconnect_busy(void)
{
    return boot_data.reconnect.active;
}

void mp_bootstrap_set_save(const char *save_filename)
{
    if (!save_filename || !save_filename[0]) {
        return;
    }
    snprintf(boot_data.save_filename, sizeof(boot_data.save_filename), "%s",
             save_filename);
    boot_data.is_resume = 1;
    boot_data.state = MP_BOOT_SAVE_SELECTED;

    MP_LOG_INFO("BOOT", "Save file selected for resume: '%s'", boot_data.save_filename);
}

void mp_bootstrap_rebind_loaded_save(void)
{
    MP_LOG_INFO("BOOT", "Rebinding multiplayer subsystems from saved game");

    /* 1. Enable multiplayer mode flag */
    scenario_empire_set_multiplayer_mode(1);

    /* 2. Only init stateless/queue subsystems — ownership, routes, etc. come from save.
     *
     * CRITICAL FIX: Do NOT call mp_command_bus_init() here, because it resets
     * next_sequence_id to 1, destroying the value restored from the save.
     * The command bus was already initialized with the correct sequence ID
     * by mp_session_load_from_buffer() -> mp_command_bus_init_from_save().
     *
     * Similarly, do NOT call mp_time_sync_init() — the ticks were restored
     * from the save and must be preserved. Instead, call init_from_save()
     * to unify the tick values. */
    mp_checksum_init();
    mp_resync_init();

    /* 3. Unify restored tick values */
    mp_time_sync_init_from_save();

    /* 4. Re-mark the local player before rebuilding any local/remote ownership view. */
    mp_player_registry_mark_local_player(net_session_get_local_player_id());

    /* 5. Re-apply empire city ownership flags from deserialized data */
    mp_ownership_reapply_city_owners();

    /* 6. Re-register cities for trade view replication */
    if (net_session_is_host()) {
        mp_empire_sync_reregister_all_player_cities();
    }

    /* 7. Initialize autosave system on host */
    if (net_session_is_host()) {
        mp_autosave_init();
        mp_autosave_mark_dirty(MP_DIRTY_RECONNECT); /* Mark dirty so first autosave captures state */
    }

    MP_LOG_INFO("BOOT", "Multiplayer subsystems rebound from save");
}

int mp_bootstrap_host_resume_game(void)
{
    if (!net_session_is_host()) {
        MP_LOG_ERROR("BOOT", "Only host can resume a game");
        return 0;
    }

    if (boot_data.save_filename[0] == '\0') {
        MP_LOG_ERROR("BOOT", "No save file selected");
        return 0;
    }

    boot_data.state = MP_BOOT_PREPARING;

    /* 1. Load the saved game — this restores base game state.
     *    mp_session_load_from_buffer() is called internally by the save system
     *    and restores all 7 MP domains + command sequence ID.
     *    Non-host players are marked AWAITING_RECONNECT. */
    uint8_t name_buf[MP_MANIFEST_SCENARIO_NAME_MAX];
    string_copy(string_from_ascii(boot_data.save_filename), name_buf,
                MP_MANIFEST_SCENARIO_NAME_MAX);

    MP_LOG_INFO("BOOT", "Loading saved game: '%s'", boot_data.save_filename);

    int load_result = game_file_load_saved_game((const char *)name_buf);
    if (load_result != 1) {
        MP_LOG_ERROR("BOOT", "Failed to load saved game: '%s' (result=%d)",
                     boot_data.save_filename, load_result);
        boot_data.state = MP_BOOT_SAVE_SELECTED;
        return 0;
    }

    mp_server_rules_apply_to_config();
    mp_server_rules_capture_from_config();

    /* 2. Rebind stateless subsystems (preserves command bus seq and ticks) */
    mp_bootstrap_rebind_loaded_save();

    /* 3. Mark host player as local and connected when the host is also a player. */
    if (bootstrap_has_local_player()) {
        uint8_t host_id = net_session_get_local_player_id();
        mp_player *host_player = mp_player_registry_get(host_id);
        if (host_player) {
            host_player->is_local = 1;
            host_player->status = MP_PLAYER_IN_GAME;
            host_player->connection_state = MP_CONNECTION_CONNECTED;
        }
    }

    /* 4. Transition to resume lobby */
    boot_data.state = MP_BOOT_RESUME_LOBBY;

    MP_LOG_INFO("BOOT", "Save loaded — entering resume lobby (tick=%u, seq=%u)",
                mp_time_sync_get_authoritative_tick(),
                mp_command_bus_get_next_sequence_id());

    /* 5. Open the resume lobby window */
    mp_frontend_show_resume_lobby();

    return 1;
}

static void mp_bootstrap_host_finalize_resume(void)
{
    /* Send FULL_SNAPSHOT as a "freshness layer" — overwrites MP domain state
     * with the latest authoritative values (which may differ from the save
     * if time elapsed between save checkpoint and launch). */
    {
        uint8_t *snap_buf = (uint8_t *)malloc(MP_SNAPSHOT_MAX_SIZE);
        if (snap_buf) {
            uint32_t snap_size = 0;
            if (mp_snapshot_build_full(snap_buf, MP_SNAPSHOT_MAX_SIZE, &snap_size)) {
                net_session_broadcast(NET_MSG_FULL_SNAPSHOT, snap_buf, snap_size);
                MP_LOG_INFO("BOOT", "Resume snapshot broadcast: %u bytes", snap_size);
            }
            free(snap_buf);
        }
    }

    /* Send GAME_START_FINAL with restored authoritative tick */
    {
        uint8_t start_buf[8];
        net_serializer ss;
        net_serializer_init(&ss, start_buf, sizeof(start_buf));
        net_write_u32(&ss, mp_time_sync_get_authoritative_tick());
        net_write_u8(&ss, mp_time_sync_get_speed());
        net_session_broadcast(NET_MSG_GAME_START_FINAL, start_buf,
                              (uint32_t)net_serializer_position(&ss));
    }

    /* Transition to gameplay */
    boot_data.state = MP_BOOT_IN_GAME;
    mp_frontend_enter_game();

    MP_LOG_INFO("BOOT", "=== Resumed game launched successfully (tick=%u) ===",
                mp_time_sync_get_authoritative_tick());
}

int mp_bootstrap_host_launch_resumed_game(void)
{
    if (!net_session_is_host()) {
        MP_LOG_ERROR("BOOT", "Only host can launch resumed game");
        return 0;
    }

    /* 1. For each player still AWAITING_RECONNECT: set routes offline, mark city offline */
    for (int i = 0; i < MP_MAX_PLAYERS; i++) {
        mp_player *p = mp_player_registry_get((uint8_t)i);
        if (p && p->active && p->status == MP_PLAYER_AWAITING_RECONNECT) {
            mp_ownership_set_player_routes_offline(p->player_id);
            if (p->assigned_city_id >= 0) {
                mp_ownership_set_city_online(p->assigned_city_id, 0);
            }
            MP_LOG_INFO("BOOT", "Player %d still disconnected — routes set offline", (int)p->player_id);
        }
    }

    /* 2. Transition session to in-game state */
    net_session_transition_to_game();

    /* 3. Build manifest with saved game mode */
    mp_game_manifest_set(MP_GAME_MODE_SAVED_GAME, boot_data.save_filename,
                         0, 0, MP_SAVE_VERSION, 0, bootstrap_manifest_max_players());
    mp_game_manifest_set_player_count((uint8_t)mp_player_registry_get_count());

    /* 4. Send GAME_PREPARE (manifest with save info) to all peers */
    {
        uint8_t manifest_buf[MP_GAME_MANIFEST_WIRE_MAX];
        uint32_t manifest_size = 0;
        mp_game_manifest_serialize(manifest_buf, &manifest_size);

        for (int i = 0; i < NET_MAX_PEERS; i++) {
            const net_peer *peer = net_session_get_peer(i);
            if (peer && peer->active) {
                net_session_send_to_peer(i, NET_MSG_GAME_PREPARE,
                                         manifest_buf, manifest_size);
            }
        }
    }

    /* 5. Create save checkpoint and start chunked transfer to clients.
     *    This replaces the old snapshot-only approach: clients now receive
     *    the full save file so they have base game state (buildings, figures,
     *    terrain, map grids) — not just MP domain state. */
    {
        const char *checkpoint_path = mp_safe_file_get_save_path("mp_resume_checkpoint.sav");
        if (!game_file_write_saved_game(checkpoint_path)) {
            MP_LOG_ERROR("BOOT", "Failed to write save checkpoint — falling back to snapshot-only");
            /* Fall through to finalize without save transfer */
            mp_bootstrap_host_finalize_resume();
            return 1;
        }

        mp_save_transfer_init();
        if (!mp_save_transfer_host_begin(checkpoint_path)) {
            MP_LOG_ERROR("BOOT", "Failed to start save transfer — falling back to snapshot-only");
            platform_file_manager_remove_file(checkpoint_path);
            mp_bootstrap_host_finalize_resume();
            return 1;
        }

        /* Clean up checkpoint file — data is now in memory */
        platform_file_manager_remove_file(checkpoint_path);
    }

    /* 6. Set state to SAVE_TRANSFER — transfer continues in mp_bootstrap_update() */
    boot_data.state = MP_BOOT_SAVE_TRANSFER;

    MP_LOG_INFO("BOOT", "Save transfer started — driving from mp_bootstrap_update()");
    return 1;
}

void mp_bootstrap_update(void)
{
    if (boot_data.state == MP_BOOT_SAVE_TRANSFER && net_session_is_host()) {
        if (mp_save_transfer_host_update()) {
            return; /* Still sending chunks */
        }
        /* Transfer complete — finalize the resume */
        mp_bootstrap_host_finalize_resume();
        return;
    }

    if (net_session_is_host() && boot_data.late_join.transfer_in_progress) {
        if (mp_save_transfer_host_update()) {
            return;
        }
        finalize_late_join_transfer();
        return;
    }

    if (net_session_is_host() && boot_data.reconnect.transfer_in_progress) {
        if (mp_save_transfer_host_update()) {
            return;
        }
        finalize_reconnect_transfer();
    }
}

void mp_bootstrap_client_save_transfer_complete(void)
{
    MP_LOG_INFO("BOOT", "Client save transfer complete — loading save from buffer");

    uint32_t save_size = 0;
    const uint8_t *save_data = mp_save_transfer_client_get_data(&save_size);

    if (!save_data || save_size == 0) {
        MP_LOG_ERROR("BOOT", "No save data after transfer");
        return;
    }

    /* Write to temp file */
    const char *temp_path = mp_safe_file_get_save_path("mp_resume_received.sav");
    if (!mp_safe_file_write(temp_path, NULL, save_data, save_size)) {
        MP_LOG_ERROR("BOOT", "Failed to write temp save file: %s", temp_path);
        return;
    }

    /* Load using standard path — identical code path as host */
    int load_result = game_file_load_saved_game(temp_path);
    if (load_result != 1) {
        MP_LOG_ERROR("BOOT", "Failed to load transferred save (result=%d)", load_result);
        platform_file_manager_remove_file(temp_path);
        return;
    }

    /* Rebind MP subsystems from loaded save */
    mp_bootstrap_rebind_loaded_save();
    boot_data.state = MP_BOOT_LOADED;

    /* Cleanup temp file */
    platform_file_manager_remove_file(temp_path);

    /* Free the transfer buffer */
    mp_save_transfer_reset();

    MP_LOG_INFO("BOOT", "Client save loaded successfully — waiting for snapshot + GAME_START_FINAL");
}

static void finalize_late_join_transfer(void)
{
    uint8_t peer_index = boot_data.late_join.peer_index;

    {
        uint8_t *snap_buf = (uint8_t *)malloc(MP_SNAPSHOT_MAX_SIZE);
        if (snap_buf) {
            uint32_t snap_size = 0;
            if (mp_snapshot_build_full(snap_buf, MP_SNAPSHOT_MAX_SIZE, &snap_size)) {
                net_session_send_to_peer(peer_index, NET_MSG_FULL_SNAPSHOT,
                                         snap_buf, snap_size);
            }
            free(snap_buf);
        }
    }

    {
        uint8_t start_buf[8];
        net_serializer ss;
        net_serializer_init(&ss, start_buf, sizeof(start_buf));
        net_write_u32(&ss, mp_time_sync_get_authoritative_tick());
        net_write_u8(&ss, mp_time_sync_get_speed());
        net_session_send_to_peer(peer_index, NET_MSG_GAME_START_FINAL,
                                 start_buf, (uint32_t)net_serializer_position(&ss));
    }

    boot_data.late_join.transfer_in_progress = 0;
    boot_data.late_join.awaiting_load_complete = 1;
}

static void finalize_reconnect_transfer(void)
{
    uint8_t peer_index = boot_data.reconnect.peer_index;

    {
        uint8_t *snap_buf = (uint8_t *)malloc(MP_SNAPSHOT_MAX_SIZE);
        if (snap_buf) {
            uint32_t snap_size = 0;
            if (mp_snapshot_build_full(snap_buf, MP_SNAPSHOT_MAX_SIZE, &snap_size)) {
                net_session_send_to_peer(peer_index, NET_MSG_FULL_SNAPSHOT,
                                         snap_buf, snap_size);
            }
            free(snap_buf);
        }
    }

    {
        uint8_t start_buf[8];
        net_serializer ss;
        net_serializer_init(&ss, start_buf, sizeof(start_buf));
        net_write_u32(&ss, mp_time_sync_get_authoritative_tick());
        net_write_u8(&ss, mp_time_sync_get_speed());
        net_session_send_to_peer(peer_index, NET_MSG_GAME_START_FINAL,
                                 start_buf, (uint32_t)net_serializer_position(&ss));
    }

    boot_data.reconnect.transfer_in_progress = 0;
    boot_data.reconnect.awaiting_load_complete = 1;
}

int mp_bootstrap_host_begin_reconnect(uint8_t peer_index, uint8_t player_id)
{
    mp_game_manifest reconnect_manifest;
    uint8_t manifest_buf[MP_GAME_MANIFEST_WIRE_MAX];
    uint32_t manifest_size = 0;
    const char *checkpoint_path;
    net_session *sess;

    if (!net_session_is_host() || !net_session_is_in_game()) {
        return 0;
    }
    if (boot_data.late_join.active || boot_data.reconnect.active) {
        MP_LOG_WARN("BOOT", "Peer-scoped save transfer already active");
        return 0;
    }

    sess = net_session_get();
    if (peer_index >= NET_MAX_PEERS || !sess->peers[peer_index].active) {
        return 0;
    }
    sess->peers[peer_index].state = PEER_STATE_LOADING;

    reconnect_manifest = *mp_game_manifest_get();
    reconnect_manifest.mode = MP_GAME_MODE_SAVED_GAME;
    reconnect_manifest.player_count = (uint8_t)mp_player_registry_get_count();
    reconnect_manifest.valid = 1;
    mp_game_manifest_serialize_explicit(&reconnect_manifest, manifest_buf, &manifest_size);
    net_session_send_to_peer(peer_index, NET_MSG_GAME_PREPARE, manifest_buf, manifest_size);

    checkpoint_path = mp_safe_file_get_save_path("mp_reconnect_checkpoint.sav");
    if (!game_file_write_saved_game(checkpoint_path)) {
        MP_LOG_ERROR("BOOT", "Failed to write reconnect checkpoint");
        return 0;
    }

    mp_save_transfer_init();
    if (!mp_save_transfer_host_begin_for_peer(checkpoint_path, peer_index)) {
        MP_LOG_ERROR("BOOT", "Failed to start reconnect transfer");
        platform_file_manager_remove_file(checkpoint_path);
        return 0;
    }
    platform_file_manager_remove_file(checkpoint_path);

    boot_data.reconnect.active = 1;
    boot_data.reconnect.transfer_in_progress = 1;
    boot_data.reconnect.awaiting_load_complete = 0;
    boot_data.reconnect.peer_index = peer_index;
    boot_data.reconnect.player_id = player_id;

    MP_LOG_INFO("BOOT", "Reconnect transfer started for player %d on peer %d",
                (int)player_id, (int)peer_index);
    return 1;
}

void mp_bootstrap_host_cancel_reconnect(uint8_t peer_index)
{
    if (!boot_data.reconnect.active || boot_data.reconnect.peer_index != peer_index) {
        return;
    }
    if (boot_data.reconnect.transfer_in_progress) {
        mp_save_transfer_reset();
    }
    memset(&boot_data.reconnect, 0, sizeof(boot_data.reconnect));
}

void mp_bootstrap_host_complete_reconnect(uint8_t peer_index)
{
    if (!boot_data.reconnect.active || boot_data.reconnect.peer_index != peer_index) {
        return;
    }
    memset(&boot_data.reconnect, 0, sizeof(boot_data.reconnect));
}

void mp_bootstrap_host_cancel_late_join(uint8_t peer_index)
{
    if (!boot_data.late_join.active || boot_data.late_join.peer_index != peer_index) {
        return;
    }

    if (boot_data.late_join.transfer_in_progress) {
        mp_save_transfer_reset();
    }
    mp_time_sync_set_join_barrier(0);
    broadcast_join_barrier_event(NET_EVENT_JOIN_BARRIER_RELEASED,
                                 boot_data.late_join.player_id);
    memset(&boot_data.late_join, 0, sizeof(boot_data.late_join));
}

void mp_bootstrap_host_complete_late_join(uint8_t peer_index)
{
    if (!boot_data.late_join.active || boot_data.late_join.peer_index != peer_index) {
        return;
    }
    memset(&boot_data.late_join, 0, sizeof(boot_data.late_join));
}

/* ---- Phase 6: Late join handler ---- */

int mp_bootstrap_host_handle_late_join(uint8_t peer_index, const char *player_name,
                                       uint8_t *out_reject_reason)
{
    uint8_t reject_reason = NET_REJECT_INTERNAL_ERROR;
    if (!net_session_is_host()) {
        goto fail;
    }
    if (boot_data.late_join.active || boot_data.reconnect.active) {
        MP_LOG_WARN("BOOT", "Late join already in progress â€” rejecting new request");
        reject_reason = NET_REJECT_LATE_JOIN_BUSY;
        goto fail;
    }

    if (mp_worldgen_get_reserved_count() <= 0) {
        MP_LOG_ERROR("BOOT", "No reserved slots for late join");
        reject_reason = NET_REJECT_NO_RESERVED_SLOTS;
        goto fail;
    }

    /* 1. Activate join barrier — pauses simulation for all players */
    mp_time_sync_set_join_barrier(1);

    broadcast_join_barrier_event(NET_EVENT_JOIN_BARRIER_ACTIVE, 0);

    /* 2. Begin join transaction for rollback tracking */
    extern mp_join_transaction *mp_join_transaction_begin(uint8_t peer_index);
    mp_join_transaction *txn = mp_join_transaction_begin(peer_index);
    if (!txn) {
        MP_LOG_ERROR("BOOT", "Failed to create join transaction");
        mp_time_sync_set_join_barrier(0);
        broadcast_join_barrier_event(NET_EVENT_JOIN_BARRIER_RELEASED, 0);
        goto fail;
    }

    /* 3. Register player in registry */
    int new_player_id = -1;
    for (int i = bootstrap_player_id_start(); i < MP_MAX_PLAYERS; i++) {
        mp_player *p = mp_player_registry_get((uint8_t)i);
        if (!p || !p->active) {
            new_player_id = i;
            break;
        }
    }
    if (new_player_id < 0) {
        MP_LOG_ERROR("BOOT", "No free player ID for late join");
        reject_reason = NET_REJECT_SESSION_FULL;
        mp_join_transaction_rollback(txn);
        mp_time_sync_set_join_barrier(0);
        broadcast_join_barrier_event(NET_EVENT_JOIN_BARRIER_RELEASED, 0);
        goto fail;
    }

    if (!mp_player_registry_add((uint8_t)new_player_id, player_name, 0, 0)) {
        MP_LOG_ERROR("BOOT", "Failed to add late-join player to registry");
        reject_reason = NET_REJECT_INTERNAL_ERROR;
        mp_join_transaction_rollback(txn);
        mp_time_sync_set_join_barrier(0);
        broadcast_join_barrier_event(NET_EVENT_JOIN_BARRIER_RELEASED, 0);
        goto fail;
    }
    txn->registry_created = 1;
    txn->player_id = (uint8_t)new_player_id;

    int slot_id = mp_player_registry_assign_slot((uint8_t)new_player_id);
    if (slot_id < 0) {
        MP_LOG_ERROR("BOOT", "Failed to assign slot for late-join player");
        reject_reason = NET_REJECT_SESSION_FULL;
        mp_join_transaction_rollback(txn);
        mp_time_sync_set_join_barrier(0);
        broadcast_join_barrier_event(NET_EVENT_JOIN_BARRIER_RELEASED, 0);
        goto fail;
    }
    txn->slot_id = (uint8_t)slot_id;

    /* 4. Assign reserved spawn */
    int city_id = mp_worldgen_assign_reserved_spawn((uint8_t)slot_id);
    if (city_id < 0) {
        MP_LOG_ERROR("BOOT", "No reserved city available for late join");
        reject_reason = NET_REJECT_NO_RESERVED_SLOTS;
        mp_join_transaction_rollback(txn);
        mp_time_sync_set_join_barrier(0);
        broadcast_join_barrier_event(NET_EVENT_JOIN_BARRIER_RELEASED, 0);
        goto fail;
    }
    txn->assigned_city_id = city_id;

    /* 5. Set ownership */
    mp_ownership_set_city(city_id, MP_OWNER_REMOTE_PLAYER, (uint8_t)new_player_id);
    mp_ownership_set_city_online(city_id, 1);
    txn->ownership_created = 1;

    mp_player_registry_set_city((uint8_t)new_player_id, city_id);
    mp_player_registry_set_assigned_city((uint8_t)new_player_id, city_id);

    /* 6. Register in empire sync */
    {
        const mp_spawn_entry *spawn = mp_worldgen_get_spawn_for_slot((uint8_t)slot_id);
        int obj_id = spawn ? spawn->empire_object_id : -1;
        if (obj_id >= 0) {
            mp_empire_sync_register_player_city(city_id, (uint8_t)new_player_id, obj_id);
            txn->empire_sync_registered = 1;
        }
    }

    /* 7. Configure peer */
    {
        net_peer *peer = (net_peer *)net_session_get_peer(peer_index);
        if (peer) {
            /* We need a non-const peer for modification — get via session */
            net_session *sess = net_session_get();
            net_peer *mutable_peer = &sess->peers[peer_index];

            snprintf(mutable_peer->name, sizeof(mutable_peer->name), "%s",
                     player_name);
            net_peer_set_player_id(mutable_peer, (uint8_t)new_player_id);
            mutable_peer->state = PEER_STATE_LOADING;

            mp_player_registry_set_status((uint8_t)new_player_id, MP_PLAYER_IN_GAME);
            mp_player_registry_set_connection_state((uint8_t)new_player_id, MP_CONNECTION_CONNECTED);
        }
    }

    /* 8. Send JOIN_ACCEPT */
    {
        mp_player *player = mp_player_registry_get((uint8_t)new_player_id);
        const mp_game_manifest *manifest = mp_game_manifest_get();
        uint8_t world_uuid[MP_WORLD_UUID_SIZE] = {0};
        uint8_t accept_buf[160];
        net_serializer as;

        if (manifest && manifest->valid) {
            memcpy(world_uuid, manifest->world_instance_uuid, MP_WORLD_UUID_SIZE);
        }

        net_serializer_init(&as, accept_buf, sizeof(accept_buf));
        net_write_u8(&as, (uint8_t)new_player_id);
        net_write_u8(&as, (uint8_t)slot_id);
        net_write_u32(&as, net_session_get()->session_id);
        net_write_u32(&as, mp_worldgen_get_spawn_table()->session_seed);
        net_write_u8(&as, (uint8_t)mp_player_registry_get_count());
        if (player) {
            net_write_raw(&as, player->player_uuid, MP_PLAYER_UUID_SIZE);
            net_write_raw(&as, player->reconnect_token, MP_RECONNECT_TOKEN_SIZE);
        } else {
            uint8_t zeros[16] = {0};
            net_write_raw(&as, zeros, 16);
            net_write_raw(&as, zeros, 16);
        }
        net_write_raw(&as, world_uuid, MP_WORLD_UUID_SIZE);
        net_write_u32(&as, net_session_get()->session_id);
        net_session_send_to_peer(peer_index, NET_MSG_JOIN_ACCEPT,
                                 accept_buf, (uint32_t)net_serializer_position(&as));
    }

    /* 9. Send GAME_PREPARE + save transfer + snapshot + GAME_START_FINAL */
    {
        /* Send manifest */
        uint8_t manifest_buf[MP_GAME_MANIFEST_WIRE_MAX];
        uint32_t manifest_size = 0;
        mp_game_manifest_set_player_count((uint8_t)mp_player_registry_get_count());
        mp_game_manifest_serialize(manifest_buf, &manifest_size);
        net_session_send_to_peer(peer_index, NET_MSG_GAME_PREPARE,
                                 manifest_buf, manifest_size);
    }

    /* 10. Create a checkpoint and start an async save transfer scoped to the joining peer */
    {
        const char *checkpoint_path = mp_safe_file_get_save_path("mp_latejoin_checkpoint.sav");
        if (!game_file_write_saved_game(checkpoint_path)) {
            MP_LOG_ERROR("BOOT", "Failed to write late join checkpoint");
            reject_reason = NET_REJECT_INTERNAL_ERROR;
            mp_join_transaction_rollback(txn);
            mp_time_sync_set_join_barrier(0);
            broadcast_join_barrier_event(NET_EVENT_JOIN_BARRIER_RELEASED, (uint8_t)new_player_id);
            goto fail;
        }

        mp_save_transfer_init();
        if (!mp_save_transfer_host_begin_for_peer(checkpoint_path, peer_index)) {
            MP_LOG_ERROR("BOOT", "Failed to start peer-scoped late join transfer");
            reject_reason = NET_REJECT_INTERNAL_ERROR;
            platform_file_manager_remove_file(checkpoint_path);
            mp_join_transaction_rollback(txn);
            mp_time_sync_set_join_barrier(0);
            broadcast_join_barrier_event(NET_EVENT_JOIN_BARRIER_RELEASED, (uint8_t)new_player_id);
            goto fail;
        }

        platform_file_manager_remove_file(checkpoint_path);
    }

    boot_data.late_join.active = 1;
    boot_data.late_join.transfer_in_progress = 1;
    boot_data.late_join.awaiting_load_complete = 0;
    boot_data.late_join.peer_index = peer_index;
    boot_data.late_join.player_id = (uint8_t)new_player_id;

    /* Transaction start time for timeout tracking */
    txn->start_ms = net_tcp_get_timestamp_ms();

    MP_LOG_INFO("BOOT", "Late join initiated: player '%s' (id=%d, slot=%d, city=%d) — "
                "waiting for GAME_LOAD_COMPLETE",
                player_name, (int)new_player_id, slot_id, city_id);
    if (out_reject_reason) {
        *out_reject_reason = 0;
    }
    return 1;

fail:
    if (out_reject_reason) {
        *out_reject_reason = reject_reason;
    }
    return 0;
}

#endif /* ENABLE_MULTIPLAYER */
