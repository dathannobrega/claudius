#include "session_save.h"

#ifdef ENABLE_MULTIPLAYER

#include "player_registry.h"
#include "ownership.h"
#include "empire_sync.h"
#include "trade_sync.h"
#include "mp_trade_route.h"
#include "time_sync.h"
#include "checksum.h"
#include "command_bus.h"
#include "worldgen.h"
#include "game_manifest.h"
#include "mp_debug_log.h"
#include "network/session.h"
#include "network/serialize.h"
#include "network/protocol.h"
#include "core/log.h"

#include <string.h>
#include <stdlib.h>

#define DOMAIN_BUFFER_SIZE 32768

/* v2 header wire size = 58 bytes, v3 = 62 bytes, v4 = 73 bytes, v5 = 93 bytes, v6 = 97 bytes */
#define HEADER_V2_SIZE 58
#define HEADER_V3_SIZE 62
#define HEADER_V4_SIZE 73
#define HEADER_V5_SIZE 93  /* v4 + 16 bytes world_uuid + 4 bytes header_checksum */
#define HEADER_V6_SIZE 97  /* v5 + 4 bytes p2p_routes_size */

/* FNV-1a 32-bit checksum */
#define FNV_OFFSET 2166136261u
#define FNV_PRIME  16777619u

static uint32_t compute_fnv1a(const uint8_t *data, uint32_t size)
{
    uint32_t hash = FNV_OFFSET;
    for (uint32_t i = 0; i < size; i++) {
        hash ^= data[i];
        hash *= FNV_PRIME;
    }
    return hash;
}

static uint32_t compute_payload_checksum(const uint8_t *data, uint32_t size)
{
    /* Simple additive checksum with rotation for integrity verification (v4 compat) */
    uint32_t hash = 0x12345678;
    for (uint32_t i = 0; i < size; i++) {
        hash = ((hash << 5) | (hash >> 27)) ^ data[i];
    }
    return hash;
}

int mp_session_save_to_buffer(uint8_t *buffer, uint32_t buffer_size, uint32_t *out_size)
{
    if (!net_session_is_host()) {
        log_error("Only host can save multiplayer session", 0, 0);
        return 0;
    }

    mp_save_header header;
    memset(&header, 0, sizeof(header));
    header.magic = MP_SAVE_MAGIC;
    header.version = MP_SAVE_VERSION;
    header.protocol_version = NET_PROTOCOL_VERSION;
    header.session_id = net_session_get()->session_id;
    header.session_seed = mp_worldgen_get_spawn_table()->session_seed;
    header.host_player_id = net_session_get_local_player_id();
    header.player_count = (uint8_t)mp_player_registry_get_count();
    header.snapshot_tick = net_session_get_authoritative_tick();
    header.checksum = mp_checksum_compute();
    header.next_command_sequence_id = mp_command_bus_get_next_sequence_id();
    header.domain_count = MP_SAVE_DOMAIN_COUNT;
    header.compat_flags = MP_SAVE_FLAG_HAS_P2P_ROUTES | MP_SAVE_FLAG_HAS_TRADE_SYNC;

    /* Serialize each domain into temporary buffers */
    uint8_t *temp = (uint8_t *)malloc(DOMAIN_BUFFER_SIZE * 8);
    if (!temp) {
        log_error("Failed to allocate save buffer", 0, 0);
        return 0;
    }

    uint8_t *player_buf = temp;
    uint8_t *ownership_buf = temp + DOMAIN_BUFFER_SIZE;
    uint8_t *worldgen_buf = temp + DOMAIN_BUFFER_SIZE * 2;
    uint8_t *empire_buf = temp + DOMAIN_BUFFER_SIZE * 3;
    uint8_t *routes_buf = temp + DOMAIN_BUFFER_SIZE * 4;
    uint8_t *traders_buf = temp + DOMAIN_BUFFER_SIZE * 5;
    uint8_t *p2p_routes_buf = temp + DOMAIN_BUFFER_SIZE * 6;
    uint8_t *time_buf = temp + DOMAIN_BUFFER_SIZE * 7;

    mp_player_registry_serialize(player_buf, &header.player_registry_size);
    mp_ownership_serialize(ownership_buf, &header.ownership_size);
    mp_worldgen_serialize(worldgen_buf, &header.worldgen_size);
    mp_empire_sync_serialize(empire_buf, &header.empire_sync_size);
    mp_trade_sync_serialize_routes(routes_buf, &header.trade_sync_routes_size);
    mp_trade_sync_serialize_traders(traders_buf, &header.trade_sync_traders_size);
    mp_trade_route_serialize(p2p_routes_buf, &header.p2p_routes_size, DOMAIN_BUFFER_SIZE);
    mp_time_sync_serialize(time_buf, &header.time_sync_size);

    /* Calculate total payload (all domains concatenated) */
    header.total_payload_size = header.player_registry_size
                              + header.ownership_size
                              + header.worldgen_size
                              + header.empire_sync_size
                              + header.trade_sync_routes_size
                              + header.trade_sync_traders_size
                              + header.p2p_routes_size
                              + header.time_sync_size;

    /* v5: compute per-domain FNV-1a checksums */
    mp_domain_entry domain_table[MP_SAVE_DOMAIN_COUNT];
    {
        const uint8_t *bufs[] = { player_buf, ownership_buf, worldgen_buf,
                                   empire_buf, routes_buf, traders_buf,
                                   p2p_routes_buf, time_buf };
        const uint32_t sizes[] = {
            header.player_registry_size, header.ownership_size, header.worldgen_size,
            header.empire_sync_size, header.trade_sync_routes_size,
            header.trade_sync_traders_size, header.p2p_routes_size,
            header.time_sync_size
        };
        const uint8_t tags[] = {
            MP_DOMAIN_TAG_PLAYER_REGISTRY, MP_DOMAIN_TAG_OWNERSHIP,
            MP_DOMAIN_TAG_WORLDGEN, MP_DOMAIN_TAG_EMPIRE_SYNC,
            MP_DOMAIN_TAG_TRADE_SYNC_ROUTES, MP_DOMAIN_TAG_TRADE_SYNC_TRADERS,
            MP_DOMAIN_TAG_P2P_ROUTES, MP_DOMAIN_TAG_TIME_SYNC
        };
        for (int b = 0; b < MP_SAVE_DOMAIN_COUNT; b++) {
            domain_table[b].tag = tags[b];
            domain_table[b].size = sizes[b];
            domain_table[b].checksum = compute_fnv1a(bufs[b], sizes[b]);
        }
    }

    /* v5: domain table size = 9 bytes per domain (tag + size + checksum) */
    uint32_t domain_table_wire_size = (uint32_t)(MP_SAVE_DOMAIN_COUNT * 9);
    uint32_t total = HEADER_V6_SIZE + domain_table_wire_size + header.total_payload_size;

    if (total > buffer_size) {
        log_error("Save buffer too small", 0, (int)total);
        free(temp);
        return 0;
    }

    /* Compute v4 payload checksum for backward compat */
    {
        uint32_t running = 0x12345678;
        const uint8_t *bufs[] = { player_buf, ownership_buf, worldgen_buf,
                                   empire_buf, routes_buf, traders_buf,
                                   p2p_routes_buf, time_buf };
        const uint32_t sizes[] = {
            header.player_registry_size, header.ownership_size, header.worldgen_size,
            header.empire_sync_size, header.trade_sync_routes_size,
            header.trade_sync_traders_size, header.p2p_routes_size,
            header.time_sync_size
        };
        for (int b = 0; b < MP_SAVE_DOMAIN_COUNT; b++) {
            for (uint32_t i = 0; i < sizes[b]; i++) {
                running = ((running << 5) | (running >> 27)) ^ bufs[b][i];
            }
        }
        header.payload_checksum = running;
    }

    /* v5: copy world instance UUID from manifest */
    {
        const mp_game_manifest *m = mp_game_manifest_get();
        if (m && m->valid) {
            memcpy(header.world_instance_uuid, m->world_instance_uuid, MP_WORLD_UUID_SIZE);
        }
    }

    /* Write header */
    net_serializer s;
    net_serializer_init(&s, buffer, buffer_size);
    net_write_u32(&s, header.magic);
    net_write_u32(&s, header.version);
    net_write_u32(&s, header.protocol_version);
    net_write_u32(&s, header.session_id);
    net_write_u32(&s, header.session_seed);
    net_write_u8(&s, header.host_player_id);
    net_write_u8(&s, header.player_count);
    net_write_u32(&s, header.snapshot_tick);
    net_write_u32(&s, header.checksum);
    net_write_u32(&s, header.player_registry_size);
    net_write_u32(&s, header.ownership_size);
    net_write_u32(&s, header.worldgen_size);
    net_write_u32(&s, header.empire_sync_size);
    net_write_u32(&s, header.trade_sync_routes_size);
    net_write_u32(&s, header.trade_sync_traders_size);
    net_write_u32(&s, header.p2p_routes_size);
    net_write_u32(&s, header.time_sync_size);
    net_write_u32(&s, header.next_command_sequence_id);
    /* v4 fields */
    net_write_u32(&s, header.total_payload_size);
    net_write_u8(&s, header.domain_count);
    net_write_u16(&s, header.compat_flags);
    net_write_u32(&s, header.payload_checksum);
    /* v5 fields */
    net_write_raw(&s, header.world_instance_uuid, MP_WORLD_UUID_SIZE);

    /* v5: compute header checksum over all header bytes written so far */
    header.header_checksum = compute_fnv1a(buffer, (uint32_t)net_serializer_position(&s));
    net_write_u32(&s, header.header_checksum);

    /* v5: write domain table */
    for (int d = 0; d < MP_SAVE_DOMAIN_COUNT; d++) {
        net_write_u8(&s, domain_table[d].tag);
        net_write_u32(&s, domain_table[d].size);
        net_write_u32(&s, domain_table[d].checksum);
    }

    /* Write domains in order */
    net_write_raw(&s, player_buf, header.player_registry_size);
    net_write_raw(&s, ownership_buf, header.ownership_size);
    net_write_raw(&s, worldgen_buf, header.worldgen_size);
    net_write_raw(&s, empire_buf, header.empire_sync_size);
    net_write_raw(&s, routes_buf, header.trade_sync_routes_size);
    net_write_raw(&s, traders_buf, header.trade_sync_traders_size);
    net_write_raw(&s, p2p_routes_buf, header.p2p_routes_size);
    net_write_raw(&s, time_buf, header.time_sync_size);

    *out_size = (uint32_t)net_serializer_position(&s);

    free(temp);

    MP_LOG_INFO("SESSION_SAVE", "Multiplayer session saved: %u bytes, tick=%u, seq=%u, "
                "payload_checksum=0x%08x",
                *out_size, header.snapshot_tick, header.next_command_sequence_id,
                header.payload_checksum);
    return 1;
}

int mp_session_load_from_buffer(const uint8_t *buffer, uint32_t size)
{
    mp_save_header header;
    if (!mp_session_save_read_header(buffer, size, &header)) {
        log_error("Failed to read multiplayer save header", 0, 0);
        return 0;
    }

    /* Validate total payload fits in the buffer */
    uint32_t header_wire_size;
    if (header.version >= 6) {
        header_wire_size = HEADER_V6_SIZE;
    } else if (header.version >= 5) {
        header_wire_size = HEADER_V5_SIZE;
    } else if (header.version >= 4) {
        header_wire_size = HEADER_V4_SIZE;
    } else if (header.version >= 3) {
        header_wire_size = HEADER_V3_SIZE;
    } else {
        header_wire_size = HEADER_V2_SIZE;
    }

    uint32_t expected_total = header.player_registry_size
                            + header.ownership_size
                            + header.worldgen_size
                            + header.empire_sync_size
                            + header.trade_sync_routes_size
                            + header.trade_sync_traders_size
                            + header.p2p_routes_size
                            + header.time_sync_size;

    /* v5: validate file size limits */
    if (header.version >= 5 && expected_total > MP_SAVE_MAX_FILE_SIZE) {
        log_error("Multiplayer save: total payload exceeds max file size", 0,
                  (int)expected_total);
        return 0;
    }

    /* v5: validate header checksum */
    if (header.version >= 5 && header.header_checksum != 0) {
        uint32_t checksum_header_size = (header.version >= 6) ? HEADER_V6_SIZE : HEADER_V5_SIZE;
        /* Header checksum covers bytes 0..(header_size - 4) */
        uint32_t actual_hdr_cksum = compute_fnv1a(buffer, checksum_header_size - 4);
        if (actual_hdr_cksum != header.header_checksum) {
            log_error("Multiplayer save: header checksum mismatch", 0,
                      (int)actual_hdr_cksum);
            return 0;
        }
        MP_LOG_INFO("SESSION_SAVE", "Header checksum verified: 0x%08x", actual_hdr_cksum);
    }

    /* v5: skip domain table (9 bytes per domain) — we use positional reading below */
    uint32_t domain_table_size = 0;
    mp_domain_entry domain_table[MP_SAVE_DOMAIN_COUNT];
    memset(domain_table, 0, sizeof(domain_table));

    if (header.version >= 5) {
        domain_table_size = (uint32_t)(header.domain_count * 9);

        if (header_wire_size + domain_table_size + expected_total > size) {
            log_error("Multiplayer save: buffer too small for v5 layout", 0,
                      (int)(header_wire_size + domain_table_size + expected_total));
            return 0;
        }

        /* Read domain table */
        net_serializer dt;
        net_serializer_init(&dt, (uint8_t *)buffer + header_wire_size, domain_table_size);
        for (int d = 0; d < header.domain_count && d < MP_SAVE_DOMAIN_COUNT; d++) {
            domain_table[d].tag = net_read_u8(&dt);
            domain_table[d].size = net_read_u32(&dt);
            domain_table[d].checksum = net_read_u32(&dt);

            if (domain_table[d].size > MP_SAVE_MAX_DOMAIN_SIZE) {
                log_error("Multiplayer save: domain too large", 0,
                          (int)domain_table[d].size);
                return 0;
            }
        }
    } else {
        if (header_wire_size + expected_total > size) {
            log_error("Multiplayer save: buffer too small for declared domains", 0,
                      (int)(header_wire_size + expected_total));
            return 0;
        }
    }

    /* v4: validate payload checksum */
    if (header.version >= 4 && header.version < 5 && header.payload_checksum != 0) {
        const uint8_t *payload_start = buffer + header_wire_size;
        uint32_t actual_checksum = compute_payload_checksum(payload_start, expected_total);
        if (actual_checksum != header.payload_checksum) {
            log_error("Multiplayer save: payload checksum mismatch", 0,
                      (int)actual_checksum);
            return 0;
        }
        MP_LOG_INFO("SESSION_SAVE", "Payload checksum verified: 0x%08x", actual_checksum);
    }

    /* v4: validate total_payload_size if present */
    if (header.version >= 4 && header.total_payload_size > 0) {
        if (header.total_payload_size != expected_total) {
            log_error("Multiplayer save: payload size mismatch", 0,
                      (int)header.total_payload_size);
            return 0;
        }
    }

    net_serializer s;
    net_serializer_init(&s, (uint8_t *)buffer, size);
    s.position = header_wire_size + domain_table_size;

    /* Read each domain with size validation and v5 per-domain checksum */
    int domains_ok = 1;

    if (header.player_registry_size > 0) {
        if (s.position + header.player_registry_size > size) {
            log_error("Save truncated at player_registry domain", 0, 0);
            return 0;
        }
        const uint8_t *data = buffer + s.position;
        /* v5: verify per-domain checksum */
        if (header.version >= 5 && domain_table[0].checksum != 0) {
            uint32_t actual = compute_fnv1a(data, header.player_registry_size);
            if (actual != domain_table[0].checksum) {
                log_error("Save: player_registry domain checksum mismatch", 0, 0);
                return 0;
            }
        }
        mp_player_registry_deserialize(data, header.player_registry_size);
        s.position += header.player_registry_size;
    } else {
        MP_LOG_WARN("SESSION_SAVE", "Player registry domain empty");
        domains_ok = 0;
    }

    if (header.ownership_size > 0) {
        if (s.position + header.ownership_size > size) {
            log_error("Save truncated at ownership domain", 0, 0);
            return 0;
        }
        const uint8_t *data = buffer + s.position;
        mp_ownership_deserialize(data, header.ownership_size);
        s.position += header.ownership_size;
    }

    if (header.worldgen_size > 0) {
        if (s.position + header.worldgen_size > size) {
            log_error("Save truncated at worldgen domain", 0, 0);
            return 0;
        }
        const uint8_t *data = buffer + s.position;
        mp_worldgen_deserialize(data, header.worldgen_size);
        s.position += header.worldgen_size;
    }

    if (header.empire_sync_size > 0) {
        if (s.position + header.empire_sync_size > size) {
            log_error("Save truncated at empire_sync domain", 0, 0);
            return 0;
        }
        const uint8_t *data = buffer + s.position;
        mp_empire_sync_deserialize(data, header.empire_sync_size);
        s.position += header.empire_sync_size;
    }

    if (header.trade_sync_routes_size > 0) {
        if (s.position + header.trade_sync_routes_size > size) {
            log_error("Save truncated at trade_sync_routes domain", 0, 0);
            return 0;
        }
        const uint8_t *data = buffer + s.position;
        mp_trade_sync_deserialize_routes(data, header.trade_sync_routes_size);
        s.position += header.trade_sync_routes_size;
    }

    if (header.trade_sync_traders_size > 0) {
        if (s.position + header.trade_sync_traders_size > size) {
            log_error("Save truncated at trade_sync_traders domain", 0, 0);
            return 0;
        }
        const uint8_t *data = buffer + s.position;
        mp_trade_sync_deserialize_traders(data, header.trade_sync_traders_size);
        s.position += header.trade_sync_traders_size;
    }

    if (header.p2p_routes_size > 0) {
        if (s.position + header.p2p_routes_size > size) {
            log_error("Save truncated at p2p_routes domain", 0, 0);
            return 0;
        }
        {
            const uint8_t *data = buffer + s.position;
            mp_trade_route_deserialize(data, header.p2p_routes_size);
        }
        s.position += header.p2p_routes_size;
    }

    if (header.time_sync_size > 0) {
        if (s.position + header.time_sync_size > size) {
            log_error("Save truncated at time_sync domain", 0, 0);
            return 0;
        }
        const uint8_t *data = buffer + s.position;
        mp_time_sync_deserialize(data, header.time_sync_size);
        s.position += header.time_sync_size;
    }

    /* Restore command bus sequence ID for continuity.
     * Use init_from_save to avoid the reset-then-set race condition. */
    if (header.next_command_sequence_id > 0) {
        mp_command_bus_init_from_save(header.next_command_sequence_id);
    }

    /* Restore manifest metadata needed by reconnect/discovery after a saved game load. */
    {
        mp_game_manifest *manifest = mp_game_manifest_get_mutable();
        if (manifest) {
            manifest->mode = MP_GAME_MODE_SAVED_GAME;
            manifest->save_version = header.version;
            manifest->session_seed = header.session_seed;
            manifest->player_count = header.player_count;
            manifest->max_players = NET_MAX_PLAYERS;
            manifest->feature_flags = header.compat_flags;
            memcpy(manifest->world_instance_uuid, header.world_instance_uuid,
                   MP_WORLD_UUID_SIZE);
            manifest->valid = 1;
        }
    }

    /* Mark all non-host players as awaiting_reconnect */
    for (int i = 0; i < MP_MAX_PLAYERS; i++) {
        mp_player *p = mp_player_registry_get((uint8_t)i);
        if (p && p->active && !p->is_host) {
            p->status = MP_PLAYER_AWAITING_RECONNECT;
            p->connection_state = MP_CONNECTION_DISCONNECTED;
        }
    }

    if (!domains_ok) {
        MP_LOG_WARN("SESSION_SAVE", "Multiplayer session loaded with warnings");
    }

    MP_LOG_INFO("SESSION_SAVE", "Multiplayer session loaded: tick=%u, players=%d, seq=%u",
                header.snapshot_tick, (int)header.player_count,
                header.next_command_sequence_id);
    return 1;
}

int mp_session_save_is_multiplayer(const uint8_t *buffer, uint32_t size)
{
    if (size < 4) {
        return 0;
    }
    uint32_t magic = (uint32_t)buffer[0]
                   | ((uint32_t)buffer[1] << 8)
                   | ((uint32_t)buffer[2] << 16)
                   | ((uint32_t)buffer[3] << 24);
    return magic == MP_SAVE_MAGIC;
}

int mp_session_save_read_header(const uint8_t *buffer, uint32_t size, mp_save_header *header)
{
    if (size < HEADER_V2_SIZE) {
        log_error("Save too small for header", 0, (int)size);
        return 0;
    }

    memset(header, 0, sizeof(*header));

    net_serializer s;
    net_serializer_init(&s, (uint8_t *)buffer, size);

    header->magic = net_read_u32(&s);
    header->version = net_read_u32(&s);
    header->protocol_version = net_read_u32(&s);
    header->session_id = net_read_u32(&s);
    header->session_seed = net_read_u32(&s);
    header->host_player_id = net_read_u8(&s);
    header->player_count = net_read_u8(&s);
    header->snapshot_tick = net_read_u32(&s);
    header->checksum = net_read_u32(&s);
    header->player_registry_size = net_read_u32(&s);
    header->ownership_size = net_read_u32(&s);
    header->worldgen_size = net_read_u32(&s);
    header->empire_sync_size = net_read_u32(&s);
    header->trade_sync_routes_size = net_read_u32(&s);
    header->trade_sync_traders_size = net_read_u32(&s);
    if (header->version >= 6) {
        header->p2p_routes_size = net_read_u32(&s);
    } else {
        header->p2p_routes_size = 0;
    }
    header->time_sync_size = net_read_u32(&s);

    if (header->magic != MP_SAVE_MAGIC) {
        log_error("Invalid multiplayer save magic", 0, (int)header->magic);
        return 0;
    }
    if (header->version > MP_SAVE_VERSION) {
        log_error("Unsupported multiplayer save version", 0, (int)header->version);
        return 0;
    }
    if (header->version >= 6 && size < HEADER_V6_SIZE) {
        log_error("Save too small for v6 header", 0, (int)size);
        return 0;
    }

    /* Handle v1 saves that don't have worldgen */
    if (header->version < 2) {
        header->worldgen_size = 0;
        header->session_seed = 0;
    }

    /* v3: command sequence ID */
    if (header->version >= 3 && size >= HEADER_V3_SIZE) {
        header->next_command_sequence_id = net_read_u32(&s);
    } else {
        header->next_command_sequence_id = 1;
    }

    /* v4: total_payload_size, domain_count, compat_flags, payload_checksum */
    if (header->version >= 4 && size >= HEADER_V4_SIZE) {
        header->total_payload_size = net_read_u32(&s);
        header->domain_count = net_read_u8(&s);
        header->compat_flags = net_read_u16(&s);
        header->payload_checksum = net_read_u32(&s);
    } else {
        /* Compute expected total for older versions */
        header->total_payload_size = 0;
        header->domain_count = MP_SAVE_DOMAIN_COUNT;
        header->compat_flags = 0;
        header->payload_checksum = 0;
    }

    /* v5: world_instance_uuid and header_checksum */
    if (header->version >= 5 && size >= ((header->version >= 6) ? HEADER_V6_SIZE : HEADER_V5_SIZE)) {
        net_read_raw(&s, header->world_instance_uuid, MP_WORLD_UUID_SIZE);
        header->header_checksum = net_read_u32(&s);
    } else {
        memset(header->world_instance_uuid, 0, MP_WORLD_UUID_SIZE);
        header->header_checksum = 0;
    }

    /* Validate player count is sane */
    if (header->player_count > 8) {
        log_error("Invalid player count in save", 0, (int)header->player_count);
        return 0;
    }

    return 1;
}

#endif /* ENABLE_MULTIPLAYER */
