/*
CHAT.H

Text chat in network games (port/linux/game/chat.c): T says something to
everyone, Y to the player's team; the host passes it on
(network_distributed.c) and every machine shows it on its players' HUD.
*/

#ifndef __CHAT_H
#define __CHAT_H
#pragma once

#include "cseries.h"

/* ---------- constants */

/* the longest message's text, its end included */
#define CHAT_MAXIMUM_TEXT_SIZE 63
/* a player's messages: no more than one in this long (milliseconds) */
#define CHAT_COOLDOWN_MILLISECONDS 10000

/* ---------- prototypes/CHAT.C */

/* each frame of a game, beside the console's: TRUE while typing */
boolean chat_update(void);
boolean chat_is_active(void);
/* whether a key's press goes to the game (the platform layer passes only
the console's otherwise: xinput_sdl.c) */
boolean chat_key_passes(int virtual_key);
/* the text, printable ASCII only and trimmed, into destination (which may
be the source) */
void chat_text_clean(char *destination, char const *source, long size);
/* whether the text has a link in it (an address, a web site's name or an IP
address, its dots written as "(.)", "[dot]", " dot " and the like too):
such a message is not sent, and the host passes none on */
boolean chat_text_has_link(char const *text);
/* a message the host passed on (or the host's own), on the HUD of each of
this machine's players it is for */
void chat_show(wchar_t const *name, long team_index, boolean team_only, char const *text);

/* ---------- prototypes/NETWORK_DISTRIBUTED.C */

/* a local player's message: to the host, which passes it on (the host's
own, passed on at once) */
void distributed_chat_send(short local_player_index, boolean team_only, char const *text);

#endif // __CHAT_H
