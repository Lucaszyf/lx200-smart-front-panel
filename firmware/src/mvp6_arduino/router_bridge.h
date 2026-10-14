/**
 * router_bridge.h — 输入选择器 / 主板 / 手柄 三路UART路由与转发
 *
 * ESP32三路硬件UART外设按实际原理图走线分配角色（详见config.h说明）：
 * 外设0=输入选择器(RS232/USB-C/WiFi)，外设1=主板，外设2=手柄。
 *
 * MVP2 范围：A2 状态下，WiFi(TCP) <-> 输入选择器 双向原始字节透传，不做协议
 * 解析。对应固件v4"需求4"。
 *
 * MVP3 范围：B0/B1/B2 状态下，手柄 <-> 主板 双向原始字节直通，不做转码。
 * 对应固件v4"需求5"（KP通道最高优先级零延迟转发）。独立任务、最高优先级。
 *
 * MVP4 范围：B0/B1 状态下解析输入选择器传来的LX200 ASCII指令，B2状态下
 * 解析TCP客户端传来的指令，统一调用 transcode_engine 转码成HEX，仲裁后
 * 注入主板。
 *
 * MVP5 新增（需求7）：主板->手柄的下行数据，在不阻塞转发主路径的前提下，
 * 异步镜像一份到队列，接入OLED的MIRROR1/MIRROR2两行显示（目前是原始数据
 * 预览，不是按手柄显示协议解析的内容，详见 router_bridge.cpp 里的说明）。
 * 需求8（下行数据回传给发起指令的外部通道）：当前覆盖的移动/停止/速率
 * 指令在真实LX200协议里本来就不期望有响应，本阶段不实现回传。
 *
 * MVP6 新增（需求9）：首次进入B0/B1/B2状态时，自动触发一次"强力盲视复位"
 * （虚拟盲发ESC/BACK键5次，每次间隔80ms），确保主板退回到已知的菜单基线，
 * 再开始处理转码指令。具体宏序列实现在 virtual_handset.cpp。
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

void router_bridge_init();

/** 启动WiFi AP + TCP服务 + 输入选择器桥接任务、手柄<->主板桥接任务、
 *  指令转码注入任务、以及下行镜像任务，并向看门狗上报心跳。 */
void router_bridge_start();

/**
 * 尝试注入单个字节到主板，遵循手柄仲裁规则（最近HANDSET_FRAME_GAP_MS内
 * 手柄有活动就失败）。成功返回true，因手柄活动被拒绝返回false。
 * 供 virtual_handset.cpp 发送虚拟按键序列时复用同一套仲裁逻辑。
 */
bool router_bridge_try_inject_byte(uint8_t b);

/** 查询手柄当前是否空闲（最近HANDSET_FRAME_GAP_MS内没有活动）。 */
bool router_bridge_handset_idle();
