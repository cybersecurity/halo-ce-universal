/* Host policy for the mouse's optional magnetism. The user's preference
   stays intact; a controller or touch aiming takes its usual path. */
#include "cseries.h"
#include "game/game.h"
#include "main/main.h"
#include "networking/network_game_globals.h"
#include "network_distributed.h"
#include "network_aim_assist.h"
#include <string.h>

int config_boolean(char const *name);
static boolean client_blocks_mouse = TRUE;
static short sent_policy = NONE;
struct aim_assist_policy_message
{
    struct distributed_message_header header;
    word block_mouse;
    word reserved;
};

void network_aim_assist_reset(void)
{
    /* Until the host's reliable policy arrives, a client cannot opt in. */
    client_blocks_mouse = TRUE;
    sent_policy = NONE;
}

boolean network_aim_assist_block_mouse(void)
{
    if (!game_in_progress() || main_menu_is_active()) return FALSE;
    if (game_connection() == _game_connection_network_server)
        return config_boolean("network.block_mouse_aim_assist") != 0;
    return game_connection() == _game_connection_network_client && client_blocks_mouse;
}

void network_aim_assist_send_policy(long machine_index)
{
    struct aim_assist_policy_message message;
    if (game_connection() != _game_connection_network_server) return;
    memset(&message, 0, sizeof(message));
    message.block_mouse = config_boolean("network.block_mouse_aim_assist") != 0;
    distributed_send_to_machine_reliably(machine_index, &message,
        _distributed_message_aim_assist_policy, 1, sizeof(message));
}

void network_aim_assist_update(void)
{
    struct aim_assist_policy_message message;
    short policy;
    if (!game_in_progress() || main_menu_is_active() ||
        game_connection() != _game_connection_network_server) return;
    policy = config_boolean("network.block_mouse_aim_assist") != 0;
    if (sent_policy == policy) return;
    sent_policy = policy;
    memset(&message, 0, sizeof(message));
    message.block_mouse = policy;
    distributed_send(&message, _distributed_message_aim_assist_policy, 1,
        sizeof(message), _distributed_to_clients_reliably);
}

boolean network_aim_assist_handles_message(word const *message, word size)
{
    struct distributed_message_header header;
    if (size < sizeof(header)) return FALSE;
    memcpy(&header, message, sizeof(header));
    return header.type == _distributed_message_aim_assist_policy;
}

void network_aim_assist_handle_message(long machine_index, word const *data, word size)
{
    struct aim_assist_policy_message message;
    if (machine_index != NONE || game_connection() != _game_connection_network_client ||
        !game_in_progress() || main_menu_is_active() || network_connection_last_read_was_unreliable() || size != sizeof(message)) return;
    memcpy(&message, data, sizeof(message));
    if (GET_MESSAGE_TYPE(message.header.header) != 2 ||
        message.header.type != _distributed_message_aim_assist_policy ||
        message.header.count != 1 || message.block_mouse > 1 || message.reserved != 0) return;
    client_blocks_mouse = message.block_mouse != 0;
}
