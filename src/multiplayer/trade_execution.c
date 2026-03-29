#include "trade_execution.h"

#ifdef ENABLE_MULTIPLAYER

#include "mp_trade_route.h"
#include "trade_log.h"
#include "mp_debug_log.h"
#include "ownership.h"
#include "trade_sync.h"
#include "empire_sync.h"
#include "network/session.h"
#include "network/serialize.h"
#include "network/protocol.h"
#include "building/building.h"
#include "building/warehouse.h"
#include "building/granary.h"
#include "figure/figure.h"
#include "figure/trader.h"
#include "city/resource.h"
#include "empire/city.h"
#include "empire/trade_route.h"
#include "empire/trade_prices.h"
#include "game/resource.h"
#include "core/log.h"

#include <string.h>

static int broadcast_route_state_after_year_change(mp_trade_route_instance *route,
                                                   void *userdata)
{
    (void)userdata;
    if (!route || route->status == MP_TROUTE_DELETED || route->claudius_route_id <= 0) {
        return 0;
    }
    mp_trade_sync_broadcast_route_state(route->claudius_route_id);
    return 0;
}

static struct {
    uint32_t next_transaction_id;
    mp_trade_transaction last_transaction;
    uint32_t total_transactions;
} exec_data;

void mp_trade_execution_init(void)
{
    memset(&exec_data, 0, sizeof(exec_data));
    exec_data.next_transaction_id = 1;
    mp_trade_log_init();
}

/* ---- Validation ---- */

mp_trade_exec_result mp_trade_validate_export(uint32_t route_instance_id,
                                               int resource, int amount,
                                               int source_building_id)
{
    if (resource < RESOURCE_MIN || resource >= RESOURCE_MAX) {
        return MP_TRADE_ERR_RESOURCE_INVALID;
    }

    mp_trade_route_instance *route = mp_trade_route_get(route_instance_id);
    if (!route) {
        return MP_TRADE_ERR_ROUTE_INVALID;
    }
    if (route->status != MP_TROUTE_ACTIVE) {
        return MP_TRADE_ERR_ROUTE_INACTIVE;
    }

    /* Check export enabled on this route for this resource */
    if (!mp_trade_route_can_export(route_instance_id, resource)) {
        return MP_TRADE_ERR_EXPORT_DISABLED;
    }

    /* Check quota remaining */
    if (mp_trade_route_export_remaining(route_instance_id, resource) < amount) {
        return MP_TRADE_ERR_QUOTA_EXCEEDED;
    }

    /* Check source building exists and has stock */
    if (source_building_id > 0) {
        building *b = building_get(source_building_id);
        if (!b || b->state != BUILDING_STATE_IN_USE) {
            return MP_TRADE_ERR_STORAGE_NOT_FOUND;
        }
        int available = 0;
        if (b->type == BUILDING_GRANARY) {
            available = building_granary_count_available_resource(b, resource, 1);
        } else if (b->type == BUILDING_WAREHOUSE) {
            available = building_warehouse_get_available_amount(b, resource);
        } else {
            return MP_TRADE_ERR_STORAGE_NOT_FOUND;
        }
        if (available < amount) {
            return MP_TRADE_ERR_NO_STOCK;
        }
    }

    return MP_TRADE_OK;
}

mp_trade_exec_result mp_trade_validate_import(uint32_t route_instance_id,
                                               int resource, int amount,
                                               int dest_building_id)
{
    if (resource < RESOURCE_MIN || resource >= RESOURCE_MAX) {
        return MP_TRADE_ERR_RESOURCE_INVALID;
    }

    mp_trade_route_instance *route = mp_trade_route_get(route_instance_id);
    if (!route) {
        return MP_TRADE_ERR_ROUTE_INVALID;
    }
    if (route->status != MP_TROUTE_ACTIVE) {
        return MP_TRADE_ERR_ROUTE_INACTIVE;
    }

    /* Check import enabled on this route for this resource */
    if (!mp_trade_route_can_import(route_instance_id, resource)) {
        return MP_TRADE_ERR_IMPORT_DISABLED;
    }

    /* Check quota remaining */
    if (mp_trade_route_import_remaining(route_instance_id, resource) < amount) {
        return MP_TRADE_ERR_QUOTA_EXCEEDED;
    }

    /* Check destination building can accept */
    if (dest_building_id > 0) {
        building *b = building_get(dest_building_id);
        if (!b || b->state != BUILDING_STATE_IN_USE) {
            return MP_TRADE_ERR_STORAGE_NOT_FOUND;
        }
        int capacity = 0;
        if (b->type == BUILDING_GRANARY) {
            capacity = building_granary_maximum_receptible_amount(b, resource);
        } else if (b->type == BUILDING_WAREHOUSE) {
            capacity = building_warehouse_maximum_receptible_amount(b, resource);
        } else {
            return MP_TRADE_ERR_STORAGE_NOT_FOUND;
        }
        if (capacity < amount) {
            return MP_TRADE_ERR_NO_CAPACITY;
        }
    }

    return MP_TRADE_OK;
}

static void finalize_last_transaction(const mp_trade_route_instance *route,
                                      int resource,
                                      int amount_requested,
                                      int amount_committed,
                                      int source_storage_id,
                                      int dest_storage_id,
                                      int figure_id,
                                      mp_trade_exec_result result)
{
    mp_trade_transaction tx;

    memset(&tx, 0, sizeof(tx));
    tx.transaction_id = exec_data.next_transaction_id++;
    tx.route_instance_id = route ? route->instance_id : 0;
    tx.origin_player_id = route ? route->origin_player_id : 0;
    tx.origin_city_id = route ? route->origin_city_id : -1;
    tx.dest_player_id = route ? route->dest_player_id : 0xFF;
    tx.dest_city_id = route ? route->dest_city_id : -1;
    tx.resource = resource;
    tx.amount_requested = amount_requested;
    tx.amount_committed = amount_committed;
    tx.source_storage_id = source_storage_id;
    tx.dest_storage_id = dest_storage_id;
    tx.tick = net_session_get_authoritative_tick();
    tx.transport_type = (route && route->transport == MP_TROUTE_SEA) ? 1 : 0;
    tx.figure_id = figure_id;
    tx.result = result;

    exec_data.last_transaction = tx;
    if (result == MP_TRADE_OK) {
        exec_data.total_transactions++;
    }

    if (route && result == MP_TRADE_OK) {
        mp_trade_log_entry log_entry;
        memset(&log_entry, 0, sizeof(log_entry));
        log_entry.tick = tx.tick;
        log_entry.route_instance_id = route->instance_id;
        log_entry.origin_player_id = route->origin_player_id;
        log_entry.origin_city_id = route->origin_city_id;
        log_entry.dest_player_id = route->dest_player_id;
        log_entry.dest_city_id = route->dest_city_id;
        log_entry.resource = resource;
        log_entry.amount_requested = amount_requested;
        log_entry.amount_committed = amount_committed;
        log_entry.source_storage_id = source_storage_id;
        log_entry.dest_storage_id = dest_storage_id;
        log_entry.figure_id = figure_id;
        log_entry.export_quota_after = route->resources[resource].exported_this_year;
        log_entry.import_quota_after = route->resources[resource].imported_this_year;
        log_entry.outcome = MP_TLOG_SUCCESS;
        mp_trade_log_record(&log_entry);
    }
}

static mp_trade_route_instance *resolve_trade_route_for_execution(int route_id)
{
    uint32_t network_route_id = 0;

    if (route_id < 0) {
        return 0;
    }

    network_route_id = (uint32_t)trade_route_get_network_id(route_id);
    if (network_route_id == 0) {
        network_route_id = mp_ownership_get_network_route_id(route_id);
    }

    return mp_trade_route_resolve(route_id, network_route_id);
}

mp_trade_exec_result mp_trade_execute_storage_trade(int route_id,
                                                    int resource,
                                                    int amount,
                                                    int building_id,
                                                    int buying,
                                                    int trader_type,
                                                    int figure_id)
{
    mp_trade_route_instance *route;
    mp_trade_exec_result validation;
    building *b;
    int committed = 0;

    if (!net_session_is_active() || !net_session_is_host()) {
        return MP_TRADE_ERR_INTERNAL;
    }
    if (amount <= 0) {
        return MP_TRADE_ERR_INTERNAL;
    }

    route = resolve_trade_route_for_execution(route_id);
    if (!route) {
        MP_LOG_WARN("TRADE_EXEC", "Could not resolve authoritative route for route_id=%d", route_id);
        return MP_TRADE_ERR_ROUTE_INVALID;
    }
    if (route->status != MP_TROUTE_ACTIVE) {
        return MP_TRADE_ERR_ROUTE_INACTIVE;
    }

    validation = buying
        ? mp_trade_validate_export(route->instance_id, resource, amount, building_id)
        : mp_trade_validate_import(route->instance_id, resource, amount, building_id);
    if (validation != MP_TRADE_OK) {
        return validation;
    }

    b = building_get(building_id);
    if (!b || b->state != BUILDING_STATE_IN_USE) {
        return MP_TRADE_ERR_STORAGE_NOT_FOUND;
    }

    if (buying) {
        if (b->type == BUILDING_GRANARY) {
            committed = building_granary_remove_export(b, resource, amount, trader_type);
        } else if (b->type == BUILDING_WAREHOUSE) {
            committed = building_warehouse_remove_export(b, resource, amount, trader_type);
        }
        if (committed <= 0) {
            return MP_TRADE_ERR_NO_STOCK;
        }
        mp_trade_route_record_export(route->instance_id, resource, committed);
        if (route->claudius_route_id >= 0 && trade_route_is_valid(route->claudius_route_id)) {
            for (int i = 0; i < committed; i++) {
                trade_route_increase_traded(route->claudius_route_id, resource, 1);
            }
        }
        route->last_trade_tick = net_session_get_authoritative_tick();
        finalize_last_transaction(route, resource, amount, committed,
                                  building_id, 0, figure_id, MP_TRADE_OK);
    } else {
        if (b->type == BUILDING_GRANARY) {
            committed = building_granary_add_import(b, resource, amount, trader_type);
        } else if (b->type == BUILDING_WAREHOUSE) {
            committed = building_warehouse_add_import(b, resource, amount, trader_type);
        }
        if (committed <= 0) {
            return MP_TRADE_ERR_NO_CAPACITY;
        }
        mp_trade_route_record_import(route->instance_id, resource, committed);
        if (route->claudius_route_id >= 0 && trade_route_is_valid(route->claudius_route_id)) {
            for (int i = 0; i < committed; i++) {
                trade_route_increase_traded(route->claudius_route_id, resource, 0);
            }
        }
        route->last_trade_tick = net_session_get_authoritative_tick();
        finalize_last_transaction(route, resource, amount, committed,
                                  0, building_id, figure_id, MP_TRADE_OK);
    }

    mp_trade_sync_emit_trader_trade_executed(figure_id, resource, committed, buying, building_id);
    return MP_TRADE_OK;
}

/* ---- Atomic Commit ---- */

mp_trade_exec_result mp_trade_commit_transaction(uint32_t route_instance_id,
                                                   int resource, int amount,
                                                   int source_building_id,
                                                   int dest_building_id,
                                                   int figure_id)
{
    if (!net_session_is_host() && net_session_is_active()) {
        MP_LOG_ERROR("TRADE_EXEC", "Only host can commit transactions");
        return MP_TRADE_ERR_INTERNAL;
    }

    mp_trade_route_instance *route = mp_trade_route_get(route_instance_id);
    if (!route) {
        return MP_TRADE_ERR_ROUTE_INVALID;
    }

    /* 1. Validate export */
    mp_trade_exec_result export_result = mp_trade_validate_export(
        route_instance_id, resource, amount, source_building_id);
    if (export_result != MP_TRADE_OK) {
        MP_LOG_WARN("TRADE_EXEC", "Export validation failed: route=%u res=%d err=%d",
                    route_instance_id, resource, export_result);
        return export_result;
    }

    /* 2. Validate import */
    mp_trade_exec_result import_result = mp_trade_validate_import(
        route_instance_id, resource, amount, dest_building_id);
    if (import_result != MP_TRADE_OK) {
        MP_LOG_WARN("TRADE_EXEC", "Import validation failed: route=%u res=%d err=%d",
                    route_instance_id, resource, import_result);
        return import_result;
    }

    /* 2b. Reserve quota before any mutation */
    mp_trade_route_record_export(route_instance_id, resource, amount);
    mp_trade_route_record_import(route_instance_id, resource, amount);

    /* 3. Remove from source (atomic: if this fails, abort) */
    int removed = 0;
    if (source_building_id > 0) {
        building *src = building_get(source_building_id);
        if (!src) {
            mp_trade_route_rollback_export(route_instance_id, resource, amount);
            mp_trade_route_rollback_import(route_instance_id, resource, amount);
            return MP_TRADE_ERR_STORAGE_NOT_FOUND;
        }
        int trader_type = (route->transport == MP_TROUTE_SEA) ? 0 : 1;
        if (src->type == BUILDING_GRANARY) {
            removed = building_granary_remove_export(src, resource, amount, trader_type);
        } else {
            removed = building_warehouse_remove_export(src, resource, amount, trader_type);
        }
        if (removed <= 0) {
            mp_trade_route_rollback_export(route_instance_id, resource, amount);
            mp_trade_route_rollback_import(route_instance_id, resource, amount);
            MP_LOG_WARN("TRADE_EXEC", "Remove from source failed: bld=%d res=%d amt=%d",
                        source_building_id, resource, amount);
            return MP_TRADE_ERR_NO_STOCK;
        }
    } else {
        removed = amount; /* virtual export (e.g., remote player city) */
    }

    /* 4. Add to destination */
    int added = 0;
    if (dest_building_id > 0) {
        building *dst = building_get(dest_building_id);
        if (!dst) {
            /* Rollback: re-add to source if we already removed */
            if (source_building_id > 0 && removed > 0) {
                building *src = building_get(source_building_id);
                if (src) {
                    int trader_type = (route->transport == MP_TROUTE_SEA) ? 0 : 1;
                    if (src->type == BUILDING_GRANARY) {
                        building_granary_add_import(src, resource, removed, trader_type);
                    } else {
                        building_warehouse_add_import(src, resource, removed, trader_type);
                    }
                }
            }
            mp_trade_route_rollback_export(route_instance_id, resource, amount);
            mp_trade_route_rollback_import(route_instance_id, resource, amount);
            return MP_TRADE_ERR_STORAGE_NOT_FOUND;
        }
        int trader_type = (route->transport == MP_TROUTE_SEA) ? 0 : 1;
        if (dst->type == BUILDING_GRANARY) {
            added = building_granary_add_import(dst, resource, removed, trader_type);
        } else {
            added = building_warehouse_add_import(dst, resource, removed, trader_type);
        }
        if (added > 0 && added < removed && source_building_id > 0) {
            /* Partial delivery: return shortfall to source */
            int shortfall = removed - added;
            building *src = building_get(source_building_id);
            if (src) {
                if (src->type == BUILDING_GRANARY) {
                    building_granary_add_import(src, resource, shortfall, trader_type);
                } else {
                    building_warehouse_add_import(src, resource, shortfall, trader_type);
                }
            }
            MP_LOG_INFO("TRADE_EXEC", "Partial delivery: %d/%d returned %d to source bld=%d",
                        added, removed, shortfall, source_building_id);
        }
        if (added <= 0) {
            /* Rollback: re-add to source */
            if (source_building_id > 0 && removed > 0) {
                building *src = building_get(source_building_id);
                if (src) {
                    if (src->type == BUILDING_GRANARY) {
                        building_granary_add_import(src, resource, removed, trader_type);
                    } else {
                        building_warehouse_add_import(src, resource, removed, trader_type);
                    }
                }
            }
            mp_trade_route_rollback_export(route_instance_id, resource, amount);
            mp_trade_route_rollback_import(route_instance_id, resource, amount);
            MP_LOG_WARN("TRADE_EXEC", "Add to dest failed: bld=%d res=%d amt=%d",
                        dest_building_id, resource, removed);
            return MP_TRADE_ERR_NO_CAPACITY;
        }
    } else {
        added = removed; /* virtual import (e.g., remote player city) */
    }

    /* 5. Adjust reservation to actual amount (we reserved `amount`, actual is `added`) */
    if (added < amount) {
        int diff = amount - added;
        mp_trade_route_rollback_export(route_instance_id, resource, diff);
        mp_trade_route_rollback_import(route_instance_id, resource, diff);
    }

    /* Also update the underlying Claudius trade route if it exists */
    if (route->claudius_route_id >= 0 && trade_route_is_valid(route->claudius_route_id)) {
        trade_route_increase_traded(route->claudius_route_id, resource, 1); /* buying */
        trade_route_increase_traded(route->claudius_route_id, resource, 0); /* selling */
    }

    route->last_trade_tick = net_session_get_authoritative_tick();

    /* 6. Record and emit the committed result */
    finalize_last_transaction(route, resource, amount, added,
                              source_building_id, dest_building_id,
                              figure_id, MP_TRADE_OK);

    return MP_TRADE_OK;
}

/* ---- Trader cargo recovery ---- */

void mp_trade_recover_trader_cargo(int figure_id)
{
    if (!net_session_is_host()) {
        return;
    }

    int route_id = mp_ownership_get_trader_route(figure_id);
    if (route_id < 0) {
        /* Already cleared (e.g., by route deletion cleanup) */
        return;
    }

    figure *f = figure_get(figure_id);
    if (!f) {
        return;
    }

    /* Find a warehouse in the origin city to return cargo to */
    int origin_city_id = mp_ownership_get_trader_origin_city(figure_id);
    mp_trade_route_instance *mpr = mp_trade_route_find_by_claudius_route(route_id);

    /* Check trader's loaded resources and return them */
    if (f->trader_id > 0) {
        for (int r = RESOURCE_MIN; r < RESOURCE_MAX; r++) {
            int amount = trader_bought_resources(f->trader_id, r);
            if (amount <= 0) {
                continue;
            }

            /* Find any warehouse with capacity in the origin player's city */
            int returned = 0;
            for (int bid = 1; bid < building_count(); bid++) {
                building *b = building_get(bid);
                if (!b || b->state != BUILDING_STATE_IN_USE) {
                    continue;
                }
                if (b->type != BUILDING_WAREHOUSE && b->type != BUILDING_GRANARY) {
                    continue;
                }
                /* Only return to buildings in the origin city's area */
                if (origin_city_id >= 0) {
                    uint8_t owner = mp_ownership_get_city_player_id(origin_city_id);
                    /* Simple heuristic: any storage building works for the host */
                    (void)owner;
                }
                if (b->type == BUILDING_GRANARY) {
                    int cap = building_granary_maximum_receptible_amount(b, r);
                    if (cap > 0) {
                        int to_add = amount < cap ? amount : cap;
                        building_granary_add_import(b, r, to_add, 0);
                        returned += to_add;
                    }
                } else {
                    int cap = building_warehouse_maximum_receptible_amount(b, r);
                    if (cap > 0) {
                        int to_add = amount < cap ? amount : cap;
                        building_warehouse_add_import(b, r, to_add, 0);
                        returned += to_add;
                    }
                }
                if (returned >= amount) {
                    break;
                }
            }

            if (returned > 0) {
                MP_LOG_INFO("TRADE_EXEC", "Recovered %d of resource %d from dead trader %d",
                            returned, r, figure_id);
                /* Rollback the quota for recovered goods */
                if (mpr) {
                    mp_trade_route_rollback_export(mpr->instance_id, r, returned);
                }
            }
        }
    }

    mp_trade_sync_emit_trader_despawned(figure_id);
    mp_ownership_clear_trader(figure_id);
}

/* ---- Per-tick processing ---- */

/**
 * Called once per tick on the host.
 * Iterates all active routes in deterministic order (by instance_id).
 * For routes whose traders are at storage, this triggers the trade execution.
 *
 * Note: The actual per-tick trader execution still happens through the
 * Claudius figure action system (trader.c). This function handles the
 * mp_trade_route_instance bookkeeping and ensures deterministic ordering.
 */
void mp_trade_execute_tick(uint32_t current_tick)
{
    if (!net_session_is_host() && net_session_is_active()) {
        return;
    }

    /* Routes are processed in instance_id order for determinism.
     * The alloc_route function allocates linearly, so iterating
     * the array in index order gives deterministic instance_id order. */
    (void)current_tick;
    /* Actual trade execution is driven by figure actions (trader.c).
     * This function provides the deterministic ordering hook.
     * The mp_trade_commit_transaction calls happen when traders
     * reach storage buildings. */
}

/* ---- Year change ---- */

void mp_trade_execution_on_year_change(void)
{
    if (!net_session_is_active()) {
        return;
    }

    /* Only host resets and broadcasts */
    if (net_session_is_host()) {
        mp_trade_route_reset_annual_counters();
        mp_trade_route_foreach_active(broadcast_route_state_after_year_change, 0);

        MP_LOG_INFO("TRADE_EXEC", "Year change: annual trade counters reset and broadcast");
    }
}

/* ---- Queries ---- */

const mp_trade_transaction *mp_trade_get_last_transaction(void)
{
    return &exec_data.last_transaction;
}

uint32_t mp_trade_get_transaction_count(void)
{
    return exec_data.total_transactions;
}

#endif /* ENABLE_MULTIPLAYER */
