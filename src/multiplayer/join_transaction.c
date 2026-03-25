#include "join_transaction.h"

#ifdef ENABLE_MULTIPLAYER

#include "ownership.h"
#include "empire_sync.h"
#include "player_registry.h"
#include "worldgen.h"
#include "time_sync.h"
#include "mp_debug_log.h"

#include <string.h>

static mp_join_transaction transactions[MP_MAX_JOIN_TRANSACTIONS];

void mp_join_transaction_init(void)
{
    memset(transactions, 0, sizeof(transactions));
}

mp_join_transaction *mp_join_transaction_begin(uint8_t peer_index)
{
    for (int i = 0; i < MP_MAX_JOIN_TRANSACTIONS; i++) {
        if (!transactions[i].active) {
            memset(&transactions[i], 0, sizeof(mp_join_transaction));
            transactions[i].active = 1;
            transactions[i].peer_index = peer_index;
            transactions[i].assigned_city_id = -1;
            MP_LOG_INFO("JOIN_TXN", "Transaction started for peer %d", (int)peer_index);
            return &transactions[i];
        }
    }
    MP_LOG_ERROR("JOIN_TXN", "No free transaction slots");
    return NULL;
}

mp_join_transaction *mp_join_transaction_find_by_peer(uint8_t peer_index)
{
    for (int i = 0; i < MP_MAX_JOIN_TRANSACTIONS; i++) {
        if (transactions[i].active && transactions[i].peer_index == peer_index) {
            return &transactions[i];
        }
    }
    return NULL;
}

void mp_join_transaction_commit(mp_join_transaction *txn)
{
    if (!txn || !txn->active) {
        return;
    }
    MP_LOG_INFO("JOIN_TXN", "Transaction committed for peer %d (player %d, city %d)",
                (int)txn->peer_index, (int)txn->player_id, txn->assigned_city_id);
    memset(txn, 0, sizeof(mp_join_transaction));
}

void mp_join_transaction_rollback(mp_join_transaction *txn)
{
    if (!txn || !txn->active) {
        return;
    }

    MP_LOG_INFO("JOIN_TXN", "Rolling back transaction for peer %d (player %d)",
                (int)txn->peer_index, (int)txn->player_id);

    /* Undo in reverse order of creation */
    if (txn->empire_sync_registered) {
        mp_empire_sync_unregister_player_city(txn->player_id);
    }

    if (txn->ownership_created && txn->assigned_city_id >= 0) {
        mp_ownership_set_city(txn->assigned_city_id, MP_OWNER_RESERVED, 0);
        mp_worldgen_return_to_reserved(txn->assigned_city_id);
    }

    if (txn->registry_created) {
        mp_player_registry_remove(txn->player_id);
    }

    /* Release join barrier */
    mp_time_sync_set_join_barrier(0);

    memset(txn, 0, sizeof(mp_join_transaction));
}

void mp_join_transaction_check_timeouts(uint32_t current_ms)
{
    for (int i = 0; i < MP_MAX_JOIN_TRANSACTIONS; i++) {
        if (transactions[i].active && transactions[i].start_ms > 0) {
            if (current_ms - transactions[i].start_ms > MP_JOIN_TIMEOUT_MS) {
                MP_LOG_WARN("JOIN_TXN", "Transaction timed out for peer %d after %u ms",
                            (int)transactions[i].peer_index, MP_JOIN_TIMEOUT_MS);
                mp_join_transaction_rollback(&transactions[i]);
            }
        }
    }
}

#endif /* ENABLE_MULTIPLAYER */
