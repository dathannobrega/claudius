#ifndef MULTIPLAYER_BOOTSTRAP_H
#define MULTIPLAYER_BOOTSTRAP_H

#ifdef ENABLE_MULTIPLAYER

#include <stdint.h>

/**
 * Multiplayer bootstrap pipeline.
 *
 * Orchestrates the critical path from lobby to gameplay:
 *
 * HOST FRESH START:
 *   1. Host selects scenario in host_setup window
 *   2. mp_bootstrap_set_scenario() records the selection
 *   3. Host clicks Start in lobby → mp_bootstrap_host_start_game()
 *   4. Sends GAME_PREPARE (manifest) to all peers
 *   5. Host loads scenario locally
 *   6. mp_bootstrap_bind_loaded_scenario() initializes MP subsystems
 *   7. Worldgen runs, spawns locked, ownership applied
 *   8. Host broadcasts spawn table
 *   9. Host sends GAME_START_FINAL
 *  10. Transition to WINDOW_CITY
 *
 * HOST RESUME:
 *   1. Load save → rebind
 *   2. Send GAME_PREPARE (manifest)
 *   3. Chunked save transfer to clients
 *   4. Send FULL_SNAPSHOT (latest MP state)
 *   5. Send GAME_START_FINAL
 *
 * CLIENT FRESH START:
 *   1. Receives GAME_PREPARE → stores manifest
 *   2. mp_bootstrap_client_prepare() loads the same scenario locally
 *   3. mp_bootstrap_bind_loaded_scenario() initializes MP subsystems
 *   4. Receives spawn table via HOST_EVENT
 *   5. Receives GAME_START_FINAL → mp_bootstrap_client_enter_game()
 *   6. Transition to WINDOW_CITY
 *
 * CLIENT RESUME:
 *   1. Receive GAME_PREPARE
 *   2. Receive save file chunks → write temp → load
 *   3. Rebind MP subsystems
 *   4. Receive + apply FULL_SNAPSHOT
 *   5. Receive GAME_START_FINAL → enter game
 */

typedef enum {
    MP_BOOT_IDLE = 0,
    MP_BOOT_SCENARIO_SELECTED,  /* Host has chosen a scenario */
    MP_BOOT_SAVE_SELECTED,      /* Host has chosen a save file to resume */
    MP_BOOT_PREPARING,          /* GAME_PREPARE sent, loading locally */
    MP_BOOT_LOADED,             /* Scenario loaded, subsystems bound */
    MP_BOOT_RESUME_LOBBY,       /* Waiting for players to reconnect */
    MP_BOOT_SAVE_TRANSFER,      /* Host sending save / client receiving save */
    MP_BOOT_IN_GAME             /* Playing */
} mp_boot_state;

/* Initialize bootstrap state (call once on session create) */
void mp_bootstrap_init(void);

/* Reset bootstrap state (call on disconnect) */
void mp_bootstrap_reset(void);

/* Get current bootstrap state */
mp_boot_state mp_bootstrap_get_state(void);

/**
 * HOST: Record the scenario the host has selected.
 * Called from the host setup screen after file selection.
 * @param scenario_name  Scenario filename (without extension)
 */
void mp_bootstrap_set_scenario(const char *scenario_name);

/**
 * HOST: Get the selected scenario name.
 */
const char *mp_bootstrap_get_scenario_name(void);

/**
 * HOST: Initiate the game start sequence.
 * - Sets the manifest with current session info
 * - Sends GAME_PREPARE to all peers
 * - Loads the scenario locally
 * - Binds MP subsystems
 * - Generates and locks worldgen
 * - Broadcasts spawn table
 * - Sends GAME_START_FINAL
 * - Transitions to WINDOW_CITY
 * @return 1 on success, 0 on failure
 */
int mp_bootstrap_host_start_game(void);

/**
 * CLIENT: Handle GAME_PREPARE message from host.
 * - Deserializes manifest
 * - Loads the same scenario locally
 * - Binds MP subsystems
 * @param payload  Serialized manifest data
 * @param size     Size of payload
 * @return 1 on success, 0 on failure
 */
int mp_bootstrap_client_prepare(const uint8_t *payload, uint32_t size);

/**
 * CLIENT: Handle GAME_START_FINAL message from host.
 * - Transitions session state to in-game
 * - Transitions to WINDOW_CITY
 * @return 1 on success, 0 on failure
 */
int mp_bootstrap_client_enter_game(void);

/**
 * COMMON: Bind multiplayer subsystems to the currently loaded scenario.
 * Called AFTER the scenario is loaded via game_file_start_scenario_by_name().
 * Does NOT reinitialize the player registry (preserves lobby roster).
 *
 * Steps:
 *   1. Enable multiplayer mode
 *   2. Init ownership, empire_sync, time_sync, command_bus, checksum, resync
 *   3. Assign empire cities to players based on spawn table
 */
void mp_bootstrap_bind_loaded_scenario(void);

/**
 * HOST: Set save filename for resume flow.
 */
void mp_bootstrap_set_save(const char *save_filename);

/**
 * HOST: Resume game from a saved multiplayer session.
 * @return 1 on success, 0 on failure
 */
int mp_bootstrap_host_resume_game(void);

/**
 * HOST: Launch the resumed game after players reconnect in resume lobby.
 * This is now non-blocking: it starts the save transfer and returns.
 * Call mp_bootstrap_update() each frame to drive the transfer.
 * @return 1 on success, 0 on failure
 */
int mp_bootstrap_host_launch_resumed_game(void);

/**
 * Update the bootstrap pipeline. Call each frame.
 * Drives the save transfer on the host side.
 * When transfer completes, finalizes the resume and enters game.
 */
void mp_bootstrap_update(void);

/**
 * CLIENT: Called when save transfer completes.
 * Writes received data to temp file, loads it, rebinds subsystems.
 */
void mp_bootstrap_client_save_transfer_complete(void);

/**
 * COMMON: Rebind subsystems after loading a save (NOT a fresh scenario).
 * Only reinitializes stateless subsystems; ownership/routes come from save.
 */
void mp_bootstrap_rebind_loaded_save(void);

/**
 * Query whether the current bootstrap is a resume (not fresh start).
 */
int mp_bootstrap_is_resume(void);
int mp_bootstrap_is_late_join_busy(void);
void mp_bootstrap_host_cancel_late_join(uint8_t peer_index);
void mp_bootstrap_host_complete_late_join(uint8_t peer_index);
int mp_bootstrap_host_handle_late_join(uint8_t peer_index, const char *player_name,
                                       uint8_t *out_reject_reason);

#endif /* ENABLE_MULTIPLAYER */

#endif /* MULTIPLAYER_BOOTSTRAP_H */
