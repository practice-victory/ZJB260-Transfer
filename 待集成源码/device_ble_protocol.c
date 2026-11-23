#include "device_ble_protocol.h"

/* 逐字节异或校验：请求计算前5字节，应答计算前10字节。 */
static uint8_t device_ble_checksum(const uint8_t *data, uint8_t length)
{
    uint8_t value = 0;
    uint8_t i;

    for (i = 0; i < length; i++) {
        value ^= data[i];
    }
    return value;
}

/* 所有应答都带全量业务状态，便于小程序统一刷新界面。 */
static void device_ble_pack(const device_control_t *control,
                            uint8_t command, uint8_t sequence,
                            uint8_t result, uint8_t *response)
{
    uint32_t seconds = control->state.remaining_ms / 1000u;

    if (control->state.remaining_ms % 1000u) {
        seconds++;
    }
    response[0] = 0xaa;
    response[1] = 0x55;
    response[2] = command;
    response[3] = sequence;
    response[4] = result;
    /* 协议状态中强度使用对外控制值：1档20，2档40，…，5档100。 */
    response[5] = device_control_level_value(control);
    response[6] = control->state.minutes;
    response[7] = control->state.running;
    response[8] = (uint8_t)seconds;
    response[9] = (uint8_t)(seconds >> 8);
    response[10] = device_ble_checksum(response, 10);
}

void device_ble_protocol_init(device_ble_protocol_t *protocol)
{
    uint8_t i;

    if (!protocol) {
        return;
    }
    protocol->has_last = 0;
    for (i = 0; i < DEVICE_BLE_REQUEST_SIZE; i++) {
        protocol->last_request[i] = 0;
    }
    for (i = 0; i < DEVICE_BLE_RESPONSE_SIZE; i++) {
        protocol->last_response[i] = 0;
    }
}

uint8_t device_ble_protocol_handle(device_ble_protocol_t *protocol,
                                  device_control_t *control,
                                  const uint8_t *request, uint16_t length,
                                  uint8_t *response, uint16_t capacity)
{
    uint8_t frame[DEVICE_BLE_REQUEST_SIZE];
    uint8_t command = 0;
    uint8_t sequence = 0;
    uint8_t result = DEVICE_BLE_OK;
    uint8_t same;
    uint8_t i;

    /* 先检查回复空间，不能执行了命令却因空间不足无法组装回复。 */
    if (!protocol || !control || !request || !response ||
        capacity < DEVICE_BLE_RESPONSE_SIZE) {
        return 0;
    }

    /* 长度不符时不读取命令/序号，统一以80、00回复帧错误。 */
    if (length != DEVICE_BLE_REQUEST_SIZE) {
        result = DEVICE_BLE_BAD_FRAME;
    } else {
        /* 本地拷贝避免request与response指向同一缓冲区时覆盖请求。 */
        for (i = 0; i < DEVICE_BLE_REQUEST_SIZE; i++) {
            frame[i] = request[i];
        }
        command = frame[2];
        sequence = frame[3];
        if (frame[0] != 0xaa || frame[1] != 0x55 ||
            frame[5] != device_ble_checksum(frame, 5)) {
            result = DEVICE_BLE_BAD_FRAME;
        }
    }

    if (result == DEVICE_BLE_OK) {
        /* 相邻的完全相同请求返回原应答，不重复开始或重置时间。 */
        same = protocol->has_last;
        for (i = 0; i < DEVICE_BLE_REQUEST_SIZE; i++) {
            if (frame[i] != protocol->last_request[i]) {
                same = 0;
            }
        }
        if (same) {
            for (i = 0; i < DEVICE_BLE_RESPONSE_SIZE; i++) {
                response[i] = protocol->last_response[i];
            }
            return DEVICE_BLE_RESPONSE_SIZE;
        }

        switch (command) {
        case DEVICE_BLE_SET_LEVEL:
            /* 小程序传20/40/60/80/100，内部仍保存1~5档。 */
            if (!frame[4] || frame[4] % DEVICE_LEVEL_VALUE_STEP ||
                frame[4] > DEVICE_LEVEL_MAX * DEVICE_LEVEL_VALUE_STEP) {
                result = DEVICE_BLE_BAD_VALUE;
            } else {
                control->state.level = (uint8_t)(frame[4] / DEVICE_LEVEL_VALUE_STEP);
            }
            break;
        case DEVICE_BLE_SET_TIME:
            /* 时间按分钟原值传输，仅允许0、5、10、15、20。 */
            if (frame[4] < DEVICE_TIME_MIN_MINUTES ||
                frame[4] > DEVICE_TIME_MAX_MINUTES ||
                (frame[4] % DEVICE_TIME_STEP_MINUTES)) {
                result = DEVICE_BLE_BAD_VALUE;
            } else if (control->state.minutes != frame[4]) {
                /* 与实体键一致：改变设定时间才重置剩余时间。 */
                control->state.minutes = frame[4];
                control->state.remaining_ms = (uint32_t)frame[4] * 60000u;
                /* 设为0分钟即停止；非0分钟不改变当前运行/停止状态。 */
                if (!frame[4]) {
                    control->state.running = 0;
                }
            }
            break;
        case DEVICE_BLE_LEVEL_UP:
        case DEVICE_BLE_LEVEL_DOWN:
        case DEVICE_BLE_TIME_UP:
        case DEVICE_BLE_TIME_DOWN:
            if (frame[4] != 0) {
                result = DEVICE_BLE_BAD_VALUE;
            } else {
                /* 命令06~09依次对应key1~key4的单次操作，不伪造按住状态。 */
                device_control_adjust(control, (uint8_t)(command - DEVICE_BLE_LEVEL_UP));
            }
            break;
        case DEVICE_BLE_START:
        case DEVICE_BLE_PAUSE:
        case DEVICE_BLE_QUERY:
            if (frame[4] != 0) {
                result = DEVICE_BLE_BAD_VALUE;
            } else if (command == DEVICE_BLE_START) {
                /* 0分钟是停止/待机设置，不允许蓝牙启动。 */
                if (control->state.minutes) {
                    /* 明确开始而非切换；停止保留剩余时间，耗尽后才重装。 */
                    if (!control->state.remaining_ms) {
                        control->state.remaining_ms = (uint32_t)control->state.minutes * 60000u;
                    }
                    control->state.running = 1;
                }
            } else if (command == DEVICE_BLE_PAUSE) {
                /* 暂停等同停止，保留remaining_ms供下一次开始继续。 */
                control->state.running = 0;
            }
            /* 查询仅组装状态，不更改设定和运行状态。 */
            break;
        default:
            result = DEVICE_BLE_BAD_COMMAND;
            break;
        }
    }

    device_ble_pack(control, (uint8_t)(command | 0x80u), sequence, result, response);
    /* 只缓存成功请求；非法报文不能覆盖上一条成功请求的重试记录。 */
    if (result == DEVICE_BLE_OK) {
        for (i = 0; i < DEVICE_BLE_REQUEST_SIZE; i++) {
            protocol->last_request[i] = frame[i];
        }
        for (i = 0; i < DEVICE_BLE_RESPONSE_SIZE; i++) {
            protocol->last_response[i] = response[i];
        }
        protocol->has_last = 1;
    }
    return DEVICE_BLE_RESPONSE_SIZE;
}

uint8_t device_ble_protocol_status(const device_control_t *control,
                                  uint8_t *response, uint16_t capacity)
{
    if (!control || !response || capacity < DEVICE_BLE_RESPONSE_SIZE) {
        return 0;
    }
    device_ble_pack(control, DEVICE_BLE_STATUS_EVENT, 0, DEVICE_BLE_OK, response);
    return DEVICE_BLE_RESPONSE_SIZE;
}