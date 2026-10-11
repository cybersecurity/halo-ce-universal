#ifndef NETWORK_AIM_ASSIST_H
#define NETWORK_AIM_ASSIST_H
#include "cseries.h"
void network_aim_assist_reset(void);
void network_aim_assist_update(void);
void network_aim_assist_send_policy(long machine_index);
boolean network_aim_assist_block_mouse(void);
boolean network_aim_assist_handles_message(word const *message, word size);
void network_aim_assist_handle_message(long machine_index, word const *message, word size);
#endif
