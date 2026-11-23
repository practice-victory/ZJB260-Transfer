#include "app_config.h"

#ifdef CONFIG_BOARD_AW31N_DEMO
#include "includes.h"
#include "iic_api.h"
#include "irq.h"
#include "sys_timer.h"
#include "board_zjb260_gpio.h"
#include "board_zjb260_panel.h"
#include "zjb260_app.h"

#define ZJB260_PANEL_PERIOD_MS 10u
#define ZJB260_KEY_STABLE_SAMPLES 3u
#define ZJB260_DISPLAY_RETRY_MS 250u

#define TM1650_IIC_BUS HW_IIC_0
#define TM1650_CONTROL 0x48u
#define TM1650_DISPLAY_ON 0x39u /* 3级亮度、7段、开屏 */
#define TM1650_DIG1 0x68u
#define TM1650_DIG2 0x6au
#define TM1650_DIG3 0x6cu
#define TM1650_DIG4 0x6eu

static const struct {
    uint8_t gpio;
    uint8_t mask;
} panel_keys[] = {
    {ZJB260_GPIO_KEY_LEVEL_UP, DEVICE_KEY1},
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

/* 强度十位、强度个位、时间十位、时间个位。按图纸推定，待实板核对。 */
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

static void zjb260_panel_request_poll(void *priv)
{
    (void)priv;
    poll_pending = 1;
}

static void zjb260_panel_read_keys(void)
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
    displayed_level = 0;
    displayed_minutes = 0;
    display_retry_pending = 0;
    last_poll_ms = now_ms;
    poll_pending = 1;

    if (!panel_timer) {
        panel_timer = sys_timer_add(NULL, zjb260_panel_request_poll, ZJB260_PANEL_PERIOD_MS);
    }
}

void zjb260_panel_process(uint32_t now_ms)
{
    zjb260_app_output_t output;

    /* 定时中断只请求采样，业务状态和板上输出都在应用任务处理。 */
    if (!poll_pending && (panel_timer || now_ms - last_poll_ms < ZJB260_PANEL_PERIOD_MS)) {
        return;
    }
    CPU_SR_ALLOC();
    OS_ENTER_CRITICAL();
    poll_pending = 0;
    OS_EXIT_CRITICAL();
    last_poll_ms = now_ms;

    zjb260_panel_read_keys();
    zjb260_app_set_keys(now_ms, stable_keys);
    zjb260_app_get_output(&output);
    if (output.running != last_running) {
        gpio_write(ZJB260_GPIO_RUN_LED, output.running);
        last_running = output.running;
    }
    zjb260_panel_update_display(&output, now_ms);
}

#endif
