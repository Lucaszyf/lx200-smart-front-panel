/**
 * virtual_handset.h — 虚拟手柄宏序列执行（需求9）
 *
 * 对应固件v4需求9"强力盲视复位协议"：每一组自动化指令注入序列开始前，
 * 强制盲发5次ESC/BACK键，每步预留80ms延迟，把主板强行退回已知的菜单
 * 基线，不依赖内存里的上一次状态做叠加推演。
 *
 * ⚠️ 需求9原文还描述了一个更完整的"虚拟手柄状态树"能力（通过分析UART0/
 * UART1双向流量预测手柄当前光标位置和菜单状态，为后续控制提供上下文
 * 感知）——这部分需要知道主板菜单系统的完整状态机（跟MVP5提到的"手柄
 * 显示协议"是同一类缺口，可能还需要更多），本阶段没有实现，只做了"盲发
 * 复位宏序列"这个具体、可独立验证的子集。
 *
 * ⚠️ "ESC/BACK"键在按键码文档里没有字面对应，本模块目前用MODE键代替，
 * 这个假设需要你确认（详见 handset_keycodes.h 里的说明），确认前建议只在
 * 测试环境验证这个功能，不要在实际连接主板时启用。
 */

#pragma once

#include <stdbool.h>

typedef enum {
    MACRO_STATE_IDLE = 0,
    MACRO_STATE_RUNNING,
    MACRO_STATE_COMPLETED,
    MACRO_STATE_INTERRUPTED,
} macro_state_t;

void virtual_handset_init();

/**
 * 执行"强力盲视复位"序列：5次ESC/BACK键按下+松开，每次按住
 * VIRTUAL_KEY_HOLD_MS(80ms)，间隔VIRTUAL_KEY_GAP_MS(60ms)。
 * 等待期间持续（约1ms粒度）检查手柄活动，一旦检测到立即中止，满足
 * "<5ms内让出控制权"的要求。
 *
 * @return true=完整执行完毕；false=被物理手柄活动中止，提前终止
 */
bool virtual_handset_blind_reset();

/** 查询上一次宏序列执行的最终状态（完成/被中断/从未执行过）。 */
macro_state_t virtual_handset_last_macro_state();
