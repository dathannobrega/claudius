#include "client_identity.h"

#ifdef ENABLE_MULTIPLAYER

#include "mp_debug_log.h"
#include "network/serialize.h"
#include "platform/platform.h"
#include "core/log.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define IDENTITY_FILENAME "mp_identity.dat"
#define IDENTITY_WIRE_SIZE 224  /* Conservative upper bound for serialized identity */

static mp_client_identity identity;
static int identity_loaded;

static const char *get_identity_path(void)
{
    static char path[512];
    char *pref = platform_get_pref_path();
    if (!pref) {
        return NULL;
    }
    snprintf(path, sizeof(path), "%s%s", pref, IDENTITY_FILENAME);
    return path;
}

void mp_client_identity_init(void)
{
    memset(&identity, 0, sizeof(identity));
    identity.slot_id = 0xFF;
    identity_loaded = 0;
}

void mp_client_identity_set(const uint8_t *uuid, const uint8_t *token,
                             uint8_t slot_id,
                             const uint8_t *world_uuid,
                             const char *host_address, uint16_t host_port,
                             const char *player_name, uint32_t session_id,
                             uint32_t resume_generation)
{
    identity.magic = MP_CLIENT_IDENTITY_MAGIC;
    identity.version = MP_CLIENT_IDENTITY_VERSION;

    if (uuid) {
        memcpy(identity.player_uuid, uuid, 16);
    }
    if (token) {
        memcpy(identity.reconnect_token, token, 16);
    }
    identity.slot_id = slot_id;
    if (world_uuid) {
        memcpy(identity.world_instance_uuid, world_uuid, MP_WORLD_UUID_SIZE);
    }
    if (host_address) {
        strncpy(identity.last_host_address, host_address, sizeof(identity.last_host_address) - 1);
        identity.last_host_address[sizeof(identity.last_host_address) - 1] = '\0';
    }
    identity.last_host_port = host_port;
    if (player_name) {
        strncpy(identity.player_name, player_name, sizeof(identity.player_name) - 1);
        identity.player_name[sizeof(identity.player_name) - 1] = '\0';
    }
    identity.last_session_id = session_id;
    identity.resume_generation = resume_generation;
    identity_loaded = 1;
}

int mp_client_identity_save(void)
{
    const char *path = get_identity_path();
    if (!path) {
        MP_LOG_ERROR("IDENTITY", "Cannot determine identity file path");
        return 0;
    }

    uint8_t buf[IDENTITY_WIRE_SIZE];
    net_serializer s;
    net_serializer_init(&s, buf, sizeof(buf));

    net_write_u32(&s, identity.magic);
    net_write_u32(&s, identity.version);
    net_write_raw(&s, identity.player_uuid, 16);
    net_write_raw(&s, identity.reconnect_token, 16);
    net_write_u8(&s, identity.slot_id);
    net_write_raw(&s, identity.world_instance_uuid, MP_WORLD_UUID_SIZE);
    net_write_string(&s, identity.last_host_address, sizeof(identity.last_host_address));
    net_write_u16(&s, identity.last_host_port);
    net_write_string(&s, identity.player_name, sizeof(identity.player_name));
    net_write_u32(&s, identity.last_session_id);
    net_write_u32(&s, identity.resume_generation);

    if (net_serializer_has_overflow(&s)) {
        MP_LOG_ERROR("IDENTITY", "Serialization overflow");
        return 0;
    }

    FILE *f = fopen(path, "wb");
    if (!f) {
        MP_LOG_ERROR("IDENTITY", "Cannot write identity file: %s", path);
        return 0;
    }

    size_t written = fwrite(buf, 1, net_serializer_position(&s), f);
    fclose(f);

    if (written != net_serializer_position(&s)) {
        MP_LOG_ERROR("IDENTITY", "Short write: %zu of %zu bytes",
                     written, net_serializer_position(&s));
        return 0;
    }

    MP_LOG_INFO("IDENTITY", "Identity saved: session=0x%08x name='%s'",
                identity.last_session_id, identity.player_name);
    return 1;
}

int mp_client_identity_load(void)
{
    const char *path = get_identity_path();
    if (!path) {
        return 0;
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        return 0; /* No identity file — first time player */
    }

    uint8_t buf[IDENTITY_WIRE_SIZE];
    size_t read_count = fread(buf, 1, sizeof(buf), f);
    fclose(f);

    if (read_count < 8) {
        MP_LOG_WARN("IDENTITY", "Identity file too small: %zu bytes", read_count);
        return 0;
    }

    net_serializer s;
    net_serializer_init(&s, buf, (uint32_t)read_count);

    uint32_t magic = net_read_u32(&s);
    uint32_t version = net_read_u32(&s);

    if (magic != MP_CLIENT_IDENTITY_MAGIC) {
        MP_LOG_WARN("IDENTITY", "Invalid identity magic: 0x%08x", magic);
        return 0;
    }
    if (version > MP_CLIENT_IDENTITY_VERSION) {
        MP_LOG_WARN("IDENTITY", "Unsupported identity version: %u", version);
        return 0;
    }

    identity.magic = magic;
    identity.version = version;
    net_read_raw(&s, identity.player_uuid, 16);
    net_read_raw(&s, identity.reconnect_token, 16);
    if (version >= 2) {
        identity.slot_id = net_read_u8(&s);
        net_read_raw(&s, identity.world_instance_uuid, MP_WORLD_UUID_SIZE);
        net_read_string(&s, identity.last_host_address, sizeof(identity.last_host_address));
        identity.last_host_port = net_read_u16(&s);
        net_read_string(&s, identity.player_name, sizeof(identity.player_name));
        identity.last_session_id = net_read_u32(&s);
        identity.resume_generation = net_read_u32(&s);
    } else {
        identity.slot_id = 0xFF;
        net_read_raw(&s, identity.world_instance_uuid, MP_WORLD_UUID_SIZE);
        net_read_string(&s, identity.last_host_address, sizeof(identity.last_host_address));
        identity.last_host_port = net_read_u16(&s);
        net_read_string(&s, identity.player_name, sizeof(identity.player_name));
        identity.last_session_id = net_read_u32(&s);
        identity.resume_generation = 0;
    }

    if (net_serializer_has_overflow(&s)) {
        MP_LOG_WARN("IDENTITY", "Identity file truncated");
        memset(&identity, 0, sizeof(identity));
        return 0;
    }

    identity_loaded = 1;
    MP_LOG_INFO("IDENTITY", "Identity loaded: session=0x%08x name='%s'",
                identity.last_session_id, identity.player_name);
    return 1;
}

void mp_client_identity_clear(void)
{
    const char *path = get_identity_path();
    if (path) {
        remove(path);
    }
    memset(&identity, 0, sizeof(identity));
    identity.slot_id = 0xFF;
    identity_loaded = 0;
    MP_LOG_INFO("IDENTITY", "Identity cleared");
}

int mp_client_identity_get_for_hello(uint8_t *out_uuid, uint8_t *out_token)
{
    if (!identity_loaded) {
        return 0;
    }

    /* Check that we actually have a UUID (not all zeros) */
    int has_uuid = 0;
    for (int i = 0; i < 16; i++) {
        if (identity.player_uuid[i] != 0) {
            has_uuid = 1;
            break;
        }
    }
    if (!has_uuid) {
        return 0;
    }

    if (out_uuid) {
        memcpy(out_uuid, identity.player_uuid, 16);
    }
    if (out_token) {
        memcpy(out_token, identity.reconnect_token, 16);
    }
    return 1;
}

int mp_client_identity_matches_world(const uint8_t *world_uuid)
{
    if (!identity_loaded || !world_uuid) {
        return 0;
    }
    return memcmp(identity.world_instance_uuid, world_uuid, MP_WORLD_UUID_SIZE) == 0;
}

const mp_client_identity *mp_client_identity_get(void)
{
    return &identity;
}

#endif /* ENABLE_MULTIPLAYER */
