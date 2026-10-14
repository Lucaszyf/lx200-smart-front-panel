/**
 * config.h — LX200 智能面板 MVP0 全局引脚与常量定义（Arduino IDE 版本）
 *
 * 引脚编号来源：IO分配表.xlsx
 * 时序/协议来源：固件需求说明v4.docx、智能面板硬件需求说明_v8.docx
 */

#pragma once

/* ========================================================================
 * 1. 看门狗（STWD100XY）
 * ==================================================================== */
#define WDT_FEED_GPIO             7    // IO7：喂狗脉冲输出
#define WDT_FEED_INTERVAL_MS      200  // 喂狗周期：远小于硬件1.6s超时窗口
#define WDT_HEARTBEAT_TIMEOUT_MS  800  // 关键任务心跳超时：超过则停止喂狗，放任硬件复位

/* ========================================================================
 * 2. SW-UI-INPUT / SW-UI-MODE 开关状态读取
 * ==================================================================== */
// SW0（SW-UI-MODE）：0 = A模式，1 = B模式
#define SW0_MODE_GPIO             21

// SW1（SW-UI-INPUT）2bit编码，GPIO48=A0，GPIO47=A1：
//   A1=0,A0=0 -> SW1=0(RS232)；A1=0,A0=1 -> SW1=1(USB-C)；
//   A1=1,A0=0 -> SW1=2(WiFi/BT)；A1=1,A0=1 -> 非法编码
#define SW1_BIT_A0_GPIO           48
#define SW1_BIT_A1_GPIO           47

#define SWITCH_DEBOUNCE_MS        50   // 机械开关消抖时间

// 以下三个跟"手柄/主板指令仲裁"相关的定时器，数值故意跟上面的消抖时间
// 分开定义，避免混用（固件v4文档明确要求这几个语义不同的宏要独立定义）：
#define HANDSET_IDLE_TIMEOUT_MS   200  // 手柄完全空闲仲裁超时：宏序列(MVP6虚拟手柄)
                                        // 需要等到这个值才能开始注入，MVP4单条指令
                                        // 注入目前只用下面的HANDSET_FRAME_GAP_MS
#define HANDSET_FRAME_GAP_MS      20   // 简化版"帧间插针"窗口：MVP4单条指令注入用这个，
                                        // 见router_bridge.h说明
#define VIRTUAL_KEY_HOLD_MS       80   // 虚拟按键保持时长（MVP6虚拟手柄状态机会用到，先占位）
#define VIRTUAL_KEY_GAP_MS        60   // 虚拟按键间隔（同上，先占位）

/* ========================================================================
 * 3. OLED（SPI，SSD1306，128x64）
 * ==================================================================== */
#define OLED_PIN_CS               9
#define OLED_PIN_DC               10
#define OLED_PIN_RST              11
#define OLED_PIN_CLK              12
#define OLED_PIN_MOSI             13
#define OLED_SPI_CLOCK_HZ         1000000UL  // 1MHz，跑稳后可以往上调

#define OLED_WIDTH                128
#define OLED_HEIGHT               64
#define OLED_PAGES                (OLED_HEIGHT / 8)  // 8 pages

// 每行文字用2个page（16px高，字形从原7px点阵竖向2倍拉伸而来），
// 行与行之间跳过1个page(8px)做间距：
//   line0 -> page0,1   间距page2   line1 -> page3,4   间距page5   line2 -> page6,7
// 合计 2+1+2+1+2 = 8 page，正好用满64px屏幕高度。
#define OLED_LINE_HEIGHT_PAGES    2

/* ========================================================================
 * 4. INA3221 电源遥测（I2C）
 * ==================================================================== */
#define INA_I2C_SCL_GPIO          1
#define INA_I2C_SDA_GPIO          2
#define INA_I2C_FREQ_HZ           100000

#define INA_I2C_ADDR              0x40  // 地址引脚接GND

// 三通道顺序：CH1=18V，CH2=5V，CH3=3.3V
// 采样电阻均为 0.05Ω(50mΩ)，对应电流采样比例 1A = 0.05V
#define INA_CH1_LABEL             "18V"
#define INA_CH1_SHUNT_OHMS        0.05f

#define INA_CH2_LABEL             "5V"
#define INA_CH2_SHUNT_OHMS        0.05f

#define INA_CH3_LABEL             "3.3V"
#define INA_CH3_SHUNT_OHMS        0.05f

#define TELEMETRY_REFRESH_MS      500   // 2Hz 刷新率

/* ========================================================================
 * 5. 电平转换/输入选择器使能脚
 * ==================================================================== */
#define LEVEL_SHIFT_EN0_GPIO      14  // TTL电平转换通道0使能
#define LEVEL_SHIFT_EN1_GPIO      8   // TTL电平转换通道1使能
#define LEVEL_SHIFT_EN2_GPIO      6   // TTL电平转换通道2使能
#define TMUX1209_EN_GPIO          38  // SW1(IC-SW-INPUT/TMUX1209PWR)总使能

/* 注：根据原理图，最终电路里没有74LVC1G125三态缓冲器这颗芯片，
 * 需求文档里"三态缓冲使能双条件置位"这套逻辑没有对应硬件可控制，本设计
 * 不定义该使能脚。主板UART TX这条线的上电毛刺保护，改为依赖：
 * (a) ESP32 GPIO 复位后默认高阻态，应用代码初始化该UART之前不会主动驱动；
 * (b) 主板RX引脚侧按需求文档要求并联的4.7k~10k上拉电阻。
 * 这是比原设计更弱的保护，建议上电时用示波器实测主板RX引脚确认表现符合
 * 预期（结合看门狗，如果ESP32卡死重启，该UART的GPIO会重新回到高阻态）。 */

/* ========================================================================
 * 6. 三路UART，按角色命名
 *
 * ESP32-S3有3组硬件UART外设，代码里叫UART0/UART1/UART2，对应
 * HardwareSerial(0)/(1)/(2)。固件v4/硬件v8文档写的"UART0=手柄，
 * UART1=主板，UART2=输入选择器"这个角色映射，跟实际原理图对不上。
 * 原理图上的0/1/2后缀，是主板自己34针接口原有的通道编号(继承自原厂
 * 主板，跟ESP32的UART外设编号是两套不同的编号体系)。
 *
 * 以原理图实际走线为准，真实对应关系是：
 * UART0(TX=43 RX=44) = 输入选择器(RS232/USB-C/WiFi)
 * UART1(TX=17 RX=18) = 主板
 * UART2(TX=4  RX=5)  = 手柄
 *
 * 代码里用按角色命名的宏(INPUT_SELECTOR_*、MAINBOARD_*、HANDSET_*)，
 * 每组宏的注释里也标注对应的UART编号，方便对照原理图和IO分配表。
 * ==================================================================== */

// 输入选择器（RS232/USB-C/WiFi三选一，A2状态下承载WiFi<->RS232透传）
// = UART0（ESP32 UART外设0）
#define INPUT_SELECTOR_TX_GPIO    43
#define INPUT_SELECTOR_RX_GPIO    44
#define INPUT_SELECTOR_BAUD_RATE  9600

// 主板 = UART1（ESP32 UART外设1）
#define MAINBOARD_TX_GPIO         17
#define MAINBOARD_RX_GPIO         18
#define MAINBOARD_BAUD_RATE       9600

// 手柄 = UART2（ESP32 UART外设2）
#define HANDSET_TX_GPIO           4
#define HANDSET_RX_GPIO           5
#define HANDSET_BAUD_RATE         9600

/* 注：输入选择器用的GPIO43/44同时也是ESP32-S3启动ROM固定输出日志的引脚。
 * 这个影响比之前担心的"手柄用这两个脚"要小很多：A0/A1下ESP32电气上完全
 * 旁路(跟这两个脚上有没有ROM启动日志无关)；A2下要等WiFi客户端真正连上、
 * 那时早已开机完毕，不会撞上ROM刚启动那一小段日志输出窗口。仍建议实测
 * 确认A2首次连接时没有异常字节混入。 */

/* ========================================================================
 * 7. WiFi（A2状态下的透传通道，AP模式，STA模式留待后续NVS配置阶段加入）
 * ==================================================================== */
#define WIFI_AP_SSID              "LX200Panel"
#define WIFI_AP_PASSWORD          "lx200panel"  // 8位以上，WPA2最低要求
#define WIFI_TCP_PORT             4030  // SkySafari/AlpacaScope等LX200-over-WiFi客户端的事实标准端口
