/**
 * mvp6_arduino.ino — LX200 智能面板固件入口（Arduino IDE 版本）
 *
 * 已实现范围：
 *   1. 开机喂狗（STWD100XY，需求12）
 *   2. 六态识别与显示（A0/A1/A2/B0/B1/B2）
 *   3. OLED点亮（SPI，SSD1306 128x64；初始化失败不影响其他模块运行）
 *   4. 三路电压/电流遥测显示（18V/5V/3.3V，2Hz刷新）
 *   5. A0/A1状态下自动关闭三路电平转换通道使能，确保ESP32真正电气隔离
 *   6. A2状态下 WiFi(TCP,AP模式) <-> 输入选择器UART 双向原始字节透传（需求4）
 *      TCP端口4030（SkySafari等LX200-over-WiFi客户端的事实标准端口）
 *   7. B0/B1/B2状态下 手柄 <-> 主板 双向原始字节直通（需求5）；74LVC1G125
 *      三态缓冲器在最终原理图里不存在，上电毛刺保护改为依赖GPIO默认高阻态
 *      +主板侧上拉电阻
 *   8. B0/B1/B2状态下 LX200 ASCII指令解析 + 转码 + 仲裁注入主板（需求6）
 *      移动/停止指令已填入真实手柄按键HEX码，可以实际注入；⚠️ 速率指令
 *      （:RG#等）手柄按键码文档未覆盖，仍是占位符，详见transcode_engine.cpp
 *   9. 主板下行数据异步镜像队列（需求7）：不阻塞转发主路径。屏幕6行布局：
 *      状态栏（日期时间+WiFi/蓝牙图标）、V、I、模式+方向合并行（超宽从屏幕
 *      水平中点开始滚动）、手柄镜像两行（大字体，目前是原始数据hex/可打印
 *      字符预览，不是按手柄真实显示协议解析的内容——那份协议文档还没有）
 *  10. 时间同步：从WiFi/RS232/USB-C这条ASCII协议路径截获标准LX200指令
 *      :SL#(设置时间)/:SC#(设置日期)，同步面板自己的显示时钟（无RTC硬件，
 *      断电即丢）。不转发/转码给主板；手柄上直接设置时间的操作无法截获。
 *  11. 虚拟手柄盲视复位（需求9，MVP6新增）：首次进入B0/B1/B2状态时，自动
 *      盲发5次ESC/BACK键（每次间隔80ms），把主板拉回已知菜单基线。等待期间
 *      以1ms粒度检查手柄活动，检测到立即中止。⚠️ "ESC/BACK"键在按键码
 *      文档里没有字面对应，目前用MODE键代替，这个假设需要确认（详见
 *      handset_keycodes.h），确认前建议只在测试环境验证、不要接实际主板。
 *      需求9里更完整的"虚拟手柄状态树"（预测手柄光标/菜单状态）本阶段
 *      未实现，需要主板菜单协议文档，这个缺口跟镜像显示是同一类问题。
 *
 * ⚠️ 字库修复：之前"Mode"这个词显示成"M     :"，字库里当时没收录小写
 * o/d/e，查不到就按空格处理。这次已经把这几个字符（以及备用的大写D）
 * 补进 oled_font.h。
 *
 * ⚠️ ESP32三路UART外设的角色分配，按实际原理图走线而非固件v4/硬件v8文档
 * 的文字描述——两者不一致，以原理图为准（详见config.h开头的说明）：
 *   外设0(GPIO43/44)=输入选择器　外设1(GPIO17/18)=主板　外设2(GPIO4/5)=手柄
 *
 * 明确不包含：需求8（下行数据回传外部通道，当前指令集不需要响应，暂不
 * 实现）、完整虚拟手柄状态树（见上方11条说明）、BLE/Web远程注入、NVS配置、
 * WiFi STA模式、蓝牙经典SPP透传（不需要，详见README）。
 *
 * 开发板选择：Arduino IDE -> 工具 -> 开发板 -> ESP32S3 Dev Module
 * （对应 ESP32-S3-WROOM-1-N16R8：Flash Size 选 16MB，PSRAM 选 OPI PSRAM，
 * Flash Mode 选 QIO 80MHz）
 *
 * ⚠️ 从这个版本起，"USB CDC On Boot" 必须设为 Enabled：输入选择器和主板/
 * 手柄现在占用了真正的硬件UART外设，如果这个选项是Disabled，调试用的
 * Serial会跟输入选择器所用的外设0冲突。改成Enabled后，调试串口走的是
 * Native USB，需要在端口列表里找那个口来看日志，不再是烧录用的CH340口。
 *
 * 调试提示：本文件及各模块在初始化阶段都打印了详细的分步日志（串口波特率
 * 115200）。如果串口完全没有任何输出，最常见的原因是端口选错了（见上一条）。
 */

#include "config.h"
#include "watchdog_feed.h"
#include "switch_monitor.h"
#include "oled_display.h"
#include "telemetry_adc.h"
#include "router_bridge.h"
#include "time_sync.h"
#include "virtual_handset.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h" // esp_reset_reason()

// TMUX1209总使能：开机默认拉高，常开。EN0/EN1/EN2三路电平转换通道使能已经
// 移交 switch_monitor.c 管理（需要跟随六态识别结果联动开关，见 apply_power_rail_policy()）。
static void power_rails_default_init()
{
    Serial.println("[main] power_rails_default_init() 开始...");
    pinMode(TMUX1209_EN_GPIO, OUTPUT);
    digitalWrite(TMUX1209_EN_GPIO, HIGH);
    Serial.printf("[main] TMUX1209总使能(IO%d)已置高（默认开启状态）\n", TMUX1209_EN_GPIO);
}

static void print_boot_banner()
{
    Serial.println();
    Serial.println("========================================");
    Serial.println("  LX200 Smart Front Panel Firmware");
    Serial.println("========================================");
    Serial.printf("Chip model     : %s\n", ESP.getChipModel());
    Serial.printf("Chip revision  : %d\n", ESP.getChipRevision());
    Serial.printf("CPU frequency  : %d MHz\n", ESP.getCpuFreqMHz());
    Serial.printf("Flash size     : %d bytes\n", ESP.getFlashChipSize());
    Serial.printf("PSRAM size     : %d bytes\n", ESP.getPsramSize());
    Serial.printf("Free heap      : %d bytes\n", ESP.getFreeHeap());
    Serial.printf("Reset reason   : %d\n", (int)esp_reset_reason());
    Serial.println("----------------------------------------");
    Serial.println("如果看到这条横幅，说明串口连接正常、Serial输出没问题。");
    Serial.println("如果之前一直看不到任何输出，很可能是端口选错了（见本文");
    Serial.println("件顶部注释），不是代码没在跑。");
    Serial.println("========================================");
    Serial.println();
}

void setup()
{
    Serial.begin(115200);
    delay(500); // 给串口监视器/USB枚举多留一点时间，调试阶段宁可多等，不要错过早期日志

    print_boot_banner();

    // --------------------------------------------------------------
    // 第一步，且只做这一步：起喂狗任务。不等待任何其他外设初始化。
    // --------------------------------------------------------------
    Serial.println("[main] 步骤1/7: 启动看门狗喂狗任务...");
    watchdog_feed_start();
    Serial.println("[main] 步骤1/7: 完成");

    // --------------------------------------------------------------
    // 外设初始化，每一步都打印开始/完成，方便定位卡在哪一步
    // --------------------------------------------------------------
    Serial.println("[main] 步骤2/7: 初始化OLED...");
    oled_init();
    Serial.printf("[main] 步骤2/7: 完成 (oled_is_ready=%d)\n", oled_is_ready());
    oled_display_start();
    if (oled_is_ready()) {
        oled_show_boot_message("SYSTEM BOOTING");
    } else {
        Serial.println("[main] 警告: OLED初始化失败，跳过显示，但不影响后续模块");
    }

    Serial.println("[main] 步骤3/7: 初始化开关检测(switch_monitor)...");
    switch_monitor_init();
    Serial.println("[main] 步骤3/7: 完成");

    Serial.println("[main] 步骤4/7: 初始化电源遥测(telemetry_adc / INA3221)...");
    telemetry_adc_init();
    Serial.println("[main] 步骤4/7: 完成");

    Serial.println("[main] 步骤5/7: 初始化TMUX1209总使能...");
    power_rails_default_init();
    Serial.println("[main] 步骤5/7: 完成");

    Serial.println("[main] 步骤6/7: 初始化WiFi/路由桥接(router_bridge)...");
    router_bridge_init();
    virtual_handset_init(); // 依赖router_bridge导出的注入原语，须在其之后初始化
    Serial.println("[main] 步骤6/7: 完成");

    // --------------------------------------------------------------
    // 启动各功能任务
    // --------------------------------------------------------------
    Serial.println("[main] 步骤7/7: 启动各功能任务...");
    switch_monitor_start();
    telemetry_adc_start();
    router_bridge_start();
    Serial.println("[main] 步骤7/7: 完成");

    Serial.println();
    Serial.println("[main] ==== 初始化全部完成，进入正常运行 ====");
    Serial.println();
}

void loop()
{
    // 所有实际工作都在 FreeRTOS 任务里（看门狗/开关监控/遥测/路由桥接），
    // Arduino 的 loop() 本身跑在一个低优先级任务里。这里加一条低频心跳日志，
    // 方便确认系统仍在正常运行、没有卡死或者陷入某种静默重启循环。同时顺便
    // 刷新一下状态栏的日期时间显示（没必要单独起任务，5秒刷新一次足够）。
    static uint32_t s_loop_count = 0;
    s_loop_count++;

    char datetime_str[24];
    time_sync_get_display_string(datetime_str, sizeof(datetime_str));
    oled_set_datetime(datetime_str);
    Serial.printf("[main] loop心跳 #%lu (运行时长 %lus, 剩余堆内存 %u bytes)\n",
                  (unsigned long)s_loop_count, (unsigned long)(millis() / 1000), ESP.getFreeHeap());
    vTaskDelay(pdMS_TO_TICKS(5000));
}
