#ifndef MULTIPLAYER_JOIN_TRANSACTION_H
#define MULTIPLAYER_JOIN_TRANSACTION_H

#ifdef ENABLE_MULTIPLAYER

#include <stdint.h>

/**
 * Join transaction tracking for rollback on incomplete joins.
 *
 * When a client disconnects during the join process (save transfer,
 * state loading, etc.), partial state must be cleaned up:
 * - Ownership records for assigned city
 * - Player registry entry
 * - Empire sync registration
 * - Reserved city pool restoration
 * - Join barrier release
 *
 * Transactions are created when a late join begins and committed
 * or rolled back when the join completes or fails.
 */

#define MP_MAX_JOIN_TRANSACTIONS 4
#define MP_JOIN_TIMEOUT_MS       30000  /* 30 seconds */

typedef struct {
    int active;
    uint8_t peer_index;
    uint8_t player_id;
    uint8_t slot_id;
    int assigned_city_id;
    /* Rollback flags — track what was created so we know what to undo */
    int ownership_created;
    int registry_created;
    int empire_sync_registered;
    uint32_t start_ms;
} mp_join_transaction;

void mp_join_transaction_init(void);

/**
 * Begin tracking a join transaction.
 * @return Pointer to the transaction, or NULL if no slots available
 */
mp_join_transaction *mp_join_transaction_begin(uint8_t peer_index);

/**
 * Find a transaction by peer index.
 * @return Pointer to the transaction, or NULL if not found
 */
mp_join_transaction *mp_join_transaction_find_by_peer(uint8_t peer_index);

/**
 * Commit a transaction (join completed successfully).
 * Clears the transaction without rolling back.
 */
void mp_join_transaction_commit(mp_join_transaction *txn);

/**
 * Roll back a transaction (join failed).
 * Undoes all tracked state changes.
 */
void mp_join_transaction_rollback(mp_join_transaction *txn);

/**
 * Check for timed-out transactions and roll them back.
 * @param current_ms Current time in milliseconds
 */
void mp_join_transaction_check_timeouts(uint32_t current_ms);

#endif /* ENABLE_MULTIPLAYER */

#endif /* MULTIPLAYER_JOIN_TRANSACTION_H */
