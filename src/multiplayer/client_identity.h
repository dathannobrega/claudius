#ifndef MULTIPLAYER_CLIENT_IDENTITY_H
#define MULTIPLAYER_CLIENT_IDENTITY_H

#ifdef ENABLE_MULTIPLAYER

#include <stdint.h>

/**
 * Client identity persistence.
 *
 * Stores player UUID and reconnect token to local disk so clients can
 * reconnect to the same session after restarting the application.
 * The identity file includes a world_instance_uuid to prevent stale
 * identity from being used on a different host's session.
 *
 * File location: {platform_get_pref_path()}/mp_identity.dat
 * Format: binary via net_serializer for portability
 */

#define MP_CLIENT_IDENTITY_MAGIC    0x4D504944  /* "MPID" */
#define MP_CLIENT_IDENTITY_VERSION  2
#define MP_WORLD_UUID_SIZE          16

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint8_t player_uuid[16];
    uint8_t reconnect_token[16];
    uint8_t slot_id;
    uint8_t world_instance_uuid[MP_WORLD_UUID_SIZE];
    char last_host_address[64];
    uint16_t last_host_port;
    char player_name[32];
    uint32_t last_session_id;
    uint32_t resume_generation;
} mp_client_identity;

void mp_client_identity_init(void);

/**
 * Save identity to disk. Called on JOIN_ACCEPT.
 * @return 1 on success, 0 on failure
 */
int mp_client_identity_save(void);

/**
 * Load identity from disk. Called before HELLO.
 * @return 1 on success (identity loaded), 0 if no identity or load failed
 */
int mp_client_identity_load(void);

/**
 * Clear identity file. Called on explicit leave/disconnect.
 */
void mp_client_identity_clear(void);

/**
 * Populate UUID and reconnect token from loaded identity for HELLO message.
 * @return 1 if identity was loaded and fields populated, 0 otherwise
 */
int mp_client_identity_get_for_hello(uint8_t *out_uuid, uint8_t *out_token);

/**
 * Check if loaded identity matches a specific world instance.
 * @return 1 if matches, 0 otherwise
 */
int mp_client_identity_matches_world(const uint8_t *world_uuid);

/**
 * Set identity fields from JOIN_ACCEPT data (before saving).
 */
void mp_client_identity_set(const uint8_t *uuid, const uint8_t *token,
                             uint8_t slot_id,
                             const uint8_t *world_uuid,
                             const char *host_address, uint16_t host_port,
                             const char *player_name, uint32_t session_id,
                             uint32_t resume_generation);

/**
 * Get the current loaded identity (const).
 */
const mp_client_identity *mp_client_identity_get(void);

#endif /* ENABLE_MULTIPLAYER */

#endif /* MULTIPLAYER_CLIENT_IDENTITY_H */
