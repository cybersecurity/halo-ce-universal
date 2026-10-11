/* Co-op has no competitive game engine to announce player deaths.
   Only the host formats a death, then uses the existing reliable notices. */
#include "cseries.h"
#include "game/game.h"
#include "game/game_globals.h"
#include "game/game_allegiance.h"
#include "game/players.h"
#include "networking/network_game_globals.h"
#include "objects/objects.h"
#include "objects/damage.h"
#include "objects/damage_effect_definitions.h"
#include "units/units.h"
#include "tag_groups.h"
#include "network_coop.h"
#include "network_distributed.h"
#include "network_killfeed.h"
#include <stdio.h>
#include <string.h>

static long killfeed_dead_units[HALO_PORT_MAXIMUM_OBJECTS_PER_MAP];
/* The retail damage tag's categories (falling and vehicle collision). */
enum { KILLFEED_FALLING = 1, KILLFEED_VEHICLE = 9 };

void network_killfeed_reset(void)
{
    unsigned i;
    for (i = 0; i < HALO_PORT_MAXIMUM_OBJECTS_PER_MAP; i++) killfeed_dead_units[i] = NONE;
}

static boolean killfeed_present(struct player_datum const *player)
{
    return player && !player->quit_out_of_game;
}

/* Same printable names as the other distributed notices. Do not allow
   profile text to insert console formatting or a new line. */
static void killfeed_player_name(struct player_datum const *player, char *name, size_t size)
{
    unsigned i = 0;
    while (i < 12 && i + 1 < size && player->name[i])
    {
        unsigned c = player->name[i];
        name[i++] = c < 32 || c > 126 || c == '|' ? '?' : (char)c;
    }
    name[i] = 0;
    if (!i && size > 8) strcpy(name, "Unknown");
}

static char const *killfeed_unit_kind(char const *tag)
{
    if (strstr(tag,"wraith")) return "Wraith Tank";
    if (strstr(tag,"scorpion")) return "Scorpion Tank";
    if (strstr(tag,"warthog")) return "Warthog";
    if (strstr(tag,"ghost")) return "Ghost";
    if (strstr(tag,"banshee")) return "Banshee";
    if (strstr(tag,"flood") && strstr(tag,"combat")) return "Combat Flood";
    if (strstr(tag,"flood") && strstr(tag,"carrier")) return "Carrier Flood";
    if (strstr(tag,"flood") && strstr(tag,"infection")) return "Infection Flood";
    if (strstr(tag,"grunt")) return "Grunt";
    if (strstr(tag,"elite")) return "Elite";
    if (strstr(tag,"hunter")) return "Hunter";
    if (strstr(tag,"jackal")) return "Jackal";
    if (strstr(tag,"sentinel")) return "Sentinel";
    if (strstr(tag,"marine")) return "Marine";
    return NULL;
}

static char const *killfeed_damage_kind(char const *tag)
{
    if (strstr(tag,"plasma grenade")) return "Plasma Grenade";
    if (strstr(tag,"frag grenade")) return "Frag Grenade";
    if (strstr(tag,"wraith")) return "Wraith Tank";
    if (strstr(tag,"rocket")) return "Rocket";
    return NULL;
}

void network_killfeed_note_death(long unit_index, struct damage_data const *damage)
{
    struct unit_datum *unit;
    struct object_datum *owner;
    struct player_datum *dead, *killer;
    struct damage_definition const *definition;
    char const *tag, *cause, *kind, *damage_tag;
    char name[32], killer_name[32], line[240];
    long slot = DATUM_INDEX_TO_ABSOLUTE_INDEX(unit_index);
    long dead_index, killer_index;
    if (game_connection() != _game_connection_network_server || !network_coop_active() ||
        slot < 0 || slot >= HALO_PORT_MAXIMUM_OBJECTS_PER_MAP ||
        !(unit = unit_try_and_get(unit_index)) || killfeed_dead_units[slot] == unit_index) return;
    dead_index = unit->unit.player_index;
    dead = player_try_and_get(dead_index);
    if (!killfeed_present(dead) || dead->unit_index != unit_index) return;
    killfeed_dead_units[slot] = unit_index;
    if (!damage)
    {
        if (TEST_FLAG(unit->object.damage_flags, _object_die_act_of_god_silent_bit) ||
            TEST_FLAG(unit->object.damage_flags, _object_die_act_of_god_no_statistics_bit)) return;
        killfeed_player_name(dead, name, sizeof(name));
        snprintf(line, sizeof(line), "%s %s", name,
            TEST_FLAG(unit->object.flags, _object_outside_of_map_bit) ? "fell out of the world" : "died");
        distributed_send_notice(line);
        return;
    }
    if (TEST_FLAG(damage->flags, _damage_no_statistics_bit)) return;
    killer_index = damage->owner_player_index;
    killer = player_try_and_get(killer_index);
    owner = object_try_and_get(damage->owner_object_index);
    definition = damage->definition_index != NONE ? &damage_effect_definition_get(damage->definition_index)->damage : NULL;
    tag = damage->definition_index != NONE ? tag_get_name(damage->definition_index) : "";
    if (!tag) tag = "";
    damage_tag = tag;
    cause = killfeed_damage_kind(tag);
    tag = owner ? tag_get_name(owner->definition_index) : "";
    kind = killfeed_unit_kind(tag ? tag : "");
    killfeed_player_name(dead, name, sizeof(name));
    if (killfeed_present(killer) && killer_index != dead_index)
    {
        killfeed_player_name(killer, killer_name, sizeof(killer_name));
        snprintf(line, sizeof(line), "%s was killed by %s%s%s%s%s", name, killer_name,
            cause ? " (" : "", cause ? cause : "", cause ? ")" : "",
            game_team_is_enemy(killer->team_index, dead->team_index) ? "" : " [team kill]");
    }
    else if (killer_index == dead_index)
        snprintf(line, sizeof(line), "%s killed themselves%s%s%s", name,
            cause ? " (" : "", cause ? cause : "", cause ? ")" : "");
    else if (TEST_FLAG(unit->object.flags, _object_outside_of_map_bit) ||
        strstr(damage_tag, "distance"))
        snprintf(line, sizeof(line), "%s fell out of the world", name);
    else if (TEST_FLAG(unit->object.damage_flags, _object_die_act_of_god_bit))
        snprintf(line, sizeof(line), "%s died", name);
    else if (definition && definition->category == KILLFEED_FALLING)
        snprintf(line, sizeof(line), "%s died of fall", name);
    else if (definition && definition->category == KILLFEED_VEHICLE && kind)
        snprintf(line, sizeof(line), "%s was run over by %s", name, kind);
    else snprintf(line, sizeof(line), "%s was killed by %s", name, cause ? cause : kind ? kind : "an unknown cause");
    line[sizeof(line) - 1] = 0;
    distributed_send_notice(line);
}
