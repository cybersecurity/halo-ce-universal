/* A player can escape a stuck unit without giving clients arbitrary unit
   commands. The host accepts only the sender's current living unit. */
#include "cseries.h"
#include "game/game.h"
#include "game/players.h"
#include "main/main.h"
#include "main/console.h"
#include "networking/network_game_globals.h"
#include "network_distributed.h"
#include "network_suicide.h"
#include "units/units.h"
#include <ctype.h>
#include <string.h>

boolean network_distributed_client_send_reliably(void *message, word size);
struct suicide_request
{
    struct distributed_message_header header;
    long player_index;
    long unit_index;
};
static struct
{
    long player_index;
    unsigned long requested_at;
    boolean requested;
} suicide_rates[HALO_PORT_MAXIMUM_NETWORK_PLAYERS];

void network_suicide_reset(void)
{
    memset(suicide_rates, 0, sizeof(suicide_rates));
}

static boolean suicide_living_unit(long index, long unit_index)
{
    struct player_datum *player = player_try_and_get(index);
    struct unit_datum *unit = unit_try_and_get(unit_index);
    return player && !player->quit_out_of_game && player->unit_index == unit_index &&
        unit && unit->unit.player_index == index && !TEST_FLAG(unit->object.damage_flags, _object_dead_bit);
}

static boolean suicide_accept(long index, long unit_index)
{
    long slot = DATUM_INDEX_TO_ABSOLUTE_INDEX(index);
    unsigned long now = system_milliseconds();
    if (slot < 0 || slot >= HALO_PORT_MAXIMUM_NETWORK_PLAYERS || !suicide_living_unit(index, unit_index))
        return FALSE;
    if (suicide_rates[slot].requested && suicide_rates[slot].player_index == index &&
        now - suicide_rates[slot].requested_at < 1000) return FALSE;
    suicide_rates[slot].player_index = index;
    suicide_rates[slot].requested_at = now;
    suicide_rates[slot].requested = TRUE;
    unit_kill(unit_index);
    return TRUE;
}

static boolean suicide_request(void)
{
    long index = local_player_get_player_index(0);
    struct player_datum *player = player_try_and_get(index);
    struct suicide_request message;
    if (!game_in_progress() || main_menu_is_active() || !player ||
        !suicide_living_unit(index, player->unit_index))
    {
        console_warning("kill/suicide requires a living local player");
        return FALSE;
    }
    if (game_connection() != _game_connection_network_client)
        return suicide_accept(index, player->unit_index);
    memset(&message, 0, sizeof(message));
    message.header.type = _distributed_message_suicide_request;
    message.header.count = 1;
    message.header.game_time = game_time_get();
    message.player_index = index;
    message.unit_index = player->unit_index;
    build_message_header(&message.header.header, sizeof(message), 2, 0);
    return network_distributed_client_send_reliably(&message, sizeof(message));
}

boolean network_suicide_console_command(char const *expression, boolean *success)
{
    char const *text = expression;
    char word[8];
    unsigned length = 0;
    boolean parenthesis;
    while (*text == ' ' || *text == '\t') text++;
    parenthesis = *text == '(';
    if (parenthesis) text++;
    while (*text == ' ' || *text == '\t') text++;
    while (isalpha((unsigned char)*text) && length < sizeof(word) - 1)
        word[length++] = (char)tolower((unsigned char)*text++);
    word[length] = 0;
    if ((strcmp(word, "kill") && strcmp(word, "suicide")) ||
        (*text && *text != ' ' && *text != '\t' && *text != ')')) return FALSE;
    while (*text == ' ' || *text == '\t') text++;
    if (parenthesis && *text == ')') text++;
    else if (parenthesis) { *success = FALSE; return TRUE; }
    while (*text == ' ' || *text == '\t') text++;
    if (*text)
    {
        console_warning("kill/suicide takes no arguments");
        *success = FALSE;
    }
    else *success = suicide_request();
    return TRUE;
}

boolean network_suicide_handles_message(word const *data, word size)
{
    struct distributed_message_header header;
    if (size < sizeof(header)) return FALSE;
    memcpy(&header, data, sizeof(header));
    return header.type == _distributed_message_suicide_request;
}

void network_suicide_handle_message(long machine_index, word const *data, word size, boolean from_stream)
{
    struct suicide_request message;
    struct player_datum *player;
    if (!from_stream || machine_index == NONE || game_connection() != _game_connection_network_server ||
        !game_in_progress() || main_menu_is_active() || size != sizeof(message)) return;
    memcpy(&message, data, sizeof(message));
    if (GET_MESSAGE_TYPE(message.header.header) != 2 || message.header.count != 1 ||
        message.header.type != _distributed_message_suicide_request) return;
    player = player_try_and_get(message.player_index);
    if (!player || player->network_player_data.machine_index != machine_index) return;
    suicide_accept(message.player_index, message.unit_index);
}
