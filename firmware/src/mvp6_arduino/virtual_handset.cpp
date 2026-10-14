#include "virtual_handset.h"
#include "handset_keycodes.h"
#include "router_bridge.h"
#include "config.h"

#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static macro_state_t s_last_state = MACRO_STATE_IDLE;

void virtual_handset_init()
{
    Serial.println("[virtual_handset] virtual_handset_init() 完成");
}

// 等待指定毫秒数，期间以约1ms粒度持续检查手柄是否变为活动，一旦检测到
// 立即返回false（被中止），正常等完返回true。这个检查粒度是满足需求9
// "<5ms内让出控制权"的关键——不能简单地一次性vTaskDelay(80ms)再检查，
// 那样最坏情况下手柄要等将近80ms才能拿回控制权。
static bool wait_with_interrupt_check(uint32_t wait_ms)
{
    for (uint32_t elapsed = 0; elapsed < wait_ms; elapsed++) {
        if (!router_bridge_handset_idle()) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    return true;
}

bool virtual_handset_blind_reset()
{
    Serial.println("[virtual_handset] 开始执行强力盲视复位序列 (5x ESC/BACK)...");
    s_last_state = MACRO_STATE_RUNNING;

    for (int i = 1; i <= 5; i++) {
        if (!router_bridge_try_inject_byte(KEY_ESC_BACK_DOWN)) {
            Serial.printf("[virtual_handset] 第%d/5次按下时检测到手柄活动，中止复位序列\n", i);
            s_last_state = MACRO_STATE_INTERRUPTED;
            return false;
        }
        if (!wait_with_interrupt_check(VIRTUAL_KEY_HOLD_MS)) {
            Serial.printf("[virtual_handset] 第%d/5次按住期间检测到手柄活动，中止复位序列\n", i);
            s_last_state = MACRO_STATE_INTERRUPTED;
            return false;
        }

        if (!router_bridge_try_inject_byte(KEY_ESC_BACK_UP)) {
            Serial.printf("[virtual_handset] 第%d/5次松开时检测到手柄活动，中止复位序列\n", i);
            s_last_state = MACRO_STATE_INTERRUPTED;
            return false;
        }
        if (!wait_with_interrupt_check(VIRTUAL_KEY_GAP_MS)) {
            Serial.printf("[virtual_handset] 第%d/5次间隔期间检测到手柄活动，中止复位序列\n", i);
            s_last_state = MACRO_STATE_INTERRUPTED;
            return false;
        }

        Serial.printf("[virtual_handset] 第%d/5次盲发完成\n", i);
    }

    Serial.println("[virtual_handset] 强力盲视复位序列执行完毕，主板应已退回菜单基线");
    s_last_state = MACRO_STATE_COMPLETED;
    return true;
}

macro_state_t virtual_handset_last_macro_state()
{
    return s_last_state;
}
