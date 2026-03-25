#ifndef MULTIPLAYER_SAVE_TRANSFER_H
#define MULTIPLAYER_SAVE_TRANSFER_H

#ifdef ENABLE_MULTIPLAYER

#include <stdint.h>

/**
 * Chunked save file transfer from host to clients.
 *
 * The host reads the full save file and sends it in 8KB chunks over TCP.
 * Clients reassemble the chunks and verify integrity via FNV-1a checksum.
 * This replaces the broken snapshot-only approach where clients had zero
 * base game state (buildings, figures, terrain) after resume.
 *
 * Wire format:
 *   BEGIN:    total_size(u32) + chunk_size(u32) + chunk_count(u32) + checksum(u32)
 *   CHUNK:   chunk_index(u32) + offset(u32) + size(u32) + raw_data[]
 *   COMPLETE: total_size(u32) + checksum(u32)
 */

#define MP_SAVE_TRANSFER_CHUNK_SIZE   8192
#define MP_SAVE_TRANSFER_MAX_SIZE     (8 * 1024 * 1024)  /* 8MB max save file */
#define MP_SAVE_TRANSFER_CHUNKS_PER_FRAME  4              /* 32KB/frame throughput */

typedef enum {
    MP_TRANSFER_IDLE = 0,
    MP_TRANSFER_SENDING,
    MP_TRANSFER_RECEIVING,
    MP_TRANSFER_COMPLETE,
    MP_TRANSFER_FAILED
} mp_transfer_state;

void mp_save_transfer_init(void);
void mp_save_transfer_reset(void);

/* Host API */
int mp_save_transfer_host_begin(const char *save_path);
int mp_save_transfer_host_update(void);   /* Returns 1 if still in progress */
int mp_save_transfer_host_is_complete(void);

/* Client handlers (called from session.c message dispatch) */
void mp_save_transfer_client_receive_begin(const uint8_t *payload, uint32_t size);
void mp_save_transfer_client_receive_chunk(const uint8_t *payload, uint32_t size);
void mp_save_transfer_client_receive_complete(const uint8_t *payload, uint32_t size);

/* Client result */
const uint8_t *mp_save_transfer_client_get_data(uint32_t *out_size);
mp_transfer_state mp_save_transfer_get_state(void);
float mp_save_transfer_get_progress(void);

#endif /* ENABLE_MULTIPLAYER */

#endif /* MULTIPLAYER_SAVE_TRANSFER_H */
