#include "zjb260_app.h"

static device_control_t control;
static device_ble_protocol_t protocol;
static uint8_t keys;

void zjb260_app_init(uint32_t now_ms)
{
    device_control_init(&control, now_ms);
    zjb260_app_reset_ble();
    keys = 0;
}

uint8_t zjb260_app_tick(uint32_t now_ms)
{
    return device_control_process(&control, now_ms, keys);
}

uint8_t zjb260_app_set_keys(uint32_t now_ms, uint8_t pressed_keys)
{
    keys = pressed_keys;
    return zjb260_app_tick(now_ms);
}

void zjb260_app_reset_ble(void)
{
    device_ble_protocol_init(&protocol);
}

uint8_t zjb260_app_handle_ble(uint32_t now_ms,
                              const uint8_t *request, uint16_t length,
                              uint8_t *response, uint16_t capacity)
{
    /* 蓝牙操作也从最新的倒计时和实际按键状态开始处理。 */
    zjb260_app_tick(now_ms);
    return device_ble_protocol_handle(&protocol, &control, request, length,
                                      response, capacity);
}

void zjb260_app_get_output(zjb260_app_output_t *output)
{
    if (output) {
        output->level = control.state.level;
        output->remaining_minutes = device_control_display_minutes(&control);
        output->running = control.state.running;
    }
}
