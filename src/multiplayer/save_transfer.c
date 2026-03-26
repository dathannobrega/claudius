#include "save_transfer.h"

#ifdef ENABLE_MULTIPLAYER

#include "core/log.h"
#include "mp_debug_log.h"
#include "network/protocol.h"
#include "network/serialize.h"
#include "network/session.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static struct {
    mp_transfer_state state;
    struct {
        uint8_t *file_data;
        uint32_t file_size;
        uint32_t checksum;
        uint32_t chunk_count;
        uint32_t chunks_sent;
        uint32_t target_peer_mask;
    } host;
    struct {
        uint8_t *recv_buffer;
        uint32_t total_size;
        uint32_t received_size;
        uint32_t checksum;
        uint32_t chunk_count;
        uint32_t chunks_received;
    } client;
} transfer_data;

void mp_save_transfer_init(void)
{
    mp_save_transfer_reset();
}

void mp_save_transfer_reset(void)
{
    if (transfer_data.host.file_data) {
        free(transfer_data.host.file_data);
    }
    if (transfer_data.client.recv_buffer) {
        free(transfer_data.client.recv_buffer);
    }
    memset(&transfer_data, 0, sizeof(transfer_data));
}

static uint32_t build_active_peer_mask(void)
{
    uint32_t mask = 0;
    for (int i = 0; i < NET_MAX_PEERS; i++) {
        const net_peer *peer = net_session_get_peer(i);
        if (peer && peer->active) {
            mask |= (1u << i);
        }
    }
    return mask;
}

static int should_send_to_peer(int peer_index)
{
    if (peer_index < 0 || peer_index >= NET_MAX_PEERS) {
        return 0;
    }
    return (transfer_data.host.target_peer_mask & (1u << peer_index)) != 0;
}

static int host_begin_with_mask(const char *save_path, uint32_t peer_mask)
{
    if (!save_path || !save_path[0]) {
        MP_LOG_ERROR("TRANSFER", "No save path provided");
        return 0;
    }
    if (peer_mask == 0) {
        MP_LOG_ERROR("TRANSFER", "No target peers for save transfer");
        return 0;
    }

    {
        FILE *f = fopen(save_path, "rb");
        long file_len;
        uint8_t *data;
        size_t read_count;
        uint32_t file_size;

        if (!f) {
            MP_LOG_ERROR("TRANSFER", "Failed to open save file: %s", save_path);
            return 0;
        }

        fseek(f, 0, SEEK_END);
        file_len = ftell(f);
        fseek(f, 0, SEEK_SET);

        if (file_len <= 0 || (uint32_t)file_len > MP_SAVE_TRANSFER_MAX_SIZE) {
            MP_LOG_ERROR("TRANSFER", "Save file size invalid: %ld bytes (max %d)",
                         file_len, MP_SAVE_TRANSFER_MAX_SIZE);
            fclose(f);
            return 0;
        }

        file_size = (uint32_t)file_len;
        data = (uint8_t *)malloc(file_size);
        if (!data) {
            MP_LOG_ERROR("TRANSFER", "Failed to allocate %u bytes for save file", file_size);
            fclose(f);
            return 0;
        }

        read_count = fread(data, 1, file_size, f);
        fclose(f);

        if (read_count != file_size) {
            MP_LOG_ERROR("TRANSFER", "Read only %zu of %u bytes", read_count, file_size);
            free(data);
            return 0;
        }

        if (transfer_data.host.file_data) {
            free(transfer_data.host.file_data);
        }

        transfer_data.host.file_data = data;
        transfer_data.host.file_size = file_size;
        transfer_data.host.checksum = compute_fnv1a(data, file_size);
        transfer_data.host.chunk_count =
            (file_size + MP_SAVE_TRANSFER_CHUNK_SIZE - 1) / MP_SAVE_TRANSFER_CHUNK_SIZE;
        transfer_data.host.chunks_sent = 0;
        transfer_data.host.target_peer_mask = peer_mask;
        transfer_data.state = MP_TRANSFER_SENDING;
    }

    {
        uint8_t begin_buf[16];
        net_serializer s;
        net_serializer_init(&s, begin_buf, sizeof(begin_buf));
        net_write_u32(&s, transfer_data.host.file_size);
        net_write_u32(&s, MP_SAVE_TRANSFER_CHUNK_SIZE);
        net_write_u32(&s, transfer_data.host.chunk_count);
        net_write_u32(&s, transfer_data.host.checksum);

        for (int i = 0; i < NET_MAX_PEERS; i++) {
            const net_peer *peer = net_session_get_peer(i);
            if (peer && peer->active && should_send_to_peer(i)) {
                net_session_send_to_peer(i, NET_MSG_SAVE_TRANSFER_BEGIN,
                    begin_buf, (uint32_t)net_serializer_position(&s));
            }
        }
    }

    MP_LOG_INFO("TRANSFER", "Save transfer started: %u bytes, %u chunks, checksum=0x%08x mask=0x%02x",
                transfer_data.host.file_size,
                transfer_data.host.chunk_count,
                transfer_data.host.checksum,
                (unsigned int)transfer_data.host.target_peer_mask);
    return 1;
}

int mp_save_transfer_host_begin(const char *save_path)
{
    return host_begin_with_mask(save_path, build_active_peer_mask());
}

int mp_save_transfer_host_begin_for_peer(const char *save_path, int peer_index)
{
    if (peer_index < 0 || peer_index >= NET_MAX_PEERS) {
        MP_LOG_ERROR("TRANSFER", "Invalid target peer index: %d", peer_index);
        return 0;
    }
    return host_begin_with_mask(save_path, (uint32_t)(1u << peer_index));
}

int mp_save_transfer_host_update(void)
{
    if (transfer_data.state != MP_TRANSFER_SENDING) {
        return 0;
    }

    {
        int chunks_this_frame = 0;
        while (chunks_this_frame < MP_SAVE_TRANSFER_CHUNKS_PER_FRAME &&
               transfer_data.host.chunks_sent < transfer_data.host.chunk_count) {
            uint32_t chunk_index = transfer_data.host.chunks_sent;
            uint32_t offset = chunk_index * MP_SAVE_TRANSFER_CHUNK_SIZE;
            uint32_t remaining = transfer_data.host.file_size - offset;
            uint32_t chunk_size = remaining < MP_SAVE_TRANSFER_CHUNK_SIZE
                ? remaining
                : MP_SAVE_TRANSFER_CHUNK_SIZE;
            uint8_t chunk_buf[12 + MP_SAVE_TRANSFER_CHUNK_SIZE];
            net_serializer s;

            net_serializer_init(&s, chunk_buf, sizeof(chunk_buf));
            net_write_u32(&s, chunk_index);
            net_write_u32(&s, offset);
            net_write_u32(&s, chunk_size);
            net_write_raw(&s, transfer_data.host.file_data + offset, chunk_size);

            for (int i = 0; i < NET_MAX_PEERS; i++) {
                const net_peer *peer = net_session_get_peer(i);
                if (peer && peer->active && should_send_to_peer(i)) {
                    net_session_send_to_peer(i, NET_MSG_SAVE_TRANSFER_CHUNK,
                        chunk_buf, (uint32_t)net_serializer_position(&s));
                }
            }

            transfer_data.host.chunks_sent++;
            chunks_this_frame++;
        }
    }

    if (transfer_data.host.chunks_sent >= transfer_data.host.chunk_count) {
        uint8_t complete_buf[8];
        net_serializer s;
        net_serializer_init(&s, complete_buf, sizeof(complete_buf));
        net_write_u32(&s, transfer_data.host.file_size);
        net_write_u32(&s, transfer_data.host.checksum);

        for (int i = 0; i < NET_MAX_PEERS; i++) {
            const net_peer *peer = net_session_get_peer(i);
            if (peer && peer->active && should_send_to_peer(i)) {
                net_session_send_to_peer(i, NET_MSG_SAVE_TRANSFER_COMPLETE,
                    complete_buf, (uint32_t)net_serializer_position(&s));
            }
        }

        transfer_data.state = MP_TRANSFER_COMPLETE;
        free(transfer_data.host.file_data);
        transfer_data.host.file_data = NULL;

        MP_LOG_INFO("TRANSFER", "Save transfer complete: %u chunks sent",
                    transfer_data.host.chunks_sent);
        return 0;
    }

    return 1;
}

int mp_save_transfer_host_is_complete(void)
{
    return transfer_data.state == MP_TRANSFER_COMPLETE;
}

void mp_save_transfer_client_receive_begin(const uint8_t *payload, uint32_t size)
{
    if (size < 16) {
        MP_LOG_ERROR("TRANSFER", "BEGIN message too small: %u bytes", size);
        transfer_data.state = MP_TRANSFER_FAILED;
        return;
    }

    {
        net_serializer s;
        uint32_t total_size;
        uint32_t chunk_size;
        uint32_t chunk_count;
        uint32_t checksum;

        net_serializer_init(&s, (uint8_t *)payload, size);
        total_size = net_read_u32(&s);
        chunk_size = net_read_u32(&s);
        chunk_count = net_read_u32(&s);
        checksum = net_read_u32(&s);
        (void)chunk_size;

        if (total_size == 0 || total_size > MP_SAVE_TRANSFER_MAX_SIZE) {
            MP_LOG_ERROR("TRANSFER", "Invalid transfer size: %u", total_size);
            transfer_data.state = MP_TRANSFER_FAILED;
            return;
        }

        if (transfer_data.client.recv_buffer) {
            free(transfer_data.client.recv_buffer);
        }

        transfer_data.client.recv_buffer = (uint8_t *)malloc(total_size);
        if (!transfer_data.client.recv_buffer) {
            MP_LOG_ERROR("TRANSFER", "Failed to allocate %u bytes for receive buffer", total_size);
            transfer_data.state = MP_TRANSFER_FAILED;
            return;
        }

        memset(transfer_data.client.recv_buffer, 0, total_size);
        transfer_data.client.total_size = total_size;
        transfer_data.client.received_size = 0;
        transfer_data.client.checksum = checksum;
        transfer_data.client.chunk_count = chunk_count;
        transfer_data.client.chunks_received = 0;
        transfer_data.state = MP_TRANSFER_RECEIVING;

        MP_LOG_INFO("TRANSFER", "Save transfer begin: expecting %u bytes in %u chunks, checksum=0x%08x",
                    total_size, chunk_count, checksum);
    }
}

void mp_save_transfer_client_receive_chunk(const uint8_t *payload, uint32_t size)
{
    if (transfer_data.state != MP_TRANSFER_RECEIVING) {
        MP_LOG_WARN("TRANSFER", "Received chunk while not in RECEIVING state");
        return;
    }
    if (size < 12) {
        MP_LOG_ERROR("TRANSFER", "CHUNK message too small: %u bytes", size);
        return;
    }

    {
        net_serializer s;
        uint32_t chunk_index;
        uint32_t offset;
        uint32_t chunk_size;

        net_serializer_init(&s, (uint8_t *)payload, size);
        chunk_index = net_read_u32(&s);
        offset = net_read_u32(&s);
        chunk_size = net_read_u32(&s);

        if (offset + chunk_size > transfer_data.client.total_size) {
            MP_LOG_ERROR("TRANSFER", "Chunk %u overflows buffer: offset=%u size=%u total=%u",
                         chunk_index, offset, chunk_size, transfer_data.client.total_size);
            transfer_data.state = MP_TRANSFER_FAILED;
            return;
        }
        if (size < 12 + chunk_size) {
            MP_LOG_ERROR("TRANSFER", "Chunk %u payload too small: need %u, have %u",
                         chunk_index, 12 + chunk_size, size);
            return;
        }

        memcpy(transfer_data.client.recv_buffer + offset, payload + 12, chunk_size);
        transfer_data.client.received_size += chunk_size;
        transfer_data.client.chunks_received++;

        if (transfer_data.client.chunks_received % 32 == 0) {
            MP_LOG_TRACE("TRANSFER", "Chunk %u/%u received (%u/%u bytes)",
                         transfer_data.client.chunks_received,
                         transfer_data.client.chunk_count,
                         transfer_data.client.received_size,
                         transfer_data.client.total_size);
        }
    }
}

void mp_save_transfer_client_receive_complete(const uint8_t *payload, uint32_t size)
{
    if (transfer_data.state != MP_TRANSFER_RECEIVING) {
        MP_LOG_WARN("TRANSFER", "Received COMPLETE while not in RECEIVING state");
        return;
    }
    if (size < 8) {
        MP_LOG_ERROR("TRANSFER", "COMPLETE message too small: %u bytes", size);
        transfer_data.state = MP_TRANSFER_FAILED;
        return;
    }

    {
        net_serializer s;
        uint32_t total_size;
        uint32_t checksum;
        uint32_t actual_checksum;

        net_serializer_init(&s, (uint8_t *)payload, size);
        total_size = net_read_u32(&s);
        checksum = net_read_u32(&s);

        if (transfer_data.client.received_size != total_size) {
            MP_LOG_ERROR("TRANSFER", "Size mismatch: received %u, expected %u",
                         transfer_data.client.received_size, total_size);
            transfer_data.state = MP_TRANSFER_FAILED;
            return;
        }

        actual_checksum = compute_fnv1a(transfer_data.client.recv_buffer,
                                        transfer_data.client.total_size);
        if (actual_checksum != checksum) {
            MP_LOG_ERROR("TRANSFER", "Checksum mismatch: computed=0x%08x, expected=0x%08x",
                         actual_checksum, checksum);
            transfer_data.state = MP_TRANSFER_FAILED;
            return;
        }

        transfer_data.state = MP_TRANSFER_COMPLETE;

        MP_LOG_INFO("TRANSFER", "Save transfer verified: %u bytes, %u chunks, checksum=0x%08x",
                    total_size, transfer_data.client.chunks_received, checksum);
    }
}

const uint8_t *mp_save_transfer_client_get_data(uint32_t *out_size)
{
    if (transfer_data.state != MP_TRANSFER_COMPLETE || !transfer_data.client.recv_buffer) {
        if (out_size) {
            *out_size = 0;
        }
        return NULL;
    }
    if (out_size) {
        *out_size = transfer_data.client.total_size;
    }
    return transfer_data.client.recv_buffer;
}

mp_transfer_state mp_save_transfer_get_state(void)
{
    return transfer_data.state;
}

float mp_save_transfer_get_progress(void)
{
    if (transfer_data.state == MP_TRANSFER_SENDING) {
        if (transfer_data.host.chunk_count == 0) {
            return 0.0f;
        }
        return (float)transfer_data.host.chunks_sent /
               (float)transfer_data.host.chunk_count;
    }
    if (transfer_data.state == MP_TRANSFER_RECEIVING) {
        if (transfer_data.client.chunk_count == 0) {
            return 0.0f;
        }
        return (float)transfer_data.client.chunks_received /
               (float)transfer_data.client.chunk_count;
    }
    if (transfer_data.state == MP_TRANSFER_COMPLETE) {
        return 1.0f;
    }
    return 0.0f;
}

#endif /* ENABLE_MULTIPLAYER */
