#ifndef BOARD_ZJB260_PANEL_H
#define BOARD_ZJB260_PANEL_H

#include <stdint.h>

/* Transfer 应用任务入口，使用与 zjb260_app 相同的毫秒时钟。 */
void zjb260_panel_init(uint32_t now_ms);

/* 只做按键采样与消抖，把稳定键位交给 zjb260_app。 */
void zjb260_panel_process(uint32_t now_ms);

/*
 * 唯一的输出应用入口：读业务状态后刷新数码管、运行灯和到点提示音。
 * 按键路径和蓝牙命令路径都在改完状态后调用它，避免两套刷新逻辑。
 */
void zjb260_panel_apply(uint32_t now_ms);

#endif
