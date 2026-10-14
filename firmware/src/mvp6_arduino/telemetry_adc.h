/**
 * telemetry_adc.h — INA3221 三路电压/电流采集（Arduino IDE 版本）
 * 需求文档要求：2Hz刷新率，显示时通过 oled_display 的API提交。
 */

#pragma once

void telemetry_adc_init();

/** 启动2Hz采集任务，并向看门狗上报心跳。 */
void telemetry_adc_start();
