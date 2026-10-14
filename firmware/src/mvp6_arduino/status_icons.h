/**
 * status_icons.h — 状态栏图标（8x8单色点阵，自己设计，非拷贝现成图标库）
 *
 * 编码格式：每个图标8个字节，对应8列（从左到右）；每个字节 bit0=该列最
 * 上面一个像素，bit7=该列最下面一个像素。
 *
 * ⚠️ 这几个图标是按WiFi/蓝牙通用视觉特征手工设计的最简近似，没有实机
 * 视觉验证，烧录后建议检查是否清晰可辨、AP和Station两个图标是否能区分
 * 开，不满意可以直接改这里的字节数组。
 */

#pragma once

#include <stdint.h>

#define ICON_WIDTH 8
#define ICON_HEIGHT 8

// WiFi AP：3层信号弧+顶点，经典"广播中"造型
static const uint8_t k_icon_wifi_ap[8] = {0x02, 0x0A, 0x2A, 0xAA, 0xAA, 0x2A, 0x0A, 0x02};

// WiFi Station：信号弧 + 底部实心方块代表"接入设备"，跟AP区分
static const uint8_t k_icon_wifi_sta[8] = {0x00, 0xC4, 0xD4, 0xD4, 0xD4, 0xD4, 0xC4, 0x00};

// 蓝牙：经典蓝牙符文的简化像素近似
static const uint8_t k_icon_bt[8] = {0x00, 0x24, 0x18, 0xFF, 0x5A, 0x24, 0x00, 0x00};
