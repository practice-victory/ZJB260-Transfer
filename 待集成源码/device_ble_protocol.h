#ifndef DEVICE_BLE_PROTOCOL_H
#define DEVICE_BLE_PROTOCOL_H

#include "device_control.h"

#define DEVICE_BLE_REQUEST_SIZE   6u
#define DEVICE_BLE_RESPONSE_SIZE  11u

#define DEVICE_BLE_SET_LEVEL      0x01u
#define DEVICE_BLE_SET_TIME       0x02u
#define DEVICE_BLE_START          0x03u
#define DEVICE_BLE_PAUSE          0x04u
#define DEVICE_BLE_QUERY          0x05u
#define DEVICE_BLE_LEVEL_UP       0x06u  /* key1：档位加1 */
#define DEVICE_BLE_LEVEL_DOWN     0x07u  /* key2：档位减1 */
#define DEVICE_BLE_TIME_UP        0x08u  /* key3：时间加5分钟 */
#define DEVICE_BLE_TIME_DOWN      0x09u  /* key4：时间减5分钟 */
#define DEVICE_BLE_STATUS_EVENT   0xf0u

#define DEVICE_BLE_OK             0u
#define DEVICE_BLE_BAD_FRAME      1u
#define DEVICE_BLE_BAD_COMMAND    2u
#define DEVICE_BLE_BAD_VALUE      3u

/*
 * 自定义协议V1，不是SDK原有协议，所有数值均为二进制，不是ASCII字符串。
 *
 * 小程序下行：固定6字节，每次BLE写入必须恰好包含一帧，不支持拆包/粘包。
 * 下标       0    1    2     3     4      5
 * 含义      AA   55   命令  序号  参数   前5字节逐字节异或
 * 命令01：设置强度，参数20/40/60/80/100（对应1~5档）。
 * 命令02：设置时间，参数0/5/10/15/20，单位分钟。
 * 命令03：开始/继续；命令04：停止；命令05：查询；三者参数必须为0。
 * 命令06：档位加1；命令07：档位减1；命令08：时间加5分钟；命令09：时间减5分钟。
 * 命令06~09参数必须为0，每个新请求只调整一次，到达上下限仍返回成功。
 * 加减不会改变运行/停止状态；时间设定值实际变化时才重置剩余时间。
 * 蓝牙指令不模拟持续按住，不影响实体按键的按下沿和500ms连发计时。
 * 小程序长按可每500ms发送一条新序号指令，松手即停止发送，不在设备端锁定按住。
 * 同一设定时间重复下发不重置倒计时；更换设定时间才重置剩余时间。
 * 开始不是启停切换；停止保留剩余时间；到时后开始重装设定时间。
 * running=1时开始灯应点亮，running=0时开始灯应熄灭；具体LED引脚由硬件层实现。
 *
 * 上行应答/主动通知：固定11字节，无需协商大MTU。
 * 下标       0    1    2             3     4       5     6        7       8     9     10
 * 含义      AA   55   命令|80       序号  结果码  强度  设定分   运行位  秒低  秒高  异或
 * 结果码：0成功、1帧错误、2未知命令、3参数错误；强度为20/40/60/80/100。
 * 剩余秒数向上取整，以uint16小端传输；运行位为0或1。
 * 主动通知使用命令F0、序号0、结果0，其余格式相同。
 * 校验均为前面所有字节异或；仅用于误码检测，不提供加密或身份认证。
 *
 * 小程序示例（十六进制字节）：
 * 设置3档（强度60），序号01：AA 55 01 01 3C C3
 * 设置15分钟，序号02：AA 55 02 02 0F F0
 * 开始，序号03：AA 55 03 03 00 FF
 * 暂停，序号04：AA 55 04 04 00 FF
 * 查询，序号05：AA 55 05 05 00 FF
 * key1档位加1，序号06：AA 55 06 06 00 FF
 * key2档位减1，序号07：AA 55 07 07 00 FF
 * key3时间加5分钟，序号08：AA 55 08 08 00 FF
 * key4时间减5分钟，序号09：AA 55 09 09 00 FF
 *
 * 序号由小程序每个新请求递增（允许255回绕至0），一次仅等待一个请求。
 * 应答丢失时重发完全相同的帧；紧邻的相同有效帧只返回缓存应答，不重复执行。
 * 缓存只保留最后一个成功请求，历史请求及跨连接重发不保证去重。
 * 重连后先查询，不自动重发旧控制命令。
 */
typedef struct {
    uint8_t has_last;
    uint8_t last_request[DEVICE_BLE_REQUEST_SIZE];
    uint8_t last_response[DEVICE_BLE_RESPONSE_SIZE];
} device_ble_protocol_t;

/* 每次新连接建立时清除去重缓存，不重置设备业务状态。 */
void device_ble_protocol_init(device_ble_protocol_t *protocol);

/*
 * 在与按键/倒计时相同的任务中调用，不得从另一中断并发修改control。
 * 调用前先用当前毫秒时钟和真实按键状态调用device_control_process，
 * 保证倒计时是最新的；不要为了处理蓝牙而伪造按下/释放事件。
 * request为一次完整BLE写入内容，length为实际长度。
 * response至少容纳11字节；空间不够或指针无效时返回0且不执行命令。
 * 其他情况返回11，包括错误应答；错误帧不会修改业务状态。
 * 返回后调用者负责应用硬件输出，并通过已订阅的notify发送应答。
 * 发送失败要保留应答等待重试，不能假定返回11代表蓝牙已发送成功。
 */
uint8_t device_ble_protocol_handle(device_ble_protocol_t *protocol,
                                  device_control_t *control,
                                  const uint8_t *request, uint16_t length,
                                  uint8_t *response, uint16_t capacity);

/*
 * 生成当前全量状态通知，返回11；指针无效或容量不足返回0。
 * 订阅成功、按键/蓝牙设置变化、启停或到时后发送；运行中建议每秒最多一次。
 * 不要每次DEVICE_CHANGED_TIME都发送，否则5~10ms周期会造成通知过密。
 * 本接口只组包，不负责连接、CCC订阅检查、发送队列或发送重试。
 */
uint8_t device_ble_protocol_status(const device_control_t *control,
                                  uint8_t *response, uint16_t capacity);

#endif