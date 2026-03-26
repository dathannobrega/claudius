#ifdef ENABLE_MULTIPLAYER

#include "multiplayer_status.h"

#include "core/string.h"
#include "graphics/button.h"
#include "graphics/generic_button.h"
#include "graphics/graphics.h"
#include "graphics/lang_text.h"
#include "graphics/list_box.h"
#include "graphics/panel.h"
#include "graphics/text.h"
#include "graphics/window.h"
#include "input/input.h"
#include "multiplayer/checksum.h"
#include "multiplayer/player_registry.h"
#include "multiplayer/resync.h"
#include "network/peer.h"
#include "network/session.h"
#include "translation/translation.h"
#include "window/multiplayer_window_flow.h"

#include <stdio.h>
#include <string.h>

#define PANEL_X 80
#define PANEL_Y 48
#define PANEL_WIDTH_BLOCKS 28
#define PANEL_HEIGHT_BLOCKS 22
#define PANEL_WIDTH (PANEL_WIDTH_BLOCKS * 16)
#define PANEL_HEIGHT (PANEL_HEIGHT_BLOCKS * 16)

#define INFO_X (PANEL_X + 24)
#define INFO_Y (PANEL_Y + 48)

#define LIST_X (PANEL_X + 16)
#define LIST_Y (PANEL_Y + 144)
#define LIST_WIDTH_BLOCKS 24
#define LIST_HEIGHT_BLOCKS 8
#define LIST_ITEM_HEIGHT 20
#define LIST_WIDTH (LIST_WIDTH_BLOCKS * 16)

#define BUTTON_ROW_Y (PANEL_Y + PANEL_HEIGHT - 48)
#define CLIENT_BUTTON_WIDTH 128
#define HOST_BUTTON_WIDTH 144
#define BUTTON_HEIGHT 24
#define BUTTON_GAP 8

#define NAME_COLUMN_X (LIST_X + 4)
#define HOST_COLUMN_X (LIST_X + 150)
#define PING_COLUMN_X (LIST_X + 214)
#define QUALITY_COLUMN_X (LIST_X + 286)

static void button_resync(const generic_button *button);
static void button_disconnect(const generic_button *button);
static void button_close(const generic_button *button);
static void draw_player_item(const list_box_item *item);
static void select_player(unsigned int index, int is_double_click);
static void on_return(window_id from);

static generic_button client_buttons[3];
static generic_button host_buttons[2];

static const uint8_t local_text[] = "Local";
static const uint8_t awaiting_text[] = "Awaiting";
static const uint8_t offline_text[] = "Offline";
static const uint8_t desynced_text[] = "Desynced";

static list_box_type player_list = {
    .x = LIST_X,
    .y = LIST_Y,
    .width_blocks = LIST_WIDTH_BLOCKS,
    .height_blocks = LIST_HEIGHT_BLOCKS,
    .item_height = LIST_ITEM_HEIGHT,
    .draw_inner_panel = 1,
    .extend_to_hidden_scrollbar = 0,
    .decorate_scrollbar = 0,
    .draw_item = draw_player_item,
    .on_select = select_player,
    .handle_tooltip = 0
};

static struct {
    unsigned int focus_button_id;
    int player_ids[MP_MAX_PLAYERS];
    int player_count;
} data;

static void configure_buttons(void)
{
    int client_row_width = CLIENT_BUTTON_WIDTH * 3 + BUTTON_GAP * 2;
    int client_start_x = PANEL_X + (PANEL_WIDTH - client_row_width) / 2;
    client_buttons[0] = (generic_button){client_start_x, BUTTON_ROW_Y,
        CLIENT_BUTTON_WIDTH, BUTTON_HEIGHT, button_resync, 0, 0};
    client_buttons[1] = (generic_button){client_start_x + CLIENT_BUTTON_WIDTH + BUTTON_GAP,
        BUTTON_ROW_Y, CLIENT_BUTTON_WIDTH, BUTTON_HEIGHT, button_disconnect, 0, 0};
    client_buttons[2] = (generic_button){client_start_x + (CLIENT_BUTTON_WIDTH + BUTTON_GAP) * 2,
        BUTTON_ROW_Y, CLIENT_BUTTON_WIDTH, BUTTON_HEIGHT, button_close, 0, 0};

    {
        int host_row_width = HOST_BUTTON_WIDTH * 2 + BUTTON_GAP;
        int host_start_x = PANEL_X + (PANEL_WIDTH - host_row_width) / 2;
        host_buttons[0] = (generic_button){host_start_x, BUTTON_ROW_Y,
            HOST_BUTTON_WIDTH, BUTTON_HEIGHT, button_disconnect, 0, 0};
        host_buttons[1] = (generic_button){host_start_x + HOST_BUTTON_WIDTH + BUTTON_GAP,
            BUTTON_ROW_Y, HOST_BUTTON_WIDTH, BUTTON_HEIGHT, button_close, 0, 0};
    }
}

static void refresh_player_list(void)
{
    data.player_count = 0;
    for (int id = mp_player_registry_get_first_active_id();
         id >= 0 && data.player_count < MP_MAX_PLAYERS;
         id = mp_player_registry_get_next_active_id(id)) {
        data.player_ids[data.player_count++] = id;
    }
    list_box_update_total_items(&player_list, data.player_count);
}

static translation_key quality_translation(net_peer_quality quality)
{
    switch (quality) {
        case PEER_QUALITY_GOOD:
            return TR_MP_STATUS_QUALITY_GOOD;
        case PEER_QUALITY_DEGRADED:
            return TR_MP_STATUS_QUALITY_DEGRADED;
        case PEER_QUALITY_POOR:
            return TR_MP_STATUS_QUALITY_POOR;
        case PEER_QUALITY_CRITICAL:
            return TR_MP_STATUS_QUALITY_CRITICAL;
        default:
            return TR_MP_STATUS_QUALITY_UNKNOWN;
    }
}

static font_t quality_font(net_peer_quality quality)
{
    switch (quality) {
        case PEER_QUALITY_GOOD:
            return FONT_NORMAL_GREEN;
        case PEER_QUALITY_DEGRADED:
            return FONT_NORMAL_BROWN;
        case PEER_QUALITY_POOR:
        case PEER_QUALITY_CRITICAL:
            return FONT_NORMAL_RED;
        default:
            return FONT_NORMAL_PLAIN;
    }
}

static const net_peer *find_player_peer(uint8_t player_id)
{
    for (int i = 0; i < NET_MAX_PEERS; i++) {
        const net_peer *peer = net_session_get_peer(i);
        if (peer && peer->active && peer->player_id == player_id) {
            return peer;
        }
    }
    return 0;
}

static const uint8_t *player_override_text(const mp_player *player, font_t *font)
{
    if (player->status == MP_PLAYER_AWAITING_RECONNECT) {
        *font = FONT_NORMAL_BROWN;
        return awaiting_text;
    }
    if (player->status == MP_PLAYER_DESYNCED) {
        *font = FONT_NORMAL_RED;
        return desynced_text;
    }
    if (player->connection_state == MP_CONNECTION_DISCONNECTED ||
        player->status == MP_PLAYER_DISCONNECTED) {
        *font = FONT_NORMAL_RED;
        return offline_text;
    }
    if (player->is_local) {
        *font = FONT_NORMAL_PLAIN;
        return local_text;
    }
    return 0;
}

static void draw_player_item(const list_box_item *item)
{
    if ((int)item->index >= data.player_count) {
        return;
    }

    mp_player *player = mp_player_registry_get((uint8_t)data.player_ids[item->index]);
    if (!player || !player->active) {
        return;
    }

    {
        font_t name_font = item->is_focused ? FONT_NORMAL_WHITE : FONT_NORMAL_GREEN;
        uint8_t name_buf[64];
        string_copy(string_from_ascii(player->name), name_buf, sizeof(name_buf));
        text_ellipsize(name_buf, name_font, HOST_COLUMN_X - NAME_COLUMN_X - 12);
        text_draw(name_buf, NAME_COLUMN_X, item->y + 2, name_font, 0);
    }

    if (player->is_host) {
        const uint8_t *host_label = translation_for(TR_MP_HOST);
        text_draw(host_label, HOST_COLUMN_X, item->y + 2, FONT_NORMAL_BROWN, 0);
    }

    {
        font_t override_font = FONT_NORMAL_PLAIN;
        const uint8_t *override_text = player_override_text(player, &override_font);
        if (override_text) {
            text_draw(override_text, PING_COLUMN_X, item->y + 2, override_font, 0);
        } else {
            const net_peer *peer = find_player_peer(player->player_id);
            if (peer) {
                char ping_str[16];
                uint8_t ping_buf[16];
                snprintf(ping_str, sizeof(ping_str), "%u ms", peer->rtt_smoothed_ms);
                string_copy(string_from_ascii(ping_str), ping_buf, sizeof(ping_buf));
                text_draw(ping_buf, PING_COLUMN_X, item->y + 2, FONT_NORMAL_PLAIN, 0);

                {
                    translation_key qkey = quality_translation(peer->quality);
                    const uint8_t *qtext = translation_for(qkey);
                    text_draw(qtext, QUALITY_COLUMN_X, item->y + 2,
                        quality_font(peer->quality), 0);
                }
            }
        }
    }

    if (item->is_focused) {
        button_border_draw(item->x, item->y, item->width, item->height, 1);
    }
}

static void select_player(unsigned int index, int is_double_click)
{
    window_invalidate();
    (void)index;
    (void)is_double_click;
}

static void button_resync(const generic_button *button)
{
    if (net_session_is_client()) {
        mp_resync_request_full_snapshot();
    }
    (void)button;
}

static void button_disconnect(const generic_button *button)
{
    window_multiplayer_exit_to_menu();
    (void)button;
}

static void button_close(const generic_button *button)
{
    window_go_back();
    (void)button;
}

static void draw_background(void)
{
    window_draw_underlying_window();
    graphics_in_dialog();
    outer_panel_draw(PANEL_X, PANEL_Y, PANEL_WIDTH_BLOCKS, PANEL_HEIGHT_BLOCKS);

    lang_text_draw_centered(CUSTOM_TRANSLATION, TR_MP_STATUS_TITLE,
        PANEL_X, PANEL_Y + 14, PANEL_WIDTH, FONT_LARGE_BLACK);

    graphics_reset_dialog();
}

static void draw_foreground(void)
{
    graphics_in_dialog();

    {
        int y = INFO_Y;
        int label_x = INFO_X;
        int value_x = INFO_X + 140;

        lang_text_draw(CUSTOM_TRANSLATION, TR_MP_STATUS_CONNECTION, label_x, y, FONT_NORMAL_BLACK);
        if (net_session_is_host()) {
            const uint8_t *host_text = translation_for(TR_MP_HOST);
            text_draw(host_text, value_x, y, FONT_NORMAL_GREEN, 0);
        } else {
            const net_peer *host_peer = net_session_get_host_peer();
            if (host_peer && host_peer->active) {
                translation_key qkey = quality_translation(host_peer->quality);
                const uint8_t *qtext = translation_for(qkey);
                text_draw(qtext, value_x, y, quality_font(host_peer->quality), 0);
            }
        }
        y += 20;

        lang_text_draw(CUSTOM_TRANSLATION, TR_MP_STATUS_SYNC, label_x, y, FONT_NORMAL_BLACK);
        {
            int has_desync = mp_checksum_has_desync();
            translation_key sync_key = has_desync ? TR_MP_STATUS_DESYNCED : TR_MP_STATUS_SYNCED;
            const uint8_t *sync_text = translation_for(sync_key);
            text_draw(sync_text, value_x, y, has_desync ? FONT_NORMAL_RED : FONT_NORMAL_GREEN, 0);
        }
        y += 20;

        lang_text_draw(CUSTOM_TRANSLATION, TR_MP_STATUS_TICK, label_x, y, FONT_NORMAL_BLACK);
        {
            char tick_str[32];
            uint8_t tick_buf[32];
            snprintf(tick_str, sizeof(tick_str), "%u", net_session_get_authoritative_tick());
            string_copy(string_from_ascii(tick_str), tick_buf, sizeof(tick_buf));
            text_draw(tick_buf, value_x, y, FONT_NORMAL_PLAIN, 0);
        }
    }

    lang_text_draw(CUSTOM_TRANSLATION, TR_MP_STATUS_PLAYERS_ONLINE,
        LIST_X, LIST_Y - 18, FONT_NORMAL_BLACK);

    refresh_player_list();
    list_box_draw(&player_list);

    {
        generic_button *buttons = net_session_is_client() ? client_buttons : host_buttons;
        int button_count = net_session_is_client() ? 3 : 2;
        for (int i = 0; i < button_count; i++) {
            large_label_draw(buttons[i].x, buttons[i].y, buttons[i].width / 16,
                data.focus_button_id == (unsigned int)(i + 1));
        }
    }

    if (net_session_is_client()) {
        lang_text_draw_centered(CUSTOM_TRANSLATION, TR_MP_STATUS_BUTTON_RESYNC,
            client_buttons[0].x, client_buttons[0].y + 5, client_buttons[0].width,
            FONT_NORMAL_GREEN);
        lang_text_draw_centered(CUSTOM_TRANSLATION, TR_MP_STATUS_BUTTON_DISCONNECT,
            client_buttons[1].x, client_buttons[1].y + 5, client_buttons[1].width,
            FONT_NORMAL_GREEN);
        lang_text_draw_centered(CUSTOM_TRANSLATION, TR_MP_STATUS_BUTTON_CLOSE,
            client_buttons[2].x, client_buttons[2].y + 5, client_buttons[2].width,
            FONT_NORMAL_GREEN);
    } else {
        lang_text_draw_centered(CUSTOM_TRANSLATION, TR_MP_STATUS_BUTTON_DISCONNECT,
            host_buttons[0].x, host_buttons[0].y + 5, host_buttons[0].width,
            FONT_NORMAL_GREEN);
        lang_text_draw_centered(CUSTOM_TRANSLATION, TR_MP_STATUS_BUTTON_CLOSE,
            host_buttons[1].x, host_buttons[1].y + 5, host_buttons[1].width,
            FONT_NORMAL_GREEN);
    }

    graphics_reset_dialog();
}

static void handle_input(const mouse *m, const hotkeys *h)
{
    const mouse *m_dialog = mouse_in_dialog(m);

    if (!net_session_is_active()) {
        window_multiplayer_exit_to_menu();
        return;
    }

    {
        generic_button *buttons = net_session_is_client() ? client_buttons : host_buttons;
        int button_count = net_session_is_client() ? 3 : 2;
        if (generic_buttons_handle_mouse(m_dialog, 0, 0, buttons, button_count,
                                         &data.focus_button_id)) {
            return;
        }
    }

    if (list_box_handle_input(&player_list, m_dialog, 1)) {
        return;
    }

    if (input_go_back_requested(m, h)) {
        window_go_back();
        return;
    }

    list_box_request_refresh(&player_list);
}

static void on_return(window_id from)
{
    window_invalidate();
    (void)from;
}

void window_multiplayer_status_show(void)
{
    memset(&data, 0, sizeof(data));
    configure_buttons();
    refresh_player_list();
    list_box_init(&player_list, data.player_count);

    {
        window_type window = {
            WINDOW_MULTIPLAYER_STATUS,
            draw_background,
            draw_foreground,
            handle_input,
            0,
            on_return
        };
        window_show(&window);
    }
}

#endif /* ENABLE_MULTIPLAYER */
