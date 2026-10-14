#include "watchdog_feed.h"
#include "config.h"

#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static volatile bool s_registered[WDT_HB_MODULE_COUNT] = {false};
static volatile uint32_t s_last_heartbeat_ms[WDT_HB_MODULE_COUNT] = {0};
static volatile bool s_feeding_suspended = false;

static void feed_pulse()
{
    // WDI 引脚喂狗方式：拉高维持一小段时间再拉低（沿触发型喂狗）。
    // 如果实测发现STWD100XY是电平触发而非脉冲触发，把delayMicroseconds那行
    // 去掉，直接维持固定电平即可。
    digitalWrite(WDT_FEED_GPIO, HIGH);
    delayMicroseconds(50);
    digitalWrite(WDT_FEED_GPIO, LOW);
}

static bool all_registered_modules_alive()
{
    uint32_t now_ms = millis();
    for (int i = 0; i < WDT_HB_MODULE_COUNT; i++) {
        if (!s_registered[i]) {
            continue; // 该模块尚未接入心跳监督，不参与判断
        }
        // 用无符号减法做差值计算，millis()发生回绕(约49.7天一次)时依然正确
        uint32_t elapsed_ms = now_ms - s_last_heartbeat_ms[i];
        if (elapsed_ms > WDT_HEARTBEAT_TIMEOUT_MS) {
            Serial.printf("[watchdog] 模块 %d 心跳超时 (%lums 未上报)，停止喂狗，放任硬件复位\n",
                          i, (unsigned long)elapsed_ms);
            return false;
        }
    }
    return true;
}

static void watchdog_task(void *arg)
{
    (void)arg;
    TickType_t last_wake = xTaskGetTickCount();

    while (1) {
        if (!s_feeding_suspended) {
            if (all_registered_modules_alive()) {
                feed_pulse();
            } else {
                s_feeding_suspended = true;
                Serial.println("[watchdog] 看门狗喂狗已停止，等待硬件在约1.6s后强制复位");
            }
        }
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(WDT_FEED_INTERVAL_MS));
    }
}

void watchdog_feed_start()
{
    pinMode(WDT_FEED_GPIO, OUTPUT);
    digitalWrite(WDT_FEED_GPIO, LOW);

    // 喂狗任务优先级给高一点，但不要给最高（留给未来 router_bridge 的
    // 串口转发任务，需求文档要求它必须是最高优先级）。
    xTaskCreate(watchdog_task, "wdt_feed", 2048, NULL, configMAX_PRIORITIES - 2, NULL);
    Serial.printf("[watchdog] 看门狗喂狗任务已启动，周期 %dms\n", WDT_FEED_INTERVAL_MS);
}

void watchdog_heartbeat_register(watchdog_heartbeat_module_t module)
{
    if (module >= WDT_HB_MODULE_COUNT) return;
    s_last_heartbeat_ms[module] = millis();
    s_registered[module] = true;
    Serial.printf("[watchdog] 模块 %d 已注册进心跳监督\n", module);
}

void watchdog_heartbeat_report(watchdog_heartbeat_module_t module)
{
    if (module >= WDT_HB_MODULE_COUNT) return;
    s_last_heartbeat_ms[module] = millis();
}
