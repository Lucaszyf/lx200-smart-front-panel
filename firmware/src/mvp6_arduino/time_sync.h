/**
 * time_sync.h — 从LX200 ASCII指令截获时间/日期，同步面板自己的显示时钟
 *
 * 只处理来自WiFi/RS232/USB-C(输入选择器/TCP)这条ASCII协议路径的标准LX200
 * 指令：
 *   :SLHH:MM:SS#   设置本地时间
 *   :SCMM/DD/YY#   设置日期
 * 这两条是公开的LX200标准协议指令。截获后只用来同步ESP32自己的软件时钟
 * （没有RTC硬件，靠settimeofday()维护，掉电会丢），不会把这两条指令转发/
 * 转码给主板——转发需要把"设置时间"翻译成主板菜单导航的按键序列，这是
 * 主板自己的菜单状态机逻辑，没有对应协议文档，做不了。
 *
 * 手柄上直接设置时间的操作（按MODE+数字键导航菜单）目前无法截获同步——
 * 那需要知道主板菜单系统的完整状态机（按MODE进入什么菜单、下一步该按什么
 * 键代表哪个数值），这是比按键码本身更复杂的第三份协议知识，现在没有。
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

/**
 * 尝试从一条完整的ASCII指令里识别并处理时间/日期设置指令。
 * @return true=这条指令是:SL#或:SC#、已经处理（不管解析是否成功都返回true，
 *         表示"这条指令归时间同步模块管"，调用方不应该再当成别的指令处理）；
 *         false=不是时间/日期指令，调用方应该按正常流程继续处理。
 */
bool time_sync_try_handle_command(const char *ascii_cmd);

/**
 * 获取当前格式化的日期时间字符串，形如"2026-08-20 14:35"。
 * 如果还没有同步过（开机默认状态），返回"SYNC PENDING"这样的占位提示。
 */
void time_sync_get_display_string(char *out, size_t out_cap);

/** 是否已经至少同步过一次时间。 */
bool time_sync_is_synced();
