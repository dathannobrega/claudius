#ifndef NETWORK_DISCOVERY_LAN_H
#define NETWORK_DISCOVERY_LAN_H

#ifdef ENABLE_MULTIPLAYER

#include <stdint.h>

#define NET_DISCOVERY_MAGIC         0x41554744  /* "AUGD" */
#define NET_MAX_DISCOVERED_HOSTS    16
#define NET_DISCOVERY_BROADCAST_MS  2000
#define NET_DISCOVERY_WORLD_UUID_SIZE 16

typedef enum {
    NET_DISCOVERY_PHASE_LOBBY = 0,
    NET_DISCOVERY_PHASE_IN_GAME,
    NET_DISCOVERY_PHASE_RESUME_LOBBY
} net_discovery_session_phase;

typedef enum {
    NET_DISCOVERY_JOIN_OPEN_LOBBY = 0,
    NET_DISCOVERY_JOIN_RECONNECT_ONLY,
    NET_DISCOVERY_JOIN_LATE_JOIN_ALLOWED,
    NET_DISCOVERY_JOIN_CLOSED
} net_discovery_join_policy;

typedef struct {
    char host_name[32];
    uint16_t game_port;
    uint8_t player_count;
    uint8_t max_players;
    uint32_t session_id;
    uint16_t protocol_version;
    uint8_t session_phase;
    uint8_t join_policy;
    uint8_t reserved_slots_free;
    uint8_t world_instance_uuid[NET_DISCOVERY_WORLD_UUID_SIZE];
    uint32_t resume_generation;
} net_discovery_announcement;

typedef struct {
    int active;
    char host_name[32];
    char host_ip[48];
    uint16_t port;
    uint8_t player_count;
    uint8_t max_players;
    uint32_t session_id;
    uint16_t protocol_version;
    uint8_t session_phase;
    uint8_t join_policy;
    uint8_t reserved_slots_free;
    uint8_t world_instance_uuid[NET_DISCOVERY_WORLD_UUID_SIZE];
    uint32_t resume_generation;
    uint32_t last_seen_ms;
} net_discovered_host;

void net_discovery_init(void);
void net_discovery_shutdown(void);

/* Host: start announcing on LAN */
int net_discovery_start_announcing(const net_discovery_announcement *announcement);
void net_discovery_stop_announcing(void);
void net_discovery_update_announcing(const net_discovery_announcement *announcement);

/* Client: start listening for announcements */
int net_discovery_start_listening(void);
void net_discovery_stop_listening(void);

/* Update must be called each frame */
void net_discovery_update(void);

/* Query discovered hosts */
int net_discovery_get_host_count(void);
const net_discovered_host *net_discovery_get_host(int index);
void net_discovery_clear_hosts(void);

const char *net_discovery_session_phase_name(net_discovery_session_phase phase);
const char *net_discovery_join_policy_name(net_discovery_join_policy policy);

#endif /* ENABLE_MULTIPLAYER */

#endif /* NETWORK_DISCOVERY_LAN_H */
