#ifndef HALO_HOST_GAME_EVIDENCE_H
#define HALO_HOST_GAME_EVIDENCE_H
#include <stdint.h>
void mac_game_evidence_configure(uint32_t globals_va, uint32_t scenario_va);
void mac_ui_evidence_configure(uint32_t count, uint32_t targets, uint32_t click_x, uint32_t click_y);
void mac_game_evidence_presented(void);
#endif
