#ifndef BOARD_ZJB260_GPIO_H
#define BOARD_ZJB260_GPIO_H

#include "gpio.h"

/* 数码管两线接口由主板电阻上拉。 */
#define ZJB260_GPIO_DISPLAY_SDA      IO_PORTA_00 /* U9.9 SDA_A */
#define ZJB260_GPIO_DISPLAY_CLK      IO_PORTA_01 /* U9.8 CLK_A */

/* 按键外部上拉，按下接地。PA3 当前保留给串口日志。 */
#define ZJB260_GPIO_KEY_LEVEL_UP     IO_PORTA_02 /* U9.7 ENG+ */
#define ZJB260_GPIO_KEY_LEVEL_DOWN   IO_PORTA_03 /* U9.6 ENG- */
#define ZJB260_GPIO_KEY_TIME_UP      IO_PORTA_04 /* U9.5 TIM+ */
#define ZJB260_GPIO_KEY_TIME_DOWN    IO_PORTA_05 /* U9.4 TIM-，以芯片规格书为准 */
#define ZJB260_GPIO_KEY_START_PAUSE  IO_PORTA_06 /* U9.3 STAR/STOP */

/* 经三极管驱动，高电平有效。 */
#define ZJB260_GPIO_RUN_LED          IO_PORTA_08 /* U9.1 LED */
#define ZJB260_GPIO_BUZZER           IO_PORTA_10 /* U9.17 BEE */

/* J3 接口脚位，待接口协议确认后再启用。方向以主板为准。 */
#define ZJB260_GPIO_J3_TXD           IO_PORTA_07 /* U9.2 */
#define ZJB260_GPIO_J3_RXD           IO_PORTA_09 /* U9.20 */
#define ZJB260_GPIO_J3_HOST          IO_PORTA_11 /* U9.16 */

#endif
