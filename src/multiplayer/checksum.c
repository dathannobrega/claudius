#include "checksum.h"

#ifdef ENABLE_MULTIPLAYER

#include "empire_sync.h"
#include "mp_trade_route.h"
#include "ownership.h"
#include "player_registry.h"
#include "resync.h"
#include "snapshot.h"
#include "time_sync.h"
#include "trade_sync.h"
#include "worldgen.h"
#include "network/session.h"
#include "network/serialize.h"
#include "network/protocol.h"
#include "game/time.h"
#include "core/log.h"

#include <string.h>
#include <stdlib.h>

static struct {
    uint32_t last_check_tick;
    uint32_t host_checksum;
    int has_desync;
    uint8_t desynced_player;
    uint8_t mismatch_counts[MP_MAX_PLAYERS];
} checksum_data;

void mp_checksum_init(void)
{
    memset(&checksum_data, 0, sizeof(checksum_data));
}

/* FNV-1a hash */
static uint32_t fnv1a_init(void)
{
    return 2166136261u;
}

static uint32_t fnv1a_update(uint32_t hash, const void *data, size_t size)
{
    const uint8_t *bytes = (const uint8_t *)data;
    for (size_t i = 0; i < size; i++) {
        hash ^= bytes[i];
        hash *= 16777619u;
    }
    return hash;
}

static uint32_t fnv1a_u32(uint32_t hash, uint32_t value)
{
    return fnv1a_update(hash, &value, sizeof(value));
}

static uint32_t fnv1a_i32(uint32_t hash, int32_t value)
{
    return fnv1a_update(hash, &value, sizeof(value));
}

static uint32_t hash_domain_bytes(uint32_t hash, const char *label,
                                  const uint8_t *buffer, uint32_t size)
{
    (void)label;
    hash = fnv1a_u32(hash, size);
    if (size > 0) {
        hash = fnv1a_update(hash, buffer, size);
    }
    return hash;
}

uint32_t mp_checksum_compute(void)
{
    uint32_t hash = fnv1a_init();
    uint8_t *buffer = (uint8_t *)malloc(MP_SNAPSHOT_MAX_SIZE);
    uint32_t size = 0;

    if (!buffer) {
        log_error("Failed to allocate checksum domain buffer", 0, MP_SNAPSHOT_MAX_SIZE);
        return hash;
    }

    /* Hash canonical time state only. Avoid local client-only counters. */
    hash = fnv1a_u32(hash, net_session_get_authoritative_tick());
    hash = fnv1a_i32(hash, mp_time_sync_is_paused());
    hash = fnv1a_u32(hash, mp_time_sync_get_speed());
    hash = fnv1a_i32(hash, game_time_year());
    hash = fnv1a_i32(hash, game_time_month());
    hash = fnv1a_i32(hash, game_time_day());
    hash = fnv1a_i32(hash, game_time_tick());

    memset(buffer, 0, MP_SNAPSHOT_MAX_SIZE);
    mp_ownership_serialize(buffer, &size);
    hash = hash_domain_bytes(hash, "ownership", buffer, size);

    memset(buffer, 0, MP_SNAPSHOT_MAX_SIZE);
    mp_empire_sync_serialize(buffer, &size);
    hash = hash_domain_bytes(hash, "empire_sync", buffer, size);

    memset(buffer, 0, MP_SNAPSHOT_MAX_SIZE);
    mp_trade_sync_serialize_routes(buffer, &size);
    hash = hash_domain_bytes(hash, "trade_routes_legacy", buffer, size);

    memset(buffer, 0, MP_SNAPSHOT_MAX_SIZE);
    mp_trade_sync_serialize_traders(buffer, &size);
    hash = hash_domain_bytes(hash, "trade_traders", buffer, size);

    memset(buffer, 0, MP_SNAPSHOT_MAX_SIZE);
    mp_trade_route_serialize(buffer, &size, MP_SNAPSHOT_MAX_SIZE);
    hash = hash_domain_bytes(hash, "trade_routes_p2p", buffer, size);

    memset(buffer, 0, MP_SNAPSHOT_MAX_SIZE);
    mp_worldgen_serialize(buffer, &size);
    hash = hash_domain_bytes(hash, "worldgen", buffer, size);

    free(buffer);
    return hash;
}

void mp_checksum_request_from_clients(uint32_t tick)
{
    if (mp_time_sync_is_join_barrier_active()) {
        return;
    }

    checksum_data.host_checksum = mp_checksum_compute();
    checksum_data.last_check_tick = tick;

    uint8_t buf[8];
    net_serializer s;
    net_serializer_init(&s, buf, sizeof(buf));
    net_write_u32(&s, tick);
    net_write_u32(&s, checksum_data.host_checksum);

    net_session_broadcast(NET_MSG_CHECKSUM_REQUEST, buf, (uint32_t)net_serializer_position(&s));
}

void multiplayer_checksum_receive_response(uint8_t player_id,
                                            const uint8_t *data, uint32_t size)
{
    net_serializer s;
    net_serializer_init(&s, (uint8_t *)data, size);

    if (player_id >= MP_MAX_PLAYERS) {
        return;
    }

    uint32_t tick = net_read_u32(&s);
    uint32_t client_checksum = net_read_u32(&s);
    net_read_u8(&s); /* player_id in payload - use parameter instead */

    if (tick != checksum_data.last_check_tick) {
        return; /* Stale response */
    }

    {
        mp_player *p = mp_player_registry_get(player_id);
        if (p) {
            p->last_checksum = client_checksum;
            p->last_checksum_tick = tick;
        }
    }

    if (client_checksum != checksum_data.host_checksum) {
        if (checksum_data.mismatch_counts[player_id] < 255) {
            checksum_data.mismatch_counts[player_id]++;
        }

        if (checksum_data.mismatch_counts[player_id] == 1) {
            log_error("Checksum mismatch detected, requesting resync", 0, player_id);
            multiplayer_resync_handle_request(player_id);
            return;
        }

        checksum_data.has_desync = 1;
        checksum_data.desynced_player = player_id;
        log_error("DESYNC confirmed for player", 0, player_id);
        log_error("Host checksum vs client", 0,
                  (int)(checksum_data.host_checksum ^ client_checksum));
    } else {
        checksum_data.mismatch_counts[player_id] = 0;
        if (checksum_data.desynced_player == player_id) {
            checksum_data.has_desync = 0;
            checksum_data.desynced_player = 0;
        }
    }
}

void multiplayer_checksum_handle_request(const uint8_t *data, uint32_t size)
{
    net_serializer s;
    net_serializer_init(&s, (uint8_t *)data, size);

    uint32_t tick = net_read_u32(&s);
    uint32_t host_checksum = net_read_u32(&s);
    (void)host_checksum;

    if (mp_resync_is_in_progress() || mp_time_sync_is_join_barrier_active()) {
        return;
    }

    uint32_t local_checksum = mp_checksum_compute();

    uint8_t resp[16];
    net_serializer rs;
    net_serializer_init(&rs, resp, sizeof(resp));
    net_write_u32(&rs, tick);
    net_write_u32(&rs, local_checksum);
    net_write_u8(&rs, net_session_get_local_player_id());

    net_session_send_to_host(NET_MSG_CHECKSUM_RESPONSE, resp,
                            (uint32_t)net_serializer_position(&rs));
}

int mp_checksum_should_check(uint32_t current_tick)
{
    if (mp_time_sync_is_join_barrier_active()) {
        return 0;
    }

    return (current_tick > 0) &&
           (current_tick - checksum_data.last_check_tick >= NET_CHECKSUM_INTERVAL_TICKS);
}

int mp_checksum_has_desync(void)
{
    return checksum_data.has_desync;
}

uint8_t mp_checksum_desynced_player(void)
{
    return checksum_data.desynced_player;
}

#endif /* ENABLE_MULTIPLAYER */
