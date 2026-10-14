/**
 * handset_keycodes.h — 手柄物理按键HEX码总表
 *
 * 来源：Meade_LX200_Classic_Hand_Controller_Key_Codes.pdf（更正版）
 * 格式：1起始位+8数据位(LSB在前)+1停止位，最高位(MSB)按下(Key Down)恒为0、
 * 松开(Key Up)恒为1。
 *
 * ⚠️ 已按更正版资料修复了最初版本里S/E/W三个键Key-Up码的3处笔误：
 *   S: 0xC3 -> 0xD3　E: 0xD5 -> 0xC5　W: 0xB7 -> 0xD7
 * （原W值0xB7正好和数字键7的Key-Up码撞车，就是之前怀疑的那个疑点，现已
 * 确认是笔误、不是巧合，修正后不再撞车）
 *
 * 手柄上SLEW/FIND/CNTR/GUIDE和数字7/4/1/0是同一个物理按键复用的（按下/
 * 松开的HEX码完全相同，主板根据当前菜单上下文决定解释成数字还是速率），
 * 下方额外提供了这几个的别名，方便调用方按功能名引用而不用死记数字。
 */

#pragma once

#include <stdint.h>

#define KEY_ENTER_DOWN 0x0D
#define KEY_ENTER_UP   0x8D
#define KEY_MODE_DOWN  0x4D
#define KEY_MODE_UP    0xCD
#define KEY_GOTO_DOWN  0x47
#define KEY_GOTO_UP    0xC7
#define KEY_N_DOWN     0x4E
#define KEY_N_UP       0xCE
#define KEY_S_DOWN     0x53
#define KEY_S_UP       0xD3  // 已更正，原0xC3是笔误
#define KEY_E_DOWN     0x45
#define KEY_E_UP       0xC5  // 已更正，原0xD5是笔误
#define KEY_W_DOWN     0x57
#define KEY_W_UP       0xD7  // 已更正，原0xB7是笔误（曾与KEY_7_UP撞车）
#define KEY_9_DOWN     0x39
#define KEY_9_UP       0xB9
#define KEY_8_DOWN     0x38
#define KEY_8_UP       0xB8
#define KEY_7_DOWN     0x37
#define KEY_7_UP       0xB7
#define KEY_6_DOWN     0x36
#define KEY_6_UP       0xB6
#define KEY_5_DOWN     0x35
#define KEY_5_UP       0xB5
#define KEY_4_DOWN     0x34
#define KEY_4_UP       0xB4
#define KEY_3_DOWN     0x33
#define KEY_3_UP       0xB3
#define KEY_2_DOWN     0x32
#define KEY_2_UP       0xB2
#define KEY_1_DOWN     0x31
#define KEY_1_UP       0xB1
#define KEY_0_DOWN     0x30
#define KEY_0_UP       0xB0
#define KEY_PREV_DOWN  0x2E
#define KEY_PREV_UP    0xAE
#define KEY_NEXT_DOWN  0x44
#define KEY_NEXT_UP    0xC4

// 速率/功能键别名：跟对应数字键共享同一个物理按键的HEX码
#define KEY_SLEW_DOWN  KEY_7_DOWN
#define KEY_SLEW_UP    KEY_7_UP
#define KEY_FIND_DOWN  KEY_4_DOWN
#define KEY_FIND_UP    KEY_4_UP
#define KEY_CNTR_DOWN  KEY_1_DOWN
#define KEY_CNTR_UP    KEY_1_UP
#define KEY_GUIDE_DOWN KEY_0_DOWN
#define KEY_GUIDE_UP   KEY_0_UP
#define KEY_RET_DOWN   KEY_8_DOWN
#define KEY_RET_UP     KEY_8_UP
#define KEY_M_DOWN     KEY_9_DOWN
#define KEY_M_UP       KEY_9_UP
#define KEY_FOCUS_DOWN KEY_5_DOWN
#define KEY_FOCUS_UP   KEY_5_UP
#define KEY_STAR_DOWN  KEY_6_DOWN
#define KEY_STAR_UP    KEY_6_UP
#define KEY_MAP_DOWN   KEY_2_DOWN
#define KEY_MAP_UP     KEY_2_UP
#define KEY_CNGC_DOWN  KEY_3_DOWN
#define KEY_CNGC_UP    KEY_3_UP

/* ============================================================================
 * ⚠️ 待确认：需求9里"强力盲视复位协议"要求盲发的"ESC/BACK"键，PDF列出的
 * 按键里没有一个字面上叫这个名字（PDF只有ENTER/MODE/GOTO/N/S/E/W/0-9/
 * PREV/NEXT）。
 *
 * 经典LX200手柄的常见设计里，MODE键经常兼任"退出当前菜单/返回上一级"的
 * 功能，这里先假设MODE键就是需求文档说的ESC/BACK，用作强制复位序列。
 *
 * 这个假设有实际风险：强制复位会真的连续5次把这个键发给主板，如果假设
 * 错了，可能让主板进入不期望的菜单状态（跟之前"字库缺字符"那种纯显示
 * 层面的假设不一样，这个会真的操作主板）。用MVP6功能前，请务必确认这个
 * 假设是否正确，或者告诉我实际的ESC/BACK键对应哪个物理按键。
 * ========================================================================= */
#define KEY_ESC_BACK_DOWN  KEY_MODE_DOWN
#define KEY_ESC_BACK_UP    KEY_MODE_UP
