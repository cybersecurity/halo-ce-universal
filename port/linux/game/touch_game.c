/*
TOUCH_GAME.C

What the touch controls (port/linux/src/touch_input.c) need to know of the
game. The platform layer cannot see the game's types, so it asks here
(this file is compiled as the game's own sources are).
*/

#include "cseries.h"
#include "game/game.h"
#include "cutscene/cinematics.h"

/**
 * @brief Asks whether a cinematic is playing that A would skip
 * (player_control.c, player_control_action_test_check_reset_input_blob).
 * The controller is read before the game exists (input_initialize, during
 * shell_initialize), so cinematic_globals is tested first: game_initialize
 * makes it after the game time globals that game_in_progress reads
 * (game.c).
 * @return nonzero if A would skip the cinematic now
 */
int touch_game_cinematic_skippable(void)
{
	return cinematic_globals && game_in_progress() && cinematic_in_progress() &&
		cinematic_can_be_skipped();
}
