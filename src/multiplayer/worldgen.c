#include "worldgen.h"

#ifdef ENABLE_MULTIPLAYER

#include "player_registry.h"
#include "ownership.h"
#include "empire/city.h"
#include "empire/object.h"
#include "empire/type.h"
#include "empire/empire.h"
#include "empire/trade_route.h"
#include "network/serialize.h"
#include "core/log.h"

#include <string.h>
#include <math.h>

/* Worldgen parameters */
#define MIN_PLAYER_DISTANCE     80   /* Minimum pixel distance between player spawns */
#define MIN_AI_DISTANCE         40   /* Minimum pixel distance from AI trade cities */
#define MAX_CLUSTER_RADIUS      400  /* Maximum distance from cluster center */
#define FAIRNESS_TOLERANCE      50   /* Max fairness score difference between spawns */

/* FNV-1a 32-bit for deterministic hashing */
#define FNV_OFFSET 2166136261u
#define FNV_PRIME  16777619u

static mp_spawn_table spawn_table;

typedef struct {
    int city_id;
    int object_id;
    int x;
    int y;
    int is_sea;
    int is_land;
    int route_cost;
    int is_xml_slot;       /* From XML <multiplayer> element */
    int xml_slot_id;       /* Assigned slot from XML, or -1 */
    int ai_distance;       /* Distance to nearest AI trade city */
} spawn_candidate;

static int collect_procedural_candidates(spawn_candidate *candidates, int max_candidates,
                                         const int *ai_xs, const int *ai_ys, int ai_count);

/* ---- Deterministic PRNG seeded from session_seed ---- */

static uint32_t prng_state;

static void prng_seed(uint32_t seed)
{
    prng_state = seed ^ FNV_OFFSET;
    if (prng_state == 0) {
        prng_state = FNV_OFFSET;
    }
}

static uint32_t prng_next(void)
{
    /* xorshift32 */
    prng_state ^= prng_state << 13;
    prng_state ^= prng_state >> 17;
    prng_state ^= prng_state << 5;
    return prng_state;
}

static int prng_range(int min_val, int max_val)
{
    if (min_val >= max_val) {
        return min_val;
    }
    uint32_t range = (uint32_t)(max_val - min_val + 1);
    return min_val + (int)(prng_next() % range);
}

/* ---- Distance calculation ---- */

static int distance_sq(int x1, int y1, int x2, int y2)
{
    int dx = x1 - x2;
    int dy = y1 - y2;
    return dx * dx + dy * dy;
}

static int distance(int x1, int y1, int x2, int y2)
{
    int dsq = distance_sq(x1, y1, x2, y2);
    if (dsq <= 0) {
        return 0;
    }
    /* Integer square root approximation */
    int result = 1;
    int temp = dsq;
    while (temp > 1) {
        temp >>= 2;
        result <<= 1;
    }
    /* Newton's method refinement */
    for (int i = 0; i < 8; i++) {
        if (result == 0) break;
        result = (result + dsq / result) / 2;
    }
    return result;
}

/* ---- Candidate Collection ---- */

static int collect_xml_slot_candidates(spawn_candidate *candidates, int max_candidates)
{
    int count = 0;
    int num_cities = empire_city_get_array_size();

    for (int i = 0; i < num_cities && count < max_candidates; i++) {
        empire_city *city = empire_city_get(i);
        if (!city || !city->in_use) {
            continue;
        }
        /* Check for XML multiplayer metadata in replicated_flags.
         * Bits 0-7 = slot, bit 8 = host_start, bit 9 = allow_human.
         * A city with bit 9 set is an explicit multiplayer slot. */
#ifdef ENABLE_MULTIPLAYER
        if (city->replicated_flags & 0x200) {  /* bit 9 = allow_human */
            int xml_slot = city->replicated_flags & 0xFF;
            candidates[count].city_id = i;
            candidates[count].object_id = city->empire_object_id;

            /* Get position from empire object */
            const empire_object *obj = empire_object_get(city->empire_object_id);
            if (obj) {
                candidates[count].x = obj->x;
                candidates[count].y = obj->y;
            }

            candidates[count].is_sea = city->is_sea_trade;
            candidates[count].is_land = !city->is_sea_trade;
            candidates[count].route_cost = city->cost_to_open;
            candidates[count].is_xml_slot = 1;
            candidates[count].xml_slot_id = xml_slot;
            candidates[count].ai_distance = 0;
            count++;
        }
#endif
    }
    return count;
}

static int collect_ai_trade_cities(int *ai_xs, int *ai_ys, int max_ai)
{
    int count = 0;
    int num_cities = empire_city_get_array_size();

    for (int i = 0; i < num_cities && count < max_ai; i++) {
        empire_city *city = empire_city_get(i);
        if (!city || !city->in_use) {
            continue;
        }
        if (city->type != EMPIRE_CITY_TRADE && city->type != EMPIRE_CITY_FUTURE_TRADE) {
            continue;
        }

        const empire_object *obj = empire_object_get(city->empire_object_id);
        if (obj) {
            ai_xs[count] = obj->x;
            ai_ys[count] = obj->y;
            count++;
        }
    }
    return count;
}

static int compute_ai_min_distance(int x, int y, const int *ai_xs, const int *ai_ys, int ai_count)
{
    int min_dist = 999999;
    for (int i = 0; i < ai_count; i++) {
        int d = distance(x, y, ai_xs[i], ai_ys[i]);
        if (d < min_dist) {
            min_dist = d;
        }
    }
    return min_dist;
}

static int city_has_any_trade_resource(const empire_city *city)
{
    for (int r = 0; r < RESOURCE_MAX; r++) {
        if (city->buys_resource[r] || city->sells_resource[r]) {
            return 1;
        }
    }
    return 0;
}

static int city_has_valid_empire_object(const empire_city *city, const empire_object **out_obj)
{
    const empire_object *obj;

    if (!city || !city->in_use || city->empire_object_id < 0) {
        return 0;
    }

    obj = empire_object_get(city->empire_object_id);
    if (!obj || obj->type != EMPIRE_OBJECT_CITY) {
        return 0;
    }

    if (out_obj) {
        *out_obj = obj;
    }
    return 1;
}

static int city_is_trade_capable(const empire_city *city)
{
    if (!city || !city->in_use) {
        return 0;
    }
    return city->route_id > 0 && city_has_any_trade_resource(city);
}

static int city_is_spawn_candidate(int city_id, const empire_city *city)
{
    if (!city || !city->in_use) {
        return 0;
    }
    if (city->type == EMPIRE_CITY_OURS) {
        return 0;
    }
    if (!city_has_valid_empire_object(city, 0)) {
        return 0;
    }
    if (spawn_table.locked && mp_ownership_is_city_player_owned(city_id)) {
        return 0;
    }
    return 1;
}

static int spawn_table_contains_city(int city_id)
{
    for (int i = 0; i < spawn_table.spawn_count; i++) {
        if (spawn_table.spawns[i].valid &&
            spawn_table.spawns[i].empire_city_id == city_id) {
            return 1;
        }
    }

    for (int i = 0; i < spawn_table.reserved_count; i++) {
        if (spawn_table.reserved_spawns[i].empire_city_id == city_id) {
            return 1;
        }
    }

    return 0;
}

static void compact_reserved_spawns(void)
{
    int write_index = 0;

    for (int read_index = 0; read_index < spawn_table.reserved_count; read_index++) {
        mp_spawn_entry *entry = &spawn_table.reserved_spawns[read_index];
        if (!entry->valid || entry->empire_city_id < 0) {
            continue;
        }

        if (write_index != read_index) {
            spawn_table.reserved_spawns[write_index] = *entry;
        }
        write_index++;
    }

    while (write_index < spawn_table.reserved_count) {
        memset(&spawn_table.reserved_spawns[write_index], 0,
               sizeof(spawn_table.reserved_spawns[write_index]));
        write_index++;
    }

    spawn_table.reserved_count = (uint8_t)write_index;
}

static int count_dynamic_city_candidates(void)
{
    int ai_xs[200], ai_ys[200];
    int ai_count;
    spawn_candidate candidates[MP_WORLDGEN_MAX_CANDIDATES];

    compact_reserved_spawns();

    ai_count = collect_ai_trade_cities(ai_xs, ai_ys, 200);
    return collect_procedural_candidates(candidates, MP_WORLDGEN_MAX_CANDIDATES,
                                         ai_xs, ai_ys, ai_count);
}

static void collect_storable_resources(resource_type *resources, int *count)
{
    int out = 0;

    for (resource_type r = RESOURCE_MIN; r < RESOURCE_MAX; r++) {
        if (resource_is_storable(r)) {
            resources[out++] = r;
        }
    }

    if (count) {
        *count = out;
    }
}

static void assign_default_trade_profile(empire_city *city, full_empire_object *full)
{
    resource_type storable[RESOURCE_MAX];
    int storable_count = 0;
    uint32_t hash;
    int sell_count = 0;
    int buy_count = 0;

    if (!city || !full || city->route_id <= 0) {
        return;
    }

    collect_storable_resources(storable, &storable_count);
    if (storable_count <= 0) {
        return;
    }

    hash = spawn_table.session_seed ? spawn_table.session_seed : FNV_OFFSET;
    hash ^= (uint32_t)(city->empire_object_id + 1) * FNV_PRIME;
    hash ^= (uint32_t)(city->route_id + 17) * 1315423911u;

    for (int i = 0; i < storable_count && (sell_count < 2 || buy_count < 2); i++) {
        resource_type sell_resource = storable[(hash + (uint32_t)i) % (uint32_t)storable_count];
        resource_type buy_resource = storable[((hash >> 8) + (uint32_t)(i * 3 + 1)) % (uint32_t)storable_count];

        if (sell_count < 2 && !city->sells_resource[sell_resource]) {
            city->sells_resource[sell_resource] = 1;
            full->city_sells_resource[sell_resource] = 1500;
            trade_route_set(city->route_id, sell_resource, 1500, 0);
            sell_count++;
        }

        if (buy_count < 2 &&
            buy_resource != sell_resource &&
            !city->buys_resource[buy_resource]) {
            city->buys_resource[buy_resource] = 1;
            full->city_buys_resource[buy_resource] = 1500;
            trade_route_set(city->route_id, buy_resource, 1500, 1);
            buy_count++;
        }
    }

    if (sell_count == 0) {
        resource_type fallback = storable[(hash + 3u) % (uint32_t)storable_count];
        city->sells_resource[fallback] = 1;
        full->city_sells_resource[fallback] = 1500;
        trade_route_set(city->route_id, fallback, 1500, 0);
    }

    if (buy_count == 0) {
        resource_type fallback = storable[(hash + 7u) % (uint32_t)storable_count];
        if (!city->sells_resource[fallback]) {
            city->buys_resource[fallback] = 1;
            full->city_buys_resource[fallback] = 1500;
            trade_route_set(city->route_id, fallback, 1500, 1);
        }
    }
}

static int prepare_city_for_multiplayer(int city_id)
{
    empire_city *city = empire_city_get(city_id);
    full_empire_object *full;
    int route_id;

    if (!city_is_spawn_candidate(city_id, city)) {
        return 0;
    }

    full = empire_object_get_full(city->empire_object_id);
    if (!full) {
        return 0;
    }

    if (city->type != EMPIRE_CITY_TRADE &&
        city->type != EMPIRE_CITY_FUTURE_TRADE &&
        city->type != EMPIRE_CITY_DISTANT_ROMAN) {
        city->type = EMPIRE_CITY_TRADE;
        full->city_type = EMPIRE_CITY_TRADE;
    }

    route_id = city->route_id > 0 ? city->route_id : full->obj.trade_route_id;
    if (route_id <= 0) {
        route_id = trade_route_new();
    } else {
        trade_route_ensure_id(route_id);
    }
    city->route_id = route_id;
    full->obj.trade_route_id = route_id;

    if (city->cost_to_open <= 0) {
        city->cost_to_open = full->trade_route_cost > 0 ? full->trade_route_cost : 500;
    }
    full->trade_route_cost = city->cost_to_open;

    city->is_open = 1;
    full->trade_route_open = 1;

    if (city->trader_entry_delay <= 0) {
        city->trader_entry_delay = 4;
    }

    if (!city_has_any_trade_resource(city)) {
        assign_default_trade_profile(city, full);
    } else {
        for (resource_type r = RESOURCE_MIN; r < RESOURCE_MAX; r++) {
            if (city->sells_resource[r]) {
                int amount = full->city_sells_resource[r] > 0 ? full->city_sells_resource[r] : 1500;
                full->city_sells_resource[r] = amount;
                trade_route_set(route_id, r, amount, 0);
            }
            if (city->buys_resource[r]) {
                int amount = full->city_buys_resource[r] > 0 ? full->city_buys_resource[r] : 1500;
                full->city_buys_resource[r] = amount;
                trade_route_set(route_id, r, amount, 1);
            }
        }
    }

    city->is_sea_trade = empire_object_is_sea_trade_route(full->obj.trade_route_id);
    return 1;
}

static int prepare_spawn_entry_city(mp_spawn_entry *entry)
{
    empire_city *city;
    const empire_object *obj;

    if (!entry || !entry->valid) {
        return 0;
    }
    if (!prepare_city_for_multiplayer(entry->empire_city_id)) {
        return 0;
    }

    city = empire_city_get(entry->empire_city_id);
    if (!city || !city_has_valid_empire_object(city, &obj)) {
        return 0;
    }

    entry->empire_object_id = city->empire_object_id;
    entry->x = obj->x;
    entry->y = obj->y;
    entry->is_sea_trade = city->is_sea_trade ? 1 : 0;
    entry->is_land_trade = city->is_sea_trade ? 0 : 1;
    return 1;
}

static int collect_procedural_candidates(spawn_candidate *candidates, int max_candidates,
                                          const int *ai_xs, const int *ai_ys, int ai_count)
{
    int count = 0;
    int num_cities = empire_city_get_array_size();

    /* Look for any city that can be promoted into a player-owned multiplayer city. */
    for (int i = 0; i < num_cities && count < max_candidates; i++) {
        empire_city *city = empire_city_get(i);
        const empire_object *obj = 0;
        int is_trade_ready;
        int ai_dist;

        if (!city_is_spawn_candidate(i, city)) {
            continue;
        }

        if (spawn_table_contains_city(i)) {
            continue;
        }

        if (!city_has_valid_empire_object(city, &obj)) {
            continue;
        }

        is_trade_ready = city_is_trade_capable(city);
        ai_dist = compute_ai_min_distance(obj->x, obj->y, ai_xs, ai_ys, ai_count);

        candidates[count].city_id = i;
        candidates[count].object_id = city->empire_object_id;
        candidates[count].x = obj->x;
        candidates[count].y = obj->y;
        candidates[count].is_sea = is_trade_ready ? city->is_sea_trade : 0;
        candidates[count].is_land = is_trade_ready ? !city->is_sea_trade : 1;
        candidates[count].route_cost = city->cost_to_open > 0 ? city->cost_to_open : 500;
        candidates[count].is_xml_slot = 0;
        candidates[count].xml_slot_id = -1;
        candidates[count].ai_distance = ai_dist;
        count++;
    }

    return count;
}

/* ---- Cluster center computation ---- */

static void compute_cluster_center(const spawn_candidate *candidates, int count,
                                    int *center_x, int *center_y)
{
    if (count == 0) {
        *center_x = 500;
        *center_y = 500;
        return;
    }

    long sum_x = 0, sum_y = 0;
    for (int i = 0; i < count; i++) {
        sum_x += candidates[i].x;
        sum_y += candidates[i].y;
    }
    *center_x = (int)(sum_x / count);
    *center_y = (int)(sum_y / count);
}

/* ---- Fairness scoring ---- */

static int compute_fairness_score(const spawn_candidate *c,
                                   int center_x, int center_y,
                                   int avg_route_cost)
{
    int score = 0;

    /* Distance from cluster center - prefer closer */
    int dist_from_center = distance(c->x, c->y, center_x, center_y);
    if (dist_from_center > MAX_CLUSTER_RADIUS) {
        score += (dist_from_center - MAX_CLUSTER_RADIUS) / 10;
    }

    /* Route cost deviation from average */
    int cost_diff = c->route_cost - avg_route_cost;
    if (cost_diff < 0) cost_diff = -cost_diff;
    score += cost_diff / 100;

    /* Penalty for no sea access (in maps where sea trade exists) */
    if (!c->is_sea && !c->is_land) {
        score += 100;
    }

    return score;
}

/* ---- Selection with constraints ---- */

static int check_min_distance(const mp_spawn_entry *spawns, int spawn_count,
                               int x, int y, int min_dist)
{
    for (int i = 0; i < spawn_count; i++) {
        if (!spawns[i].valid) {
            continue;
        }
        int d = distance(x, y, spawns[i].x, spawns[i].y);
        if (d < min_dist) {
            return 0;
        }
    }
    return 1;
}

static int select_spawns_from_candidates(spawn_candidate *candidates, int candidate_count,
                                          mp_spawn_entry *spawns, int player_count,
                                          const int *ai_xs, const int *ai_ys, int ai_count)
{
    if (candidate_count < player_count) {
        log_error("Not enough spawn candidates", 0, candidate_count);
        return 0;
    }

    /* Compute cluster center and average route cost */
    int center_x, center_y;
    compute_cluster_center(candidates, candidate_count, &center_x, &center_y);

    int total_cost = 0;
    for (int i = 0; i < candidate_count; i++) {
        total_cost += candidates[i].route_cost;
    }
    int avg_cost = candidate_count > 0 ? total_cost / candidate_count : 0;

    /* Score all candidates */
    int scores[MP_WORLDGEN_MAX_CANDIDATES];
    for (int i = 0; i < candidate_count; i++) {
        scores[i] = compute_fairness_score(&candidates[i], center_x, center_y, avg_cost);
    }

    /* Fisher-Yates shuffle with deterministic PRNG, then sort by score */
    int indices[MP_WORLDGEN_MAX_CANDIDATES];
    for (int i = 0; i < candidate_count; i++) {
        indices[i] = i;
    }
    for (int i = candidate_count - 1; i > 0; i--) {
        int j = prng_range(0, i);
        int tmp = indices[i];
        indices[i] = indices[j];
        indices[j] = tmp;
    }

    /* Stable insertion sort by score (preserves random order for equal scores) */
    for (int i = 1; i < candidate_count; i++) {
        int key = indices[i];
        int key_score = scores[key];
        int j = i - 1;
        while (j >= 0 && scores[indices[j]] > key_score) {
            indices[j + 1] = indices[j];
            j--;
        }
        indices[j + 1] = key;
    }

    /* Greedy selection with constraint checking */
    int selected = 0;
    for (int i = 0; i < candidate_count && selected < player_count; i++) {
        int ci = indices[i];
        spawn_candidate *c = &candidates[ci];

        /* Check minimum distance from already-selected spawns */
        if (!check_min_distance(spawns, selected, c->x, c->y, MIN_PLAYER_DISTANCE)) {
            continue;
        }

        /* Check minimum distance from AI cities */
        int ai_dist = compute_ai_min_distance(c->x, c->y, ai_xs, ai_ys, ai_count);
        if (ai_dist < MIN_AI_DISTANCE) {
            continue;
        }

        /* Accept this candidate */
        mp_spawn_entry *entry = &spawns[selected];
        entry->valid = 1;
        entry->slot_id = (uint8_t)selected;
        entry->empire_city_id = c->city_id;
        entry->empire_object_id = c->object_id;
        entry->x = c->x;
        entry->y = c->y;
        entry->is_sea_trade = c->is_sea;
        entry->is_land_trade = c->is_land;
        entry->nearest_ai_distance = ai_dist;
        entry->nearest_player_distance = 0; /* Will compute after */
        entry->fairness_score = scores[ci];
        selected++;
    }

    /* Compute nearest-player distances */
    for (int i = 0; i < selected; i++) {
        int min_pd = 999999;
        for (int j = 0; j < selected; j++) {
            if (i == j) continue;
            int d = distance(spawns[i].x, spawns[i].y, spawns[j].x, spawns[j].y);
            if (d < min_pd) {
                min_pd = d;
            }
        }
        spawns[i].nearest_player_distance = min_pd;
    }

    return selected;
}

/* ---- Public API ---- */

void mp_worldgen_init(void)
{
    memset(&spawn_table, 0, sizeof(spawn_table));
}

void mp_worldgen_clear(void)
{
    mp_worldgen_init();
}

int mp_worldgen_generate_player_spawns(uint32_t session_seed, int player_count,
                                        int use_xml_slots)
{
    if (player_count < 0 || player_count > MP_MAX_PLAYERS) {
        log_error("Invalid player count for worldgen", 0, player_count);
        return 0;
    }

    memset(&spawn_table, 0, sizeof(spawn_table));
    spawn_table.session_seed = session_seed;
    spawn_table.player_count = (uint8_t)player_count;

    if (player_count == 0) {
        log_info("Worldgen: initialized empty active-player spawn table", 0, 0);
        return 1;
    }

    prng_seed(session_seed);

    /* Collect AI city positions for distance checks */
    int ai_xs[200], ai_ys[200];
    int ai_count = collect_ai_trade_cities(ai_xs, ai_ys, 200);

    spawn_candidate candidates[MP_WORLDGEN_MAX_CANDIDATES];
    int candidate_count = 0;

    /* Phase 1: Try XML-defined slots first */
    if (use_xml_slots) {
        spawn_candidate xml_candidates[MP_WORLDGEN_MAX_SPAWNS];
        int xml_count = collect_xml_slot_candidates(xml_candidates, MP_WORLDGEN_MAX_SPAWNS);

        if (xml_count >= player_count) {
            /* Enough XML slots - use them directly */
            int selected = select_spawns_from_candidates(xml_candidates, xml_count,
                spawn_table.spawns, player_count, ai_xs, ai_ys, ai_count);
            if (selected >= player_count) {
                spawn_table.spawn_count = (uint8_t)selected;
                log_info("Worldgen: used XML slots", 0, selected);
                return 1;
            }
        }

        /* Partial XML slots - add to candidate pool */
        for (int i = 0; i < xml_count && candidate_count < MP_WORLDGEN_MAX_CANDIDATES; i++) {
            candidates[candidate_count++] = xml_candidates[i];
        }
    }

    /* Phase 2: Collect procedural candidates */
    int proc_count = collect_procedural_candidates(
        candidates + candidate_count,
        MP_WORLDGEN_MAX_CANDIDATES - candidate_count,
        ai_xs, ai_ys, ai_count);
    candidate_count += proc_count;

    /* Phase 3: Select with constraints */
    int selected = select_spawns_from_candidates(candidates, candidate_count,
        spawn_table.spawns, player_count, ai_xs, ai_ys, ai_count);

    if (selected < player_count) {
        /* Retry with relaxed constraints if strict selection failed */
        log_info("Worldgen: relaxing constraints, only found", 0, selected);

        /* Reset and try without minimum distance constraints */
        memset(spawn_table.spawns, 0, sizeof(spawn_table.spawns));
        int relaxed_selected = 0;

        /* Just take the best-scored candidates in order */
        for (int i = 0; i < candidate_count && relaxed_selected < player_count; i++) {
            spawn_candidate *c = &candidates[i];
            mp_spawn_entry *entry = &spawn_table.spawns[relaxed_selected];
            entry->valid = 1;
            entry->slot_id = (uint8_t)relaxed_selected;
            entry->empire_city_id = c->city_id;
            entry->empire_object_id = c->object_id;
            entry->x = c->x;
            entry->y = c->y;
            entry->is_sea_trade = c->is_sea;
            entry->is_land_trade = c->is_land;
            entry->nearest_ai_distance = c->ai_distance;
            entry->nearest_player_distance = 0;
            entry->fairness_score = 0;
            relaxed_selected++;
        }
        selected = relaxed_selected;
    }

    if (selected < 2) {
        log_error("Worldgen: cannot find enough spawn positions", 0, selected);
        return 0;
    }

    spawn_table.spawn_count = (uint8_t)selected;
    log_info("Worldgen: spawns generated", 0, selected);
    return 1;
}

const mp_spawn_table *mp_worldgen_get_spawn_table(void)
{
    return &spawn_table;
}

mp_spawn_table *mp_worldgen_get_spawn_table_mutable(void)
{
    return &spawn_table;
}

int mp_worldgen_reroll(uint32_t new_seed, int player_count)
{
    if (spawn_table.locked) {
        log_error("Cannot reroll: spawn table is locked", 0, 0);
        return 0;
    }
    return mp_worldgen_generate_player_spawns(new_seed, player_count, 1);
}

void mp_worldgen_lock(void)
{
    spawn_table.locked = 1;
    log_info("Worldgen: spawn table locked", 0, 0);
}

const mp_spawn_entry *mp_worldgen_get_spawn_for_slot(uint8_t slot_id)
{
    for (int i = 0; i < spawn_table.spawn_count; i++) {
        if (spawn_table.spawns[i].valid && spawn_table.spawns[i].slot_id == slot_id) {
            return &spawn_table.spawns[i];
        }
    }
    return 0;
}

int mp_worldgen_apply_spawns(void)
{
    if (!spawn_table.locked) {
        log_error("Cannot apply spawns: table not locked", 0, 0);
        return 0;
    }

    int applied = 0;
    for (int i = 0; i < spawn_table.spawn_count; i++) {
        mp_spawn_entry *entry = &spawn_table.spawns[i];
        if (!entry->valid) {
            continue;
        }

        /* Configure the empire city for this player slot */
        empire_city *city = empire_city_get(entry->empire_city_id);
        if (!city || !city->in_use) {
            log_error("Worldgen: invalid city for slot", 0, entry->slot_id);
            continue;
        }

        if (!prepare_spawn_entry_city(entry)) {
            log_error("Worldgen: failed to prepare city for slot", 0, entry->slot_id);
            continue;
        }

        /* Mark the city as player-owned in ownership system */
        mp_player *player = mp_player_registry_get_by_slot(entry->slot_id);
        if (player) {
            mp_ownership_set_city(entry->empire_city_id,
                player->is_local ? MP_OWNER_LOCAL_PLAYER : MP_OWNER_REMOTE_PLAYER,
                player->player_id);
            mp_player_registry_set_city(player->player_id, entry->empire_city_id);
            mp_player_registry_set_assigned_city(player->player_id, entry->empire_city_id);

#ifdef ENABLE_MULTIPLAYER
            city->owner_type = player->is_local ? CITY_OWNER_LOCAL : CITY_OWNER_REMOTE;
            city->owner_player_id = player->player_id;
            city->authoritative_city_id = entry->empire_city_id;
#endif
            applied++;
            log_info("Worldgen: applied spawn for slot", player->name, entry->slot_id);
        }
    }

    return applied;
}

/* ---- Reserved spawns for late join ---- */

int mp_worldgen_expand_dynamic_city_pool(int additional_count)
{
    int capacity;
    int ai_xs[200], ai_ys[200];
    int ai_count;
    spawn_candidate candidates[MP_WORLDGEN_MAX_CANDIDATES];
    spawn_candidate filtered[MP_WORLDGEN_MAX_CANDIDATES];
    int candidate_count;
    int filtered_count = 0;
    int selected;
    mp_spawn_entry temp_spawns[MP_WORLDGEN_MAX_SPAWNS];

    if (additional_count <= 0) {
        return 0;
    }

    compact_reserved_spawns();

    capacity = MP_WORLDGEN_MAX_SPAWNS -
               (spawn_table.spawn_count + mp_worldgen_get_reserved_count());
    if (capacity <= 0) {
        return 0;
    }
    if (additional_count > capacity) {
        additional_count = capacity;
    }

    ai_count = collect_ai_trade_cities(ai_xs, ai_ys, 200);
    candidate_count = collect_procedural_candidates(
        candidates, MP_WORLDGEN_MAX_CANDIDATES, ai_xs, ai_ys, ai_count);

    for (int c = 0; c < candidate_count; c++) {
        if (!spawn_table_contains_city(candidates[c].city_id)) {
            filtered[filtered_count++] = candidates[c];
        }
    }

    memset(temp_spawns, 0, sizeof(temp_spawns));
    selected = select_spawns_from_candidates(filtered, filtered_count,
        temp_spawns, additional_count, ai_xs, ai_ys, ai_count);

    for (int i = 0; i < selected && spawn_table.reserved_count < MP_WORLDGEN_MAX_SPAWNS; i++) {
        int target = spawn_table.reserved_count++;
        spawn_table.reserved_spawns[target] = temp_spawns[i];
        spawn_table.reserved_spawns[target].slot_id = 0xFF;
        spawn_table.reserved_spawns[target].valid = 1;
    }

    if (selected > 0) {
        log_info("Worldgen: dynamic pool expanded", 0, selected);
    }
    return selected;
}

int mp_worldgen_generate_reserved_spawns(int reserve_count)
{
    if (reserve_count <= 0) {
        memset(spawn_table.reserved_spawns, 0, sizeof(spawn_table.reserved_spawns));
        spawn_table.reserved_count = 0;
        return 0;
    }
    if (reserve_count > MP_WORLDGEN_MAX_SPAWNS) {
        reserve_count = MP_WORLDGEN_MAX_SPAWNS;
    }

    memset(spawn_table.reserved_spawns, 0, sizeof(spawn_table.reserved_spawns));
    spawn_table.reserved_count = 0;
    mp_worldgen_expand_dynamic_city_pool(reserve_count);

    log_info("Worldgen: reserved spawns generated", 0, spawn_table.reserved_count);
    return spawn_table.reserved_count;
}

int mp_worldgen_generate_dynamic_city_pool(int pool_count)
{
    return mp_worldgen_generate_reserved_spawns(pool_count);
}

int mp_worldgen_assign_reserved_spawn(uint8_t slot_id)
{
    compact_reserved_spawns();

    for (int i = 0; i < spawn_table.reserved_count; i++) {
        if (spawn_table.reserved_spawns[i].valid) {
            int city_id = spawn_table.reserved_spawns[i].empire_city_id;
            mp_spawn_entry activated = spawn_table.reserved_spawns[i];

            activated.slot_id = slot_id;
            activated.valid = 1;
            if (!prepare_spawn_entry_city(&activated)) {
                log_error("Worldgen: failed to prepare reserved city", 0, city_id);
                continue;
            }

            for (int move = i; move + 1 < spawn_table.reserved_count; move++) {
                spawn_table.reserved_spawns[move] = spawn_table.reserved_spawns[move + 1];
            }
            memset(&spawn_table.reserved_spawns[spawn_table.reserved_count - 1], 0,
                   sizeof(spawn_table.reserved_spawns[spawn_table.reserved_count - 1]));
            spawn_table.reserved_count--;

            if (spawn_table.spawn_count < MP_WORLDGEN_MAX_SPAWNS) {
                spawn_table.spawns[spawn_table.spawn_count++] = activated;
                spawn_table.player_count = spawn_table.spawn_count;
            }

            log_info("Worldgen: assigned reserved spawn to slot", 0, (int)slot_id);
            return city_id;
        }
    }
    return -1;
}

void mp_worldgen_return_to_reserved(int empire_city_id)
{
    compact_reserved_spawns();

    for (int s = 0; s < spawn_table.spawn_count; s++) {
        if (spawn_table.spawns[s].valid &&
            spawn_table.spawns[s].empire_city_id == empire_city_id) {
            mp_spawn_entry returned = spawn_table.spawns[s];

            returned.valid = 1;
            returned.slot_id = 0xFF;

            for (int move = s; move + 1 < spawn_table.spawn_count; move++) {
                spawn_table.spawns[move] = spawn_table.spawns[move + 1];
            }
            memset(&spawn_table.spawns[spawn_table.spawn_count - 1], 0,
                   sizeof(spawn_table.spawns[spawn_table.spawn_count - 1]));
            spawn_table.spawn_count--;
            if (spawn_table.player_count > spawn_table.spawn_count) {
                spawn_table.player_count = spawn_table.spawn_count;
            }

            if (spawn_table.reserved_count < MP_WORLDGEN_MAX_SPAWNS) {
                spawn_table.reserved_spawns[spawn_table.reserved_count++] = returned;
            }

            log_info("Worldgen: returned city to reserved pool", 0, empire_city_id);
            return;
        }
    }
}

int mp_worldgen_get_reserved_count(void)
{
    int count = 0;
    for (int i = 0; i < spawn_table.reserved_count; i++) {
        if (spawn_table.reserved_spawns[i].valid) {
            count++;
        }
    }
    return count;
}

int mp_worldgen_get_late_join_capacity(void)
{
    int reserved;
    int slot_capacity;
    int candidate_capacity;

    compact_reserved_spawns();

    slot_capacity = MP_WORLDGEN_MAX_SPAWNS - spawn_table.spawn_count;
    if (slot_capacity <= 0) {
        return 0;
    }

    reserved = mp_worldgen_get_reserved_count();
    if (reserved >= slot_capacity) {
        return slot_capacity;
    }

    candidate_capacity = count_dynamic_city_candidates();
    if (candidate_capacity > slot_capacity - reserved) {
        candidate_capacity = slot_capacity - reserved;
    }

    return reserved + candidate_capacity;
}

int mp_worldgen_get_dynamic_city_pool_remaining(void)
{
    return mp_worldgen_get_late_join_capacity();
}

/* ---- Serialization ---- */

void mp_worldgen_serialize(uint8_t *buffer, uint32_t *size)
{
    net_serializer s;
    net_serializer_init(&s, buffer, 4096);

    net_write_u32(&s, spawn_table.session_seed);
    net_write_u8(&s, spawn_table.player_count);
    net_write_u8(&s, spawn_table.spawn_count);
    net_write_u32(&s, spawn_table.generation_tick);
    net_write_u8(&s, (uint8_t)spawn_table.locked);

    for (int i = 0; i < spawn_table.spawn_count; i++) {
        mp_spawn_entry *e = &spawn_table.spawns[i];
        net_write_u8(&s, (uint8_t)e->valid);
        net_write_u8(&s, e->slot_id);
        net_write_i32(&s, e->empire_city_id);
        net_write_i32(&s, e->empire_object_id);
        net_write_i32(&s, e->x);
        net_write_i32(&s, e->y);
        net_write_u8(&s, (uint8_t)e->is_sea_trade);
        net_write_u8(&s, (uint8_t)e->is_land_trade);
        net_write_i32(&s, e->nearest_ai_distance);
        net_write_i32(&s, e->nearest_player_distance);
        net_write_i32(&s, e->fairness_score);
    }

    /* Serialize reserved spawns */
    net_write_u8(&s, spawn_table.reserved_count);
    for (int i = 0; i < spawn_table.reserved_count; i++) {
        mp_spawn_entry *e = &spawn_table.reserved_spawns[i];
        net_write_u8(&s, (uint8_t)e->valid);
        net_write_u8(&s, e->slot_id);
        net_write_i32(&s, e->empire_city_id);
        net_write_i32(&s, e->empire_object_id);
        net_write_i32(&s, e->x);
        net_write_i32(&s, e->y);
        net_write_u8(&s, (uint8_t)e->is_sea_trade);
        net_write_u8(&s, (uint8_t)e->is_land_trade);
        net_write_i32(&s, e->nearest_ai_distance);
        net_write_i32(&s, e->nearest_player_distance);
        net_write_i32(&s, e->fairness_score);
    }

    *size = (uint32_t)net_serializer_position(&s);
}

void mp_worldgen_deserialize(const uint8_t *buffer, uint32_t size)
{
    mp_worldgen_clear();

    net_serializer s;
    net_serializer_init(&s, (uint8_t *)buffer, size);

    spawn_table.session_seed = net_read_u32(&s);
    spawn_table.player_count = net_read_u8(&s);
    spawn_table.spawn_count = net_read_u8(&s);
    spawn_table.generation_tick = net_read_u32(&s);
    spawn_table.locked = net_read_u8(&s);

    for (int i = 0; i < spawn_table.spawn_count && !net_serializer_has_overflow(&s); i++) {
        mp_spawn_entry *e = &spawn_table.spawns[i];
        e->valid = net_read_u8(&s);
        e->slot_id = net_read_u8(&s);
        e->empire_city_id = net_read_i32(&s);
        e->empire_object_id = net_read_i32(&s);
        e->x = net_read_i32(&s);
        e->y = net_read_i32(&s);
        e->is_sea_trade = net_read_u8(&s);
        e->is_land_trade = net_read_u8(&s);
        e->nearest_ai_distance = net_read_i32(&s);
        e->nearest_player_distance = net_read_i32(&s);
        e->fairness_score = net_read_i32(&s);
    }

    /* Deserialize reserved spawns (if present in buffer) */
    if (!net_serializer_has_overflow(&s) && net_serializer_remaining(&s) > 0) {
        spawn_table.reserved_count = net_read_u8(&s);
        if (spawn_table.reserved_count > MP_WORLDGEN_MAX_SPAWNS) {
            spawn_table.reserved_count = MP_WORLDGEN_MAX_SPAWNS;
        }
        for (int i = 0; i < spawn_table.reserved_count && !net_serializer_has_overflow(&s); i++) {
            mp_spawn_entry *e = &spawn_table.reserved_spawns[i];
            e->valid = net_read_u8(&s);
            e->slot_id = net_read_u8(&s);
            e->empire_city_id = net_read_i32(&s);
            e->empire_object_id = net_read_i32(&s);
            e->x = net_read_i32(&s);
            e->y = net_read_i32(&s);
            e->is_sea_trade = net_read_u8(&s);
            e->is_land_trade = net_read_u8(&s);
            e->nearest_ai_distance = net_read_i32(&s);
            e->nearest_player_distance = net_read_i32(&s);
            e->fairness_score = net_read_i32(&s);
        }
    }

    for (int i = 0; i < spawn_table.spawn_count; i++) {
        if (spawn_table.spawns[i].valid) {
            prepare_spawn_entry_city(&spawn_table.spawns[i]);
        }
    }

    for (int i = 0; i < spawn_table.reserved_count; i++) {
        if (spawn_table.reserved_spawns[i].empire_city_id >= 0) {
            prepare_spawn_entry_city(&spawn_table.reserved_spawns[i]);
        }
    }

    compact_reserved_spawns();
}

#endif /* ENABLE_MULTIPLAYER */
