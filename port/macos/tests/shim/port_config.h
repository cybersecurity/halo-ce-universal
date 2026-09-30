/* port/linux/src/port_config.h's reading functions, answered by raytrace_test.c */
#ifndef TEST_PORT_CONFIG_H
#define TEST_PORT_CONFIG_H
const char *config_string(const char *name);
double config_real(const char *name);
int config_boolean(const char *name);
#endif
