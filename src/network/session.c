#include "session.h"

#ifdef ENABLE_MULTIPLAYER

#include "transport_tcp.h"
#include "transport_udp.h"
#include "serialize.h"
#include "discovery_lan.h"
#include "multiplayer/bootstrap.h"
#include "multiplayer/player_registry.h"
#include "multiplayer/ownership.h"
#include "multiplayer/empire_sync.h"
#include "multiplayer/time_sync.h"
#include "multiplayer/worldgen.h"
#include "multiplayer/checksum.h"
#include "multiplayer/command_bus.h"
#include "multiplayer/resync.h"
#include "multiplayer/save_transfer.h"
#include "multiplayer/snapshot.h"
#include "multiplayer/join_transaction.h"
#include "multiplayer/client_identity.h"
#include "multiplayer/game_manifest.h"
#include "multiplayer/mp_debug_log.h"
#include "multiplayer/dedicated_server.h"
#include "core/config.h"
#include "core/log.h"
#include "core/random.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static net_session session;

/* Persistent player name — survives session init/shutdown cycles */
static char persistent_player_name[NET_MAX_PLAYER_NAME] = "Player";
static int persistent_name_set;

/* Join status tracking for UI feedback */
static net_join_status join_status;
static uint8_t join_reject_reason;
static uint32_t join_start_ms;
static uint32_t join_correlation_id; /* Unique ID per join attempt for log correlation */
static uint32_t heartbeat_sample_counter;

#define NET_JOIN_TIMEOUT_MS 10000 /* 10 seconds to complete handshake */
#define NET_HANDSHAKE_TRACK_MAX 32
#define NET_HANDSHAKE_ATTEMPTS_PER_WINDOW_DEFAULT 8u
#define NET_HANDSHAKE_WINDOW_MS_DEFAULT 10000u
#define NET_HANDSHAKE_COOLDOWN_MS_DEFAULT 10000u
#define NET_HANDSHAKE_PENDING_PER_IP_DEFAULT 2u

/* Chat message ring buffer for client-side display */
#define MP_CHAT_HISTORY_SIZE 64
#define MP_CHAT_MESSAGE_MAX 128

typedef struct {
    uint8_t sender_id;
    char message[MP_CHAT_MESSAGE_MAX];
    uint32_t timestamp_tick;
} mp_chat_entry;

static struct {
    mp_chat_entry entries[MP_CHAT_HISTORY_SIZE];
    int write_index;
    int count;
    int unread;
} chat_history;

typedef struct {
    int in_use;
    char remote_address[48];
    uint32_t window_start_ms;
    uint32_t attempts_in_window;
    uint32_t blocked_until_ms;
} net_handshake_ip_state;

static net_handshake_ip_state handshake_ip_state[NET_HANDSHAKE_TRACK_MAX];

static uint32_t generate_session_id(void)
{
    uint32_t value = 0;
    if (random_fill_secure_bytes((uint8_t *)&value, sizeof(value))) {
        return value ? value : 1u;
    }
    srand((unsigned int)time(NULL));
    value = (uint32_t)rand() ^ ((uint32_t)rand() << 16);
    return value ? value : 1u;
}

static int uuid_is_nonzero(const uint8_t *uuid, size_t size)
{
    if (!uuid) {
        return 0;
    }
    for (size_t i = 0; i < size; i++) {
        if (uuid[i] != 0) {
            return 1;
        }
    }
    return 0;
}

static int find_peer_index(const net_peer *peer)
{
    if (!peer) {
        return -1;
    }
    for (int i = 0; i < NET_MAX_PEERS; i++) {
        if (&session.peers[i] == peer) {
            return i;
        }
    }
    return -1;
}

static int session_host_uses_player_slot(void)
{
    return !mp_dedicated_server_is_enabled();
}

static void copy_text(char *dst, size_t dst_size, const char *src)
{
    if (!dst || dst_size == 0) {
        return;
    }
    snprintf(dst, dst_size, "%s", src ? src : "");
}

static uint8_t first_assignable_player_id(void)
{
    return session_host_uses_player_slot() ? 1 : 0;
}

static void reset_handshake_ip_state(void)
{
    memset(handshake_ip_state, 0, sizeof(handshake_ip_state));
}

static uint32_t handshake_attempts_per_window_limit(void)
{
    const mp_dedicated_server_options *options = mp_dedicated_server_get_options();
    if (mp_dedicated_server_is_enabled() && options && options->handshake_attempts_per_window > 0) {
        return options->handshake_attempts_per_window;
    }
    return NET_HANDSHAKE_ATTEMPTS_PER_WINDOW_DEFAULT;
}

static uint32_t handshake_window_ms_limit(void)
{
    const mp_dedicated_server_options *options = mp_dedicated_server_get_options();
    if (mp_dedicated_server_is_enabled() && options && options->handshake_window_ms > 0) {
        return options->handshake_window_ms;
    }
    return NET_HANDSHAKE_WINDOW_MS_DEFAULT;
}

static uint32_t handshake_cooldown_ms_limit(void)
{
    const mp_dedicated_server_options *options = mp_dedicated_server_get_options();
    if (mp_dedicated_server_is_enabled() && options && options->handshake_cooldown_ms > 0) {
        return options->handshake_cooldown_ms;
    }
    return NET_HANDSHAKE_COOLDOWN_MS_DEFAULT;
}

static uint32_t handshake_pending_per_ip_limit(void)
{
    const mp_dedicated_server_options *options = mp_dedicated_server_get_options();
    if (mp_dedicated_server_is_enabled() && options && options->pending_connections_per_ip > 0) {
        return options->pending_connections_per_ip;
    }
    return NET_HANDSHAKE_PENDING_PER_IP_DEFAULT;
}

static int handshake_count_pending_peers_for_address(const char *address)
{
    int pending = 0;

    if (!address || !address[0]) {
        return 0;
    }

    for (int i = 0; i < NET_MAX_PEERS; i++) {
        net_peer *peer = &session.peers[i];
        if (!peer->active) {
            continue;
        }
        if (strcmp(peer->remote_address, address) != 0) {
            continue;
        }
        if (peer->state == PEER_STATE_CONNECTING) {
            pending++;
        }
    }

    return pending;
}

static net_handshake_ip_state *find_or_alloc_handshake_state(const char *address)
{
    net_handshake_ip_state *candidate = NULL;

    if (!address || !address[0]) {
        return NULL;
    }

    for (int i = 0; i < NET_HANDSHAKE_TRACK_MAX; i++) {
        if (handshake_ip_state[i].in_use &&
            strcmp(handshake_ip_state[i].remote_address, address) == 0) {
            return &handshake_ip_state[i];
        }
        if (!candidate && !handshake_ip_state[i].in_use) {
            candidate = &handshake_ip_state[i];
        }
    }

    if (!candidate) {
        candidate = &handshake_ip_state[0];
        for (int i = 1; i < NET_HANDSHAKE_TRACK_MAX; i++) {
            if (handshake_ip_state[i].window_start_ms < candidate->window_start_ms) {
                candidate = &handshake_ip_state[i];
            }
        }
    }

    memset(candidate, 0, sizeof(*candidate));
    candidate->in_use = 1;
    copy_text(candidate->remote_address, sizeof(candidate->remote_address), address);
    return candidate;
}

static int handshake_rate_limit_exceeded(const char *address, uint32_t now_ms)
{
    net_handshake_ip_state *state;
    uint32_t window_ms;
    uint32_t attempts_limit;

    if (!address || !address[0]) {
        return 0;
    }

    state = find_or_alloc_handshake_state(address);
    if (!state) {
        return 0;
    }

    if (state->blocked_until_ms != 0) {
        if (now_ms < state->blocked_until_ms) {
            return 1;
        }
        state->blocked_until_ms = 0;
        state->attempts_in_window = 0;
        state->window_start_ms = now_ms;
    }

    window_ms = handshake_window_ms_limit();
    attempts_limit = handshake_attempts_per_window_limit();

    if (state->window_start_ms == 0 || now_ms - state->window_start_ms >= window_ms) {
        state->window_start_ms = now_ms;
        state->attempts_in_window = 0;
    }

    if (state->attempts_in_window >= attempts_limit) {
        state->blocked_until_ms = now_ms + handshake_cooldown_ms_limit();
        return 1;
    }

    state->attempts_in_window++;
    return 0;
}

static void copy_current_world_uuid(uint8_t *out_uuid)
{
    const mp_game_manifest *manifest = mp_game_manifest_get();
    if (!out_uuid) {
        return;
    }
    memset(out_uuid, 0, MP_WORLD_UUID_SIZE);
    if (manifest && manifest->valid) {
        memcpy(out_uuid, manifest->world_instance_uuid, MP_WORLD_UUID_SIZE);
    }
}

static uint32_t current_resume_generation(void)
{
    return session.session_id;
}

static int should_clear_persisted_identity(net_join_status preserved_join_status,
                                           uint8_t preserved_reject_reason)
{
    if (preserved_join_status != NET_JOIN_STATUS_REJECTED) {
        return 0;
    }

    return preserved_reject_reason == NET_REJECT_WORLD_MISMATCH ||
           preserved_reject_reason == NET_REJECT_RESUME_GENERATION_MISMATCH;
}

static int has_reconnectable_player(void)
{
    for (int i = 0; i < MP_MAX_PLAYERS; i++) {
        mp_player *player = mp_player_registry_get((uint8_t)i);
        if (!player || !player->active) {
            continue;
        }
        if (player->status == MP_PLAYER_AWAITING_RECONNECT ||
            player->status == MP_PLAYER_DISCONNECTED ||
            player->connection_state == MP_CONNECTION_DISCONNECTED) {
            return 1;
        }
    }
    return 0;
}

static net_discovery_session_phase current_discovery_phase(void)
{
    if (session.state == NET_SESSION_HOSTING_GAME) {
        return NET_DISCOVERY_PHASE_IN_GAME;
    }
    if (session.state == NET_SESSION_HOSTING_LOBBY) {
        if (mp_bootstrap_is_resume()) {
            return NET_DISCOVERY_PHASE_RESUME_LOBBY;
        }
        return NET_DISCOVERY_PHASE_LOBBY;
    }
    return NET_DISCOVERY_PHASE_LOBBY;
}

static net_discovery_join_policy current_discovery_join_policy(void)
{
    if (session.state == NET_SESSION_HOSTING_LOBBY) {
        if (mp_bootstrap_is_resume()) {
            return NET_DISCOVERY_JOIN_RECONNECT_ONLY;
        }
        return NET_DISCOVERY_JOIN_OPEN_LOBBY;
    }

    if (session.state == NET_SESSION_HOSTING_GAME) {
        if (mp_bootstrap_is_late_join_busy() || mp_bootstrap_is_reconnect_busy()) {
            return NET_DISCOVERY_JOIN_CLOSED;
        }
        if (mp_worldgen_get_reserved_count() > 0) {
            return NET_DISCOVERY_JOIN_LATE_JOIN_ALLOWED;
        }
        if (has_reconnectable_player()) {
            return NET_DISCOVERY_JOIN_RECONNECT_ONLY;
        }
    }

    return NET_DISCOVERY_JOIN_CLOSED;
}

static void build_discovery_announcement(net_discovery_announcement *announcement)
{
    const mp_game_manifest *manifest = mp_game_manifest_get();
    uint8_t player_count = (uint8_t)mp_player_registry_get_count();
    uint8_t max_players = NET_MAX_PLAYERS;

    if (!announcement) {
        return;
    }
    memset(announcement, 0, sizeof(*announcement));

    copy_text(announcement->host_name, sizeof(announcement->host_name),
              session.local_player_name);
    announcement->game_port = session.port;
    if (mp_dedicated_server_is_enabled()) {
        const mp_dedicated_server_options *options = mp_dedicated_server_get_options();
        if (options) {
            max_players = options->max_players;
        }
    }

    announcement->player_count = player_count;
    if (manifest && manifest->valid) {
        if (manifest->max_players > 0) {
            max_players = manifest->max_players;
        }
        if (session.role == NET_ROLE_HOST) {
            mp_game_manifest_set_player_count(announcement->player_count);
        }
        memcpy(announcement->world_instance_uuid,
               manifest->world_instance_uuid,
               MP_WORLD_UUID_SIZE);
    }
    announcement->max_players = max_players;
    announcement->session_id = session.session_id;
    announcement->protocol_version = NET_PROTOCOL_VERSION;
    announcement->session_phase = (uint8_t)current_discovery_phase();
    announcement->join_policy = (uint8_t)current_discovery_join_policy();
    announcement->reserved_slots_free = (uint8_t)mp_worldgen_get_reserved_count();
    announcement->resume_generation = current_resume_generation();
}

static int find_free_peer_slot(void)
{
    for (int i = 0; i < NET_MAX_PEERS; i++) {
        if (!session.peers[i].active) {
            return i;
        }
    }
    return -1;
}

static void close_peer(int index)
{
    if (index < 0 || index >= NET_MAX_PEERS) {
        return;
    }
    net_peer *peer = &session.peers[index];
    if (!peer->active) {
        return;
    }
    net_tcp_close(peer->socket_fd);
    net_peer_reset(peer);
    session.peer_count--;
}

static int peer_accepts_lobby_message(const net_peer *peer)
{
    if (!peer || !peer->active) {
        return 0;
    }
    return peer->state == PEER_STATE_JOINED ||
           peer->state == PEER_STATE_READY ||
           peer->state == PEER_STATE_LOADING ||
           peer->state == PEER_STATE_IN_GAME;
}

static int peer_accepts_in_game_message(const net_peer *peer)
{
    return peer && peer->active && peer->state == PEER_STATE_IN_GAME;
}

static void apply_lobby_snapshot(const uint8_t *payload, uint32_t size)
{
    mp_player_registry_deserialize(payload, size);
    mp_player_registry_mark_local_player(session.local_player_id);
}

static void broadcast_lobby_snapshot(void)
{
    uint8_t snapshot_buf[8192];
    uint32_t snapshot_size = 0;
    mp_player_registry_serialize(snapshot_buf, &snapshot_size);
    net_session_broadcast_lobby(NET_MSG_LOBBY_SNAPSHOT, snapshot_buf, snapshot_size);
    net_session_refresh_discovery_announcement();
}

static void broadcast_full_snapshot_to_in_game_peers(void)
{
    uint8_t *snap_buf = (uint8_t *)malloc(MP_SNAPSHOT_MAX_SIZE);
    if (!snap_buf) {
        return;
    }

    {
        uint32_t snap_size = 0;
        if (mp_snapshot_build_full(snap_buf, MP_SNAPSHOT_MAX_SIZE, &snap_size)) {
            net_session_broadcast_in_game(NET_MSG_FULL_SNAPSHOT, snap_buf, snap_size);
        }
    }

    free(snap_buf);
}

static void handle_peer_disconnect(int peer_index)
{
    net_peer *peer = &session.peers[peer_index];
    uint8_t player_id;
    char player_name[NET_MAX_PLAYER_NAME];
    if (!peer->active) {
        return;
    }
    copy_text(player_name, sizeof(player_name), peer->name);

    /* Phase 7: Check for incomplete join transaction and roll back */
    {
        mp_join_transaction *txn = mp_join_transaction_find_by_peer((uint8_t)peer_index);
        if (txn && txn->active) {
            MP_LOG_INFO("SESSION", "Peer %d disconnected during join — rolling back transaction",
                        peer_index);
            mp_bootstrap_host_cancel_late_join((uint8_t)peer_index);
            mp_join_transaction_rollback(txn);
            close_peer(peer_index);
            return;
        }
        mp_bootstrap_host_cancel_reconnect((uint8_t)peer_index);
    }

    player_id = peer->player_id;
    mp_player *player = mp_player_registry_get(player_id);
    if (player && player->active) {
        if (session.state == NET_SESSION_HOSTING_LOBBY) {
            if (mp_bootstrap_is_resume()) {
                uint32_t timeout_ticks = 3000;
                mp_player_registry_mark_disconnected(player_id,
                    session.authoritative_tick, timeout_ticks);
                mp_ownership_set_player_routes_offline(player_id);
                mp_empire_sync_unregister_player_city(player_id);
                if (player->assigned_city_id >= 0) {
                    mp_ownership_set_city_online(player->assigned_city_id, 0);
                }
            } else {
                mp_player_registry_remove(player_id);
            }
        } else {
            mp_player_registry_mark_disconnected(player_id,
                session.authoritative_tick, 3000);
            mp_ownership_set_player_routes_offline(player_id);
            mp_empire_sync_unregister_player_city(player_id);
            if (player->assigned_city_id >= 0) {
                mp_ownership_set_city_online(player->assigned_city_id, 0);
            }
        }
    }

    close_peer(peer_index);
    net_session_refresh_discovery_announcement();

    if (session.state == NET_SESSION_HOSTING_LOBBY) {
        broadcast_lobby_snapshot();
    } else {
        uint8_t event_buf[32];
        net_serializer es;
        net_serializer_init(&es, event_buf, sizeof(event_buf));
        net_write_u16(&es, NET_EVENT_PLAYER_LEFT);
        net_write_u32(&es, session.authoritative_tick);
        net_write_u8(&es, player_id);
        net_session_broadcast_in_game(NET_MSG_HOST_EVENT, event_buf,
                                      (uint32_t)net_serializer_position(&es));
        log_info("Player disconnected, city frozen", player_name, (int)player_id);
    }
}

static void send_raw_to_peer(net_peer *peer, uint16_t message_type,
                             const uint8_t *payload, uint32_t size)
{
    size_t encoded = net_codec_encode(&peer->codec,
                                      message_type,
                                      session.session_id,
                                      session.authoritative_tick,
                                      payload, size,
                                      peer->send_buffer,
                                      sizeof(peer->send_buffer));
    if (encoded == 0) {
        log_error("Failed to encode packet", net_protocol_message_name(message_type), 0);
        return;
    }

    int sent = net_tcp_send(peer->socket_fd, peer->send_buffer, encoded);
    if (sent < 0) {
        log_error("Failed to send to peer", peer->name, 0);
        return;
    }
    peer->bytes_sent += sent;
    peer->packets_sent++;
}

static uint32_t next_heartbeat_sample_id(void)
{
    heartbeat_sample_counter++;
    if (heartbeat_sample_counter == 0) {
        heartbeat_sample_counter = 1;
    }
    return heartbeat_sample_counter;
}

static int parse_heartbeat_payload(const uint8_t *payload, uint32_t size,
                                   net_msg_heartbeat *out)
{
    net_serializer s;

    if (!payload || !out || size < (uint32_t)(sizeof(uint32_t) * 2 + sizeof(uint8_t))) {
        return 0;
    }

    net_serializer_init(&s, (uint8_t *)payload, size);
    out->timestamp_ms = net_read_u32(&s);
    out->sample_id = net_read_u32(&s);
    out->flags = net_read_u8(&s);
    return !net_serializer_has_overflow(&s);
}

static void send_heartbeat_to_peer(net_peer *peer, uint32_t timestamp_ms,
                                   uint32_t sample_id, uint8_t flags)
{
    uint8_t hb_buf[16];
    net_serializer hs;

    if (!peer || !peer->active) {
        return;
    }

    net_serializer_init(&hs, hb_buf, sizeof(hb_buf));
    net_write_u32(&hs, timestamp_ms);
    net_write_u32(&hs, sample_id);
    net_write_u8(&hs, flags);
    send_raw_to_peer(peer, NET_MSG_HEARTBEAT, hb_buf,
                     (uint32_t)net_serializer_position(&hs));
}

static void send_heartbeat_to_host(uint32_t timestamp_ms,
                                   uint32_t sample_id, uint8_t flags)
{
    uint8_t hb_buf[16];
    net_serializer hs;

    net_serializer_init(&hs, hb_buf, sizeof(hb_buf));
    net_write_u32(&hs, timestamp_ms);
    net_write_u32(&hs, sample_id);
    net_write_u8(&hs, flags);
    net_session_send_to_host(NET_MSG_HEARTBEAT, hb_buf,
                             (uint32_t)net_serializer_position(&hs));
}

static void send_join_accept_to_peer(net_peer *peer, uint8_t player_id, uint8_t slot_id,
                                     const uint8_t *player_uuid,
                                     const uint8_t *reconnect_token,
                                     uint8_t player_count,
                                     uint32_t session_seed)
{
    uint8_t accept_world_uuid[MP_WORLD_UUID_SIZE];
    uint8_t accept_buf[160];
    net_serializer as;

    copy_current_world_uuid(accept_world_uuid);

    net_serializer_init(&as, accept_buf, sizeof(accept_buf));
    net_write_u8(&as, player_id);
    net_write_u8(&as, slot_id);
    net_write_u32(&as, session.session_id);
    net_write_u32(&as, session_seed);
    net_write_u8(&as, player_count);
    if (player_uuid) {
        net_write_raw(&as, player_uuid, MP_PLAYER_UUID_SIZE);
    } else {
        uint8_t zeros[MP_PLAYER_UUID_SIZE] = {0};
        net_write_raw(&as, zeros, MP_PLAYER_UUID_SIZE);
    }
    if (reconnect_token) {
        net_write_raw(&as, reconnect_token, MP_RECONNECT_TOKEN_SIZE);
    } else {
        uint8_t zeros[MP_RECONNECT_TOKEN_SIZE] = {0};
        net_write_raw(&as, zeros, MP_RECONNECT_TOKEN_SIZE);
    }
    net_write_raw(&as, accept_world_uuid, MP_WORLD_UUID_SIZE);
    net_write_u32(&as, current_resume_generation());
    send_raw_to_peer(peer, NET_MSG_JOIN_ACCEPT, accept_buf,
                     (uint32_t)net_serializer_position(&as));
}

static void reject_peer(net_peer *peer, uint8_t reason, const char *log_context)
{
    int peer_index;
    uint8_t reject_buf[2];
    net_serializer rs;

    if (!peer) {
        return;
    }

    net_serializer_init(&rs, reject_buf, sizeof(reject_buf));
    net_write_u8(&rs, reason);
    send_raw_to_peer(peer, NET_MSG_JOIN_REJECT, reject_buf,
                     (uint32_t)net_serializer_position(&rs));

    MP_LOG_WARN("HANDSHAKE", "JOIN_REJECT sent: peer='%s' addr='%s' reason=%d context=%s",
                peer->name[0] ? peer->name : "<pending>",
                peer->remote_address[0] ? peer->remote_address : "<unknown>",
                (int)reason,
                log_context ? log_context : "n/a");

    peer_index = find_peer_index(peer);
    if (peer_index >= 0) {
        close_peer(peer_index);
    } else if (peer->socket_fd >= 0) {
        net_tcp_close(peer->socket_fd);
        net_peer_reset(peer);
    }
}

static int try_reconnect_player(net_peer *peer, const uint8_t *uuid,
                                const uint8_t *reconnect_token)
{
    /* Check if a player with this UUID is awaiting reconnect */
    mp_player *existing = mp_player_registry_get_by_uuid(uuid);
    if (!existing || !existing->active) {
        return 0;
    }
    if (existing->status != MP_PLAYER_AWAITING_RECONNECT &&
        existing->connection_state != MP_CONNECTION_DISCONNECTED) {
        return 0;
    }

    /* Validate reconnect token */
    if (!mp_player_registry_validate_reconnect(uuid, reconnect_token)) {
        log_error("Invalid reconnect token", 0, 0);
        return 0;
    }

    /* Reconnect the player */
    uint8_t old_player_id = existing->player_id;
    int peer_index = find_peer_index(peer);
    if (peer_index < 0) {
        return 0;
    }

    mp_player_registry_handle_reconnect(uuid, (uint8_t)peer_index);
    mp_player_registry_set_connection_state(old_player_id, MP_CONNECTION_CONNECTED);

    /* Restore routes */
    mp_ownership_set_player_routes_online(old_player_id);

    /* Mark city back online */
    if (existing->assigned_city_id >= 0) {
        mp_ownership_set_city_online(existing->assigned_city_id, 1);
    }

    /* Update peer identity */
    copy_text(peer->name, sizeof(peer->name), existing->name);
    net_peer_set_player_id(peer, old_player_id);
    peer->state = PEER_STATE_JOINED;

    log_info("Player reconnected", existing->name, (int)old_player_id);

    send_join_accept_to_peer(peer, old_player_id, existing->slot_id,
                             existing->player_uuid, existing->reconnect_token,
                             (uint8_t)mp_player_registry_get_count(),
                             mp_worldgen_get_spawn_table_mutable()->session_seed);

    {
        if (mp_bootstrap_host_begin_reconnect((uint8_t)peer_index, old_player_id)) {
            uint8_t event_buf[32];
            net_serializer es;
            net_serializer_init(&es, event_buf, sizeof(event_buf));
            net_write_u16(&es, NET_EVENT_PLAYER_RECONNECTED);
            net_write_u32(&es, session.authoritative_tick);
            net_write_u8(&es, old_player_id);
            net_session_broadcast_in_game(NET_MSG_HOST_EVENT, event_buf,
                                          (uint32_t)net_serializer_position(&es));
            net_session_refresh_discovery_announcement();
            return 1;
        }
    }

    peer->state = PEER_STATE_IN_GAME;

    {
        uint8_t event_buf[32];
        net_serializer es;
        net_serializer_init(&es, event_buf, sizeof(event_buf));
        net_write_u16(&es, NET_EVENT_PLAYER_RECONNECTED);
        net_write_u32(&es, session.authoritative_tick);
        net_write_u8(&es, old_player_id);
        net_session_broadcast_in_game(NET_MSG_HOST_EVENT, event_buf,
                                      (uint32_t)net_serializer_position(&es));
    }

    /* Send full snapshot to reconnecting player so they can resync */
    multiplayer_resync_handle_request(old_player_id);
    net_session_refresh_discovery_announcement();

    return 1;
}

static void handle_hello_impl(net_peer *peer, const uint8_t *payload, uint32_t size)
{
    net_serializer s;
    uint32_t magic;
    uint16_t version;
    uint32_t save_version = 0;
    uint32_t map_hash = 0;
    uint32_t scenario_hash = 0;
    uint32_t feature_flags = 0;
    char name[NET_MAX_PLAYER_NAME];
    uint8_t player_uuid[MP_PLAYER_UUID_SIZE];
    uint8_t reconnect_token[MP_RECONNECT_TOKEN_SIZE];
    uint8_t requested_slot_id = 0xFF;
    uint8_t requested_world_uuid[MP_WORLD_UUID_SIZE];
    uint32_t requested_resume_generation = 0;
    uint8_t current_world_uuid[MP_WORLD_UUID_SIZE];
    int peer_index;
    int has_uuid;
    int reserved_slots;
    mp_player *existing;

    memset(name, 0, sizeof(name));
    memset(player_uuid, 0, sizeof(player_uuid));
    memset(reconnect_token, 0, sizeof(reconnect_token));
    memset(requested_world_uuid, 0, sizeof(requested_world_uuid));
    copy_current_world_uuid(current_world_uuid);

    if (!peer || !payload || size < 6) {
        MP_LOG_ERROR("HANDSHAKE", "HELLO too small: %u bytes", size);
        reject_peer(peer, NET_REJECT_INTERNAL_ERROR, "hello_too_small");
        return;
    }

    net_serializer_init(&s, (uint8_t *)payload, size);
    magic = net_read_u32(&s);
    version = net_read_u16(&s);

    if (net_serializer_has_overflow(&s)) {
        reject_peer(peer, NET_REJECT_INTERNAL_ERROR, "hello_header_overflow");
        return;
    }

    if (net_serializer_remaining(&s) >= (size_t)(sizeof(uint32_t) * 4 +
                                                 NET_MAX_PLAYER_NAME +
                                                 MP_PLAYER_UUID_SIZE +
                                                 MP_RECONNECT_TOKEN_SIZE)) {
        save_version = net_read_u32(&s);
        map_hash = net_read_u32(&s);
        scenario_hash = net_read_u32(&s);
        feature_flags = net_read_u32(&s);
        net_read_raw(&s, name, NET_MAX_PLAYER_NAME);
        net_read_raw(&s, player_uuid, MP_PLAYER_UUID_SIZE);
        net_read_raw(&s, reconnect_token, MP_RECONNECT_TOKEN_SIZE);
    } else if (net_serializer_remaining(&s) >= NET_MAX_PLAYER_NAME) {
        net_read_raw(&s, name, NET_MAX_PLAYER_NAME);
    } else {
        reject_peer(peer, NET_REJECT_INTERNAL_ERROR, "hello_name_missing");
        return;
    }

    if (net_serializer_remaining(&s) >= 1) {
        requested_slot_id = net_read_u8(&s);
    }
    if (net_serializer_remaining(&s) >= MP_WORLD_UUID_SIZE) {
        net_read_raw(&s, requested_world_uuid, MP_WORLD_UUID_SIZE);
    }
    if (net_serializer_remaining(&s) >= sizeof(uint32_t)) {
        requested_resume_generation = net_read_u32(&s);
    }

    if (net_serializer_has_overflow(&s)) {
        MP_LOG_ERROR("HANDSHAKE", "Malformed HELLO: payload size=%u", size);
        reject_peer(peer, NET_REJECT_INTERNAL_ERROR, "hello_malformed");
        return;
    }

    name[NET_MAX_PLAYER_NAME - 1] = '\0';
    {
        int len = (int)strlen(name);
        while (len > 0 && name[len - 1] == ' ') {
            name[--len] = '\0';
        }
        if (len == 0) {
            copy_text(name, sizeof(name), "Unknown");
        }
    }

    copy_text(peer->name, sizeof(peer->name), name);

    MP_LOG_INFO("HANDSHAKE", "HELLO received: name='%s' magic=0x%08x version=%d "
                "save_ver=%u map_hash=0x%08x scenario_hash=0x%08x flags=0x%08x "
                "slot=%d resume_gen=%u",
                name, magic, (int)version, save_version, map_hash, scenario_hash,
                feature_flags, (int)requested_slot_id, requested_resume_generation);

    if (magic != NET_MAGIC || !net_protocol_check_version(version)) {
        reject_peer(peer, NET_REJECT_VERSION_MISMATCH, "protocol_mismatch");
        return;
    }

    if (handshake_rate_limit_exceeded(peer->remote_address, net_tcp_get_timestamp_ms())) {
        reject_peer(peer, NET_REJECT_RATE_LIMITED, "hello_rate_limited");
        return;
    }

    peer_index = find_peer_index(peer);
    has_uuid = uuid_is_nonzero(player_uuid, MP_PLAYER_UUID_SIZE);
    existing = has_uuid ? mp_player_registry_get_by_uuid(player_uuid) : NULL;
    reserved_slots = mp_worldgen_get_reserved_count();

    if (mp_dedicated_server_is_banned(has_uuid ? player_uuid : NULL,
                                      peer->remote_address)) {
        reject_peer(peer, NET_REJECT_BANNED, "banlist_match");
        return;
    }

    if (session.state == NET_SESSION_HOSTING_LOBBY && mp_bootstrap_is_resume()) {
        uint8_t old_player_id;

        if (!has_uuid) {
            reject_peer(peer, NET_REJECT_RECONNECT_REQUIRED, "resume_requires_identity");
            return;
        }
        if (uuid_is_nonzero(requested_world_uuid, MP_WORLD_UUID_SIZE) &&
            uuid_is_nonzero(current_world_uuid, MP_WORLD_UUID_SIZE) &&
            memcmp(requested_world_uuid, current_world_uuid, MP_WORLD_UUID_SIZE) != 0) {
            reject_peer(peer, NET_REJECT_WORLD_MISMATCH, "resume_world_mismatch");
            return;
        }
        if (requested_resume_generation != 0 &&
            current_resume_generation() != 0 &&
            requested_resume_generation != current_resume_generation()) {
            reject_peer(peer, NET_REJECT_RESUME_GENERATION_MISMATCH,
                        "resume_generation_mismatch");
            return;
        }
        if (!existing || !existing->active) {
            reject_peer(peer, NET_REJECT_SLOT_NOT_FOUND, "resume_uuid_not_found");
            return;
        }
        if (requested_slot_id != 0xFF && existing->slot_id != requested_slot_id) {
            reject_peer(peer, NET_REJECT_SLOT_NOT_FOUND, "resume_slot_mismatch");
            return;
        }
        if ((existing->status != MP_PLAYER_AWAITING_RECONNECT &&
             existing->status != MP_PLAYER_DISCONNECTED) ||
            !mp_player_registry_validate_reconnect(player_uuid, reconnect_token)) {
            reject_peer(peer, NET_REJECT_RECONNECT_REQUIRED, "resume_token_invalid");
            return;
        }
        if (peer_index < 0) {
            reject_peer(peer, NET_REJECT_INTERNAL_ERROR, "resume_missing_peer_index");
            return;
        }

        old_player_id = existing->player_id;
        mp_player_registry_handle_reconnect(player_uuid, (uint8_t)peer_index);
        mp_player_registry_set_status(old_player_id, MP_PLAYER_LOBBY);
        mp_player_registry_set_connection_state(old_player_id, MP_CONNECTION_CONNECTED);

        copy_text(peer->name, sizeof(peer->name), existing->name);
        net_peer_set_player_id(peer, old_player_id);
        peer->state = PEER_STATE_JOINED;

        if (existing->assigned_city_id >= 0) {
            mp_ownership_set_city_online(existing->assigned_city_id, 1);
        }
        mp_ownership_set_player_routes_online(old_player_id);

        send_join_accept_to_peer(peer, old_player_id, existing->slot_id,
                                 existing->player_uuid, existing->reconnect_token,
                                 (uint8_t)mp_player_registry_get_count(),
                                 mp_worldgen_get_spawn_table_mutable()->session_seed);
        broadcast_lobby_snapshot();
        return;
    }

    if (session.state == NET_SESSION_HOSTING_GAME) {
        int reconnect_candidate = existing && existing->active &&
            (existing->status == MP_PLAYER_AWAITING_RECONNECT ||
             existing->status == MP_PLAYER_DISCONNECTED ||
             existing->connection_state == MP_CONNECTION_DISCONNECTED);

        if (reconnect_candidate) {
            if (uuid_is_nonzero(requested_world_uuid, MP_WORLD_UUID_SIZE) &&
                uuid_is_nonzero(current_world_uuid, MP_WORLD_UUID_SIZE) &&
                memcmp(requested_world_uuid, current_world_uuid, MP_WORLD_UUID_SIZE) != 0) {
                reject_peer(peer, NET_REJECT_WORLD_MISMATCH, "game_world_mismatch");
                return;
            }
            if (requested_resume_generation != 0 &&
                current_resume_generation() != 0 &&
                requested_resume_generation != current_resume_generation()) {
                reject_peer(peer, NET_REJECT_RESUME_GENERATION_MISMATCH,
                            "game_generation_mismatch");
                return;
            }
            if (requested_slot_id != 0xFF && existing->slot_id != requested_slot_id) {
                reject_peer(peer, NET_REJECT_SLOT_NOT_FOUND, "game_slot_mismatch");
                return;
            }
            if (try_reconnect_player(peer, player_uuid, reconnect_token)) {
                return;
            }
            reject_peer(peer, NET_REJECT_RECONNECT_REQUIRED, "game_reconnect_failed");
            return;
        }
        if (has_uuid && existing && existing->active) {
            reject_peer(peer, NET_REJECT_RECONNECT_REQUIRED, "identity_already_active");
            return;
        }

        if (mp_bootstrap_is_late_join_busy()) {
            reject_peer(peer, NET_REJECT_LATE_JOIN_BUSY, "late_join_busy");
            return;
        }
        if (reserved_slots > 0) {
            uint8_t late_join_reject_reason = NET_REJECT_INTERNAL_ERROR;

            if (peer_index < 0) {
                reject_peer(peer, NET_REJECT_INTERNAL_ERROR, "late_join_missing_peer_index");
                return;
            }
            if (mp_bootstrap_host_handle_late_join((uint8_t)peer_index, name,
                                                   &late_join_reject_reason)) {
                net_session_refresh_discovery_announcement();
                return;
            }
            reject_peer(peer, late_join_reject_reason, "late_join_rejected");
            return;
        }
        if (has_reconnectable_player()) {
            reject_peer(peer, NET_REJECT_RECONNECT_REQUIRED, "reconnect_only");
            return;
        }
        reject_peer(peer, NET_REJECT_GAME_IN_PROGRESS, "game_closed");
        return;
    }

    for (int i = 0; i < NET_MAX_PEERS; i++) {
        if (session.peers[i].active && session.peers[i].player_id != peer->player_id &&
            strncmp(session.peers[i].name, name, NET_MAX_PLAYER_NAME - 1) == 0) {
            reject_peer(peer, NET_REJECT_NAME_TAKEN, "duplicate_peer_name");
            return;
        }
    }
    if (net_session_has_local_player() &&
        strncmp(session.local_player_name, name, NET_MAX_PLAYER_NAME - 1) == 0) {
        reject_peer(peer, NET_REJECT_NAME_TAKEN, "duplicate_host_name");
        return;
    }

    {
        int new_player_id = -1;
        int slot;
        mp_player *new_player;
        uint32_t now = net_tcp_get_timestamp_ms();
        uint32_t session_seed = mp_worldgen_get_spawn_table_mutable()->session_seed;

        for (int i = first_assignable_player_id(); i < MP_MAX_PLAYERS; i++) {
            mp_player *candidate = mp_player_registry_get((uint8_t)i);
            if (!candidate || !candidate->active) {
                new_player_id = i;
                break;
            }
        }
        if (new_player_id < 0) {
            reject_peer(peer, NET_REJECT_SESSION_FULL, "lobby_full");
            return;
        }

        if (!mp_player_registry_add((uint8_t)new_player_id, name, 0, 0)) {
            reject_peer(peer, NET_REJECT_INTERNAL_ERROR, "registry_add_failed");
            return;
        }

        slot = mp_player_registry_assign_slot((uint8_t)new_player_id);
        if (slot < 0) {
            mp_player_registry_remove((uint8_t)new_player_id);
            reject_peer(peer, NET_REJECT_SESSION_FULL, "slot_assign_failed");
            return;
        }

        net_peer_set_player_id(peer, (uint8_t)new_player_id);
        peer->state = PEER_STATE_JOINED;
        peer->last_heartbeat_recv_ms = now;

        mp_player_registry_set_status((uint8_t)new_player_id, MP_PLAYER_LOBBY);
        mp_player_registry_set_connection_state((uint8_t)new_player_id, MP_CONNECTION_CONNECTED);

        new_player = mp_player_registry_get((uint8_t)new_player_id);
        send_join_accept_to_peer(peer, (uint8_t)new_player_id, (uint8_t)slot,
                                 new_player ? new_player->player_uuid : NULL,
                                 new_player ? new_player->reconnect_token : NULL,
                                 (uint8_t)mp_player_registry_get_count(),
                                 session_seed);

        MP_LOG_INFO("HANDSHAKE", "JOIN_ACCEPT sent: player_id=%d slot=%d session=0x%08x seed=%u",
                    (int)new_player_id, slot, session.session_id, session_seed);
        log_info("Player joined", name, (int)new_player_id);
        broadcast_lobby_snapshot();
    }
}

static void handle_hello(net_peer *peer, const uint8_t *payload, uint32_t size)
{
    handle_hello_impl(peer, payload, size);
    return;
#if 0
    /* Minimum HELLO size: magic(4) + version(2) + name(32) = 38 bytes */
    if (size < 38) {
        MP_LOG_ERROR("HANDSHAKE", "HELLO too small: %u bytes (minimum 38)", size);
        log_error("HELLO packet too small", 0, (int)size);
        return;
    }

    net_serializer s;
    net_serializer_init(&s, (uint8_t *)payload, size);

    /* Read extended hello */
    uint32_t magic = net_read_u32(&s);
    uint16_t version = net_read_u16(&s);
    uint32_t save_version = net_read_u32(&s);
    uint32_t map_hash = net_read_u32(&s);
    uint32_t scenario_hash = net_read_u32(&s);
    uint32_t feature_flags = net_read_u32(&s);
    char name[NET_MAX_PLAYER_NAME];
    net_read_raw(&s, name, NET_MAX_PLAYER_NAME);
    name[NET_MAX_PLAYER_NAME - 1] = '\0';
    uint8_t player_uuid[16];
    net_read_raw(&s, player_uuid, 16);
    uint8_t reconnect_token[16];
    net_read_raw(&s, reconnect_token, 16);

    /* Suppress unused variable warnings for forward compatibility */
    (void)save_version;
    (void)map_hash;
    (void)scenario_hash;
    (void)feature_flags;

    if (net_serializer_has_overflow(&s)) {
        /* Fall back to legacy hello (magic + version + name only) */
        net_serializer_init(&s, (uint8_t *)payload, size);
        magic = net_read_u32(&s);
        version = net_read_u16(&s);
        net_read_raw(&s, name, NET_MAX_PLAYER_NAME);
        name[NET_MAX_PLAYER_NAME - 1] = '\0';
        memset(player_uuid, 0, 16);
        memset(reconnect_token, 0, 16);

        if (net_serializer_has_overflow(&s)) {
            MP_LOG_ERROR("HANDSHAKE", "Malformed HELLO: payload size=%u, overflow after legacy parse", size);
            log_error("Malformed HELLO from peer", 0, 0);
            return;
        }
    }

    /* Defensive: trim trailing spaces and ensure valid name */
    {
        int len = (int)strlen(name);
        while (len > 0 && (name[len - 1] == ' ' || name[len - 1] == '\0')) {
            name[--len] = '\0';
        }
        if (len == 0) {
            strncpy(name, "Unknown", NET_MAX_PLAYER_NAME - 1);
        }
    }

    MP_LOG_INFO("HANDSHAKE", "HELLO received: name='%s' magic=0x%08x version=%d "
                "save_ver=%u map_hash=0x%08x scenario_hash=0x%08x flags=0x%08x",
                name, magic, (int)version, save_version, map_hash, scenario_hash, feature_flags);

    if (magic != NET_MAGIC) {
        MP_LOG_ERROR("HANDSHAKE", "REJECT: invalid magic 0x%08x (expected 0x%08x)", magic, NET_MAGIC);
        log_error("Invalid magic in HELLO", 0, 0);
        uint8_t reject_buf[2];
        net_serializer rs;
        net_serializer_init(&rs, reject_buf, sizeof(reject_buf));
        net_write_u8(&rs, NET_REJECT_VERSION_MISMATCH);
        send_raw_to_peer(peer, NET_MSG_JOIN_REJECT, reject_buf,
                         (uint32_t)net_serializer_position(&rs));
        return;
    }

    if (!net_protocol_check_version(version)) {
        MP_LOG_ERROR("HANDSHAKE", "REJECT: version mismatch, remote=%d local=%d",
                     (int)version, NET_PROTOCOL_VERSION);
        log_error("Protocol version mismatch", 0, (int)version);
        uint8_t reject_buf[2];
        net_serializer rs;
        net_serializer_init(&rs, reject_buf, sizeof(reject_buf));
        net_write_u8(&rs, NET_REJECT_VERSION_MISMATCH);
        send_raw_to_peer(peer, NET_MSG_JOIN_REJECT, reject_buf,
                         (uint32_t)net_serializer_position(&rs));
        return;
    }

    /* If in resume lobby, try reconnect by UUID */
    if (session.state == NET_SESSION_HOSTING_LOBBY) {
        if (mp_bootstrap_is_resume()) {
            int has_uuid = 0;
            for (int i = 0; i < 16; i++) {
                if (player_uuid[i] != 0) {
                    has_uuid = 1;
                    break;
                }
            }
            if (has_uuid) {
                mp_player *existing = mp_player_registry_get_by_uuid(player_uuid);
                if (existing && existing->active &&
                    existing->status == MP_PLAYER_AWAITING_RECONNECT) {
                    /* Reconnect to saved player slot */
                    uint8_t old_player_id = existing->player_id;
                    int peer_index = -1;
                    for (int i = 0; i < NET_MAX_PEERS; i++) {
                        if (&session.peers[i] == peer) {
                            peer_index = i;
                            break;
                        }
                    }
                    if (peer_index >= 0) {
                        mp_player_registry_handle_reconnect(player_uuid, (uint8_t)peer_index);

                        strncpy(peer->name, existing->name, NET_MAX_PLAYER_NAME - 1);
                        peer->name[NET_MAX_PLAYER_NAME - 1] = '\0';
                        net_peer_set_player_id(peer, old_player_id);
                        peer->state = PEER_STATE_JOINED;

                        /* Mark city online */
                        if (existing->assigned_city_id >= 0) {
                            mp_ownership_set_city_online(existing->assigned_city_id, 1);
                        }
                        mp_ownership_set_player_routes_online(old_player_id);

                        log_info("Player reconnected in resume lobby", existing->name,
                                 (int)old_player_id);

                        /* Send JOIN_ACCEPT with reconnect info */
                        uint8_t accept_buf[128];
                        net_serializer as;
                        net_serializer_init(&as, accept_buf, sizeof(accept_buf));
                        net_write_u8(&as, old_player_id);
                        net_write_u8(&as, existing->slot_id);
                        net_write_u32(&as, session.session_id);
                        net_write_u32(&as, mp_worldgen_get_spawn_table_mutable()->session_seed);
                        net_write_u8(&as, (uint8_t)(session.peer_count + 1));
                        net_write_raw(&as, existing->player_uuid, MP_PLAYER_UUID_SIZE);
                        net_write_raw(&as, existing->reconnect_token, MP_RECONNECT_TOKEN_SIZE);
                        send_raw_to_peer(peer, NET_MSG_JOIN_ACCEPT, accept_buf,
                                         (uint32_t)net_serializer_position(&as));

                        broadcast_lobby_snapshot();
                        return;
                    }
                }
            }
            /* Fall through to normal lobby join if UUID didn't match */
        }
    }

    /* If game is in progress, try reconnect by UUID */
    if (session.state == NET_SESSION_HOSTING_GAME) {
        /* Check for non-zero UUID */
        int has_uuid = 0;
        for (int i = 0; i < 16; i++) {
            if (player_uuid[i] != 0) {
                has_uuid = 1;
                break;
            }
        }

        MP_LOG_INFO("HANDSHAKE", "Game in progress, has_uuid=%d — attempting reconnect for '%s'",
                    has_uuid, name);

        if (has_uuid && try_reconnect_player(peer, player_uuid, reconnect_token)) {
            MP_LOG_INFO("HANDSHAKE", "Reconnect succeeded for '%s'", name);
            return; /* Reconnect succeeded */
        }

        /* Phase 6: Try late join if reserved slots available */
        {
            if (mp_worldgen_get_reserved_count() > 0) {
                MP_LOG_INFO("HANDSHAKE", "Attempting late join for '%s' — %d reserved slots available",
                            name, mp_worldgen_get_reserved_count());
                int peer_index = -1;
                for (int i = 0; i < NET_MAX_PEERS; i++) {
                    if (&session.peers[i] == peer) {
                        peer_index = i;
                        break;
                    }
                }
                if (peer_index >= 0) {
                    uint8_t reject_reason = NET_REJECT_NO_RESERVED_SLOTS;
                    if (mp_bootstrap_host_handle_late_join((uint8_t)peer_index, name,
                            &reject_reason)) {
                        return;
                    }
                    net_serializer_init(&rs, reject_buf, sizeof(reject_buf));
                    net_write_u8(&rs, reject_reason);
                    send_raw_to_peer(peer, NET_MSG_JOIN_REJECT, reject_buf,
                                     (uint32_t)net_serializer_position(&rs));
                    return;
                }
            }
        }

        /* No reconnect or late join possible — reject */
        MP_LOG_WARN("HANDSHAKE", "REJECT: game in progress, no valid UUID for reconnect (name='%s')", name);
        uint8_t reject_buf[2];
        net_serializer rs;
        net_serializer_init(&rs, reject_buf, sizeof(reject_buf));
        net_write_u8(&rs, NET_REJECT_GAME_IN_PROGRESS);
        send_raw_to_peer(peer, NET_MSG_JOIN_REJECT, reject_buf,
                         (uint32_t)net_serializer_position(&rs));
        return;
    }

    /* Check for duplicate name */
    for (int i = 0; i < NET_MAX_PEERS; i++) {
        if (session.peers[i].active && session.peers[i].player_id != peer->player_id) {
            if (strncmp(session.peers[i].name, name, NET_MAX_PLAYER_NAME - 1) == 0) {
                uint8_t reject_buf[2];
                net_serializer rs;
                net_serializer_init(&rs, reject_buf, sizeof(reject_buf));
                net_write_u8(&rs, NET_REJECT_NAME_TAKEN);
                send_raw_to_peer(peer, NET_MSG_JOIN_REJECT, reject_buf,
                                 (uint32_t)net_serializer_position(&rs));
                return;
            }
        }
    }
    /* Also check against host name */
    if (strncmp(session.local_player_name, name, NET_MAX_PLAYER_NAME - 1) == 0) {
        MP_LOG_WARN("HANDSHAKE", "REJECT: name '%s' conflicts with host name", name);
        uint8_t reject_buf[2];
        net_serializer rs;
        net_serializer_init(&rs, reject_buf, sizeof(reject_buf));
        net_write_u8(&rs, NET_REJECT_NAME_TAKEN);
        send_raw_to_peer(peer, NET_MSG_JOIN_REJECT, reject_buf,
                         (uint32_t)net_serializer_position(&rs));
        return;
    }

    /* Assign player ID */
    uint8_t new_player_id = 0;
    for (int i = 0; i < NET_MAX_PEERS; i++) {
        if (session.peers[i].active && session.peers[i].player_id >= new_player_id) {
            new_player_id = session.peers[i].player_id + 1;
        }
    }
    /* Player 0 is host */
    if (new_player_id == 0) {
        new_player_id = 1;
    }

    strncpy(peer->name, name, NET_MAX_PLAYER_NAME - 1);
    peer->name[NET_MAX_PLAYER_NAME - 1] = '\0';
    net_peer_set_player_id(peer, new_player_id);
    peer->state = PEER_STATE_JOINED;

    uint32_t now = net_tcp_get_timestamp_ms();
    peer->last_heartbeat_recv_ms = now;

    /* Register in player registry */
    mp_player_registry_add(new_player_id, name, 0, 0);
    mp_player_registry_set_status(new_player_id, MP_PLAYER_LOBBY);
    mp_player_registry_set_connection_state(new_player_id, MP_CONNECTION_CONNECTED);
    int slot = mp_player_registry_assign_slot(new_player_id);

    log_info("Player joined", name, (int)new_player_id);

    /* Build extended JOIN_ACCEPT */
    mp_player *new_player = mp_player_registry_get(new_player_id);
    uint32_t session_seed = mp_worldgen_get_spawn_table_mutable()->session_seed;

    uint8_t accept_buf[128];
    net_serializer as;
    net_serializer_init(&as, accept_buf, sizeof(accept_buf));
    net_write_u8(&as, new_player_id);
    net_write_u8(&as, (uint8_t)slot);
    net_write_u32(&as, session.session_id);
    net_write_u32(&as, session_seed);
    net_write_u8(&as, (uint8_t)(session.peer_count + 1)); /* +1 includes host */
    if (new_player) {
        net_write_raw(&as, new_player->player_uuid, MP_PLAYER_UUID_SIZE);
        net_write_raw(&as, new_player->reconnect_token, MP_RECONNECT_TOKEN_SIZE);
    } else {
        uint8_t zeros[16] = {0};
        net_write_raw(&as, zeros, 16);
        net_write_raw(&as, zeros, 16);
    }
    send_raw_to_peer(peer, NET_MSG_JOIN_ACCEPT, accept_buf,
                     (uint32_t)net_serializer_position(&as));

    MP_LOG_INFO("HANDSHAKE", "JOIN_ACCEPT sent: player_id=%d slot=%d session=0x%08x seed=%u",
                (int)new_player_id, slot, session.session_id, session_seed);

    broadcast_lobby_snapshot();
#endif
}

static void handle_chat_from_client(net_peer *peer, const uint8_t *payload, uint32_t size)
{
    /* Host received a chat message from client — relay to all */
    uint8_t relay_buf[256];
    net_serializer s;
    net_serializer_init(&s, relay_buf, sizeof(relay_buf));

    /* Override sender_id with peer's verified player_id */
    net_write_u8(&s, peer->player_id);

    /* Read original message */
    net_serializer rs;
    net_serializer_init(&rs, (uint8_t *)payload, size);
    net_read_u8(&rs); /* skip client-provided sender_id */

    char message[MP_CHAT_MESSAGE_MAX];
    net_read_string(&rs, message, sizeof(message));
    net_write_string(&s, message, sizeof(message));

    if (net_session_is_in_lobby()) {
        net_session_broadcast_lobby(NET_MSG_CHAT, relay_buf,
                                    (uint32_t)net_serializer_position(&s));
    } else {
        net_session_broadcast_in_game(NET_MSG_CHAT, relay_buf,
                                      (uint32_t)net_serializer_position(&s));
    }

    /* Also store in host's chat history */
    mp_chat_entry *entry = &chat_history.entries[chat_history.write_index];
    entry->sender_id = peer->player_id;
    copy_text(entry->message, sizeof(entry->message), message);
    entry->timestamp_tick = session.authoritative_tick;
    chat_history.write_index = (chat_history.write_index + 1) % MP_CHAT_HISTORY_SIZE;
    if (chat_history.count < MP_CHAT_HISTORY_SIZE) {
        chat_history.count++;
    }
    chat_history.unread++;
}

static void handle_client_message(net_peer *peer, const net_packet_header *header,
                                  const uint8_t *payload, uint32_t size)
{
    /* Validate payload size */
    if (size > NET_MAX_PAYLOAD_SIZE) {
        MP_LOG_ERROR("NET", "Payload too large from peer '%s': %u bytes (max %u)",
                     peer->name, size, NET_MAX_PAYLOAD_SIZE);
        return;
    }
    if (size != header->payload_size) {
        MP_LOG_ERROR("NET", "Payload size mismatch from peer '%s': header says %u, got %u",
                     peer->name, header->payload_size, size);
        return;
    }

    MP_LOG_TRACE("NET", "Message from peer '%s': type=%s(%d) size=%u seq=%u",
                 peer->name, net_protocol_message_name(header->message_type),
                 (int)header->message_type, size, header->sequence_id);

    switch (header->message_type) {
        case NET_MSG_HELLO:
            handle_hello(peer, payload, size);
            break;
        case NET_MSG_READY_STATE: {
            if (size >= 2) {
                net_serializer s;
                net_serializer_init(&s, (uint8_t *)payload, size);
                net_read_u8(&s); /* player_id - use peer's known id instead */
                uint8_t ready = net_read_u8(&s);
                peer->state = ready ? PEER_STATE_READY : PEER_STATE_JOINED;
                mp_player_registry_set_status(peer->player_id,
                    ready ? MP_PLAYER_READY : MP_PLAYER_LOBBY);
                log_info("Player ready state changed", peer->name, ready);
                broadcast_lobby_snapshot();
            }
            break;
        }
        case NET_MSG_CLIENT_COMMAND: {
            multiplayer_command_bus_receive(peer->player_id, payload, size);
            break;
        }
        case NET_MSG_CHAT: {
            handle_chat_from_client(peer, payload, size);
            break;
        }
        case NET_MSG_HEARTBEAT: {
            net_msg_heartbeat heartbeat;
            uint32_t now = net_tcp_get_timestamp_ms();
            if (!parse_heartbeat_payload(payload, size, &heartbeat)) {
                log_error("Malformed heartbeat from client", peer->name, (int)size);
                break;
            }
            if (heartbeat.flags & NET_HEARTBEAT_FLAG_RESPONSE) {
                net_peer_update_heartbeat_response(peer, now, heartbeat.sample_id);
            } else {
                net_peer_note_heartbeat_recv(peer, now);
                send_heartbeat_to_peer(peer, heartbeat.timestamp_ms, heartbeat.sample_id,
                                       NET_HEARTBEAT_FLAG_RESPONSE);
            }
            net_peer_update_quality(peer, now);
            break;
        }
        case NET_MSG_CHECKSUM_RESPONSE: {
            multiplayer_checksum_receive_response(peer->player_id, payload, size);
            break;
        }
        case NET_MSG_RESYNC_REQUEST: {
            multiplayer_resync_handle_request(peer->player_id);
            mp_player *p = mp_player_registry_get(peer->player_id);
            if (p) {
                p->resyncs_requested++;
            }
            break;
        }
        case NET_MSG_DISCONNECT_NOTICE: {
            log_info("Peer disconnecting gracefully", peer->name, 0);
            for (int i = 0; i < NET_MAX_PEERS; i++) {
                if (&session.peers[i] == peer) {
                    handle_peer_disconnect(i);
                    break;
                }
            }
            break;
        }
        case NET_MSG_GAME_LOAD_COMPLETE: {
            /* Client reports scenario loaded — track for loading barrier */
            MP_LOG_INFO("BOOT", "Peer '%s' (player %d) reports scenario loaded",
                        peer->name, (int)peer->player_id);

            /* If this peer was in LOADING state (join barrier active),
             * transition to IN_GAME and release the barrier */
            if (peer->state == PEER_STATE_LOADING) {
                peer->state = PEER_STATE_IN_GAME;
                mp_player_registry_set_status(peer->player_id, MP_PLAYER_IN_GAME);

                /* Commit the join transaction */
                mp_join_transaction *txn = mp_join_transaction_find_by_peer(
                    (uint8_t)(peer - session.peers));
                if (txn && txn->active) {
                    mp_join_transaction_commit(txn);
                    broadcast_full_snapshot_to_in_game_peers();

                    if (mp_time_sync_is_join_barrier_active()) {
                        uint8_t event_buf[16];
                        net_serializer es;
                        mp_time_sync_set_join_barrier(0);
                        net_serializer_init(&es, event_buf, sizeof(event_buf));
                        net_write_u16(&es, NET_EVENT_JOIN_BARRIER_RELEASED);
                        net_write_u32(&es, session.authoritative_tick);
                        net_write_u8(&es, peer->player_id);
                        net_session_broadcast_in_game(NET_MSG_HOST_EVENT, event_buf,
                                                      (uint32_t)net_serializer_position(&es));
                    }

                    mp_bootstrap_host_complete_late_join((uint8_t)(peer - session.peers));
                } else {
                    mp_bootstrap_host_complete_reconnect((uint8_t)(peer - session.peers));
                }

                MP_LOG_INFO("BOOT", "Join barrier released: player %d fully loaded",
                            (int)peer->player_id);
            }
            break;
        }
        default:
            if (header->message_type >= NET_MSG_COUNT) {
                log_error("Invalid message type from peer", peer->name,
                         header->message_type);
            } else {
                log_error("Unhandled message from peer",
                         net_protocol_message_name(header->message_type),
                         header->message_type);
            }
            break;
    }
}

static void handle_chat_from_host(const uint8_t *payload, uint32_t size)
{
    net_serializer s;
    net_serializer_init(&s, (uint8_t *)payload, size);

    uint8_t sender_id = net_read_u8(&s);
    char message[MP_CHAT_MESSAGE_MAX];
    net_read_string(&s, message, sizeof(message));

    if (net_serializer_has_overflow(&s)) {
        return;
    }

    /* Store in chat history */
    mp_chat_entry *entry = &chat_history.entries[chat_history.write_index];
    entry->sender_id = sender_id;
    copy_text(entry->message, sizeof(entry->message), message);
    entry->timestamp_tick = session.authoritative_tick;
    chat_history.write_index = (chat_history.write_index + 1) % MP_CHAT_HISTORY_SIZE;
    if (chat_history.count < MP_CHAT_HISTORY_SIZE) {
        chat_history.count++;
    }
    chat_history.unread++;
}

static void handle_host_message(const net_packet_header *header,
                                const uint8_t *payload, uint32_t size)
{
    /* Validate payload size */
    if (size > NET_MAX_PAYLOAD_SIZE) {
        MP_LOG_ERROR("NET", "Payload too large from host: %u bytes (max %u)",
                     size, NET_MAX_PAYLOAD_SIZE);
        return;
    }
    if (size != header->payload_size) {
        MP_LOG_ERROR("NET", "Payload size mismatch from host: header says %u, got %u",
                     header->payload_size, size);
        return;
    }

    /* TIME SYNC: Every packet header carries the host's authoritative tick.
     * Update the client's confirmed tick so the simulation can advance.
     * This is the core mechanism for client tick synchronization. */
    if (session.state == NET_SESSION_CLIENT_GAME && header->game_tick > 0) {
        mp_time_sync_set_confirmed_tick(header->game_tick);
        session.authoritative_tick = header->game_tick;
    }

    MP_LOG_TRACE("NET", "Message from host: type=%s(%d) size=%u seq=%u tick=%u",
                 net_protocol_message_name(header->message_type),
                 (int)header->message_type, size, header->sequence_id,
                 header->game_tick);

    switch (header->message_type) {
        case NET_MSG_JOIN_ACCEPT: {
            net_serializer s;
            uint8_t accept_world_uuid[MP_WORLD_UUID_SIZE];
            uint32_t accept_resume_generation = 0;
            net_serializer_init(&s, (uint8_t *)payload, size);
            session.local_player_id = net_read_u8(&s);
            uint8_t slot_id = net_read_u8(&s);
            session.session_id = net_read_u32(&s);
            uint32_t session_seed = net_read_u32(&s);
            uint8_t player_count = net_read_u8(&s);
            uint8_t assigned_uuid[16];
            uint8_t assigned_token[16];
            net_read_raw(&s, assigned_uuid, 16);
            net_read_raw(&s, assigned_token, 16);
            memset(accept_world_uuid, 0, sizeof(accept_world_uuid));
            if (net_serializer_remaining(&s) >= MP_WORLD_UUID_SIZE) {
                net_read_raw(&s, accept_world_uuid, MP_WORLD_UUID_SIZE);
            }
            if (net_serializer_remaining(&s) >= sizeof(uint32_t)) {
                accept_resume_generation = net_read_u32(&s);
            }
            if (net_serializer_has_overflow(&s)) {
                MP_LOG_ERROR("HANDSHAKE", "[join:%04x] Malformed JOIN_ACCEPT payload",
                             join_correlation_id);
                join_status = NET_JOIN_STATUS_FAILED;
                session.state = NET_SESSION_DISCONNECTING;
                break;
            }

            session.host_peer.state = PEER_STATE_JOINED;
            session.state = NET_SESSION_CLIENT_LOBBY;
            join_status = NET_JOIN_STATUS_CONNECTED;

            MP_LOG_INFO("HANDSHAKE", "[join:%04x] JOIN_ACCEPT received: player_id=%d slot=%d "
                        "session=0x%08x seed=%u player_count=%d",
                        join_correlation_id, (int)session.local_player_id, (int)slot_id,
                        session.session_id, session_seed, (int)player_count);

            mp_player_registry_clear();
            mp_player_registry_add_with_uuid(session.local_player_id,
                session.local_player_name, assigned_uuid, 1, 0);
            mp_player *self = mp_player_registry_get(session.local_player_id);
            if (self) {
                self->slot_id = slot_id;
                memcpy(self->reconnect_token, assigned_token, MP_RECONNECT_TOKEN_SIZE);
                self->status = MP_PLAYER_LOBBY;
                self->connection_state = MP_CONNECTION_CONNECTED;
            }

            /* Store session seed for worldgen */
            mp_spawn_table *table = mp_worldgen_get_spawn_table_mutable();
            table->session_seed = session_seed;

            /* Persist identity to disk for reconnect after app restart */
            {
                mp_client_identity_set(assigned_uuid, assigned_token,
                                        slot_id,
                                        accept_world_uuid,
                                        NULL, 0, /* host address/port not available here */
                                        session.local_player_name,
                                        session.session_id,
                                        accept_resume_generation);
                mp_client_identity_save();
            }

            log_info("Joined session as player", 0, session.local_player_id);
            (void)player_count;
            break;
        }
        case NET_MSG_LOBBY_SNAPSHOT: {
            apply_lobby_snapshot(payload, size);
            break;
        }
        case NET_MSG_JOIN_REJECT: {
            if (size >= 1) {
                net_serializer s;
                net_serializer_init(&s, (uint8_t *)payload, size);
                uint8_t reason = net_read_u8(&s);
                const char *reason_str = "unknown";
                switch (reason) {
                    case NET_REJECT_VERSION_MISMATCH: reason_str = "VERSION_MISMATCH"; break;
                    case NET_REJECT_SESSION_FULL: reason_str = "SESSION_FULL"; break;
                    case NET_REJECT_GAME_IN_PROGRESS: reason_str = "GAME_IN_PROGRESS"; break;
                    case NET_REJECT_NAME_TAKEN: reason_str = "NAME_TAKEN"; break;
                    case NET_REJECT_BANNED: reason_str = "BANNED"; break;
                    case NET_REJECT_NO_RESERVED_SLOTS: reason_str = "NO_RESERVED_SLOTS"; break;
                    case NET_REJECT_LATE_JOIN_BUSY: reason_str = "LATE_JOIN_BUSY"; break;
                    case NET_REJECT_RECONNECT_REQUIRED: reason_str = "RECONNECT_REQUIRED"; break;
                    case NET_REJECT_SLOT_NOT_FOUND: reason_str = "SLOT_NOT_FOUND"; break;
                    case NET_REJECT_WORLD_MISMATCH: reason_str = "WORLD_MISMATCH"; break;
                    case NET_REJECT_RESUME_GENERATION_MISMATCH:
                        reason_str = "RESUME_GENERATION_MISMATCH";
                        break;
                    case NET_REJECT_RATE_LIMITED: reason_str = "RATE_LIMITED"; break;
                    case NET_REJECT_INTERNAL_ERROR: reason_str = "INTERNAL_ERROR"; break;
                }
                MP_LOG_ERROR("HANDSHAKE", "[join:%04x] JOIN_REJECT received: reason=%s (%d)",
                             join_correlation_id, reason_str, (int)reason);
                log_error("Join rejected", reason_str, reason);
                join_reject_reason = reason;
                if (reason == NET_REJECT_WORLD_MISMATCH ||
                    reason == NET_REJECT_RESUME_GENERATION_MISMATCH) {
                    mp_client_identity_clear();
                }
            }
            join_status = NET_JOIN_STATUS_REJECTED;
            session.state = NET_SESSION_DISCONNECTING;
            break;
        }
        case NET_MSG_START_GAME: {
            /* DEPRECATED: kept for backward compatibility with older hosts.
             * New flow uses GAME_START_FINAL for session state transition. */
            MP_LOG_WARN("NET", "Received deprecated NET_MSG_START_GAME — "
                        "host may be running an older version");
            net_serializer s;
            net_serializer_init(&s, (uint8_t *)payload, size);
            session.authoritative_tick = net_read_u32(&s);
            session.game_speed = net_read_u8(&s);
            session.state = NET_SESSION_CLIENT_GAME;
            session.host_peer.state = PEER_STATE_IN_GAME;

            /* Update local player status */
            mp_player_registry_set_status(session.local_player_id, MP_PLAYER_IN_GAME);

            log_info("Game started by host (legacy)", 0, 0);
            break;
        }
        case NET_MSG_HOST_COMMAND_ACK: {
            multiplayer_command_bus_receive_ack(payload, size);
            break;
        }
        case NET_MSG_FULL_SNAPSHOT: {
            multiplayer_snapshot_receive_full(payload, size);
            break;
        }
        case NET_MSG_DELTA_SNAPSHOT: {
            multiplayer_snapshot_receive_delta(payload, size);
            break;
        }
        case NET_MSG_HOST_EVENT: {
            multiplayer_empire_sync_receive_event(payload, size);
            break;
        }
        case NET_MSG_CHECKSUM_REQUEST: {
            multiplayer_checksum_handle_request(payload, size);
            break;
        }
        case NET_MSG_RESYNC_GRANTED: {
            multiplayer_resync_apply_full_snapshot(payload, size);
            break;
        }
        case NET_MSG_CHAT: {
            handle_chat_from_host(payload, size);
            break;
        }
        case NET_MSG_HEARTBEAT: {
            net_msg_heartbeat heartbeat;
            uint32_t now = net_tcp_get_timestamp_ms();
            if (!parse_heartbeat_payload(payload, size, &heartbeat)) {
                log_error("Malformed heartbeat from host", 0, (int)size);
                break;
            }
            if (heartbeat.flags & NET_HEARTBEAT_FLAG_RESPONSE) {
                net_peer_update_heartbeat_response(&session.host_peer, now,
                                                   heartbeat.sample_id);
            } else {
                net_peer_note_heartbeat_recv(&session.host_peer, now);
                send_heartbeat_to_host(heartbeat.timestamp_ms, heartbeat.sample_id,
                                       NET_HEARTBEAT_FLAG_RESPONSE);
            }
            net_peer_update_quality(&session.host_peer, now);
            break;
        }
        case NET_MSG_DISCONNECT_NOTICE: {
            log_info("Host disconnected", 0, 0);
            session.state = NET_SESSION_DISCONNECTING;
            break;
        }
        case NET_MSG_GAME_PREPARE: {
            if (mp_bootstrap_client_prepare(payload, size)) {
                if (mp_bootstrap_get_state() != MP_BOOT_SAVE_TRANSFER) {
                    uint8_t ack_buf[1];
                    net_serializer ls;
                    net_serializer_init(&ls, ack_buf, sizeof(ack_buf));
                    net_write_u8(&ls, session.local_player_id);
                    net_session_send_to_host(NET_MSG_GAME_LOAD_COMPLETE,
                                             ack_buf, (uint32_t)net_serializer_position(&ls));
                }
            } else {
                MP_LOG_ERROR("BOOT", "Failed to prepare game — disconnecting");
                session.state = NET_SESSION_DISCONNECTING;
            }
            break;
        }
        case NET_MSG_GAME_START_FINAL: {
            /* Transition client session state (replaces START_GAME role) */
            if (session.state != NET_SESSION_CLIENT_GAME) {
                session.state = NET_SESSION_CLIENT_GAME;
                session.host_peer.state = PEER_STATE_IN_GAME;
                mp_player_registry_set_status(session.local_player_id, MP_PLAYER_IN_GAME);
            }
            /* Parse start tick and speed from payload */
            if (size >= 5) {
                net_serializer s;
                net_serializer_init(&s, (uint8_t *)payload, size);
                session.authoritative_tick = net_read_u32(&s);
                session.game_speed = net_read_u8(&s);
            }
            mp_bootstrap_client_enter_game();
            break;
        }
        case NET_MSG_SAVE_TRANSFER_BEGIN: {
            mp_save_transfer_client_receive_begin(payload, size);
            break;
        }
        case NET_MSG_SAVE_TRANSFER_CHUNK: {
            mp_save_transfer_client_receive_chunk(payload, size);
            break;
        }
        case NET_MSG_SAVE_TRANSFER_COMPLETE: {
            mp_save_transfer_client_receive_complete(payload, size);
            /* Notify bootstrap that the save transfer is done */
            if (mp_save_transfer_get_state() == MP_TRANSFER_COMPLETE) {
                mp_bootstrap_client_save_transfer_complete();
                if (mp_bootstrap_get_state() == MP_BOOT_LOADED) {
                    uint8_t ack_buf[1];
                    net_serializer ls;
                    net_serializer_init(&ls, ack_buf, sizeof(ack_buf));
                    net_write_u8(&ls, session.local_player_id);
                    net_session_send_to_host(NET_MSG_GAME_LOAD_COMPLETE,
                                             ack_buf, (uint32_t)net_serializer_position(&ls));
                }
            }
            break;
        }
        default:
            if (header->message_type >= NET_MSG_COUNT) {
                log_error("Invalid message type from host", 0, header->message_type);
            } else {
                log_error("Unhandled message from host",
                         net_protocol_message_name(header->message_type),
                         header->message_type);
            }
            break;
    }
}

static void host_accept_connections(void)
{
    if (session.listen_fd < 0) {
        return;
    }

    int client_fd = net_tcp_accept(session.listen_fd);
    char remote_address[48] = {0};
    if (client_fd < 0) {
        return;
    }

    net_tcp_get_peer_address(client_fd, remote_address, sizeof(remote_address));

    if (handshake_count_pending_peers_for_address(remote_address) >=
        (int)handshake_pending_per_ip_limit()) {
        net_peer temp_peer;
        net_peer_init(&temp_peer);
        net_peer_set_connected(&temp_peer, client_fd, "Pending");
        net_peer_set_remote_address(&temp_peer, remote_address);
        reject_peer(&temp_peer, NET_REJECT_RATE_LIMITED, "accept_pending_limit");
        return;
    }

    int slot = find_free_peer_slot();
    if (slot < 0) {
        net_peer temp_peer;
        net_peer_init(&temp_peer);
        net_peer_set_connected(&temp_peer, client_fd, "Pending");
        net_peer_set_remote_address(&temp_peer, remote_address);
        log_error("Session full, rejecting connection", 0, 0);
        reject_peer(&temp_peer, NET_REJECT_SESSION_FULL, "accept_connection_full");
        return;
    }

    net_peer_init(&session.peers[slot]);
    net_peer_set_connected(&session.peers[slot], client_fd, "");
    net_peer_set_remote_address(&session.peers[slot], remote_address);
    session.peer_count++;

    log_info("New connection accepted in slot", 0, slot);
    MP_LOG_INFO("SESSION", "New TCP connection accepted: slot=%d fd=%d addr='%s' total_peers=%d",
                slot, client_fd,
                remote_address[0] ? remote_address : "<unknown>",
                session.peer_count);
}

static void host_process_peers(void)
{
    uint32_t now = net_tcp_get_timestamp_ms();
    uint8_t recv_buf[4096];

    for (int i = 0; i < NET_MAX_PEERS; i++) {
        net_peer *peer = &session.peers[i];
        if (!peer->active) {
            continue;
        }

        /* Check timeout */
        if (peer->state != PEER_STATE_CONNECTING && net_peer_is_timed_out(peer, now)) {
            log_error("Peer timed out", peer->name, i);
            handle_peer_disconnect(i);
            continue;
        }

        /* Receive data */
        int received = net_tcp_recv(peer->socket_fd, recv_buf, sizeof(recv_buf));
        if (received < 0) {
            log_error("Peer connection lost", peer->name, i);
            handle_peer_disconnect(i);
            continue;
        }
        if (received > 0) {
            peer->bytes_received += received;
            net_codec_feed(&peer->codec, recv_buf, (size_t)received);

            /* Process all complete packets */
            net_packet_header header;
            const uint8_t *payload;
            uint32_t payload_size;

            while (net_codec_decode(&peer->codec, &header, &payload, &payload_size) == CODEC_OK) {
                peer->packets_received++;
                handle_client_message(peer, &header, payload, payload_size);
            }
        }

        /* Send heartbeats periodically */
        if (now - peer->last_heartbeat_sent_ms > NET_HEARTBEAT_INTERVAL_MS) {
            uint32_t sample_id = next_heartbeat_sample_id();
            send_heartbeat_to_peer(peer, now, sample_id, 0);
            net_peer_update_heartbeat_sent(peer, now, sample_id);
        }
        net_peer_update_quality(peer, now);
    }

    /* Periodic cleanup of expired reconnect slots */
    if (session.state == NET_SESSION_HOSTING_GAME) {
        mp_player_registry_cleanup_expired(session.authoritative_tick);
    }
}

static void client_process_host(void)
{
    net_peer *peer = &session.host_peer;
    if (!peer->active) {
        return;
    }

    uint32_t now = net_tcp_get_timestamp_ms();
    uint8_t recv_buf[4096];

    /* Check handshake timeout while joining */
    if (session.state == NET_SESSION_JOINING &&
        join_start_ms > 0 && (now - join_start_ms) > NET_JOIN_TIMEOUT_MS) {
        MP_LOG_ERROR("HANDSHAKE", "[join:%04x] Handshake timed out after %u ms",
                     join_correlation_id, now - join_start_ms);
        log_error("Join handshake timed out", 0, 0);
        join_status = NET_JOIN_STATUS_TIMEOUT;
        session.state = NET_SESSION_DISCONNECTING;
        return;
    }

    /* Check timeout for established connections */
    if (peer->state != PEER_STATE_CONNECTING &&
        peer->state != PEER_STATE_HELLO_SENT &&
        net_peer_is_timed_out(peer, now)) {
        log_error("Host connection timed out", 0, 0);
        session.state = NET_SESSION_DISCONNECTING;
        return;
    }

    /* Receive data */
    int received = net_tcp_recv(peer->socket_fd, recv_buf, sizeof(recv_buf));
    if (received < 0) {
        MP_LOG_ERROR("NET", "Host connection lost (recv returned %d)", received);
        log_error("Host connection lost", 0, 0);
        if (session.state == NET_SESSION_JOINING) {
            join_status = NET_JOIN_STATUS_FAILED;
        }
        session.state = NET_SESSION_DISCONNECTING;
        return;
    }
    if (received > 0) {
        peer->bytes_received += received;
        net_codec_feed(&peer->codec, recv_buf, (size_t)received);

        net_packet_header header;
        const uint8_t *payload;
        uint32_t payload_size;

        while (net_codec_decode(&peer->codec, &header, &payload, &payload_size) == CODEC_OK) {
            peer->packets_received++;
            handle_host_message(&header, payload, payload_size);
        }
    }

    /* Send heartbeats */
    if (now - peer->last_heartbeat_sent_ms > NET_HEARTBEAT_INTERVAL_MS) {
        uint32_t sample_id = next_heartbeat_sample_id();
        send_heartbeat_to_host(now, sample_id, 0);
        net_peer_update_heartbeat_sent(peer, now, sample_id);
    }
    net_peer_update_quality(peer, now);
}

/* ---- Public API ---- */

int net_session_init(void)
{
    memset(&session, 0, sizeof(net_session));
    memset(&chat_history, 0, sizeof(chat_history));
    mp_player_registry_clear();
    mp_client_identity_init();
    join_status = NET_JOIN_STATUS_NONE;
    join_reject_reason = 0;
    join_start_ms = 0;
    heartbeat_sample_counter = 0;
    reset_handshake_ip_state();
    session.listen_fd = -1;
    session.udp_fd = -1;
    session.local_player_id = NET_PLAYER_ID_NONE;

    for (int i = 0; i < NET_MAX_PEERS; i++) {
        net_peer_init(&session.peers[i]);
    }
    net_peer_init(&session.host_peer);

    if (!net_tcp_init()) {
        return 0;
    }
    net_udp_init();
    net_discovery_init();

    mp_debug_log_init();

    /* Load persistent player name from config.
     * If the user has never set a name, the default "Player" is used. */
    if (!persistent_name_set) {
        const char *saved_name = config_get_string(CONFIG_STRING_MP_PLAYER_NAME);
        if (saved_name && saved_name[0]) {
            copy_text(persistent_player_name, sizeof(persistent_player_name), saved_name);
        }
        persistent_name_set = 1;
    }

    log_info("Network session system initialized", 0, 0);
    MP_LOG_INFO("SESSION", "Network session initialized, player_name='%s'", persistent_player_name);
    return 1;
}

void net_session_shutdown(void)
{
    MP_LOG_INFO("SESSION", "Session shutdown");
    net_session_disconnect();
    net_discovery_shutdown();
    net_tcp_shutdown();
    net_udp_shutdown();
    mp_debug_log_shutdown();
}

int net_session_host(const char *player_name, uint16_t port)
{
    net_discovery_announcement announcement;
    const mp_dedicated_server_options *dedicated_options = mp_dedicated_server_get_options();

    if (session.state != NET_SESSION_IDLE) {
        log_error("Cannot host: session not idle", 0, 0);
        return 0;
    }

    session.listen_fd = net_tcp_listen_on(
        mp_dedicated_server_is_enabled() ? mp_dedicated_server_get_listen_address() : NULL,
        port);
    if (session.listen_fd < 0) {
        return 0;
    }

    session.udp_fd = net_udp_create(port);
    if (session.udp_fd >= 0) {
        net_udp_enable_broadcast(session.udp_fd);
    }

    session.port = port;
    session.role = NET_ROLE_HOST;
    session.state = NET_SESSION_HOSTING_LOBBY;
    session.session_id = generate_session_id();
    session.local_player_id = session_host_uses_player_slot() ? 0 : NET_PLAYER_ID_NONE;

    copy_text(session.local_player_name, sizeof(session.local_player_name), player_name);

    /* Register host in player registry only when the host is also a playable participant. */
    mp_player_registry_init();
    if (session_host_uses_player_slot()) {
        mp_player_registry_add(0, player_name, 1, 1);
        mp_player_registry_set_status(0, MP_PLAYER_LOBBY);
        mp_player_registry_set_connection_state(0, MP_CONNECTION_CONNECTED);
        mp_player_registry_assign_slot(0);
    }

    /* Start LAN discovery broadcasting so clients can find this host */
    if (!mp_dedicated_server_is_enabled() || !dedicated_options || dedicated_options->advertise_lan) {
        build_discovery_announcement(&announcement);
        net_discovery_start_announcing(&announcement);
    }

    log_info("Hosting session", player_name, (int)port);
    MP_LOG_INFO("SESSION", "Hosting session: name='%s' port=%d session_id=0x%08x",
                player_name, (int)port, session.session_id);
    return 1;
}

void net_session_kick_peer(uint8_t player_id)
{
    for (int i = 0; i < NET_MAX_PEERS; i++) {
        if (session.peers[i].active && session.peers[i].player_id == player_id) {
            /* Send disconnect notice */
            uint8_t buf[2];
            net_serializer s;
            net_serializer_init(&s, buf, sizeof(buf));
            net_write_u8(&s, player_id);
            net_write_u8(&s, 0); /* reason: kicked */
            send_raw_to_peer(&session.peers[i], NET_MSG_DISCONNECT_NOTICE,
                           buf, (uint32_t)net_serializer_position(&s));

            /* Remove from registry (kicked players don't get reconnect) */
            mp_player_registry_remove(player_id);

            close_peer(i);
            if (session.state == NET_SESSION_HOSTING_LOBBY) {
                broadcast_lobby_snapshot();
            }
            break;
        }
    }
}

int net_session_join(const char *player_name, const char *host_address, uint16_t port)
{
    if (session.state != NET_SESSION_IDLE) {
        log_error("Cannot join: session not idle", 0, 0);
        return 0;
    }

    int fd = net_tcp_connect(host_address, port);
    if (fd < 0) {
        return 0;
    }

    session.role = NET_ROLE_CLIENT;
    session.state = NET_SESSION_JOINING;
    mp_player_registry_clear();

    copy_text(session.local_player_name, sizeof(session.local_player_name), player_name);

    net_peer_init(&session.host_peer);
    net_peer_set_connected(&session.host_peer, fd, "Host");
    session.host_peer.state = PEER_STATE_HELLO_SENT;
    session.host_peer.last_heartbeat_recv_ms = net_tcp_get_timestamp_ms();

    /* Track join status for UI feedback */
    join_status = NET_JOIN_STATUS_CONNECTING;
    join_reject_reason = 0;
    join_start_ms = net_tcp_get_timestamp_ms();
    join_correlation_id = (uint32_t)(join_start_ms ^ (uint32_t)(size_t)&session) & 0xFFFF;

    /* Send extended HELLO with UUID (if reconnecting) and protocol info.
     * Use a zeroed buffer for the player name to avoid reading past
     * the source string if it's shorter than NET_MAX_PLAYER_NAME. */
    uint8_t hello_buf[128];
    net_serializer s;
    net_serializer_init(&s, hello_buf, sizeof(hello_buf));
    net_write_u32(&s, NET_MAGIC);
    net_write_u16(&s, NET_PROTOCOL_VERSION);
    net_write_u32(&s, 0); /* save_version: filled by caller if needed */
    net_write_u32(&s, 0); /* map_hash */
    net_write_u32(&s, 0); /* scenario_hash */
    net_write_u32(&s, 0); /* feature_flags */

    /* Fixed-size name field: zero-padded to NET_MAX_PLAYER_NAME bytes */
    char name_buf[NET_MAX_PLAYER_NAME];
    memset(name_buf, 0, sizeof(name_buf));
    copy_text(name_buf, sizeof(name_buf), player_name);
    net_write_raw(&s, name_buf, NET_MAX_PLAYER_NAME);

    /* Try to load persisted identity for reconnect (survives app restart).
     * Falls back to in-memory player registry, then zeros. */
    {
        uint8_t hello_uuid[16] = {0};
        uint8_t hello_token[16] = {0};
        uint8_t hello_world_uuid[MP_WORLD_UUID_SIZE] = {0};
        uint8_t hello_slot_id = 0xFF;
        uint32_t hello_resume_generation = 0;
        int have_identity = 0;
        const mp_client_identity *identity = NULL;

        /* Priority 1: persisted identity file */
        if (mp_client_identity_load()) {
            have_identity = mp_client_identity_get_for_hello(hello_uuid, hello_token);
            identity = mp_client_identity_get();
            if (have_identity && identity) {
                hello_slot_id = identity->slot_id;
                memcpy(hello_world_uuid, identity->world_instance_uuid, MP_WORLD_UUID_SIZE);
                hello_resume_generation = identity->resume_generation;
            }
        }

        /* Priority 2: in-memory player registry (same session, no restart) */
        if (!have_identity) {
            mp_player *local = mp_player_registry_get_local();
            if (local && local->active) {
                memcpy(hello_uuid, local->player_uuid, 16);
                memcpy(hello_token, local->reconnect_token, 16);
                hello_slot_id = local->slot_id;
                copy_current_world_uuid(hello_world_uuid);
                hello_resume_generation = current_resume_generation();
                have_identity = 1;
            }
        }

        net_write_raw(&s, hello_uuid, 16);
        net_write_raw(&s, hello_token, 16);
        net_write_u8(&s, hello_slot_id);
        net_write_raw(&s, hello_world_uuid, MP_WORLD_UUID_SIZE);
        net_write_u32(&s, hello_resume_generation);
    }

    net_session_send_to_host(NET_MSG_HELLO, hello_buf, (uint32_t)net_serializer_position(&s));

    log_info("Joining session at", host_address, (int)port);
    MP_LOG_INFO("SESSION", "[join:%04x] Joining session: host='%s' port=%d name='%s' hello_size=%d",
                join_correlation_id, host_address, (int)port, player_name,
                (int)net_serializer_position(&s));
    MP_LOG_INFO("HANDSHAKE", "[join:%04x] HELLO sent to host (magic=0x%08x version=%d)",
                join_correlation_id, NET_MAGIC, NET_PROTOCOL_VERSION);
    return 1;
}

void net_session_disconnect(void)
{
    net_join_status preserved_join_status = NET_JOIN_STATUS_NONE;
    uint8_t preserved_reject_reason = 0;
    int clear_identity = 0;

    if (join_status == NET_JOIN_STATUS_REJECTED ||
        join_status == NET_JOIN_STATUS_TIMEOUT ||
        join_status == NET_JOIN_STATUS_FAILED) {
        preserved_join_status = join_status;
        preserved_reject_reason = join_reject_reason;
    }
    clear_identity = should_clear_persisted_identity(preserved_join_status,
                                                     preserved_reject_reason);

    if (session.state == NET_SESSION_IDLE) {
        return;
    }

    MP_LOG_INFO("SESSION", "Disconnecting: state=%s role=%s join_status=%d",
                net_session_state_name(session.state),
                session.role == NET_ROLE_HOST ? "HOST" : "CLIENT",
                (int)join_status);

    if (session.role == NET_ROLE_HOST) {
        /* Notify all peers */
        uint8_t buf[2];
        net_serializer s;
        net_serializer_init(&s, buf, sizeof(buf));
        net_write_u8(&s, session.local_player_id);
        net_write_u8(&s, 0);

        for (int i = 0; i < NET_MAX_PEERS; i++) {
            if (session.peers[i].active) {
                send_raw_to_peer(&session.peers[i], NET_MSG_DISCONNECT_NOTICE,
                               buf, (uint32_t)net_serializer_position(&s));
                close_peer(i);
            }
        }
        net_tcp_close(session.listen_fd);
        session.listen_fd = -1;
    } else if (session.role == NET_ROLE_CLIENT) {
        if (session.host_peer.active) {
            uint8_t buf[2];
            net_serializer s;
            net_serializer_init(&s, buf, sizeof(buf));
            net_write_u8(&s, session.local_player_id);
            net_write_u8(&s, 0);
            net_session_send_to_host(NET_MSG_DISCONNECT_NOTICE, buf,
                                    (uint32_t)net_serializer_position(&s));
            net_tcp_close(session.host_peer.socket_fd);
            net_peer_reset(&session.host_peer);
        }
    }

    /* Stop LAN discovery */
    net_discovery_stop_announcing();
    net_discovery_stop_listening();

    if (session.udp_fd >= 0) {
        net_udp_close(session.udp_fd);
        session.udp_fd = -1;
    }

    session.state = NET_SESSION_IDLE;
    session.role = NET_ROLE_NONE;
    session.peer_count = 0;
    session.local_player_id = NET_PLAYER_ID_NONE;
    join_status = preserved_join_status;
    join_reject_reason = preserved_reject_reason;
    join_start_ms = 0;
    reset_handshake_ip_state();
    memset(&chat_history, 0, sizeof(chat_history));
    mp_player_registry_clear();

    if (clear_identity) {
        mp_client_identity_clear();
    }

    log_info("Session disconnected", 0, 0);
    MP_LOG_INFO("SESSION", "Session disconnected — state reset to IDLE");
}

void net_session_refresh_discovery_announcement(void)
{
    net_discovery_announcement announcement;

    if (session.role != NET_ROLE_HOST || session.state == NET_SESSION_IDLE) {
        return;
    }

    build_discovery_announcement(&announcement);
    net_discovery_update_announcing(&announcement);
}

void net_session_update(void)
{
    if (session.state == NET_SESSION_IDLE) {
        return;
    }

    if (session.state == NET_SESSION_DISCONNECTING) {
        net_session_disconnect();
        return;
    }

    if (session.role == NET_ROLE_HOST) {
        host_accept_connections();
        host_process_peers();
    } else if (session.role == NET_ROLE_CLIENT) {
        client_process_host();
    }
}

int net_session_is_active(void)
{
    return session.state != NET_SESSION_IDLE;
}

int net_session_is_host(void)
{
    return session.role == NET_ROLE_HOST;
}

int net_session_is_client(void)
{
    return session.role == NET_ROLE_CLIENT;
}

int net_session_is_in_game(void)
{
    return session.state == NET_SESSION_HOSTING_GAME ||
           session.state == NET_SESSION_CLIENT_GAME;
}

int net_session_is_in_lobby(void)
{
    return session.state == NET_SESSION_HOSTING_LOBBY ||
           session.state == NET_SESSION_CLIENT_LOBBY;
}

int net_session_has_local_player(void)
{
    return session.local_player_id != NET_PLAYER_ID_NONE &&
           mp_player_registry_get(session.local_player_id) != NULL;
}

net_session_state net_session_get_state(void)
{
    return session.state;
}

uint8_t net_session_get_local_player_id(void)
{
    return session.local_player_id;
}

uint32_t net_session_get_authoritative_tick(void)
{
    return session.authoritative_tick;
}

static const char *SESSION_STATE_NAMES[] = {
    "IDLE", "HOSTING_LOBBY", "HOSTING_GAME", "JOINING",
    "CLIENT_LOBBY", "CLIENT_GAME", "DISCONNECTING"
};

const char *net_session_state_name(net_session_state state)
{
    if (state < 0 || state > NET_SESSION_DISCONNECTING) {
        return "UNKNOWN";
    }
    return SESSION_STATE_NAMES[state];
}

int net_session_get_peer_count(void)
{
    return session.peer_count;
}

const net_peer *net_session_get_peer(int index)
{
    if (index < 0 || index >= NET_MAX_PEERS) {
        return NULL;
    }
    return &session.peers[index];
}

const net_peer *net_session_get_host_peer(void)
{
    return &session.host_peer;
}

int net_session_transition_to_game(void)
{
    if (session.role != NET_ROLE_HOST) {
        return 0;
    }

    /* Allow transition from lobby OR from already hosting game (e.g., resume) */
    if (session.state != NET_SESSION_HOSTING_LOBBY &&
        session.state != NET_SESSION_HOSTING_GAME) {
        return 0;
    }

    session.state = NET_SESSION_HOSTING_GAME;
    if (session.authoritative_tick == 0) {
        session.game_speed = 2; /* Normal speed */
    }

    /* Update host player status */
    if (net_session_has_local_player()) {
        mp_player_registry_set_status(session.local_player_id, MP_PLAYER_IN_GAME);
    }

    /* Transition all active peers to IN_GAME state.
     * NOTE: NET_MSG_START_GAME is no longer broadcast here (deprecated).
     * Clients transition via GAME_START_FINAL instead. */
    for (int i = 0; i < NET_MAX_PEERS; i++) {
        if (session.peers[i].active &&
            (session.peers[i].state == PEER_STATE_READY ||
             session.peers[i].state == PEER_STATE_JOINED)) {
            session.peers[i].state = PEER_STATE_IN_GAME;
            mp_player_registry_set_status(session.peers[i].player_id, MP_PLAYER_IN_GAME);
        }
    }

    log_info("Session transitioned to game", 0, 0);
    MP_LOG_INFO("SESSION", "Session transitioned to game: tick=%u speed=%d peers=%d",
                session.authoritative_tick, (int)session.game_speed, session.peer_count);
    net_session_refresh_discovery_announcement();
    return 1;
}

void net_session_set_game_speed(uint8_t speed)
{
    if (speed > 3) {
        speed = 3;
    }
    session.game_speed = speed;

    /* Broadcast speed change event */
    if (session.role == NET_ROLE_HOST) {
        uint8_t event_buf[16];
        net_serializer s;
        net_serializer_init(&s, event_buf, sizeof(event_buf));
        net_write_u16(&s, NET_EVENT_SPEED_CHANGED);
        net_write_u32(&s, session.authoritative_tick);
        net_write_u8(&s, speed);
        net_session_broadcast(NET_MSG_HOST_EVENT, event_buf,
                              (uint32_t)net_serializer_position(&s));
    }
}

void net_session_set_paused(int paused)
{
    session.game_paused = paused;

    /* Broadcast pause/resume event */
    if (session.role == NET_ROLE_HOST) {
        uint8_t event_buf[16];
        net_serializer s;
        net_serializer_init(&s, event_buf, sizeof(event_buf));
        net_write_u16(&s, paused ? NET_EVENT_GAME_PAUSED : NET_EVENT_GAME_RESUMED);
        net_write_u32(&s, session.authoritative_tick);
        net_session_broadcast(NET_MSG_HOST_EVENT, event_buf,
                              (uint32_t)net_serializer_position(&s));
    }
}

void net_session_advance_tick(void)
{
    session.authoritative_tick++;
}

int net_session_send_to_host(uint16_t message_type, const uint8_t *payload, uint32_t size)
{
    if (session.role != NET_ROLE_CLIENT || !session.host_peer.active) {
        return 0;
    }
    send_raw_to_peer(&session.host_peer, message_type, payload, size);
    return 1;
}

int net_session_send_to_peer(int peer_index, uint16_t message_type,
                             const uint8_t *payload, uint32_t size)
{
    if (peer_index < 0 || peer_index >= NET_MAX_PEERS) {
        return 0;
    }
    if (!session.peers[peer_index].active) {
        return 0;
    }
    send_raw_to_peer(&session.peers[peer_index], message_type, payload, size);
    return 1;
}

static int broadcast_with_filter(uint16_t message_type, const uint8_t *payload,
                                 uint32_t size, int (*filter)(const net_peer *peer))
{
    if (session.role != NET_ROLE_HOST) {
        return 0;
    }

    int sent_count = 0;
    for (int i = 0; i < NET_MAX_PEERS; i++) {
        if (filter(&session.peers[i])) {
            send_raw_to_peer(&session.peers[i], message_type, payload, size);
            sent_count++;
        }
    }
    return sent_count;
}

int net_session_broadcast_lobby(uint16_t message_type, const uint8_t *payload, uint32_t size)
{
    return broadcast_with_filter(message_type, payload, size, peer_accepts_lobby_message);
}

int net_session_broadcast_in_game(uint16_t message_type, const uint8_t *payload, uint32_t size)
{
    return broadcast_with_filter(message_type, payload, size, peer_accepts_in_game_message);
}

int net_session_broadcast(uint16_t message_type, const uint8_t *payload, uint32_t size)
{
    return net_session_broadcast_in_game(message_type, payload, size);
}

void net_session_set_ready(int is_ready)
{
    if (session.role == NET_ROLE_CLIENT) {
        uint8_t buf[2];
        net_serializer s;
        net_serializer_init(&s, buf, sizeof(buf));
        net_write_u8(&s, session.local_player_id);
        net_write_u8(&s, (uint8_t)is_ready);
        net_session_send_to_host(NET_MSG_READY_STATE, buf, 2);
    } else if (session.role == NET_ROLE_HOST) {
        if (net_session_has_local_player()) {
            mp_player_registry_set_status(session.local_player_id,
                is_ready ? MP_PLAYER_READY : MP_PLAYER_LOBBY);
            broadcast_lobby_snapshot();
        }
    }
}

int net_session_all_peers_ready(void)
{
    if (session.role != NET_ROLE_HOST) {
        return 0;
    }

    int ready_count = 0;
    for (int i = 0; i < MP_MAX_PLAYERS; i++) {
        mp_player *player = mp_player_registry_get((uint8_t)i);
        if (!player || !player->active) {
            continue;
        }
        if (player->status != MP_PLAYER_READY) {
            return 0;
        }
        ready_count++;
    }
    return ready_count > 0;
}

net_session *net_session_get(void)
{
    return &session;
}

/* ---- Chat API ---- */

int net_session_chat_get_count(void)
{
    return chat_history.count;
}

const char *net_session_chat_get_message(int index, uint8_t *out_sender_id)
{
    if (index < 0 || index >= chat_history.count) {
        return NULL;
    }
    /* Convert from logical index to ring buffer index */
    int ring_index;
    if (chat_history.count >= MP_CHAT_HISTORY_SIZE) {
        ring_index = (chat_history.write_index + index) % MP_CHAT_HISTORY_SIZE;
    } else {
        ring_index = index;
    }
    if (out_sender_id) {
        *out_sender_id = chat_history.entries[ring_index].sender_id;
    }
    return chat_history.entries[ring_index].message;
}

int net_session_chat_get_unread(void)
{
    return chat_history.unread;
}

void net_session_chat_mark_read(void)
{
    chat_history.unread = 0;
}

int net_session_send_chat(const char *message)
{
    if (!net_session_is_active()) {
        return 0;
    }

    uint8_t buf[256];
    net_serializer s;
    net_serializer_init(&s, buf, sizeof(buf));
    net_write_u8(&s, session.local_player_id);
    net_write_string(&s, message, MP_CHAT_MESSAGE_MAX);

    if (net_session_is_host()) {
        /* Host: store locally and broadcast */
        mp_chat_entry *entry = &chat_history.entries[chat_history.write_index];
        entry->sender_id = session.local_player_id;
        copy_text(entry->message, sizeof(entry->message), message);
        entry->timestamp_tick = session.authoritative_tick;
        chat_history.write_index = (chat_history.write_index + 1) % MP_CHAT_HISTORY_SIZE;
        if (chat_history.count < MP_CHAT_HISTORY_SIZE) {
            chat_history.count++;
        }
        if (net_session_is_in_lobby()) {
            net_session_broadcast_lobby(NET_MSG_CHAT, buf, (uint32_t)net_serializer_position(&s));
        } else {
            net_session_broadcast_in_game(NET_MSG_CHAT, buf, (uint32_t)net_serializer_position(&s));
        }
    } else {
        /* Client: send to host for relay */
        net_session_send_to_host(NET_MSG_CHAT, buf, (uint32_t)net_serializer_position(&s));
    }
    return 1;
}

/* ---- Player name management ---- */

void net_session_set_local_name(const char *name)
{
    if (!name || !name[0]) {
        return;
    }

    /* Trim leading/trailing spaces */
    const char *start = name;
    while (*start == ' ') {
        start++;
    }
    if (!*start) {
        return; /* All spaces — reject */
    }

    copy_text(persistent_player_name, sizeof(persistent_player_name), start);

    /* Trim trailing spaces */
    int len = (int)strlen(persistent_player_name);
    while (len > 0 && persistent_player_name[len - 1] == ' ') {
        persistent_player_name[--len] = '\0';
    }

    persistent_name_set = 1;

    /* Persist to config file */
    config_set_string(CONFIG_STRING_MP_PLAYER_NAME, persistent_player_name);

    /* Also update the active session name if one exists */
    if (session.state != NET_SESSION_IDLE) {
        copy_text(session.local_player_name, sizeof(session.local_player_name),
                  persistent_player_name);
    }

    MP_LOG_INFO("SESSION", "Player name set to '%s'", persistent_player_name);
}

const char *net_session_get_local_name(void)
{
    return persistent_player_name;
}

/* ---- Join status tracking ---- */

net_join_status net_session_get_join_status(void)
{
    return join_status;
}

uint8_t net_session_get_reject_reason(void)
{
    return join_reject_reason;
}

void net_session_clear_join_status(void)
{
    join_status = NET_JOIN_STATUS_NONE;
    join_reject_reason = 0;
    join_start_ms = 0;
}

#endif /* ENABLE_MULTIPLAYER */
