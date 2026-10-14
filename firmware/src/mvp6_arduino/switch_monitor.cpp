#include "switch_monitor.h"
#include "config.h"
#include "watchdog_feed.h"
#include "oled_display.h"

#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define POLL_INTERVAL_MS   10  // 远小于50ms消抖窗口，保证消抖计时精度

static volatile panel_mode_t s_current_mode = PANEL_MODE_UNKNOWN;

/* 六态显示格式，严格按指定文案，拆成"代码"和"描述"两部分分别渲染在
 * OLED不同行上；k_mode_strings仍保留用于日志完整打印。 */
static const char *k_mode_strings[PANEL_MODE_INVALID + 1] = {
    "A0: RS232 <--> RS232",
    "A1: USB-C <--> RS232",
    "A2: Wi-Fi <--> RS232",
    "B0: RS232 <--> KP",
    "B1: USB-C <--> KP",
    "B2: Wi-Fi <--> KP",
    "SWITCH INPUT ERROR",
};

static const char *k_mode_codes[PANEL_MODE_INVALID + 1] = {
    "Mode: A0", "Mode: A1", "Mode: A2", "Mode: B0", "Mode: B1", "Mode: B2", "Mode: ERR",
};

static const char *k_mode_descs[PANEL_MODE_INVALID + 1] = {
    "RS232 <--> RS232",
    "USB-C <--> RS232",
    "Wi-Fi <--> RS232",
    "RS232 <--> KP",
    "USB-C <--> KP",
    "Wi-Fi <--> KP",
    "SWITCH INPUT ERROR",
};

const char *switch_monitor_mode_string(panel_mode_t mode)
{
    if (mode == PANEL_MODE_UNKNOWN) return "SYSTEM BOOTING";
    if (mode < 0 || mode > PANEL_MODE_INVALID) return "?";
    return k_mode_strings[mode];
}

const char *switch_monitor_mode_code(panel_mode_t mode)
{
    if (mode < 0 || mode > PANEL_MODE_INVALID) return "?";
    return k_mode_codes[mode];
}

const char *switch_monitor_mode_desc(panel_mode_t mode)
{
    if (mode < 0 || mode > PANEL_MODE_INVALID) return "?";
    return k_mode_descs[mode];
}

// A0/A1下ESP32在电气上完全旁路，此时关闭三路TTL电平转换通道使能，
// 确保ESP32真正物理隔离，不只是软件不处理；其余状态（A2/B0/B1/B2）
// 涉及的电平转换/TMUX1209使能策略尚未逐条确认，留待对应MVP阶段实现，
// 这里统一保持开启（即MVP0的默认行为）。
static void apply_power_rail_policy(panel_mode_t mode)
{
    bool esp_isolated = (mode == PANEL_MODE_A0 || mode == PANEL_MODE_A1);
    int level = esp_isolated ? LOW : HIGH;

    digitalWrite(LEVEL_SHIFT_EN0_GPIO, level);
    digitalWrite(LEVEL_SHIFT_EN1_GPIO, level);
    digitalWrite(LEVEL_SHIFT_EN2_GPIO, level);
}

static panel_mode_t decode_raw(int sw0, int a0, int a1)
{
    // SW1 2bit编码：00=档位0(RS232) 01=档位1(USB-C) 10=档位2(WiFi/BT) 11=非法
    int sw1_code = (a1 << 1) | a0;

    if (sw1_code == 0b11) return PANEL_MODE_INVALID;

    if (sw0 == 0) { // A模式
        switch (sw1_code) {
            case 0b00: return PANEL_MODE_A0;
            case 0b01: return PANEL_MODE_A1;
            case 0b10: return PANEL_MODE_A2;
        }
    } else { // B模式
        switch (sw1_code) {
            case 0b00: return PANEL_MODE_B0;
            case 0b01: return PANEL_MODE_B1;
            case 0b10: return PANEL_MODE_B2;
        }
    }
    return PANEL_MODE_INVALID; // 理论不可达，防御性兜底
}

void switch_monitor_init()
{
    // 硬件需求文档要求这条GPIO读取路径已经过外部电平转换/保护电路处理，
    // 这里用纯输入模式，不额外加内部上下拉。
    pinMode(SW0_MODE_GPIO, INPUT);
    pinMode(SW1_BIT_A0_GPIO, INPUT);
    pinMode(SW1_BIT_A1_GPIO, INPUT);

    // EN0/EN1/EN2 三路电平转换通道使能：本模块统一管理，因为其开关状态
    // 要跟随六态识别结果联动。初始（状态尚未识别出来之前）先保持开启，
    // 状态一旦确认会立即由 apply_power_rail_policy() 按需调整。
    pinMode(LEVEL_SHIFT_EN0_GPIO, OUTPUT);
    pinMode(LEVEL_SHIFT_EN1_GPIO, OUTPUT);
    pinMode(LEVEL_SHIFT_EN2_GPIO, OUTPUT);
    digitalWrite(LEVEL_SHIFT_EN0_GPIO, HIGH);
    digitalWrite(LEVEL_SHIFT_EN1_GPIO, HIGH);
    digitalWrite(LEVEL_SHIFT_EN2_GPIO, HIGH);

    Serial.printf("[switch_mon] GPIO初始化完成 (SW0=IO%d, SW1_A0=IO%d, SW1_A1=IO%d)\n",
                  SW0_MODE_GPIO, SW1_BIT_A0_GPIO, SW1_BIT_A1_GPIO);
    Serial.printf("[switch_mon] 初始原始电平读数: SW0=%d SW1_A0=%d SW1_A1=%d\n",
                  digitalRead(SW0_MODE_GPIO), digitalRead(SW1_BIT_A0_GPIO), digitalRead(SW1_BIT_A1_GPIO));
}

static void switch_monitor_task(void *arg)
{
    (void)arg;
    watchdog_heartbeat_register(WDT_HB_SWITCH_MONITOR);

    panel_mode_t debounced_raw = PANEL_MODE_UNKNOWN; // 消抖后的原始识别结果，可能是INVALID
    panel_mode_t candidate_mode = PANEL_MODE_UNKNOWN;
    uint32_t candidate_since_ms = 0;
    bool first_loop = true;
    bool error_shown_for_current_run = false;

    while (1) {
        int sw0 = digitalRead(SW0_MODE_GPIO);
        int a0  = digitalRead(SW1_BIT_A0_GPIO);
        int a1  = digitalRead(SW1_BIT_A1_GPIO);
        panel_mode_t raw_mode = decode_raw(sw0, a0, a1);

        if (raw_mode != candidate_mode) {
            candidate_mode = raw_mode;
            candidate_since_ms = millis();
        } else {
            uint32_t stable_ms = millis() - candidate_since_ms; // 无符号减法，回绕安全
            bool stable_enough = (stable_ms >= SWITCH_DEBOUNCE_MS);

            if (stable_enough && (candidate_mode != debounced_raw || first_loop)) {
                debounced_raw = candidate_mode;
                first_loop = false;

                if (debounced_raw == PANEL_MODE_INVALID) {
                    // 关键点：s_current_mode（对外的"当前合法状态"）在此分支完全不touch，
                    // 严格维持上一次的合法状态不变。
                    if (!error_shown_for_current_run) {
                        Serial.println("[switch_mon] 检测到SW1非法编码(11)，维持上一次合法状态，OLED报错一次");
                        char line[80];
                        snprintf(line, sizeof(line), "%s  %s",
                                 switch_monitor_mode_code(PANEL_MODE_INVALID),
                                 switch_monitor_mode_desc(PANEL_MODE_INVALID));
                        oled_set_mode_line(line);
                        error_shown_for_current_run = true;
                    }
                } else {
                    error_shown_for_current_run = false;
                    s_current_mode = debounced_raw;
                    apply_power_rail_policy(debounced_raw);
                    Serial.printf("[switch_mon] 状态切换 -> %s (raw sw0=%d a1a0=%d%d)\n",
                                  switch_monitor_mode_string(debounced_raw), sw0, a1, a0);
                    char line[80];
                    snprintf(line, sizeof(line), "%s  %s",
                             switch_monitor_mode_code(debounced_raw),
                             switch_monitor_mode_desc(debounced_raw));
                    oled_set_mode_line(line);
                }
            }
        }

        watchdog_heartbeat_report(WDT_HB_SWITCH_MONITOR);
        vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
    }
}

void switch_monitor_start()
{
    xTaskCreate(switch_monitor_task, "switch_mon", 3072, NULL, 10, NULL);
}

panel_mode_t switch_monitor_get_mode()
{
    return s_current_mode;
}
