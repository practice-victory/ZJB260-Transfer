#include "app_config.h"

#ifdef CONFIG_BOARD_AW31N_DEMO
#include "includes.h"
#include "iic_api.h"
#include "irq.h"
#include "mcpwm.h"
#include "sys_timer.h"
#include "board_zjb260_gpio.h"
#include "board_zjb260_panel.h"
#include "zjb260_app.h"

#define ZJB260_PANEL_PERIOD_MS 10u
#define ZJB260_KEY_STABLE_SAMPLES 3u
#define ZJB260_DISPLAY_RETRY_MS 250u

/* 蜂鸣器：无源电磁式，需方波驱动；按键短鸣一声，到点播放一小段音阶。 */
#define ZJB260_BUZZER_FREQ 2731u
#define ZJB260_BUZZER_DUTY 5000u
#define ZJB260_BEEP_KEY_ON_MS 40u
#define ZJB260_BEEP_REPEAT_GAP_MS 200u /* 整段重复之间的停顿 */

typedef struct {
    uint16_t freq_hz;
    uint16_t on_ms;
    uint16_t gap_ms;
} beep_note_t;

/* 按键：谐振点单声。 */
static const beep_note_t beep_key_tune[] = {
    {ZJB260_BUZZER_FREQ, ZJB260_BEEP_KEY_ON_MS, 0u},
};

/* 到点：C6-E6-G6-C7 上行四音，含间隔共约 0.96s。 */
static const beep_note_t beep_expire_tune[] = {
    {1047u, 150u, 30u},
    {1319u, 150u, 30u},
    {1568u, 150u, 30u},
    {2093u, 300u, 0u},
};

#define TM1650_IIC_BUS HW_IIC_0
#define TM1650_CONTROL 0x48u
#define TM1650_DISPLAY_ON 0x39u /* 亮度位011（8档中第5档）、7段模式、开显示；亮度后续再调 */
#define TM1650_DIG1 0x68u
#define TM1650_DIG2 0x6au
#define TM1650_DIG3 0x6cu
#define TM1650_DIG4 0x6eu

static const struct {
    uint8_t gpio;
    uint8_t mask;
} panel_keys[] = {
    {ZJB260_GPIO_KEY_LEVEL_UP, DEVICE_KEY1},
    {ZJB260_GPIO_KEY_LEVEL_DOWN, DEVICE_KEY2},
    {ZJB260_GPIO_KEY_TIME_UP, DEVICE_KEY3},
    {ZJB260_GPIO_KEY_TIME_DOWN, DEVICE_KEY4},
    {ZJB260_GPIO_KEY_START_PAUSE, DEVICE_KEY5},
};

static struct iic_master_config display_bus = {
    .role = IIC_MASTER,
    .scl_io = ZJB260_GPIO_DISPLAY_CLK,
    .sda_io = ZJB260_GPIO_DISPLAY_SDA,
    .io_mode = PORT_INPUT_FLOATING,
    .hdrive = PORT_DRIVE_STRENGT_2p4mA,
    .master_frequency = 100000u,
    .io_filter = 1,
};

static const uint8_t digit_segments[] = {
    0x3f, 0x06, 0x5b, 0x4f, 0x66, 0x6d, 0x7d, 0x07, 0x7f, 0x6f
};

/* 强度十位、强度个位、时间十位、时间个位。
 * 按惯例假定COM1为左侧位（十位），实板若显示成“50”则改为{DIG1, DIG2, DIG3, DIG4}。
 */
static const uint8_t digit_addresses[] = {
    TM1650_DIG2, TM1650_DIG1, TM1650_DIG3, TM1650_DIG4
};

static uint8_t key_samples[ARRAY_SIZE(panel_keys)];
static uint8_t stable_keys;
static uint8_t last_running;
static uint16_t panel_timer;
static volatile uint8_t poll_pending;
static uint32_t last_poll_ms;
static uint8_t display_bus_ready;
static uint8_t displayed_level;
static uint8_t displayed_minutes;
static uint8_t display_retry_pending;
static uint32_t display_failed_ms;

enum {
    BEEP_IDLE = 0,
    BEEP_ON,
    BEEP_GAP,
};

static uint8_t beep_state;
static const beep_note_t *beep_tune;
static uint8_t beep_tune_len;
static uint8_t beep_note_index;
static uint8_t beep_repeat; /* 剩余重复遍数，1表示只播一遍 */
static uint32_t beep_deadline_ms;

static void zjb260_panel_request_poll(void *priv)
{
    (void)priv;
    poll_pending = 1;
}

/* 蜂鸣器走MCPWM：duty=0静音，duty=5000输出方波。 */
static struct mcpwm_config buzzer_pwm_cfg = {
    .frequency = ZJB260_BUZZER_FREQ,
    .deadtime_ns = 1000u,
    .h_pin = ZJB260_GPIO_BUZZER,
    .l_pin = (u16)-1,
    .duty = 0,
    .detect_port = (u16)-1,
    .irq_cb = NULL,
    .irq_priority = 1,
    .ch = MCPWM_CH0,
    .aligned_mode = MCPWM_EDGE_ALIGNED,
    .complementary_en = 0,
    .edge = MCPWM_EDGE_DEFAULT,
};

static int8_t buzzer_pwm_id = -1;

static void zjb260_panel_beep_hw_init(void)
{
    int id = mcpwm_init(&buzzer_pwm_cfg);
    if (id < 0) {
        return; /* 初始化失败时保持静音，不影响按键。 */
    }
    buzzer_pwm_id = (int8_t)id;
    mcpwm_start(buzzer_pwm_id); /* 初始duty为0，静音。 */
}

static void zjb260_panel_beep_hw_off(void)
{
    if (buzzer_pwm_id >= 0) {
        mcpwm_set_duty(buzzer_pwm_id, 0);
    }
}

/* 开启当前音符：先改频率再给50%占空比。 */
static void zjb260_panel_beep_note_on(uint32_t now_ms)
{
    const beep_note_t *note = &beep_tune[beep_note_index];

    if (buzzer_pwm_id >= 0) {
        mcpwm_set_frequency(buzzer_pwm_id, MCPWM_EDGE_ALIGNED, note->freq_hz);
        mcpwm_set_duty(buzzer_pwm_id, ZJB260_BUZZER_DUTY);
    }
    beep_state = BEEP_ON;
    beep_deadline_ms = now_ms + note->on_ms;
}

static void zjb260_panel_beep_play(uint32_t now_ms,
                                   const beep_note_t *tune, uint8_t len,
                                   uint8_t times)
{
    if (!tune || !len) {
        return;
    }
    beep_tune = tune;
    beep_tune_len = len;
    beep_repeat = times ? times : 1;
    beep_note_index = 0;
    zjb260_panel_beep_note_on(now_ms);
}

/* 非阻塞推进：音符到时关断，间隔到时或直接切到下一个音符。 */
static void zjb260_panel_beep_process(uint32_t now_ms)
{
    uint16_t gap_ms;

    if (beep_state == BEEP_IDLE || (int32_t)(now_ms - beep_deadline_ms) < 0) {
        return;
    }
    if (beep_state == BEEP_ON) {
        gap_ms = beep_tune[beep_note_index].gap_ms;
        zjb260_panel_beep_hw_off();
        if (++beep_note_index >= beep_tune_len) {
            if (beep_repeat > 1) {
                /* 还有重复遍数：停留一小段后从头再播。 */
                beep_repeat--;
                beep_note_index = 0;
                beep_state = BEEP_GAP;
                beep_deadline_ms = now_ms + ZJB260_BEEP_REPEAT_GAP_MS;
                return;
            }
            beep_state = BEEP_IDLE;
            return;
        }
        if (gap_ms) {
            beep_state = BEEP_GAP;
            beep_deadline_ms = now_ms + gap_ms;
        } else {
            zjb260_panel_beep_note_on(now_ms);
        }
    } else {
        zjb260_panel_beep_note_on(now_ms);
    }
}

static void zjb260_panel_read_keys(uint32_t now_ms)
{
    for (uint8_t i = 0; i < ARRAY_SIZE(panel_keys); i++) {
        uint8_t mask = panel_keys[i].mask;
        uint8_t pressed = gpio_read(panel_keys[i].gpio) == 0;
        uint8_t was_pressed = (stable_keys & mask) != 0;

        if (pressed == was_pressed) {
            key_samples[i] = 0;
        } else if (++key_samples[i] >= ZJB260_KEY_STABLE_SAMPLES) {
            stable_keys ^= mask;
            key_samples[i] = 0;
            /* 按下沿短鸣；到点音乐进行中时不打断。 */
            if (pressed && beep_state == BEEP_IDLE) {
                zjb260_panel_beep_play(now_ms, beep_key_tune,
                                       ARRAY_SIZE(beep_key_tune), 1);
            }
        }
    }
}

static uint8_t tm1650_write(uint8_t address, uint8_t value)
{
    hw_iic_start(TM1650_IIC_BUS);
    /* TM1650 的命令或显存地址作为首字节发送，最低位已是写入位。 */
    uint8_t acknowledged = hw_iic_tx_byte(TM1650_IIC_BUS, address) &&
                           hw_iic_tx_byte(TM1650_IIC_BUS, value);
    hw_iic_stop(TM1650_IIC_BUS);
    return acknowledged;
}

static uint8_t tm1650_render(uint8_t level, uint8_t minutes)
{
    uint8_t digits[] = {level / 10, level % 10, minutes / 10, minutes % 10};

    if (!display_bus_ready) {
        if (hw_iic_init(TM1650_IIC_BUS, &display_bus) != IIC_OK) {
            return 0;
        }
        display_bus_ready = 1;
    }

    if (!displayed_level && !tm1650_write(TM1650_CONTROL, TM1650_DISPLAY_ON)) {
        return 0;
    }
    for (uint8_t i = 0; i < ARRAY_SIZE(digit_addresses); i++) {
        if (!tm1650_write(digit_addresses[i], digit_segments[digits[i]])) {
            return 0;
        }
    }
    return 1;
}

static void zjb260_panel_update_display(const zjb260_app_output_t *output, uint32_t now_ms)
{
    if (output->level == displayed_level && output->remaining_minutes == displayed_minutes) {
        return;
    }
    if (display_retry_pending && now_ms - display_failed_ms < ZJB260_DISPLAY_RETRY_MS) {
        return;
    }
    if (tm1650_render(output->level, output->remaining_minutes)) {
        displayed_level = output->level;
        displayed_minutes = output->remaining_minutes;
        display_retry_pending = 0;
    } else {
        displayed_level = 0; /* 部分写入失败后，下次重新配置并刷新四位。 */
        display_failed_ms = now_ms;
        display_retry_pending = 1;
    }
}

void zjb260_panel_init(uint32_t now_ms)
{
    stable_keys = 0;
    for (uint8_t i = 0; i < ARRAY_SIZE(key_samples); i++) {
        key_samples[i] = 0;
    }
    last_running = 0;
    gpio_write(ZJB260_GPIO_RUN_LED, 0);
    beep_state = BEEP_IDLE;
    beep_tune_len = 0;
    beep_note_index = 0;
    beep_repeat = 0;
    zjb260_panel_beep_hw_init();
    displayed_level = 0;
    displayed_minutes = 0;
    display_retry_pending = 0;
    last_poll_ms = now_ms;
    poll_pending = 1;

    if (!panel_timer) {
        panel_timer = sys_timer_add(NULL, zjb260_panel_request_poll, ZJB260_PANEL_PERIOD_MS);
    }
}

/* 统一输出：数码管、运行灯、到点提示音。按键和蓝牙改完状态后都只调这一个函数。 */
void zjb260_panel_apply(uint32_t now_ms)
{
    zjb260_app_output_t output;

    zjb260_app_get_output(&output);
    /* 倒计时到零：由运行转为停止且剩余为0，播放到点提示音。 */
    if (last_running && !output.running && !output.remaining_minutes) {
        zjb260_panel_beep_play(now_ms, beep_expire_tune,
                               ARRAY_SIZE(beep_expire_tune), 2);
    }
    zjb260_panel_beep_process(now_ms);
    if (output.running != last_running) {
        gpio_write(ZJB260_GPIO_RUN_LED, output.running);
        last_running = output.running;
    }
    zjb260_panel_update_display(&output, now_ms);
}

void zjb260_panel_process(uint32_t now_ms)
{
    /* 定时中断只请求采样，按键消抖在应用任务处理；输出由 zjb260_panel_apply 统一刷新。 */
    if (!poll_pending && (panel_timer || now_ms - last_poll_ms < ZJB260_PANEL_PERIOD_MS)) {
        return;
    }
    CPU_SR_ALLOC();
    OS_ENTER_CRITICAL();
    poll_pending = 0;
    OS_EXIT_CRITICAL();
    last_poll_ms = now_ms;

    zjb260_panel_read_keys(now_ms);
    zjb260_app_set_keys(now_ms, stable_keys);
}

#endif
