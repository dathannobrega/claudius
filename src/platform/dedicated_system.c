#include "game/system.h"

#include "multiplayer/dedicated_server.h"
#include "platform/platform.h"

#include "SDL.h"

#include <stddef.h>
#include <stdint.h>

void system_resize(int width, int height)
{
    (void)width;
    (void)height;
}

void system_center(void)
{
}

void system_set_fullscreen(int fullscreen)
{
    (void)fullscreen;
}

uint64_t system_get_ticks(void)
{
#if SDL_VERSION_ATLEAST(2, 0, 18)
    return SDL_GetTicks64();
#else
    return (uint64_t)SDL_GetTicks();
#endif
}

int system_supports_select_folder_dialog(void)
{
    return 0;
}

const char *system_show_select_folder_dialog(const char *title, const char *default_path)
{
    (void)title;
    (void)default_path;
    return NULL;
}

void system_exit(void)
{
    if (mp_dedicated_server_is_enabled()) {
        mp_dedicated_server_request_shutdown();
        return;
    }
    exit_with_status(0);
}
