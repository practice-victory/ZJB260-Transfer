#include "device_control.h"

/* 执行一次加减操作；key是数组下标0~3，不是DEVICE_KEY*位掩码。 */
void device_control_adjust(device_control_t *control, uint8_t key)
{
    uint8_t minutes;

    if (!control) {
        return;
    }
    minutes = control->state.minutes;

    /* 先判断边界再加减，避免档位或时间越界。 */
    switch (key) {
    case 0: /* key1：档位加1 */
        if (control->state.level < DEVICE_LEVEL_MAX) {
            control->state.level++;
        }
        break;
    case 1:
        if (control->state.level > DEVICE_LEVEL_MIN) {
            control->state.level--;
        }
        break;
    case 2:
        if (minutes < DEVICE_TIME_MAX_MINUTES) {
            minutes += DEVICE_TIME_STEP_MINUTES;
        }
        break;
    case 3:
        if (minutes > DEVICE_TIME_MIN_MINUTES) {
            minutes -= DEVICE_TIME_STEP_MINUTES;
        }
        break;
    default:
        break;
    }

    /* 到达上下限后minutes不变，因此继续按住不会反复重置倒计时。 */
    if (minutes != control->state.minutes) {
        control->state.minutes = minutes;
        control->state.remaining_ms = (uint32_t)minutes * 60000u;
        /* 时间减到0立即停止，避免下一次调度前仍保持运行状态。 */
        if (!minutes) {
            control->state.running = 0;
        }
    }
}

/* 建立初始业务状态，并清除按键历史与连发计时。 */
void device_control_init(device_control_t *control, uint32_t now_ms)
{
    uint8_t i;

    if (!control) {
        return;
    }
    control->state.level = DEVICE_DEFAULT_LEVEL;
    control->state.minutes = DEVICE_DEFAULT_MINUTES;
    control->state.running = 0;
    control->state.remaining_ms = (uint32_t)DEVICE_DEFAULT_MINUTES * 60000u;
    control->last_update_ms = now_ms;
    control->pressed_keys = 0;
    for (i = 0; i < 4; i++) {
        control->repeat_ms[i] = 0;
    }
}

uint8_t device_control_process(device_control_t *control,
                               uint32_t now_ms, uint8_t pressed_keys)
{
    device_control_state_t before;
    uint32_t elapsed;
    uint32_t repeats;
    uint32_t remainder;
    uint8_t changes = 0;
    uint8_t active;
    uint8_t previous;
    uint8_t bit;
    uint8_t i;

    if (!control) {
        return 0;
    }
    /* 保存处理前的快照，末尾据此生成变化标志。 */
    before = control->state;
    /* 无符号减法允许毫秒计数自然回绕，前提是调用间隔小于一个回绕周期。 */
    elapsed = now_ms - control->last_update_ms;
    control->last_update_ms = now_ms;

    /* 仅运行时扣除两次调用之间的时间；暂停期间仍更新上面的时间基准。 */
    if (control->state.running) {
        if (elapsed >= control->state.remaining_ms) {
            control->state.remaining_ms = 0;
            control->state.running = 0;
            changes |= DEVICE_EXPIRED;
        } else {
            control->state.remaining_ms -= elapsed;
        }
    }

    /* 丢弃无关位；同组加减键同时按下时，将该组按键视为无效。 */
    pressed_keys &= DEVICE_KEY_MASK;
    active = pressed_keys;
    previous = control->pressed_keys;
    if ((active & (DEVICE_KEY1 | DEVICE_KEY2)) ==
        (DEVICE_KEY1 | DEVICE_KEY2)) {
        active &= (uint8_t)~(DEVICE_KEY1 | DEVICE_KEY2);
    }
    if ((active & (DEVICE_KEY3 | DEVICE_KEY4)) ==
        (DEVICE_KEY3 | DEVICE_KEY4)) {
        active &= (uint8_t)~(DEVICE_KEY3 | DEVICE_KEY4);
    }

    /* key1~key4分别计时，key5不参与长按连发。 */
    for (i = 0; i < 4; i++) {
        bit = (uint8_t)(1u << i);
        if (!(active & bit)) {
            /* 松开或被冲突屏蔽：清除余量，下次按下重新计时。 */
            control->repeat_ms[i] = 0;
        } else if (!(previous & bit)) {
            /* 本次为1、上次为0，就是逻辑上升沿：立即执行一次。 */
            control->repeat_ms[i] = 0;
            device_control_adjust(control, i);
        } else {
            /*
             * 持续按住：累计完整500ms周期，余量保留到下一次调用。
             * 例如已有490ms，本次过去20ms，则执行1次并保留10ms。
             * 先除后加余数，避免较大的elapsed直接相加导致溢出。
             * 若调用延迟跨过多个周期，按完整周期数补做调整。
             */
            repeats = elapsed / DEVICE_KEY_REPEAT_MS;
            remainder = control->repeat_ms[i] + elapsed % DEVICE_KEY_REPEAT_MS;
            repeats += remainder / DEVICE_KEY_REPEAT_MS;
            control->repeat_ms[i] = remainder % DEVICE_KEY_REPEAT_MS;
            /* 当前档位/时间最多5步必达边界，限制补做次数，避免长循环。 */
            if (repeats > 5u) {
                repeats = 5u;
            }
            while (repeats) {
                device_control_adjust(control, i);
                repeats--;
            }
        }
    }

    /* key5只在新按下时切换状态，持续按住不会反复启停。 */
    if ((active & DEVICE_KEY5) && !(previous & DEVICE_KEY5)) {
        /* 0分钟是停止/待机设置，不允许启动电机或点亮开始灯。 */
        if (!control->state.minutes) {
            control->state.running = 0;
        } else if (!control->state.running) {
            /* 到时后或停止状态无剩余时间时重装；普通暂停恢复则保留剩余值。 */
            if (!control->state.remaining_ms) {
                control->state.remaining_ms = (uint32_t)control->state.minutes * 60000u;
            }
            control->state.running = 1;
        } else {
            control->state.running = 0;
        }
    }
    /* 保存冲突处理后的有效键位，供下次判断按下沿使用。 */
    control->pressed_keys = active;

    /* 按位合并本次变化，供外部决定是否更新档位输出、启停输出和显示。 */
    if (before.level != control->state.level) {
        changes |= DEVICE_CHANGED_LEVEL;
    }
    if (before.minutes != control->state.minutes ||
        before.remaining_ms != control->state.remaining_ms) {
        changes |= DEVICE_CHANGED_TIME;
    }
    if (before.running != control->state.running) {
        changes |= DEVICE_CHANGED_RUNNING;
    }
    return changes;
}

uint8_t device_control_level_value(const device_control_t *control)
{
    if (!control || control->state.level < DEVICE_LEVEL_MIN ||
        control->state.level > DEVICE_LEVEL_MAX) {
        return 0;
    }
    return (uint8_t)(control->state.level * DEVICE_LEVEL_VALUE_STEP);
}

uint8_t device_control_display_minutes(const device_control_t *control)
{
    uint32_t remaining;

    if (!control) {
        return 0;
    }
    remaining = control->state.remaining_ms;
    /* 整分钟数加上不足一分钟的部分，避免剩余几十秒时提前显示0。 */
    return (uint8_t)(remaining / 60000u + (remaining % 60000u != 0));
}