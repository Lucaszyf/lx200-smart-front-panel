/**
 * switch_monitor.h — SW-UI-INPUT / SW-UI-MODE 六态识别（Arduino IDE 版本）
 *
 * 六态定义：
 *   SW0=0(A) / SW0=1(B)  x  SW1=00(0/RS232) / 01(1/USB-C) / 10(2/WiFi)
 *   => A0, A1, A2, B0, B1, B2
 *   SW1="11" 为非法编码：维持上一次合法状态，OLED仅在跳变瞬间报错一次
 *
 * 本模块同时管理 EN0/EN1/EN2 三路电平转换通道使能：进入 A0/A1 时自动关闭，
 * 确保ESP32在这两个状态下真正电气隔离；其余状态保持开启。
 */

#pragma once

typedef enum {
    PANEL_MODE_A0 = 0,
    PANEL_MODE_A1,
    PANEL_MODE_A2,
    PANEL_MODE_B0,
    PANEL_MODE_B1,
    PANEL_MODE_B2,
    PANEL_MODE_INVALID,   // SW1="11" 非法编码
    PANEL_MODE_UNKNOWN,   // 尚未完成第一次消抖识别（开机瞬间）
} panel_mode_t;

void switch_monitor_init();

/** 启动状态轮询任务（内含50ms消抖逻辑），状态变化时自动刷新OLED模式代码/描述行。 */
void switch_monitor_start();

/** 当前经过消抖确认的合法状态。 */
panel_mode_t switch_monitor_get_mode();

/** 返回完整状态描述字符串（用于日志），例如 "B2: Wi-Fi <--> KP" */
const char *switch_monitor_mode_string(panel_mode_t mode);

/** 返回模式代码，例如 "A0"（用于OLED模式代码行） */
const char *switch_monitor_mode_code(panel_mode_t mode);

/** 返回通道描述，例如 "RS232 <--> RS232"（用于OLED通道描述行，不含代码前缀） */
const char *switch_monitor_mode_desc(panel_mode_t mode);
