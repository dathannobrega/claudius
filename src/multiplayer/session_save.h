#ifndef MULTIPLAYER_SESSION_SAVE_H
#define MULTIPLAYER_SESSION_SAVE_H

#ifdef ENABLE_MULTIPLAYER

#include <stdint.h>

/**
 * Multiplayer session save/load.
 * Only the host creates authoritative saves.
 * Saves include multiplayer metadata alongside the normal save data.
 *
 * Saved domains:
 *   - Session metadata (seed, protocol, checksums)
 *   - Player registry (UUIDs, slots, reconnect tokens)
 *   - Ownership tables (cities, routes with lifecycle, traders)
 *   - Spawn table (worldgen results)
 *   - Empire sync (trade views)
 *   - Trade sync (route state, trader replicas)
 *   - Time sync (authoritative tick)
 *
 * v4 additions:
 *   - Total payload size for integrity check
 *   - Domain count field for forward compatibility
 *   - Compatibility flags
 *   - Stronger validation on load
 *
 * v5 additions:
 *   - World instance UUID for identity persistence
 *   - Per-domain FNV-1a checksums in domain table
 *   - Header checksum
 *   - Forward-compatible domain tags (unknown domains skipped by size)
 *   - Structured error codes
 */

#define MP_SAVE_MAGIC       0x4D504C59  /* "MPLY" */
#define MP_SAVE_VERSION     5
#define MP_SAVE_DOMAIN_COUNT 7

#define MP_SAVE_MAX_FILE_SIZE       (512 * 1024)
#define MP_SAVE_MAX_DOMAIN_SIZE     (128 * 1024)
#define MP_WORLD_UUID_SIZE          16

/* Compatibility flags */
#define MP_SAVE_FLAG_HAS_P2P_ROUTES   0x0001
#define MP_SAVE_FLAG_HAS_TRADE_SYNC   0x0002

/* Domain tags for forward compatibility */
#define MP_DOMAIN_TAG_PLAYER_REGISTRY    0x01
#define MP_DOMAIN_TAG_OWNERSHIP          0x02
#define MP_DOMAIN_TAG_WORLDGEN           0x03
#define MP_DOMAIN_TAG_EMPIRE_SYNC        0x04
#define MP_DOMAIN_TAG_TRADE_SYNC_ROUTES  0x05
#define MP_DOMAIN_TAG_TRADE_SYNC_TRADERS 0x06
#define MP_DOMAIN_TAG_TIME_SYNC          0x07

/* Load error codes */
#define MP_LOAD_OK                  0
#define MP_LOAD_ERR_MAGIC          -1
#define MP_LOAD_ERR_VERSION        -2
#define MP_LOAD_ERR_TRUNCATED      -3
#define MP_LOAD_ERR_CHECKSUM       -4
#define MP_LOAD_ERR_DOMAIN_CORRUPT -5
#define MP_LOAD_ERR_SIZE           -6

typedef struct {
    uint8_t  tag;
    uint32_t size;
    uint32_t checksum;  /* FNV-1a */
} mp_domain_entry;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t protocol_version;
    uint32_t session_id;
    uint32_t session_seed;
    uint8_t host_player_id;
    uint8_t player_count;
    uint32_t snapshot_tick;
    uint32_t checksum;
    /* Domain sizes */
    uint32_t player_registry_size;
    uint32_t ownership_size;
    uint32_t worldgen_size;
    uint32_t empire_sync_size;
    uint32_t trade_sync_routes_size;
    uint32_t trade_sync_traders_size;
    uint32_t time_sync_size;
    uint32_t next_command_sequence_id;
    /* v4 fields */
    uint32_t total_payload_size;    /* Total size of all domain data after header */
    uint8_t  domain_count;          /* Number of domains (for forward compat) */
    uint16_t compat_flags;          /* Compatibility flags */
    uint32_t payload_checksum;      /* Simple checksum of domain payload bytes */
    /* v5 fields */
    uint8_t  world_instance_uuid[MP_WORLD_UUID_SIZE];
    uint32_t header_checksum;       /* FNV-1a of all header bytes before this */
} mp_save_header;

/* Save multiplayer metadata to buffer */
int mp_session_save_to_buffer(uint8_t *buffer, uint32_t buffer_size, uint32_t *out_size);

/* Load multiplayer metadata from buffer */
int mp_session_load_from_buffer(const uint8_t *buffer, uint32_t size);

/* Check if a save file contains multiplayer data */
int mp_session_save_is_multiplayer(const uint8_t *buffer, uint32_t size);

/* Get save header for display */
int mp_session_save_read_header(const uint8_t *buffer, uint32_t size, mp_save_header *header);

#endif /* ENABLE_MULTIPLAYER */

#endif /* MULTIPLAYER_SESSION_SAVE_H */
