#ifndef BOARD_ZJB260_PANEL_H
#define BOARD_ZJB260_PANEL_H

#include <stdint.h>

/* Transfer 应用任务入口，使用与 zjb260_app 相同的毫秒时钟。 */
void zjb260_panel_init(uint32_t now_ms);
void zjb260_panel_process(uint32_t now_ms);

#endif
