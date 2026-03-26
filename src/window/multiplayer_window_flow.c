#ifdef ENABLE_MULTIPLAYER

#include "multiplayer_window_flow.h"

#include "graphics/window.h"
#include "multiplayer/bootstrap.h"
#include "multiplayer/player_registry.h"
#include "multiplayer/save_transfer.h"
#include "network/discovery_lan.h"
#include "network/session.h"
#include "window/multiplayer_menu.h"

void window_multiplayer_reset_session_state(void)
{
    net_session_disconnect();
    net_session_clear_join_status();
    mp_bootstrap_reset();
    mp_save_transfer_reset();
    mp_player_registry_clear();
    net_discovery_stop_announcing();
    net_discovery_stop_listening();
    net_discovery_clear_hosts();
}

void window_multiplayer_exit_to_menu(void)
{
    window_multiplayer_reset_session_state();
    if (!window_go_back_to(WINDOW_MULTIPLAYER_MENU)) {
        window_multiplayer_menu_show();
    }
}

#endif /* ENABLE_MULTIPLAYER */
