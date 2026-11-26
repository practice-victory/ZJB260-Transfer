#ifndef ZJB260_APP_H
#define ZJB260_APP_H

#include "device_control.h"
#include "device_ble_protocol.h"

typedef struct {
    uint8_t level;
    uint8_t remaining_minutes;
    uint8_t running; /* 1点亮运行灯，0熄灭。 */
} zjb260_app_output_t;

/* 所有入口须从 Transfer 的应用任务调用，共用同一个毫秒时钟。 */
void zjb260_app_init(uint32_t now_ms);
uint8_t zjb260_app_tick(uint32_t now_ms);

/* 传入消抖后的完整按键位；返回 DEVICE_CHANGED_* 和 DEVICE_EXPIRED。 */
uint8_t zjb260_app_set_keys(uint32_t now_ms, uint8_t pressed_keys);

/* 新连接单独清除重发缓存。收发层负责传递请求及发送返回的应答。 */
void zjb260_app_reset_ble(void);
uint8_t zjb260_app_handle_ble(uint32_t now_ms,
                              const uint8_t *request, uint16_t length,
                              uint8_t *response, uint16_t capacity);

/* 板级驱动读取显示和运行灯目标值。 */
void zjb260_app_get_output(zjb260_app_output_t *output);

/*
 * 按键等非蓝牙途径改了档位/设定分钟/运行位后，取一帧主动状态通知（11字节）。
 * 无变化或参数无效返回0；返回后由收发层通过 AE02 通知发送。
 */
uint8_t zjb260_app_take_status(uint8_t *buffer, uint16_t capacity);

#endif
