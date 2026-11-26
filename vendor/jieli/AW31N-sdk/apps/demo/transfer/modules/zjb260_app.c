#include "zjb260_app.h"

static device_control_t control;
static device_ble_protocol_t protocol;
static uint8_t keys;

/*
 * 主动上报用的状态快照：只看档位、设定分钟和运行位。
 * 倒计时造成的剩余秒数变化不触发上报，由小程序按 2~3 秒轮询同步。
 */
static uint8_t status_level;
static uint8_t status_minutes;
static uint8_t status_running;
static uint8_t status_dirty;

static void zjb260_app_note_status(void)
{
    if (control.state.level == status_level &&
        control.state.minutes == status_minutes &&
        control.state.running == status_running) {
        return;
    }
    status_level = control.state.level;
    status_minutes = control.state.minutes;
    status_running = control.state.running;
    status_dirty = 1;
}

void zjb260_app_init(uint32_t now_ms)
{
    device_control_init(&control, now_ms);
    zjb260_app_reset_ble();
    keys = 0;
    /* 初始状态作为基准，避免上电就补发一条无意义的通知。 */
    status_level = control.state.level;
    status_minutes = control.state.minutes;
    status_running = control.state.running;
    status_dirty = 0;
}

uint8_t zjb260_app_tick(uint32_t now_ms)
{
    uint8_t changes = device_control_process(&control, now_ms, keys);

    zjb260_app_note_status();
    return changes;
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
    uint8_t answered;

    /* 蓝牙操作也从最新的倒计时和实际按键状态开始处理。 */
    zjb260_app_tick(now_ms);
    answered = device_ble_protocol_handle(&protocol, &control, request, length,
                                          response, capacity);
    zjb260_app_note_status();
    /* 应答里已经带全量状态，这次变化不用再补发主动通知。 */
    if (answered == DEVICE_BLE_RESPONSE_SIZE) {
        status_dirty = 0;
    }
    return answered;
}

void zjb260_app_get_output(zjb260_app_output_t *output)
{
    if (output) {
        output->level = control.state.level;
        output->remaining_minutes = device_control_display_minutes(&control);
        output->running = control.state.running;
    }
}

uint8_t zjb260_app_take_status(uint8_t *buffer, uint16_t capacity)
{
    uint8_t length;

    if (!status_dirty) {
        return 0;
    }
    length = device_ble_protocol_status(&control, buffer, capacity);
    if (length) {
        status_dirty = 0;
    }
    return length;
}
