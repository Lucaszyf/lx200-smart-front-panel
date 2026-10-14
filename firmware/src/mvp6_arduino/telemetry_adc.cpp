#include "telemetry_adc.h"
#include "config.h"
#include "watchdog_feed.h"
#include "oled_display.h"

#include <Arduino.h>
#include <Wire.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// INA3221 寄存器地址（TI公开datasheet寄存器表，与厂家/批次无关）
#define REG_CONFIG      0x00
#define REG_CH1_SHUNT   0x01
#define REG_CH1_BUS     0x02
#define REG_CH2_SHUNT   0x03
#define REG_CH2_BUS     0x04
#define REG_CH3_SHUNT   0x05
#define REG_CH3_BUS     0x06

// 0x7127: reset=0, 平均1次, 默认转换时间, CH1/CH2/CH3全使能, 连续采集分流+总线电压
#define CONFIG_VALUE    0x7127

typedef struct {
    const char *label;
    float shunt_ohms;
    uint8_t shunt_reg;
    uint8_t bus_reg;
} ina_channel_t;

static const ina_channel_t k_channels[3] = {
    { INA_CH1_LABEL, INA_CH1_SHUNT_OHMS, REG_CH1_SHUNT, REG_CH1_BUS },
    { INA_CH2_LABEL, INA_CH2_SHUNT_OHMS, REG_CH2_SHUNT, REG_CH2_BUS },
    { INA_CH3_LABEL, INA_CH3_SHUNT_OHMS, REG_CH3_SHUNT, REG_CH3_BUS },
};

static bool ina_write_reg16(uint8_t reg, uint16_t value)
{
    Wire.beginTransmission(INA_I2C_ADDR);
    Wire.write(reg);
    Wire.write((uint8_t)(value >> 8));
    Wire.write((uint8_t)(value & 0xFF));
    return Wire.endTransmission() == 0;
}

static bool ina_read_reg16(uint8_t reg, int16_t *out)
{
    Wire.beginTransmission(INA_I2C_ADDR);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) { // false = 保持总线(repeated start)，不是普通stop
        return false;
    }
    uint8_t got = Wire.requestFrom((int)INA_I2C_ADDR, 2, true);
    if (got != 2) {
        return false;
    }
    uint8_t hi = Wire.read();
    uint8_t lo = Wire.read();
    *out = (int16_t)((hi << 8) | lo);
    return true;
}

void telemetry_adc_init()
{
    Wire.begin(INA_I2C_SDA_GPIO, INA_I2C_SCL_GPIO, INA_I2C_FREQ_HZ);

    if (!ina_write_reg16(REG_CONFIG, CONFIG_VALUE)) {
        Serial.printf("[telemetry] INA3221配置寄存器写入失败，请检查I2C地址(0x%02X)/接线\n",
                      INA_I2C_ADDR);
    } else {
        Serial.printf("[telemetry] INA3221 初始化完成 (I2C addr=0x%02X)\n", INA_I2C_ADDR);
    }
}

static void telemetry_task(void *arg)
{
    (void)arg;
    watchdog_heartbeat_register(WDT_HB_TELEMETRY);

    char v_str[3][12];
    char i_str[3][12];

    // 串口打印单独节流，跟主循环心跳的频率(约5秒)对齐，避免刷屏；
    // 实际I2C读取和OLED刷新仍然按TELEMETRY_REFRESH_MS(2Hz)正常进行，
    // 只是不再每一轮都往串口打印。
    #define TELEMETRY_LOG_INTERVAL_MS 5000
    uint32_t last_log_ms = 0;

    while (1) {
        float voltage_v[3] = {0};
        float current_ma[3] = {0};
        bool channel_ok[3] = {false, false, false};
        bool any_read_ok = false;

        for (int i = 0; i < 3; i++) {
            int16_t bus_raw = 0, shunt_raw = 0;
            bool ok1 = ina_read_reg16(k_channels[i].bus_reg, &bus_raw);
            bool ok2 = ina_read_reg16(k_channels[i].shunt_reg, &shunt_raw);

            if (ok1 && ok2) {
                channel_ok[i] = true;
                any_read_ok = true;
                // 总线电压：13位有效值(bit15-3)，LSB = 8mV
                int32_t bus_lsb = bus_raw >> 3;
                voltage_v[i] = (bus_lsb * 8) / 1000.0f;

                // 分流电压：13位有效值(bit15-3)，LSB = 40uV
                int32_t shunt_lsb = shunt_raw >> 3;
                float shunt_uv = shunt_lsb * 40.0f;
                current_ma[i] = shunt_uv / (k_channels[i].shunt_ohms * 1000.0f);
            }
        }

        if (any_read_ok) {
            // 通道1(18V)保留小数点后1位，其余两路保留2位
            snprintf(v_str[0], sizeof(v_str[0]), "%.1fV", voltage_v[0]);
            snprintf(v_str[1], sizeof(v_str[1]), "%.2fV", voltage_v[1]);
            snprintf(v_str[2], sizeof(v_str[2]), "%.2fV", voltage_v[2]);
            for (int i = 0; i < 3; i++) {
                snprintf(i_str[i], sizeof(i_str[i]), "%dmA", (int)(current_ma[i] + 0.5f));
            }
            oled_set_telemetry(v_str[0], v_str[1], v_str[2], i_str[0], i_str[1], i_str[2]);
        }

        // 串口日志节流：每约5秒才打印一次，逐通道各自报告成功/失败，
        // 不把好的和坏的混在同一行——哪个通道失败就单独打印哪个通道，
        // 避免看不出具体是哪一路出问题。
        uint32_t now_ms = millis();
        if (now_ms - last_log_ms >= TELEMETRY_LOG_INTERVAL_MS) {
            last_log_ms = now_ms;
            for (int i = 0; i < 3; i++) {
                if (channel_ok[i]) {
                    Serial.printf("[telemetry] %s: %s %s\n", k_channels[i].label, v_str[i], i_str[i]);
                } else {
                    Serial.printf("[telemetry] %s: 读取失败\n", k_channels[i].label);
                }
            }
        }

        watchdog_heartbeat_report(WDT_HB_TELEMETRY);
        vTaskDelay(pdMS_TO_TICKS(TELEMETRY_REFRESH_MS));
    }
}

void telemetry_adc_start()
{
    xTaskCreate(telemetry_task, "telemetry", 3072, NULL, 5, NULL);
}
