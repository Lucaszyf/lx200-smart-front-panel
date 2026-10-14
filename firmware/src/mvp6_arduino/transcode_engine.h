/**
 * transcode_engine.h — ASCII(LX200指令) <-> HEX(手柄按键码) 转码引擎
 *
 * 对应固件v4"需求6"。
 *
 * ⚠️ 架构澄清：这是两个完全独立、Meade官方从未互相翻译过的协议之间的转换：
 *   - 左侧：LX200 RS-232 ASCII指令集（PC/主机 <-> 主板，公开协议，来自
 *     LX200 Classic用户手册Appendix F）
 *   - 右侧：手柄Handbox二进制按键协议（原厂手柄 <-> 主板，来自
 *     Meade_LX200_Classic_Hand_Controller_Key_Codes.pdf）
 * 在原厂设计里，这两条协议各自独立、各接主板的不同物理端口（RS-232口 /
 * KP手柄口），主板原生同时认得两种协议，从不需要互相转换。本项目之所以
 * 需要在这里做转换，是因为咱们的硬件设计里，ASCII指令来源（WiFi/RS232/
 * USB-C输入选择器）经过SW1开关矩阵后，物理上接的是主板的KP手柄口而不是
 * RS-232口——所以需要ESP32自己把ASCII指令"翻译"成手柄本该发送的按键码，
 * 这个翻译表是我们自己设计的，不是抄录自某份Meade官方对照表（官方从未
 * 发布过这种对照表）。
 *
 * 移动/停止指令（对应N/S/E/W方向键的按下/松开）已经填入真实数据；速率
 * 指令（:RG#/:RC#/:RM#/:RS#）也已填入——手柄上SLEW/FIND/CNTR/GUIDE和
 * 数字7/4/1/0是同一个物理按键复用的，共享HEX码。
 *
 * ⚠️ S/E/W三个键的Key-Up码，最初拿到的PDF版本有笔误，已按更正版资料改正
 * （详见 transcode_engine.cpp 顶部注释）。
 *
 * 静态数组O(1)查询：不使用循环遍历字符串比较，用switch-case按指令的
 * 关键字符直接跳转（编译器对连续/密集case会生成跳转表，是真正的O(1)，
 * 不是伪装成O(1)的线性扫描）。
 */

#pragma once

#include <stdint.h>
#include <stddef.h>

#define TRANSCODE_MAX_HEX_LEN 8   // 单条指令最多映射几个HEX字节（:Q#对应4个方向键松开码，用到4）

/**
 * 把一条完整的LX200 ASCII指令（形如":Mn#"，已经去掉首尾的调用方无需处理）
 * 转换成对应的手柄HEX按键码序列。
 *
 * @param ascii_cmd     完整指令字符串，包含开头':'和结尾'#'
 * @param hex_out       输出缓冲区，调用方提供
 * @param hex_out_cap   输出缓冲区容量
 * @param hex_len_out   实际写入的字节数
 * @return true=指令被识别且已有HEX映射数据；false=指令未识别，两种情况都
 *         不应该往主板发送任何数据，调用方应记录日志但不注入。
 */
bool transcode_ascii_to_hex(const char *ascii_cmd, uint8_t *hex_out,
                            size_t hex_out_cap, size_t *hex_len_out);
