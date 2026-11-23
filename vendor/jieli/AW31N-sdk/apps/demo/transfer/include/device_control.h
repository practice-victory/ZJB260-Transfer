#ifndef DEVICE_CONTROL_H
#define DEVICE_CONTROL_H

#include <stdint.h>

/* 档位、时间的单位和上下限；时间以5分钟为一个调整步长。 */
#define DEVICE_LEVEL_MIN          1u
#define DEVICE_LEVEL_MAX          5u
#define DEVICE_TIME_MIN_MINUTES   0u
#define DEVICE_TIME_MAX_MINUTES   20u
#define DEVICE_TIME_STEP_MINUTES  5u
#define DEVICE_LEVEL_VALUE_STEP   20u  /* 档位对外控制值：1档=20，5档=100 */
#define DEVICE_DEFAULT_LEVEL      1u
#define DEVICE_DEFAULT_MINUTES    0u
#define DEVICE_KEY_REPEAT_MS      500u  /* 按下立即响应，之后每500ms重复一次 */

/*
 * 逻辑按键位：某一位为1表示该键已按下，为0表示松开。
 * 与GPIO高低电平、ADC原始值无关，硬件适配层需要先完成映射和消抖。
 * 例如：DEVICE_KEY1 | DEVICE_KEY3 表示档位加和时间加同时按下。
 */
#define DEVICE_KEY1               (1u << 0)  /* 档位加 */
#define DEVICE_KEY2               (1u << 1)  /* 档位减 */
#define DEVICE_KEY3               (1u << 2)  /* 时间加 */
#define DEVICE_KEY4               (1u << 3)  /* 时间减 */
#define DEVICE_KEY5               (1u << 4)  /* 开始/暂停 */
#define DEVICE_KEY_MASK           0x1fu      /* 仅保留低5位按键信息 */

/* process返回的变化标志，可同时包含多项；用返回值 & 对应宏判断。 */
#define DEVICE_CHANGED_LEVEL      (1u << 0)  /* 档位发生变化 */
#define DEVICE_CHANGED_TIME       (1u << 1)  /* 设定分钟数或剩余毫秒数变化 */
#define DEVICE_CHANGED_RUNNING    (1u << 2)  /* 运行/停止状态发生变化 */
#define DEVICE_EXPIRED            (1u << 3)  /* 本次处理过程中倒计时到零 */

/* 对外可读取的业务状态；建议只读，由模块统一更新。 */
typedef struct {
    uint8_t level;          /* 当前档位：1~5 */
    uint8_t minutes;        /* 最近一次调整后的设定分钟数，可为0~20 */
    uint8_t running;        /* 1：运行；0：停止 */
    uint32_t remaining_ms; /* 剩余工作时间，单位毫秒；暂停时保持不变 */
} device_control_state_t;

/* 每个设备一份上下文，必须先初始化；以下计时字段由模块内部维护。 */
typedef struct {
    device_control_state_t state;
    uint32_t last_update_ms; /* 上次process调用的毫秒时间戳 */
    uint32_t repeat_ms[4];   /* key1~key4不足500ms的连发计时余量 */
    uint8_t pressed_keys;   /* 上次有效按键位，用于比较逻辑按下沿 */
} device_control_t;

/* 初始化为1档、0分钟、停止；now_ms应与后续process使用同一时钟。 */
void device_control_init(device_control_t *control, uint32_t now_ms);

/* 设置档位或整档分钟数。成功返回1，非法值返回0。 */
uint8_t device_control_set_level(device_control_t *control, uint8_t level);
uint8_t device_control_set_time(device_control_t *control, uint8_t minutes);

/* 暂停保留剩余时间，剩余时间为0时必须重新设时才能启动。 */
void device_control_set_running(device_control_t *control, uint8_t running);

/*
 * 周期处理按键和倒计时，建议每5~10ms从同一任务/主循环调用。
 * control：已初始化的上下文；传空指针时返回0。
 * now_ms：持续递增的uint32_t毫秒计数，允许自然回绕；
 *         相邻调用间隔必须小于一个完整回绕周期（约49.7天）。
 * pressed_keys：消抖后的逻辑按下位，不是SDK的CLICK/HOLD事件。
 *               按住期间持续传入对应位，全部松开时传0；本模块不做消抖。
 *
 * 行为约定：
 * 1. key1~key4按下沿立即调整，持续按住每500ms重复，达到边界不再改变。
 * 2. 同组加减键同时按下时屏蔽该组；松开其中一个后，剩下的键视为新按下。
 * 3. 时间键以实际剩余时间为基准加减5分钟，限幅至0~20分钟；
 *    剩余时间大于0时保持原启停状态，减到0时停止。
 * 4. key5只响应按下沿：暂停保留剩余时间；剩余时间为0时不能启动。
 * 5. 同次调用先扣除已运行时间，再处理key1~key4，最后处理key5。
 *
 * 返回DEVICE_CHANGED_*和DEVICE_EXPIRED的组合位；无变化返回0。
 * 注意：运行时剩余毫秒数变化也会返回DEVICE_CHANGED_TIME。
 * 本模块不操作硬件，调用者应根据state.running和state.level控制实际输出。
 * 不允许中断与主循环并发访问同一上下文；调用频率决定按键响应及时性。
 */
uint8_t device_control_process(device_control_t *control,
                               uint32_t now_ms, uint8_t pressed_keys);

/*
 * 执行一次相对调整：key为0档位加、1档位减、2时间加、3时间减。
 * 注意传入的是下标，不是DEVICE_KEY*位掩码；其他值或空指针不执行操作。
 * 与实体按键共用限幅逻辑，不修改按键历史或连发计时。
 * 蓝牙调用前先周期更新倒计时，并与process在同一任务中串行执行。
 */
void device_control_adjust(device_control_t *control, uint8_t key);

/* 读取当前档位对应的对外强度值：1档=20、2档=40，…，5档=100。 */
uint8_t device_control_level_value(const device_control_t *control);

/*
 * 硬件层每次状态变化后读取这些值：
 * level用于数码管显示1~5；display_minutes用于时间数码管显示0~20；
 * running用于运行灯。外部电源的档位和启停按电源侧协议接入。
 */

/* 剩余毫秒向上取整为显示分钟；到时显示0。 */
uint8_t device_control_display_minutes(const device_control_t *control);

#endif
