#include "discovery_lan.h"

#ifdef ENABLE_MULTIPLAYER

#include "transport_tcp.h"
#include "transport_udp.h"
#include "serialize.h"
#include "session.h"
#include "core/log.h"
#include "multiplayer/mp_debug_log.h"

#include <string.h>
#include <stdio.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif

#define ANNOUNCE_PACKET_SIZE 96
#define ANNOUNCE_PACKET_LEGACY_SIZE 44

/*
 * HOST_EXPIRY_MS must be > 2 * NET_DISCOVERY_BROADCAST_MS to tolerate
 * at least one dropped packet. With broadcast at 2000ms, 8000ms gives
 * a 4:1 ratio — 3 consecutive missed packets before expiry.
 */
#define HOST_EXPIRY_MS 8000

typedef struct {
    int announcing;
    int listening;
    int udp_fd;
    uint32_t last_broadcast_ms;

    /* Announce data */
    net_discovery_announcement announcement;

    /* Discovered hosts */
    net_discovered_host hosts[NET_MAX_DISCOVERED_HOSTS];
    int host_count;
} discovery_data;

static discovery_data data;

static const uint8_t ZERO_WORLD_UUID[NET_DISCOVERY_WORLD_UUID_SIZE] = {0};

static void copy_announcement(net_discovery_announcement *dst,
                              const net_discovery_announcement *src)
{
    if (!dst || !src) {
        return;
    }
    memcpy(dst, src, sizeof(*dst));
    dst->host_name[sizeof(dst->host_name) - 1] = '\0';
}

void net_discovery_init(void)
{
    memset(&data, 0, sizeof(discovery_data));
    data.udp_fd = -1;
    MP_LOG_INFO("DISCOVERY", "Discovery system initialized (port=%d, broadcast_ms=%d, expiry_ms=%d)",
                NET_DISCOVERY_PORT, NET_DISCOVERY_BROADCAST_MS, HOST_EXPIRY_MS);
}

void net_discovery_shutdown(void)
{
    net_discovery_stop_announcing();
    net_discovery_stop_listening();
    MP_LOG_INFO("DISCOVERY", "Discovery system shutdown");
}

int net_discovery_start_announcing(const net_discovery_announcement *announcement)
{
    if (data.announcing) {
        return 1;
    }
    if (!announcement) {
        return 0;
    }

    if (data.udp_fd < 0) {
        data.udp_fd = net_udp_create(0); /* Ephemeral port for sending */
        if (data.udp_fd < 0) {
            log_error("Failed to create discovery broadcast socket", 0, 0);
            MP_LOG_ERROR("DISCOVERY", "Failed to create broadcast socket (ephemeral)");
            return 0;
        }
        if (!net_udp_enable_broadcast(data.udp_fd)) {
            MP_LOG_ERROR("DISCOVERY", "Failed to enable broadcast on discovery socket — "
                         "host will not be visible on LAN");
        }
    }

    copy_announcement(&data.announcement, announcement);
    data.announcing = 1;
    data.last_broadcast_ms = 0;

    log_info("LAN discovery announcing started", data.announcement.host_name, 0);
    MP_LOG_INFO("DISCOVERY", "Start announcing: name='%s' port=%d session=0x%08x players=%d/%d phase=%s policy=%s",
                data.announcement.host_name, (int)data.announcement.game_port,
                data.announcement.session_id, data.announcement.player_count,
                data.announcement.max_players,
                net_discovery_session_phase_name(
                    (net_discovery_session_phase)data.announcement.session_phase),
                net_discovery_join_policy_name(
                    (net_discovery_join_policy)data.announcement.join_policy));
    return 1;
}

void net_discovery_stop_announcing(void)
{
    if (!data.announcing) {
        return;
    }
    data.announcing = 0;
    MP_LOG_INFO("DISCOVERY", "Stop announcing");
    if (!data.listening && data.udp_fd >= 0) {
        net_udp_close(data.udp_fd);
        data.udp_fd = -1;
    }
}

void net_discovery_update_announcing(const net_discovery_announcement *announcement)
{
    if (!announcement) {
        return;
    }
    copy_announcement(&data.announcement, announcement);
}

int net_discovery_start_listening(void)
{
    if (data.listening) {
        return 1;
    }

    if (data.udp_fd < 0) {
        data.udp_fd = net_udp_create(NET_DISCOVERY_PORT);
        if (data.udp_fd < 0) {
            log_error("Failed to create discovery listen socket", 0, 0);
            MP_LOG_ERROR("DISCOVERY", "Failed to bind listen socket on port %d — "
                         "another instance may be running or port is blocked by firewall",
                         NET_DISCOVERY_PORT);
            return 0;
        }
        if (!net_udp_enable_broadcast(data.udp_fd)) {
            MP_LOG_WARN("DISCOVERY", "Failed to enable broadcast on listen socket");
        }
    }

    data.listening = 1;
    data.host_count = 0;
    memset(data.hosts, 0, sizeof(data.hosts));

    log_info("LAN discovery listening started", 0, 0);
    MP_LOG_INFO("DISCOVERY", "Start listening on UDP port %d", NET_DISCOVERY_PORT);
    return 1;
}

void net_discovery_stop_listening(void)
{
    if (!data.listening) {
        return;
    }
    data.listening = 0;
    MP_LOG_INFO("DISCOVERY", "Stop listening");
    if (!data.announcing && data.udp_fd >= 0) {
        net_udp_close(data.udp_fd);
        data.udp_fd = -1;
    }
}

static void send_announcement(void)
{
    uint8_t buf[ANNOUNCE_PACKET_SIZE];
    net_serializer s;
    net_serializer_init(&s, buf, sizeof(buf));

    net_write_u32(&s, NET_DISCOVERY_MAGIC);
    net_write_u32(&s, data.announcement.session_id);
    net_write_u16(&s, data.announcement.game_port);
    net_write_u8(&s, data.announcement.player_count);
    net_write_u8(&s, data.announcement.max_players);
    net_write_raw(&s, data.announcement.host_name, 32);
    net_write_u16(&s, data.announcement.protocol_version);
    net_write_u8(&s, data.announcement.session_phase);
    net_write_u8(&s, data.announcement.join_policy);
    net_write_u8(&s, data.announcement.reserved_slots_free);
    net_write_raw(&s, data.announcement.world_instance_uuid,
                  NET_DISCOVERY_WORLD_UUID_SIZE);
    net_write_u32(&s, data.announcement.resume_generation);

    if (!net_serializer_has_overflow(&s)) {
        int sent = net_udp_send_broadcast(data.udp_fd, NET_DISCOVERY_PORT,
                                          buf, (size_t)net_serializer_position(&s));
        MP_LOG_TRACE("DISCOVERY", "Broadcast announcement sent (%d bytes, result=%d)",
                     (int)net_serializer_position(&s), sent);
    }
}

static void process_announcement(const uint8_t *buf, size_t size, const net_udp_addr *from)
{
    if (size < ANNOUNCE_PACKET_LEGACY_SIZE) {
        MP_LOG_WARN("DISCOVERY", "Received undersized announcement: %d bytes (need >= %d)",
                    (int)size, ANNOUNCE_PACKET_LEGACY_SIZE);
        return;
    }

    net_serializer s;
    net_serializer_init(&s, (uint8_t *)buf, size);

    uint32_t magic = net_read_u32(&s);
    if (magic != NET_DISCOVERY_MAGIC) {
        MP_LOG_DEBUG("DISCOVERY", "Received packet with wrong magic: 0x%08x (expected 0x%08x)",
                     magic, NET_DISCOVERY_MAGIC);
        return;
    }

    uint32_t sess_id = net_read_u32(&s);
    uint16_t game_port = net_read_u16(&s);
    uint8_t player_count = net_read_u8(&s);
    uint8_t max_players = net_read_u8(&s);
    char host_name[32];
    uint16_t protocol_version = 0;
    uint8_t session_phase = NET_DISCOVERY_PHASE_LOBBY;
    uint8_t join_policy = NET_DISCOVERY_JOIN_OPEN_LOBBY;
    uint8_t reserved_slots_free = 0;
    uint8_t world_instance_uuid[NET_DISCOVERY_WORLD_UUID_SIZE];
    uint32_t resume_generation = 0;
    net_read_raw(&s, host_name, 32);
    host_name[31] = '\0';
    memcpy(world_instance_uuid, ZERO_WORLD_UUID, sizeof(world_instance_uuid));

    if (!net_serializer_has_overflow(&s) && net_serializer_remaining(&s) >= 2) {
        protocol_version = net_read_u16(&s);
    }
    if (!net_serializer_has_overflow(&s) && net_serializer_remaining(&s) >= 1) {
        session_phase = net_read_u8(&s);
    }
    if (!net_serializer_has_overflow(&s) && net_serializer_remaining(&s) >= 1) {
        join_policy = net_read_u8(&s);
    }
    if (!net_serializer_has_overflow(&s) && net_serializer_remaining(&s) >= 1) {
        reserved_slots_free = net_read_u8(&s);
    }
    if (!net_serializer_has_overflow(&s) &&
        net_serializer_remaining(&s) >= NET_DISCOVERY_WORLD_UUID_SIZE) {
        net_read_raw(&s, world_instance_uuid, NET_DISCOVERY_WORLD_UUID_SIZE);
    }
    if (!net_serializer_has_overflow(&s) && net_serializer_remaining(&s) >= 4) {
        resume_generation = net_read_u32(&s);
    }

    /* Extract sender IP directly from the UDP address using inet_ntop.
     * This avoids the old colon-stripping hack which would break IPv6. */
    char sender_ip[INET_ADDRSTRLEN];
    struct in_addr sender_in;
    sender_in.s_addr = from->addr;
    inet_ntop(AF_INET, &sender_in, sender_ip, sizeof(sender_ip));

    uint32_t now = net_tcp_get_timestamp_ms();

    /* Ignore our own announcements */
    if (data.announcing && data.announcement.session_id == sess_id) {
        return;
    }

    /* Check if we already know this host */
    for (int i = 0; i < NET_MAX_DISCOVERED_HOSTS; i++) {
        if (data.hosts[i].active && data.hosts[i].session_id == sess_id) {
            data.hosts[i].player_count = player_count;
            data.hosts[i].max_players = max_players;
            data.hosts[i].protocol_version = protocol_version;
            data.hosts[i].session_phase = session_phase;
            data.hosts[i].join_policy = join_policy;
            data.hosts[i].reserved_slots_free = reserved_slots_free;
            memcpy(data.hosts[i].world_instance_uuid, world_instance_uuid,
                   NET_DISCOVERY_WORLD_UUID_SIZE);
            data.hosts[i].resume_generation = resume_generation;
            data.hosts[i].last_seen_ms = now;
            MP_LOG_TRACE("DISCOVERY", "Updated known host '%s' at %s (players=%d/%d phase=%s policy=%s)",
                         host_name, sender_ip, player_count, max_players,
                         net_discovery_session_phase_name(
                             (net_discovery_session_phase)session_phase),
                         net_discovery_join_policy_name(
                             (net_discovery_join_policy)join_policy));
            return;
        }
    }

    /* Find free slot */
    int slot = -1;
    for (int i = 0; i < NET_MAX_DISCOVERED_HOSTS; i++) {
        if (!data.hosts[i].active) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        /* Replace oldest */
        uint32_t oldest_time = now;
        for (int i = 0; i < NET_MAX_DISCOVERED_HOSTS; i++) {
            if (data.hosts[i].last_seen_ms < oldest_time) {
                oldest_time = data.hosts[i].last_seen_ms;
                slot = i;
            }
        }
    }

    if (slot >= 0) {
        net_discovered_host *h = &data.hosts[slot];
        h->active = 1;
        snprintf(h->host_name, sizeof(h->host_name), "%s", host_name);
        /* Store clean IP (no port suffix) from inet_ntop */
        snprintf(h->host_ip, sizeof(h->host_ip), "%s", sender_ip);
        h->port = game_port;
        h->player_count = player_count;
        h->max_players = max_players;
        h->session_id = sess_id;
        h->protocol_version = protocol_version;
        h->session_phase = session_phase;
        h->join_policy = join_policy;
        h->reserved_slots_free = reserved_slots_free;
        memcpy(h->world_instance_uuid, world_instance_uuid, NET_DISCOVERY_WORLD_UUID_SIZE);
        h->resume_generation = resume_generation;
        h->last_seen_ms = now;
        if (slot >= data.host_count) {
            data.host_count = slot + 1;
        }

        MP_LOG_INFO("DISCOVERY", "New host discovered: '%s' at %s:%d (session=0x%08x, players=%d/%d phase=%s policy=%s)",
                    host_name, sender_ip, (int)game_port, sess_id, player_count, max_players,
                    net_discovery_session_phase_name(
                        (net_discovery_session_phase)session_phase),
                    net_discovery_join_policy_name(
                        (net_discovery_join_policy)join_policy));
    }
}

static void expire_old_hosts(uint32_t now)
{
    for (int i = 0; i < NET_MAX_DISCOVERED_HOSTS; i++) {
        if (data.hosts[i].active && (now - data.hosts[i].last_seen_ms) > HOST_EXPIRY_MS) {
            MP_LOG_INFO("DISCOVERY", "Host expired: '%s' at %s (last seen %ums ago)",
                        data.hosts[i].host_name, data.hosts[i].host_ip,
                        now - data.hosts[i].last_seen_ms);
            data.hosts[i].active = 0;
        }
    }
    /* Recalculate host_count */
    data.host_count = 0;
    for (int i = 0; i < NET_MAX_DISCOVERED_HOSTS; i++) {
        if (data.hosts[i].active && i >= data.host_count) {
            data.host_count = i + 1;
        }
    }
}

void net_discovery_update(void)
{
    if (data.udp_fd < 0) {
        return;
    }

    uint32_t now = net_tcp_get_timestamp_ms();

    /* Announce if hosting */
    if (data.announcing) {
        if (now - data.last_broadcast_ms > NET_DISCOVERY_BROADCAST_MS) {
            send_announcement();
            data.last_broadcast_ms = now;
        }
    }

    /* Receive if listening */
    if (data.listening) {
        uint8_t recv_buf[ANNOUNCE_PACKET_SIZE + 16];
        net_udp_addr from;

        int received = net_udp_recv(data.udp_fd, &from, recv_buf, sizeof(recv_buf));
        while (received > 0) {
            process_announcement(recv_buf, (size_t)received, &from);
            received = net_udp_recv(data.udp_fd, &from, recv_buf, sizeof(recv_buf));
        }

        expire_old_hosts(now);
    }
}

int net_discovery_get_host_count(void)
{
    int count = 0;
    for (int i = 0; i < NET_MAX_DISCOVERED_HOSTS; i++) {
        if (data.hosts[i].active) {
            count++;
        }
    }
    return count;
}

const net_discovered_host *net_discovery_get_host(int index)
{
    if (index < 0 || index >= NET_MAX_DISCOVERED_HOSTS) {
        return NULL;
    }
    return &data.hosts[index];
}

void net_discovery_clear_hosts(void)
{
    memset(data.hosts, 0, sizeof(data.hosts));
    data.host_count = 0;
}

const char *net_discovery_session_phase_name(net_discovery_session_phase phase)
{
    switch (phase) {
        case NET_DISCOVERY_PHASE_LOBBY:
            return "LOBBY";
        case NET_DISCOVERY_PHASE_IN_GAME:
            return "IN_GAME";
        case NET_DISCOVERY_PHASE_RESUME_LOBBY:
            return "RESUME_LOBBY";
        default:
            return "UNKNOWN";
    }
}

const char *net_discovery_join_policy_name(net_discovery_join_policy policy)
{
    switch (policy) {
        case NET_DISCOVERY_JOIN_OPEN_LOBBY:
            return "OPEN_LOBBY";
        case NET_DISCOVERY_JOIN_RECONNECT_ONLY:
            return "RECONNECT_ONLY";
        case NET_DISCOVERY_JOIN_LATE_JOIN_ALLOWED:
            return "LATE_JOIN_ALLOWED";
        case NET_DISCOVERY_JOIN_CLOSED:
            return "CLOSED";
        default:
            return "UNKNOWN";
    }
}

#endif /* ENABLE_MULTIPLAYER */
