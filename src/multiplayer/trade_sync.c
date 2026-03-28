#include "trade_sync.h"

#ifdef ENABLE_MULTIPLAYER

#include "mp_debug_log.h"
#include "mp_trade_route.h"
#include "ownership.h"
#include "network/session.h"
#include "network/serialize.h"
#include "network/protocol.h"
#include "trade_execution.h"
#include "building/building.h"
#include "building/granary.h"
#include "building/warehouse.h"
#include "figure/figure.h"
#include "figure/type.h"
#include "empire/city.h"
#include "empire/trade_prices.h"
#include "empire/trade_route.h"
#include "figure/figure.h"
#include "figure/trader.h"
#include "city/finance.h"
#include "city/health.h"
#include "game/resource.h"
#include "core/log.h"

#include <string.h>
#include <stdlib.h>

#define MAX_REPLICATED_TRADERS 128

typedef enum {
    TRADER_STATE_NONE = 0,
    TRADER_STATE_TRAVELING,      /* En route to destination */
    TRADER_STATE_AT_STORAGE,     /* At storage building, trading */
    TRADER_STATE_RETURNING,      /* Heading back to origin */
    TRADER_STATE_ABORTED         /* Could not complete, cleaning up */
} replicated_trader_state;

typedef struct {
    int active;
    int figure_id;
    int city_id;
    int route_id;
    uint32_t network_entity_id;
    replicated_trader_state state;
    uint32_t state_version;      /* Incremented on each state change */
    int last_resource;           /* Last traded resource (for display) */
    int last_amount;             /* Last traded amount */
} replicated_trader;

static struct {
    replicated_trader traders[MAX_REPLICATED_TRADERS];
    int trader_count;
    uint32_t next_network_entity_id;
} trade_data;

static mp_trade_route_instance *resolve_trade_event_route(uint32_t route_instance_id,
                                                          int fallback_route_id)
{
    mp_trade_route_instance *mpr = 0;

    if (route_instance_id > 0) {
        mpr = mp_trade_route_get(route_instance_id);
    }
    if (!mpr && fallback_route_id >= 0) {
        mpr = mp_trade_route_find_by_claudius_route(fallback_route_id);
    }
    return mpr;
}

static void apply_trade_event_storage_mutation(int building_id, int resource,
                                               int amount, int buying,
                                               int trader_type)
{
    building *b;
    int applied = 0;
    int price;

    if (building_id <= 0 || amount <= 0 ||
        resource < RESOURCE_MIN || resource >= RESOURCE_MAX) {
        return;
    }

    b = building_get(building_id);
    if (!b || b->state != BUILDING_STATE_IN_USE) {
        return;
    }

    if (buying) {
        if (b->type == BUILDING_GRANARY) {
            applied = building_granary_try_remove_resource(b, resource, amount);
        } else if (b->type == BUILDING_WAREHOUSE) {
            applied = building_warehouse_try_remove_resource(b, resource, amount);
        }
        if (applied > 0) {
            price = trade_price_sell(resource, trader_type);
            city_finance_process_export(price * applied);
        }
    } else {
        if (b->type == BUILDING_GRANARY) {
            applied = building_granary_try_add_resource(b, resource, amount, 0, 0);
        } else if (b->type == BUILDING_WAREHOUSE) {
            applied = building_warehouse_try_add_resource(b, resource, amount, 0);
        }
        if (applied > 0) {
            price = trade_price_buy(resource, trader_type);
            city_finance_process_import(price * applied);
        }
    }

    if (applied > 0) {
        city_health_update_sickness_level_in_building(building_id);
        if (applied != amount) {
            MP_LOG_WARN("TRADE_SYNC", "Client replay applied %d/%d units for building %d resource %d",
                        applied, amount, building_id, resource);
        }
    } else {
        MP_LOG_WARN("TRADE_SYNC", "Client replay could not apply trade mutation for building %d resource %d amount %d",
                    building_id, resource, amount);
    }
}

void mp_trade_sync_init(void)
{
    memset(&trade_data, 0, sizeof(trade_data));
    trade_data.next_network_entity_id = 1;
}

void mp_trade_sync_clear(void)
{
    mp_trade_sync_init();
}

static replicated_trader *find_trader(int figure_id)
{
    for (int i = 0; i < MAX_REPLICATED_TRADERS; i++) {
        if (trade_data.traders[i].active && trade_data.traders[i].figure_id == figure_id) {
            return &trade_data.traders[i];
        }
    }
    return NULL;
}

static replicated_trader *alloc_trader(int figure_id)
{
    replicated_trader *existing = find_trader(figure_id);
    if (existing) {
        return existing;
    }
    for (int i = 0; i < MAX_REPLICATED_TRADERS; i++) {
        if (!trade_data.traders[i].active) {
            memset(&trade_data.traders[i], 0, sizeof(replicated_trader));
            trade_data.traders[i].active = 1;
            trade_data.traders[i].figure_id = figure_id;
            trade_data.traders[i].network_entity_id = trade_data.next_network_entity_id++;
            trade_data.traders[i].state = TRADER_STATE_TRAVELING;
            trade_data.traders[i].state_version = 1;
            trade_data.trader_count++;
            return &trade_data.traders[i];
        }
    }
    log_error("Replicated trader table full", 0, figure_id);
    return NULL;
}

static void free_trader(replicated_trader *t)
{
    if (t && t->active) {
        t->active = 0;
        trade_data.trader_count--;
    }
}

/* ---- Host: emit trader lifecycle events ---- */

void mp_trade_sync_emit_trader_spawned(int figure_id, int city_id, int route_id)
{
    if (!net_session_is_host()) {
        return;
    }

    replicated_trader *t = alloc_trader(figure_id);
    if (!t) {
        return;
    }
    t->city_id = city_id;
    t->route_id = route_id;
    t->state = TRADER_STATE_TRAVELING;

    uint8_t buf[48];
    net_serializer s;
    net_serializer_init(&s, buf, sizeof(buf));
    net_write_u16(&s, NET_EVENT_TRADER_SPAWNED);
    net_write_u32(&s, net_session_get_authoritative_tick());
    net_write_u32(&s, t->network_entity_id);
    net_write_i32(&s, figure_id);
    net_write_i32(&s, city_id);
    net_write_i32(&s, route_id);
    net_write_u32(&s, t->state_version);

    net_session_broadcast(NET_MSG_HOST_EVENT, buf, (uint32_t)net_serializer_position(&s));
}

void mp_trade_sync_emit_trader_reached_storage(int figure_id, int storage_building_id)
{
    if (!net_session_is_host()) {
        return;
    }

    replicated_trader *t = find_trader(figure_id);
    if (!t) {
        return;
    }
    t->state = TRADER_STATE_AT_STORAGE;
    t->state_version++;

    uint8_t buf[32];
    net_serializer s;
    net_serializer_init(&s, buf, sizeof(buf));
    net_write_u16(&s, NET_EVENT_TRADER_REACHED_STORAGE);
    net_write_u32(&s, net_session_get_authoritative_tick());
    net_write_u32(&s, t->network_entity_id);
    net_write_i32(&s, figure_id);
    net_write_i32(&s, storage_building_id);
    net_write_u32(&s, t->state_version);

    net_session_broadcast(NET_MSG_HOST_EVENT, buf, (uint32_t)net_serializer_position(&s));
}

void mp_trade_sync_emit_trader_trade_executed(int figure_id, int resource,
                                               int amount, int buying,
                                               int building_id)
{
    if (!net_session_is_host()) {
        return;
    }

    replicated_trader *t = find_trader(figure_id);
    if (t) {
        t->last_resource = resource;
        t->last_amount = amount;
        t->state_version++;
    }

    /* Resolve route_instance_id from the Claudius route */
    uint32_t route_instance_id = 0;
    int route_id = t ? t->route_id : mp_ownership_get_trader_route(figure_id);
    if (route_id >= 0) {
        mp_trade_route_instance *mpr = mp_trade_route_find_by_claudius_route(route_id);
        if (mpr) {
            route_instance_id = mpr->instance_id;
            /* Also record in the mp_trade_route counters */
            if (buying) {
                mp_trade_route_record_import(route_instance_id, resource, amount);
            } else {
                mp_trade_route_record_export(route_instance_id, resource, amount);
            }
        }
    }

    uint8_t buf[64];
    net_serializer s;
    net_serializer_init(&s, buf, sizeof(buf));
    net_write_u16(&s, NET_EVENT_TRADER_TRADE_EXECUTED);
    net_write_u32(&s, net_session_get_authoritative_tick());
    net_write_u32(&s, t ? t->network_entity_id : 0);
    net_write_i32(&s, figure_id);
    net_write_i32(&s, resource);
    net_write_i32(&s, amount);
    net_write_u8(&s, (uint8_t)buying);
    net_write_i32(&s, building_id);
    net_write_u32(&s, route_instance_id);
    net_write_u32(&s, t ? t->state_version : 0);

    net_session_broadcast(NET_MSG_HOST_EVENT, buf, (uint32_t)net_serializer_position(&s));
}

void mp_trade_sync_emit_trader_returning(int figure_id)
{
    if (!net_session_is_host()) {
        return;
    }

    replicated_trader *t = find_trader(figure_id);
    if (t) {
        t->state = TRADER_STATE_RETURNING;
        t->state_version++;
    }

    uint8_t buf[32];
    net_serializer s;
    net_serializer_init(&s, buf, sizeof(buf));
    net_write_u16(&s, NET_EVENT_TRADER_RETURNING);
    net_write_u32(&s, net_session_get_authoritative_tick());
    net_write_u32(&s, t ? t->network_entity_id : 0);
    net_write_i32(&s, figure_id);
    net_write_u32(&s, t ? t->state_version : 0);

    net_session_broadcast(NET_MSG_HOST_EVENT, buf, (uint32_t)net_serializer_position(&s));
}

void mp_trade_sync_emit_trader_aborted(int figure_id, int reason)
{
    if (!net_session_is_host()) {
        return;
    }

    replicated_trader *t = find_trader(figure_id);
    if (t) {
        t->state = TRADER_STATE_ABORTED;
        t->state_version++;
    }

    uint8_t buf[32];
    net_serializer s;
    net_serializer_init(&s, buf, sizeof(buf));
    net_write_u16(&s, NET_EVENT_TRADER_ABORTED);
    net_write_u32(&s, net_session_get_authoritative_tick());
    net_write_u32(&s, t ? t->network_entity_id : 0);
    net_write_i32(&s, figure_id);
    net_write_i32(&s, reason);
    net_write_u32(&s, t ? t->state_version : 0);

    net_session_broadcast(NET_MSG_HOST_EVENT, buf, (uint32_t)net_serializer_position(&s));
}

void mp_trade_sync_emit_trader_despawned(int figure_id)
{
    if (!net_session_is_host()) {
        return;
    }

    replicated_trader *t = find_trader(figure_id);
    if (t) {
        free_trader(t);
    }

    uint8_t buf[16];
    net_serializer s;
    net_serializer_init(&s, buf, sizeof(buf));
    net_write_u16(&s, NET_EVENT_TRADER_DESPAWNED);
    net_write_u32(&s, net_session_get_authoritative_tick());
    net_write_i32(&s, figure_id);

    net_session_broadcast(NET_MSG_HOST_EVENT, buf, (uint32_t)net_serializer_position(&s));
}

/* ---- Host: cleanup route traders ---- */

void mp_trade_sync_cleanup_route_traders(int route_id)
{
    if (!net_session_is_host()) {
        return;
    }

    for (int i = 0; i < MAX_REPLICATED_TRADERS; i++) {
        replicated_trader *t = &trade_data.traders[i];
        if (!t->active || t->route_id != route_id) {
            continue;
        }

        /* Recover cargo from the figure before despawning */
        figure *f = figure_get(t->figure_id);
        if (f && f->state == FIGURE_STATE_ALIVE) {
            mp_trade_recover_trader_cargo(t->figure_id);
        }

        /* Emit despawn event and free the replicated entry */
        mp_trade_sync_emit_trader_despawned(t->figure_id);
        /* Note: emit_trader_despawned already frees the entry */
    }
}

/* ---- Host: broadcast route state ---- */

void mp_trade_sync_broadcast_route_state(int route_id)
{
    if (!net_session_is_host()) {
        return;
    }

    mp_trade_route_instance *mpr = mp_trade_route_find_by_claudius_route(route_id);
    uint32_t payload_size =
        (uint32_t)(sizeof(uint16_t) + sizeof(uint32_t) + sizeof(int32_t) + sizeof(uint32_t)) +
        (uint32_t)RESOURCE_MAX *
            (uint32_t)(4 * sizeof(int32_t) + 2 * sizeof(uint8_t) + 4 * sizeof(int32_t));
    uint8_t *buf;
    net_serializer s;

    if (!mpr) {
        log_error("Cannot broadcast route state for unknown route", 0, route_id);
        return;
    }

    buf = (uint8_t *)malloc(payload_size);
    if (!buf) {
        log_error("Failed to allocate route sync payload", 0, (int)payload_size);
        return;
    }

    net_serializer_init(&s, buf, payload_size);
    net_write_u16(&s, NET_EVENT_ROUTE_STATE_SYNC);
    net_write_u32(&s, net_session_get_authoritative_tick());
    net_write_i32(&s, route_id);
    net_write_u32(&s, mpr->instance_id);

    /* Write full route state */
    for (int r = RESOURCE_MIN; r < RESOURCE_MAX; r++) {
        net_write_i32(&s, trade_route_limit(route_id, r, 0));   /* buy limit */
        net_write_i32(&s, trade_route_traded(route_id, r, 0));   /* buy traded */
        net_write_i32(&s, trade_route_limit(route_id, r, 1));   /* sell limit */
        net_write_i32(&s, trade_route_traded(route_id, r, 1));   /* sell traded */
        net_write_u8(&s, mpr->resources[r].export_enabled);
        net_write_u8(&s, mpr->resources[r].import_enabled);
        net_write_i32(&s, mpr->resources[r].export_limit);
        net_write_i32(&s, mpr->resources[r].import_limit);
        net_write_i32(&s, mpr->resources[r].exported_this_year);
        net_write_i32(&s, mpr->resources[r].imported_this_year);
    }

    if (net_serializer_has_overflow(&s)) {
        log_error("Route sync payload overflow", 0, route_id);
        free(buf);
        return;
    }

    net_session_broadcast(NET_MSG_HOST_EVENT, buf, (uint32_t)net_serializer_position(&s));
    free(buf);
}

void mp_trade_sync_broadcast_route_policy(int route_id, int resource,
                                           int is_export, int enabled)
{
    if (!net_session_is_host()) {
        return;
    }

    mp_trade_route_instance *mpr = mp_trade_route_find_by_claudius_route(route_id);
    uint8_t buf[48];
    net_serializer s;
    net_serializer_init(&s, buf, sizeof(buf));
    net_write_u16(&s, NET_EVENT_ROUTE_POLICY_SET);
    net_write_u32(&s, net_session_get_authoritative_tick());
    net_write_i32(&s, route_id);
    net_write_u32(&s, mpr ? mpr->network_route_id : 0);
    net_write_i32(&s, resource);
    net_write_u8(&s, (uint8_t)is_export);
    net_write_u8(&s, (uint8_t)enabled);

    net_session_broadcast(NET_MSG_HOST_EVENT, buf, (uint32_t)net_serializer_position(&s));
}

void mp_trade_sync_broadcast_route_limit(int route_id, int resource,
                                          int is_buying, int amount)
{
    if (!net_session_is_host()) {
        return;
    }

    mp_trade_route_instance *mpr = mp_trade_route_find_by_claudius_route(route_id);
    uint8_t buf[48];
    net_serializer s;
    net_serializer_init(&s, buf, sizeof(buf));
    net_write_u16(&s, NET_EVENT_ROUTE_LIMIT_SET);
    net_write_u32(&s, net_session_get_authoritative_tick());
    net_write_i32(&s, route_id);
    net_write_u32(&s, mpr ? mpr->network_route_id : 0);
    net_write_i32(&s, resource);
    net_write_u8(&s, (uint8_t)is_buying);
    net_write_i32(&s, amount);

    net_session_broadcast(NET_MSG_HOST_EVENT, buf, (uint32_t)net_serializer_position(&s));
}

/* ---- Client: handle events from host ---- */

void mp_trade_sync_handle_event(uint16_t event_type,
                                 const uint8_t *data, uint32_t size)
{
    net_serializer s;
    net_serializer_init(&s, (uint8_t *)data, size);

    switch (event_type) {
        case NET_EVENT_TRADER_SPAWNED: {
            uint32_t net_id = net_read_u32(&s);
            int figure_id = net_read_i32(&s);
            int city_id = net_read_i32(&s);
            int route_id = net_read_i32(&s);
            uint32_t version = net_read_u32(&s);

            replicated_trader *t = alloc_trader(figure_id);
            if (t) {
                t->network_entity_id = net_id;
                t->city_id = city_id;
                t->route_id = route_id;
                t->state = TRADER_STATE_TRAVELING;
                t->state_version = version;
            }
            break;
        }
        case NET_EVENT_TRADER_REACHED_STORAGE: {
            uint32_t net_id = net_read_u32(&s);
            int figure_id = net_read_i32(&s);
            int storage_id = net_read_i32(&s);
            uint32_t version = net_read_u32(&s);

            replicated_trader *t = find_trader(figure_id);
            if (t) {
                t->state = TRADER_STATE_AT_STORAGE;
                t->state_version = version;
            }
            (void)net_id;
            (void)storage_id;
            break;
        }
        case NET_EVENT_TRADER_TRADE_EXECUTED: {
            uint32_t net_id = net_read_u32(&s);
            int figure_id = net_read_i32(&s);
            int resource = net_read_i32(&s);
            int amount = net_read_i32(&s);
            uint8_t buying = net_read_u8(&s);
            int building_id = net_read_i32(&s);
            uint32_t route_instance_id = net_read_u32(&s);
            uint32_t version = net_read_u32(&s);

            /* Client applies the trade result from host */
            replicated_trader *t = find_trader(figure_id);
            int route_id = t ? t->route_id : mp_ownership_get_trader_route(figure_id);
            mp_trade_route_instance *mpr = resolve_trade_event_route(route_instance_id, route_id);
            int trader_type = 0;
            int applied_amount = amount > 0 ? amount : 1;
            if (mpr) {
                trader_type = (mpr->transport == MP_TROUTE_LAND) ? 1 : 0;
            } else {
                figure *trade_figure = figure_get(figure_id);
                if (trade_figure &&
                    (trade_figure->type == FIGURE_TRADE_CARAVAN ||
                     trade_figure->type == FIGURE_TRADE_CARAVAN_DONKEY ||
                     trade_figure->type == FIGURE_NATIVE_TRADER)) {
                    trader_type = 1;
                }
            }
            if (mpr && route_id < 0) {
                route_id = mpr->claudius_route_id;
            }
            if (trade_route_is_valid(route_id) && resource >= RESOURCE_MIN && resource < RESOURCE_MAX) {
                for (int i = 0; i < applied_amount; i++) {
                    trade_route_increase_traded(route_id, resource, buying);
                }
            }
            /* Update mp_trade_route_instance counters on client */
            if (mpr) {
                if (buying) {
                    mp_trade_route_record_import(mpr->instance_id, resource, applied_amount);
                } else {
                    mp_trade_route_record_export(mpr->instance_id, resource, applied_amount);
                }
            }

            /* Apply warehouse/granary mutation on client so stock stays in sync.
             * This is the ONLY place clients apply warehouse mutations for trades —
             * trader_get_buy/sell_resource() is gated behind is_auth in trader.c. */
            apply_trade_event_storage_mutation(building_id, resource, applied_amount,
                                               buying, trader_type);

            /* Sync figure state so client's trader knows when to stop trading.
             * Without this, figure_trade_caravan_can_buy/sell never returns false
             * on clients because trader_amount_bought/loads_sold_or_carrying stay 0. */
            if (figure_id > 0) {
                figure *f = figure_get(figure_id);
                if (f && f->state == FIGURE_STATE_ALIVE) {
                    for (int i = 0; i < applied_amount; i++) {
                        if (buying) {
                            f->trader_amount_bought++;
                            trader_record_bought_resource(f->trader_id, resource);
                        } else {
                            f->loads_sold_or_carrying++;
                            trader_record_sold_resource(f->trader_id, resource);
                        }
                    }
                }
            }

            if (t) {
                t->last_resource = resource;
                t->last_amount = applied_amount;
                t->state_version = version;
            }
            (void)net_id;
            break;
        }
        case NET_EVENT_TRADER_RETURNING: {
            uint32_t net_id = net_read_u32(&s);
            int figure_id = net_read_i32(&s);
            uint32_t version = net_read_u32(&s);

            replicated_trader *t = find_trader(figure_id);
            if (t) {
                t->state = TRADER_STATE_RETURNING;
                t->state_version = version;
            }
            (void)net_id;
            break;
        }
        case NET_EVENT_TRADER_ABORTED: {
            uint32_t net_id = net_read_u32(&s);
            int figure_id = net_read_i32(&s);
            int reason = net_read_i32(&s);
            uint32_t version = net_read_u32(&s);

            replicated_trader *t = find_trader(figure_id);
            if (t) {
                t->state = TRADER_STATE_ABORTED;
                t->state_version = version;
            }
            (void)net_id;
            (void)reason;
            break;
        }
        case NET_EVENT_TRADER_DESPAWNED: {
            int figure_id = net_read_i32(&s);
            replicated_trader *t = find_trader(figure_id);
            if (t) {
                free_trader(t);
            }
            break;
        }
        case NET_EVENT_ROUTE_STATE_SYNC: {
            int route_id = net_read_i32(&s);
            uint32_t route_instance_id = net_read_u32(&s);
            mp_trade_route_instance *mpr = mp_trade_route_get(route_instance_id);
            if (!trade_route_is_valid(route_id) && mpr) {
                route_id = mpr->claudius_route_id;
            }
            if (trade_route_is_valid(route_id)) {
                for (int r = RESOURCE_MIN; r < RESOURCE_MAX; r++) {
                    int buy_limit = net_read_i32(&s);
                    int buy_traded = net_read_i32(&s);
                    int sell_limit = net_read_i32(&s);
                    int sell_traded = net_read_i32(&s);
                    uint8_t export_enabled = net_read_u8(&s);
                    uint8_t import_enabled = net_read_u8(&s);
                    int export_limit = net_read_i32(&s);
                    int import_limit = net_read_i32(&s);
                    int exported_this_year = net_read_i32(&s);
                    int imported_this_year = net_read_i32(&s);
                    trade_route_set_limit(route_id, r, buy_limit, 0);
                    trade_route_set_limit(route_id, r, sell_limit, 1);
                    trade_route_set_traded(route_id, r, buy_traded, 0);
                    trade_route_set_traded(route_id, r, sell_traded, 1);
                    if (mpr) {
                        mpr->resources[r].export_enabled = export_enabled;
                        mpr->resources[r].import_enabled = import_enabled;
                        mpr->resources[r].export_limit = export_limit;
                        mpr->resources[r].import_limit = import_limit;
                        mpr->resources[r].exported_this_year = exported_this_year;
                        mpr->resources[r].imported_this_year = imported_this_year;
                    }
                }
            }
            break;
        }
        default:
            break;
    }
}

int mp_trade_sync_is_authoritative(int figure_id)
{
    return net_session_is_host();
}

uint32_t mp_trade_sync_get_network_entity_id(int figure_id)
{
    replicated_trader *t = find_trader(figure_id);
    return t ? t->network_entity_id : 0;
}

int mp_trade_sync_get_trader_route(int figure_id)
{
    replicated_trader *t = find_trader(figure_id);
    return t ? t->route_id : -1;
}

/* ---- Serialization ---- */

void mp_trade_sync_serialize_routes(uint8_t *buffer, uint32_t *size)
{
    net_serializer s;
    net_serializer_init(&s, buffer, 16384);

    int route_count = trade_route_count();
    net_write_i32(&s, route_count);

    for (int i = 0; i < route_count; i++) {
        net_write_u8(&s, (uint8_t)trade_route_is_valid(i));
        if (!trade_route_is_valid(i)) {
            continue;
        }
        for (int r = RESOURCE_MIN; r < RESOURCE_MAX; r++) {
            net_write_i32(&s, trade_route_limit(i, r, 0));
            net_write_i32(&s, trade_route_traded(i, r, 0));
            net_write_i32(&s, trade_route_limit(i, r, 1));
            net_write_i32(&s, trade_route_traded(i, r, 1));
        }
    }

    *size = (uint32_t)net_serializer_position(&s);
}

void mp_trade_sync_deserialize_routes(const uint8_t *buffer, uint32_t size)
{
    net_serializer s;
    net_serializer_init(&s, (uint8_t *)buffer, size);

    int route_count = net_read_i32(&s);

    for (int i = 0; i < route_count && !net_serializer_has_overflow(&s); i++) {
        uint8_t valid = net_read_u8(&s);
        if (!valid) {
            continue;
        }
        for (int r = RESOURCE_MIN; r < RESOURCE_MAX; r++) {
            int buy_limit = net_read_i32(&s);
            int buy_traded = net_read_i32(&s);
            int sell_limit = net_read_i32(&s);
            int sell_traded = net_read_i32(&s);

            if (trade_route_is_valid(i)) {
                trade_route_set_limit(i, r, buy_limit, 0);
                trade_route_set_limit(i, r, sell_limit, 1);
                trade_route_set_traded(i, r, buy_traded, 0);
                trade_route_set_traded(i, r, sell_traded, 1);
            }
        }
    }
}

void mp_trade_sync_serialize_traders(uint8_t *buffer, uint32_t *size)
{
    net_serializer s;
    net_serializer_init(&s, buffer, 8192);

    uint16_t count = 0;
    for (int i = 0; i < MAX_REPLICATED_TRADERS; i++) {
        if (trade_data.traders[i].active) {
            count++;
        }
    }
    net_write_u16(&s, count);

    for (int i = 0; i < MAX_REPLICATED_TRADERS; i++) {
        replicated_trader *t = &trade_data.traders[i];
        if (!t->active) {
            continue;
        }
        net_write_u32(&s, t->network_entity_id);
        net_write_i32(&s, t->figure_id);
        net_write_i32(&s, t->city_id);
        net_write_i32(&s, t->route_id);
        net_write_u8(&s, (uint8_t)t->state);
        net_write_u32(&s, t->state_version);
        net_write_i32(&s, t->last_resource);
        net_write_i32(&s, t->last_amount);
    }

    *size = (uint32_t)net_serializer_position(&s);
}

void mp_trade_sync_deserialize_traders(const uint8_t *buffer, uint32_t size)
{
    memset(trade_data.traders, 0, sizeof(trade_data.traders));
    trade_data.trader_count = 0;

    net_serializer s;
    net_serializer_init(&s, (uint8_t *)buffer, size);

    uint16_t count = net_read_u16(&s);

    for (int i = 0; i < count && i < MAX_REPLICATED_TRADERS && !net_serializer_has_overflow(&s); i++) {
        replicated_trader *t = &trade_data.traders[i];
        t->active = 1;
        t->network_entity_id = net_read_u32(&s);
        t->figure_id = net_read_i32(&s);
        t->city_id = net_read_i32(&s);
        t->route_id = net_read_i32(&s);
        t->state = (replicated_trader_state)net_read_u8(&s);
        t->state_version = net_read_u32(&s);
        t->last_resource = net_read_i32(&s);
        t->last_amount = net_read_i32(&s);
        trade_data.trader_count++;
    }
}

#endif /* ENABLE_MULTIPLAYER */
