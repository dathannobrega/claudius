#ifdef ENABLE_MULTIPLAYER

#include "multiplayer_lobby.h"

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
#include "multiplayer/bootstrap.h"
#include "multiplayer/player_registry.h"
#include "network/peer.h"
#include "network/session.h"
#include "translation/translation.h"
#include "window/multiplayer_window_flow.h"

#include <stdio.h>
#include <string.h>

#define PANEL_X 64
#define PANEL_Y 32
#define PANEL_WIDTH_BLOCKS 30
#define PANEL_HEIGHT_BLOCKS 24
#define PANEL_WIDTH (PANEL_WIDTH_BLOCKS * 16)
#define PANEL_HEIGHT (PANEL_HEIGHT_BLOCKS * 16)

#define LIST_X (PANEL_X + 16)
#define LIST_Y (PANEL_Y + 64)
#define LIST_WIDTH_BLOCKS 26
#define LIST_HEIGHT_BLOCKS 12
#define LIST_ITEM_HEIGHT 24
#define LIST_WIDTH (LIST_WIDTH_BLOCKS * 16)

#define BUTTON_ROW_Y (PANEL_Y + PANEL_HEIGHT - 48)
#define HOST_BUTTON_WIDTH 128
#define CLIENT_BUTTON_WIDTH 144
#define BUTTON_HEIGHT 24
#define BUTTON_GAP 8

#define NAME_COLUMN_X (LIST_X + 8)
#define HOST_COLUMN_X (LIST_X + 168)
#define STATUS_COLUMN_X (LIST_X + 236)
#define PING_COLUMN_X (LIST_X + LIST_WIDTH - 60)

static void button_ready(const generic_button *button);
static void button_start(const generic_button *button);
static void button_leave(const generic_button *button);
static void draw_player_item(const list_box_item *item);
static void select_player(unsigned int index, int is_double_click);
static void on_return(window_id from);

static generic_button host_buttons[3];
static generic_button client_buttons[2];

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
    int host_row_width = HOST_BUTTON_WIDTH * 3 + BUTTON_GAP * 2;
    int host_start_x = PANEL_X + (PANEL_WIDTH - host_row_width) / 2;
    host_buttons[0] = (generic_button){host_start_x, BUTTON_ROW_Y,
        HOST_BUTTON_WIDTH, BUTTON_HEIGHT, button_ready, 0, 0};
    host_buttons[1] = (generic_button){host_start_x + HOST_BUTTON_WIDTH + BUTTON_GAP, BUTTON_ROW_Y,
        HOST_BUTTON_WIDTH, BUTTON_HEIGHT, button_start, 0, 0};
    host_buttons[2] = (generic_button){host_start_x + (HOST_BUTTON_WIDTH + BUTTON_GAP) * 2, BUTTON_ROW_Y,
        HOST_BUTTON_WIDTH, BUTTON_HEIGHT, button_leave, 0, 0};

    {
        int client_row_width = CLIENT_BUTTON_WIDTH * 2 + BUTTON_GAP;
        int client_start_x = PANEL_X + (PANEL_WIDTH - client_row_width) / 2;
        client_buttons[0] = (generic_button){client_start_x, BUTTON_ROW_Y,
            CLIENT_BUTTON_WIDTH, BUTTON_HEIGHT, button_ready, 0, 0};
        client_buttons[1] = (generic_button){client_start_x + CLIENT_BUTTON_WIDTH + BUTTON_GAP, BUTTON_ROW_Y,
            CLIENT_BUTTON_WIDTH, BUTTON_HEIGHT, button_leave, 0, 0};
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

static int local_player_is_ready(void)
{
    mp_player *local = mp_player_registry_get_local();
    return local && local->status == MP_PLAYER_READY;
}

static const uint8_t *status_text(const mp_player *player)
{
    if (!player) {
        return translation_for(TR_MP_LOBBY_NOT_READY);
    }

    switch (player->status) {
        case MP_PLAYER_READY:
            return translation_for(TR_MP_LOBBY_READY);
        case MP_PLAYER_IN_GAME:
            return translation_for(TR_MP_LOBBY_IN_GAME);
        case MP_PLAYER_DISCONNECTED:
            return translation_for(TR_MP_LOBBY_DISCONNECTED);
        case MP_PLAYER_AWAITING_RECONNECT:
            return translation_for(TR_MP_LOBBY_AWAITING_RECONNECT);
        case MP_PLAYER_DESYNCED:
            return translation_for(TR_MP_STATUS_DESYNCED);
        case MP_PLAYER_LOBBY:
        default:
            if (player->connection_state == MP_CONNECTION_DISCONNECTED) {
                return translation_for(TR_MP_LOBBY_DISCONNECTED);
            }
            return translation_for(TR_MP_LOBBY_NOT_READY);
    }
}

static font_t status_font(const mp_player *player)
{
    if (!player) {
        return FONT_NORMAL_PLAIN;
    }

    switch (player->status) {
        case MP_PLAYER_READY:
        case MP_PLAYER_IN_GAME:
            return FONT_NORMAL_GREEN;
        case MP_PLAYER_AWAITING_RECONNECT:
            return FONT_NORMAL_BROWN;
        case MP_PLAYER_DESYNCED:
        case MP_PLAYER_DISCONNECTED:
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
        string_copy(string_from_ascii(mp_player_registry_display_name(player)), name_buf, sizeof(name_buf));
        text_ellipsize(name_buf, name_font, HOST_COLUMN_X - NAME_COLUMN_X - 12);
        text_draw(name_buf, NAME_COLUMN_X, item->y + 4, name_font, 0);
    }

    if (player->is_host) {
        const uint8_t *host_text = translation_for(TR_MP_HOST);
        text_draw(host_text, HOST_COLUMN_X, item->y + 4, FONT_NORMAL_BROWN, 0);
    }

    {
        const uint8_t *player_status = status_text(player);
        text_draw_ellipsized(player_status, STATUS_COLUMN_X, item->y + 4,
            PING_COLUMN_X - STATUS_COLUMN_X - 8, status_font(player), 0);
    }

    if (!player->is_local) {
        const net_peer *peer = find_player_peer(player->player_id);
        if (peer) {
            char ping_str[16];
            uint8_t ping_buf[16];
            snprintf(ping_str, sizeof(ping_str), "%u ms", peer->rtt_smoothed_ms);
            string_copy(string_from_ascii(ping_str), ping_buf, sizeof(ping_buf));
            text_draw(ping_buf, PING_COLUMN_X, item->y + 4, FONT_NORMAL_PLAIN, 0);
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

static void button_ready(const generic_button *button)
{
    int next_ready = !local_player_is_ready();
    net_session_set_ready(next_ready);

    {
        mp_player *local = mp_player_registry_get_local();
        if (local) {
            mp_player_registry_set_status(local->player_id,
                next_ready ? MP_PLAYER_READY : MP_PLAYER_LOBBY);
        }
    }

    window_invalidate();
    (void)button;
}

static void button_start(const generic_button *button)
{
    if (!net_session_is_host() || !net_session_all_peers_ready()) {
        return;
    }
    if (!mp_bootstrap_host_start_game()) {
        window_invalidate();
    }
    (void)button;
}

static void button_leave(const generic_button *button)
{
    window_multiplayer_exit_to_menu();
    (void)button;
}

static void draw_background(void)
{
    graphics_in_dialog();
    outer_panel_draw(PANEL_X, PANEL_Y, PANEL_WIDTH_BLOCKS, PANEL_HEIGHT_BLOCKS);

    lang_text_draw_centered(CUSTOM_TRANSLATION, TR_MP_LOBBY_TITLE,
        PANEL_X, PANEL_Y + 14, PANEL_WIDTH, FONT_LARGE_BLACK);

    {
        int header_y = LIST_Y - 18;
        lang_text_draw(CUSTOM_TRANSLATION, TR_MP_LOBBY_PLAYERS,
            NAME_COLUMN_X, header_y, FONT_NORMAL_BLACK);
        lang_text_draw(CUSTOM_TRANSLATION, TR_MP_LOBBY_STATUS,
            STATUS_COLUMN_X, header_y, FONT_NORMAL_BLACK);
        lang_text_draw(CUSTOM_TRANSLATION, TR_MP_LOBBY_PING,
            PING_COLUMN_X, header_y, FONT_NORMAL_BLACK);
    }

    graphics_reset_dialog();
}

static void draw_foreground(void)
{
    graphics_in_dialog();

    refresh_player_list();
    list_box_draw(&player_list);

    {
        int button_count = 0;
        generic_button *buttons = net_session_is_host() ? host_buttons : client_buttons;
        button_count = net_session_is_host() ? 3 : 2;

        for (int i = 0; i < button_count; i++) {
            large_label_draw(buttons[i].x, buttons[i].y, buttons[i].width / 16,
                data.focus_button_id == (unsigned int)(i + 1));
        }
    }

    {
        translation_key ready_key = local_player_is_ready()
            ? TR_MP_LOBBY_BUTTON_NOT_READY
            : TR_MP_LOBBY_BUTTON_READY;
        const generic_button *ready_button = net_session_is_host()
            ? &host_buttons[0]
            : &client_buttons[0];
        lang_text_draw_centered(CUSTOM_TRANSLATION, ready_key,
            ready_button->x, ready_button->y + 5, ready_button->width, FONT_NORMAL_GREEN);
    }

    if (net_session_is_host()) {
        int all_ready = net_session_all_peers_ready();
        lang_text_draw_centered(CUSTOM_TRANSLATION, TR_MP_LOBBY_BUTTON_START,
            host_buttons[1].x, host_buttons[1].y + 5, host_buttons[1].width,
            all_ready ? FONT_NORMAL_GREEN : FONT_NORMAL_PLAIN);
        lang_text_draw_centered(CUSTOM_TRANSLATION, TR_MP_LOBBY_BUTTON_LEAVE,
            host_buttons[2].x, host_buttons[2].y + 5, host_buttons[2].width, FONT_NORMAL_GREEN);
    } else {
        const uint8_t *waiting = translation_for(TR_MP_LOBBY_WAITING_HOST);
        text_draw_centered(waiting, PANEL_X, BUTTON_ROW_Y - 20, PANEL_WIDTH,
            FONT_NORMAL_PLAIN, 0);
        lang_text_draw_centered(CUSTOM_TRANSLATION, TR_MP_LOBBY_BUTTON_LEAVE,
            client_buttons[1].x, client_buttons[1].y + 5, client_buttons[1].width,
            FONT_NORMAL_GREEN);
    }

    graphics_reset_dialog();
}

static void handle_input(const mouse *m, const hotkeys *h)
{
    const mouse *m_dialog = mouse_in_dialog(m);
    net_session_state state = net_session_get_state();

    if (state == NET_SESSION_HOSTING_GAME || state == NET_SESSION_CLIENT_GAME) {
        return;
    }
    if (state == NET_SESSION_IDLE) {
        window_multiplayer_exit_to_menu();
        return;
    }

    {
        generic_button *buttons = net_session_is_host() ? host_buttons : client_buttons;
        int button_count = net_session_is_host() ? 3 : 2;
        if (generic_buttons_handle_mouse(m_dialog, 0, 0, buttons, button_count,
                                         &data.focus_button_id)) {
            return;
        }
    }

    if (list_box_handle_input(&player_list, m_dialog, 1)) {
        return;
    }

    if (input_go_back_requested(m, h)) {
        button_leave(0);
        return;
    }

    list_box_request_refresh(&player_list);
}

static void on_return(window_id from)
{
    data.focus_button_id = 0;
    window_invalidate();
    (void)from;
}

void window_multiplayer_lobby_show(void)
{
    memset(&data, 0, sizeof(data));
    configure_buttons();
    refresh_player_list();
    list_box_init(&player_list, data.player_count);

    {
        window_type window = {
            WINDOW_MULTIPLAYER_LOBBY,
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
