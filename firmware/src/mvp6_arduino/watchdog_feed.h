/**
 * watchdog_feed.h — STWD100XY 硬件看门狗喂狗任务（Arduino IDE 版本）
 */

#pragma once

typedef enum {
    WDT_HB_SWITCH_MONITOR = 0,
    WDT_HB_TELEMETRY,
    WDT_HB_ROUTER_BRIDGE,
    WDT_HB_HANDSET_BRIDGE,
    WDT_HB_TRANSCODE,
    WDT_HB_MIRROR,
    WDT_HB_MODULE_COUNT
} watchdog_heartbeat_module_t;

/** 启动喂狗任务。必须在 setup() 最开头调用，不依赖任何其他外设初始化。 */
void watchdog_feed_start();

/** 各关键任务在自己的主循环里每轮调用一次，上报"我还活着"。 */
void watchdog_heartbeat_report(watchdog_heartbeat_module_t module);

/** 在模块自己的初始化函数里调用一次，声明"该模块要接受心跳监督"。 */
void watchdog_heartbeat_register(watchdog_heartbeat_module_t module);
