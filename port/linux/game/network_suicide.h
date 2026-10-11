#ifndef NETWORK_SUICIDE_H
#define NETWORK_SUICIDE_H
#include "cseries.h"
void network_suicide_reset(void);
boolean network_suicide_console_command(char const *expression, boolean *success);
boolean network_suicide_handles_message(word const *data, word size);
void network_suicide_handle_message(long machine, word const *data, word size, boolean from_stream);
#endif
