#include "trade_route.h"

#include "core/array.h"
#include "core/log.h"
#include "empire/city.h"
#include "game/save_version.h"

#include <string.h>

#ifdef ENABLE_MULTIPLAYER
#include "multiplayer/empire_sync.h"
#include "multiplayer/mp_trade_route.h"
#include "multiplayer/ownership.h"
#include "network/session.h"
#endif

typedef struct {
    int limit[RESOURCE_MAX];
    int traded[RESOURCE_MAX];
} route_resource;

typedef struct {
    route_resource buys;
    route_resource sells;
} trade_route;

static array(trade_route) routes;

int trade_route_init(void)
{
    if (!array_init(routes, LEGACY_MAX_ROUTES, 0, 0)) {
        log_error("Unable to create memory for trade routes. The game will now crash.", 0, 0);
        return 0;
    }
    // Discard route 0
    array_advance(routes);
    return 1;
}

int trade_route_new(void)
{
    array_advance(routes);
    return routes.size - 1;
}

int trade_route_ensure_id(int route_id)
{
    while ((unsigned int)route_id >= routes.size) {
        array_advance(routes);
    }
    return route_id >= 0 && (unsigned int)route_id < routes.size;
}

int trade_route_count(void)
{
    return routes.size;
}

int trade_route_is_valid(int route_id)
{
    return route_id >= 0 && (unsigned int) route_id < routes.size;
}

int empire_city_get_primary_legacy_route_id(int city_id)
{
    if (city_id < 0) {
        return -1;
    }
    return empire_city_get_route_id(city_id);
}

static void clear_route_view(trade_route_view *out)
{
    if (out) {
        memset(out, 0, sizeof(*out));
        out->route_id = -1;
        out->counterpart_city_id = -1;
        out->local_city_id = -1;
        out->counterpart_online = 1;
        out->state = 0;
    }
}

static void populate_counterpart_trade_arrays(const empire_city *counterpart, trade_route_view *out)
{
    for (resource_type r = RESOURCE_MIN; r < RESOURCE_MAX; r++) {
        out->counterpart_sells[r] = counterpart->sells_resource[r];
        out->counterpart_buys[r] = counterpart->buys_resource[r];
    }
}

static void populate_legacy_trade_arrays(int route_id, trade_route_view *out)
{
    if (!trade_route_is_valid(route_id)) {
        return;
    }

    for (resource_type r = RESOURCE_MIN; r < RESOURCE_MAX; r++) {
        out->display_sell_limit[r] = trade_route_limit(route_id, r, 0);
        out->display_sell_traded[r] = trade_route_traded(route_id, r, 0);
        out->display_buy_limit[r] = trade_route_limit(route_id, r, 1);
        out->display_buy_traded[r] = trade_route_traded(route_id, r, 1);
        out->route_export_enabled[r] = out->display_buy_limit[r] > 0;
        out->route_import_enabled[r] = out->display_sell_limit[r] > 0;
        out->route_export_limit[r] = out->display_buy_limit[r];
        out->route_import_limit[r] = out->display_sell_limit[r];
        out->route_exported_this_year[r] = out->display_buy_traded[r];
        out->route_imported_this_year[r] = out->display_sell_traded[r];
    }
}

#ifdef ENABLE_MULTIPLAYER
static int populate_mp_route_view(int local_city_id, int counterpart_city_id,
                                  const empire_city *counterpart, trade_route_view *out)
{
    if (!net_session_is_active() || local_city_id < 0) {
        return 0;
    }

    mp_trade_route_instance *mpr = mp_trade_route_find_by_endpoints(local_city_id, counterpart_city_id);
    if (!mpr) {
        return 0;
    }

    int local_is_origin = (mpr->origin_city_id == local_city_id);
    out->valid = 1;
    out->route_id = mpr->claudius_route_id;
    out->route_key = mpr->network_route_id ? mpr->network_route_id : mpr->instance_id;
    out->local_city_id = local_city_id;
    out->counterpart_city_id = counterpart_city_id;
    out->is_player_to_player = mpr->is_player_to_player;
    out->counterpart_online = mp_ownership_is_city_online(counterpart_city_id);
    out->transport = (int)mpr->transport;
    out->state = (int)mp_ownership_get_route_state(mpr->claudius_route_id);
    out->is_open = out->state != MP_ROUTE_STATE_DELETED;
    out->is_drawable = out->state != MP_ROUTE_STATE_DELETED;

    populate_counterpart_trade_arrays(counterpart, out);
    populate_legacy_trade_arrays(mpr->claudius_route_id, out);

    for (resource_type r = RESOURCE_MIN; r < RESOURCE_MAX; r++) {
        const mp_trade_route_resource *resource = &mpr->resources[r];
        if (local_is_origin) {
            out->route_export_enabled[r] = resource->export_enabled;
            out->route_import_enabled[r] = resource->import_enabled;
            out->route_export_limit[r] = resource->export_limit;
            out->route_import_limit[r] = resource->import_limit;
            out->route_exported_this_year[r] = resource->exported_this_year;
            out->route_imported_this_year[r] = resource->imported_this_year;
        } else {
            out->route_export_enabled[r] = resource->import_enabled;
            out->route_import_enabled[r] = resource->export_enabled;
            out->route_export_limit[r] = resource->import_limit;
            out->route_import_limit[r] = resource->export_limit;
            out->route_exported_this_year[r] = resource->imported_this_year;
            out->route_imported_this_year[r] = resource->exported_this_year;
        }

        out->display_sell_limit[r] = out->route_import_limit[r];
        out->display_sell_traded[r] = out->route_imported_this_year[r];
        out->display_buy_limit[r] = out->route_export_limit[r];
        out->display_buy_traded[r] = out->route_exported_this_year[r];
    }

    return 1;
}

static int populate_mp_potential_view(int local_city_id, int counterpart_city_id,
                                      const empire_city *counterpart, trade_route_view *out)
{
    if (!net_session_is_active() || local_city_id < 0 ||
        !mp_ownership_is_city_player_owned(counterpart_city_id)) {
        return 0;
    }

    out->valid = 1;
    out->route_id = -1;
    out->route_key = 0;
    out->local_city_id = local_city_id;
    out->counterpart_city_id = counterpart_city_id;
    out->is_player_to_player = 1;
    out->counterpart_online = mp_ownership_is_city_online(counterpart_city_id);
    out->transport = counterpart->is_sea_trade ? MP_TROUTE_SEA : MP_TROUTE_LAND;
    out->state = MP_ROUTE_STATE_INACTIVE;
    out->is_open = 0;
    out->is_drawable = 0;
    populate_counterpart_trade_arrays(counterpart, out);

    for (resource_type r = RESOURCE_MIN; r < RESOURCE_MAX; r++) {
        if (out->counterpart_sells[r]) {
            out->display_sell_limit[r] = 40;
        }
        if (out->counterpart_buys[r]) {
            out->display_buy_limit[r] = 40;
        }
    }
    return 1;
}
#endif

int trade_route_get_view_for_city_pair(int local_city_id, int counterpart_city_id, trade_route_view *out)
{
    clear_route_view(out);

    if (!out || counterpart_city_id < 0) {
        return 0;
    }

    const empire_city *counterpart = empire_city_get(counterpart_city_id);
    if (!counterpart || !counterpart->in_use) {
        return 0;
    }

#ifdef ENABLE_MULTIPLAYER
    if (populate_mp_route_view(local_city_id, counterpart_city_id, counterpart, out)) {
        return 1;
    }
#endif

    if (counterpart->route_id > 0 && trade_route_is_valid(counterpart->route_id)) {
        out->valid = 1;
        out->route_id = counterpart->route_id;
        out->route_key = (uint32_t)counterpart->route_id;
        out->local_city_id = local_city_id;
        out->counterpart_city_id = counterpart_city_id;
        out->is_open = counterpart->is_open;
        out->is_drawable = counterpart->is_open;
        out->transport = counterpart->is_sea_trade;
        populate_counterpart_trade_arrays(counterpart, out);
        populate_legacy_trade_arrays(counterpart->route_id, out);
        return 1;
    }

#ifdef ENABLE_MULTIPLAYER
    if (populate_mp_potential_view(local_city_id, counterpart_city_id, counterpart, out)) {
        return 1;
    }
#endif

    return 0;
}

int trade_route_list_for_city(int city_id, trade_route_view *out, int max_routes)
{
    int count = 0;
    if (city_id < 0 || !out || max_routes <= 0) {
        return 0;
    }

#ifdef ENABLE_MULTIPLAYER
    if (net_session_is_active()) {
        const int array_size = empire_city_get_array_size();
        for (int counterpart_id = 0; counterpart_id < array_size && count < max_routes; counterpart_id++) {
            if (counterpart_id == city_id) {
                continue;
            }
            if (trade_route_get_view_for_city_pair(city_id, counterpart_id, &out[count]) &&
                out[count].route_id >= 0) {
                count++;
            }
        }
        return count;
    }
#endif

    const empire_city *city = empire_city_get(city_id);
    if (city && city->route_id > 0 && trade_route_get_view_for_city_pair(-1, city_id, &out[count])) {
        count++;
    }
    return count;
}

int trade_route_collect_for_resource(int city_id, int resource, trade_route_view *out, int max_routes)
{
    if (!out || max_routes <= 0 || resource < RESOURCE_MIN || resource >= RESOURCE_MAX) {
        return 0;
    }

    trade_route_view route_list[64];
    int count = 0;
    int route_count = trade_route_list_for_city(city_id, route_list, 64);

    for (int i = 0; i < route_count && count < max_routes; i++) {
        const trade_route_view *view = &route_list[i];
        if (!view->valid || view->route_id < 0) {
            continue;
        }
        if (view->counterpart_sells[resource] || view->counterpart_buys[resource]) {
            out[count++] = *view;
        }
    }
    return count;
}

int trade_route_view_estimate_potential(const trade_route_view *route_view)
{
    int potential = 0;

    if (!route_view || !route_view->valid || route_view->route_id < 0 || !route_view->is_open) {
        return 0;
    }

#ifdef ENABLE_MULTIPLAYER
    if (route_view->is_player_to_player && route_view->state != MP_ROUTE_STATE_ACTIVE) {
        return 0;
    }
#endif

    for (resource_type r = RESOURCE_MIN; r < RESOURCE_MAX; r++) {
        if (!resource_is_storable(r)) {
            continue;
        }

        if (route_view->counterpart_buys[r] && route_view->route_export_enabled[r]) {
            int remaining = route_view->route_export_limit[r] > 0
                ? route_view->route_export_limit[r] - route_view->route_exported_this_year[r]
                : 40;
            if (remaining > 0) {
                potential += remaining;
            }
        }

        if (route_view->counterpart_sells[r] && route_view->route_import_enabled[r]) {
            int remaining = route_view->route_import_limit[r] > 0
                ? route_view->route_import_limit[r] - route_view->route_imported_this_year[r]
                : 40;
            if (remaining > 0) {
                potential += remaining;
            }
        }
    }

    return potential;
}

void trade_route_set(int route_id, resource_type resource, int limit, int buying)
{
    trade_route *route = array_item(routes, route_id);
    if (buying) {
        route->buys.limit[resource] = limit;
        route->buys.traded[resource] = 0;
    } else {
        route->sells.limit[resource] = limit;
        route->sells.traded[resource] = 0;
    }
}

int trade_route_limit(int route_id, resource_type resource, int buying)
{
    return buying ? array_item(routes, route_id)->buys.limit[resource] :
        array_item(routes, route_id)->sells.limit[resource];
}

int trade_route_traded(int route_id, resource_type resource, int buying)
{
    return buying ? array_item(routes, route_id)->buys.traded[resource] :
        array_item(routes, route_id)->sells.traded[resource];
}

void trade_route_set_limit(int route_id, resource_type resource, int amount, int buying)
{
    if (buying) {
        array_item(routes, route_id)->buys.limit[resource] = amount;
    } else {
        array_item(routes, route_id)->sells.limit[resource] = amount;
    }
}

static route_resource *get_route_resource(int route_id, int buying)
{
    if (buying) {
        return &array_item(routes, route_id)->buys;
    } else {
        return &array_item(routes, route_id)->sells;
    }
}

int trade_route_legacy_increase_limit(int route_id, resource_type resource, int buying)
{
    route_resource *route = get_route_resource(route_id, buying);
    switch (route->limit[resource]) {
        case 0: route->limit[resource] = 15; break;
        case 15: route->limit[resource] = 25; break;
        case 25: route->limit[resource] = 40; break;
    }
    return route->limit[resource];
}

int trade_route_legacy_decrease_limit(int route_id, resource_type resource, int buying)
{
    route_resource *route = get_route_resource(route_id, buying);
    if (buying) {
        route = &array_item(routes, route_id)->buys;
    } else {
        route = &array_item(routes, route_id)->sells;
    }
    switch (route->limit[resource]) {
        case 40: route->limit[resource] = 25; break;
        case 25: route->limit[resource] = 15; break;
        case 15: route->limit[resource] = 0; break;
    }
    return route->limit[resource];
}

void trade_route_increase_traded(int route_id, resource_type resource, int buying)
{
    if (buying) {
        array_item(routes, route_id)->buys.traded[resource]++;
    } else {
        array_item(routes, route_id)->sells.traded[resource]++;
    }
}

void trade_route_reset_traded(int route_id)
{
    trade_route *route = array_item(routes, route_id);
    for (resource_type r = RESOURCE_MIN; r < RESOURCE_MAX; r++) {
        route->buys.traded[r] = route->sells.traded[r] = 0;
    }
}

int trade_route_limit_reached(int route_id, resource_type resource, int buying)
{
    route_resource *route = get_route_resource(route_id, buying);
    return route->traded[resource] >= route->limit[resource];
}

void trade_routes_save_state(buffer *trade_routes)
{
    int buf_size = sizeof(int32_t) * RESOURCE_MAX * 2 * routes.size * 2;
    uint8_t *buf_data = malloc(buf_size + sizeof(int32_t));
    buffer_init(trade_routes, buf_data, buf_size + sizeof(int32_t));
    buffer_write_i32(trade_routes, routes.size);

    trade_route *route;
    array_foreach(routes, route) {
        for (int i = 0; i < 2; i++) {
            for (resource_type r = 0; r < RESOURCE_MAX; r++) {
                buffer_write_i32(trade_routes, i ? route->buys.limit[r] : route->sells.limit[r]);
                buffer_write_i32(trade_routes, i ? route->buys.traded[r] : route->sells.traded[r]);
            }
        }
    }
}

void trade_routes_load_state(buffer *trade_routes)
{
    int routes_to_load = buffer_read_i32(trade_routes);
    if (!array_init(routes, LEGACY_MAX_ROUTES, 0, 0) || !array_expand(routes, routes_to_load)) {
        log_error("Unable to create memory for trade routes. The game will now crash.", 0, 0);
        return;
    }
    for (int i = 0; i < routes_to_load; i++) {
        trade_route *route = array_next(routes);
        for (int i = 0; i < 2; i++) {
            for (int r = 0; r < resource_total_mapped(); r++) {
                resource_type remapped = resource_remap(r);
                if (i) {
                    route->buys.limit[remapped] = buffer_read_i32(trade_routes);
                    route->buys.traded[remapped] = buffer_read_i32(trade_routes);
                } else {
                    route->sells.limit[remapped] = buffer_read_i32(trade_routes);
                    route->sells.traded[remapped] = buffer_read_i32(trade_routes);
                }
            }
        }
    }
}

void trade_routes_migrate_to_buys_sells(buffer *limit, buffer *traded, int version)
{
    int routes_to_load = version <= SAVE_GAME_LAST_STATIC_SCENARIO_OBJECTS ? LEGACY_MAX_ROUTES : buffer_read_i32(limit);
    if (!array_init(routes, LEGACY_MAX_ROUTES, 0, 0) || !array_expand(routes, routes_to_load)) {
        log_error("Unable to create memory for trade routes. The game will now crash.", 0, 0);
        return;
    }
    for (int i = 0; i < routes_to_load; i++) {
        trade_route *route = array_next(routes);
        int city_id = empire_city_get_for_trade_route(i);
        if (city_id < 0) {
            continue;
        }
        for (int r = 0; r < resource_total_mapped(); r++) {
            resource_type remapped = resource_remap(r);
            int limit_amount = buffer_read_i32(limit);
            int traded_amount = buffer_read_i32(traded);
            if (empire_city_buys_resource(city_id, remapped)) {
                route->buys.limit[remapped] = limit_amount;
                route->buys.traded[remapped] = traded_amount;
                route->sells.limit[remapped] = route->sells.traded[remapped] = 0;
            } else if (empire_city_sells_resource(city_id, remapped)) {
                route->sells.limit[remapped] = limit_amount;
                route->sells.traded[remapped] = traded_amount;
                route->buys.limit[remapped] = route->buys.traded[remapped] = 0;
            } else {
                route->sells.limit[remapped] = route->sells.traded[remapped] =
                    route->buys.limit[remapped] = route->buys.traded[remapped] = 0;
            }
        }
    }
}

#ifdef ENABLE_MULTIPLAYER

#include <string.h>

#define MAX_ROUTE_BINDINGS 200

typedef struct {
    int in_use;
    int route_id;
    int origin_player_id;
    int destination_player_id;
    trade_route_player_mode mode;
    int network_route_id;
} route_player_binding;

static route_player_binding route_bindings[MAX_ROUTE_BINDINGS];

static route_player_binding *find_binding(int route_id)
{
    for (int i = 0; i < MAX_ROUTE_BINDINGS; i++) {
        if (route_bindings[i].in_use && route_bindings[i].route_id == route_id) {
            return &route_bindings[i];
        }
    }
    return NULL;
}

static route_player_binding *alloc_binding(int route_id)
{
    route_player_binding *existing = find_binding(route_id);
    if (existing) {
        return existing;
    }
    for (int i = 0; i < MAX_ROUTE_BINDINGS; i++) {
        if (!route_bindings[i].in_use) {
            memset(&route_bindings[i], 0, sizeof(route_player_binding));
            route_bindings[i].in_use = 1;
            route_bindings[i].route_id = route_id;
            return &route_bindings[i];
        }
    }
    return NULL;
}

int trade_route_is_player_to_player(int route_id)
{
    route_player_binding *b = find_binding(route_id);
    return b && b->mode == ROUTE_MODE_PLAYER_TO_PLAYER;
}

void trade_route_bind_players(int route_id, int origin_player_id, int destination_player_id)
{
    route_player_binding *b = alloc_binding(route_id);
    if (!b) {
        return;
    }
    b->origin_player_id = origin_player_id;
    b->destination_player_id = destination_player_id;

    if (origin_player_id >= 0 && destination_player_id >= 0) {
        b->mode = ROUTE_MODE_PLAYER_TO_PLAYER;
    } else if (origin_player_id >= 0) {
        b->mode = ROUTE_MODE_PLAYER_TO_AI;
    } else {
        b->mode = ROUTE_MODE_AI_TO_PLAYER;
    }
}

int trade_route_get_network_id(int route_id)
{
    route_player_binding *b = find_binding(route_id);
    return b ? b->network_route_id : 0;
}

void trade_route_set_network_id(int route_id, int network_route_id)
{
    route_player_binding *b = alloc_binding(route_id);
    if (b) {
        b->network_route_id = network_route_id;
    }
}

int trade_route_get_origin_player(int route_id)
{
    route_player_binding *b = find_binding(route_id);
    return b ? b->origin_player_id : -1;
}

int trade_route_get_dest_player(int route_id)
{
    route_player_binding *b = find_binding(route_id);
    return b ? b->destination_player_id : -1;
}

trade_route_player_mode trade_route_get_player_mode(int route_id)
{
    route_player_binding *b = find_binding(route_id);
    return b ? b->mode : ROUTE_MODE_AI_TO_PLAYER;
}

void trade_route_set_player_binding(int route_id, uint8_t origin_player_id, uint8_t dest_player_id)
{
    route_player_binding *b = alloc_binding(route_id);
    if (!b) {
        return;
    }
    b->origin_player_id = origin_player_id;
    b->destination_player_id = (dest_player_id == 0xFF) ? -1 : dest_player_id;

    if (origin_player_id < 0xFF && dest_player_id < 0xFF) {
        b->mode = ROUTE_MODE_PLAYER_TO_PLAYER;
    } else if (origin_player_id < 0xFF) {
        b->mode = ROUTE_MODE_PLAYER_TO_AI;
    } else {
        b->mode = ROUTE_MODE_AI_TO_PLAYER;
    }
}

void trade_route_clear_player_binding(int route_id)
{
    route_player_binding *b = find_binding(route_id);
    if (b) {
        memset(b, 0, sizeof(route_player_binding));
    }
}

void trade_route_set_export_enabled(int route_id, int resource, int enabled)
{
    if (!trade_route_is_valid(route_id)) {
        return;
    }
    if (resource < 0 || resource >= RESOURCE_MAX) {
        return;
    }
    if (enabled) {
        trade_route_set(route_id, resource, 1500, 0); /* default limit of 15 loads */
    } else {
        trade_route_set_limit(route_id, resource, 0, 0);
    }
}

void trade_route_set_import_enabled(int route_id, int resource, int enabled)
{
    if (!trade_route_is_valid(route_id)) {
        return;
    }
    if (resource < 0 || resource >= RESOURCE_MAX) {
        return;
    }
    if (enabled) {
        trade_route_set(route_id, resource, 1500, 1); /* default limit of 15 loads */
    } else {
        trade_route_set_limit(route_id, resource, 0, 1);
    }
}

#endif /* ENABLE_MULTIPLAYER */
