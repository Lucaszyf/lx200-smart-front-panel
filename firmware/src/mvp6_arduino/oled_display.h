/**
 * oled_display.h — OLED（SPI，SSD1306 128x64，黄/蓝双色屏）唯一持有者（Arduino IDE 版本）
 *
 * 屏幕布局6行，用满64px高度：
 *   line STATUS (page0，8px)  ：左侧日期时间"yyyy-mm-dd hh:mm"，右侧WiFi AP/
 *                                WiFi Station/蓝牙三个图标（仅对应功能开启时
 *                                才显示图标，不显示文字），小字体
 *   line V (page1，8px)       ：三路电压，三列对齐，小字体
 *   line I (page2，8px)       ：三路电流，紧贴V行下方，小字体
 *   line MODE_DESC (page3，8px)：模式代码+通道描述合并一行，格式"A0  RS232
 *                                <--> RS232"（代码和描述间2个空格），小字体，
 *                                超宽时从屏幕水平中点开始向左滚动（不是常规
 *                                从最左边露出）
 *   line MIRROR1 (page4-5，16px)：手柄显示第1行镜像，大字体(10x14)，超宽
 *                                自动向左滚动
 *   line MIRROR2 (page6-7，16px)：手柄显示第2行镜像，大字体，超宽自动滚动
 *
 * 容错设计：如果SPI总线或屏幕初始化失败，oled_init()只会打印错误日志并将
 * 内部状态标记为"未就绪"，不会让整机崩溃重启；后续所有对外API在未就绪时
 * 静默直接返回，不影响其他模块正常运行。
 */

#pragma once

#include <stdbool.h>

void oled_init();

/** 启动OLED相关的后台任务（MODE_DESC/MIRROR1/MIRROR2三行的滚动动画），需在 oled_init() 之后调用。 */
void oled_display_start();

/** 开机/复位过程中显示的提示信息，显示在MODE_DESC行位置，其余行清空。 */
void oled_show_boot_message(const char *msg);

/**
 * 更新电压/电流表格。六个字符串各自独立，v[i]与i[i]是同一路电轨，垂直
 * 对齐——同一路的电压和电流从同一个x坐标开始显示。
 */
void oled_set_telemetry(const char *v0, const char *v1, const char *v2,
                         const char *i0, const char *i1, const char *i2);

/**
 * 更新模式+描述合并行，调用方自己拼好完整字符串（代码+2空格+描述），
 * 例如"A0  RS232 <--> RS232"。超宽时从屏幕水平中点开始向左滚动。
 */
void oled_set_mode_line(const char *text);

/** 更新手柄显示镜像第1行内容，大字体，超宽自动向左滚动。 */
void oled_set_mirror_line1(const char *text);

/** 更新手柄显示镜像第2行内容，大字体，超宽自动向左滚动。 */
void oled_set_mirror_line2(const char *text);

/** 更新状态栏左侧的日期时间文本（调用方负责格式化，例如"2026-08-20 14:35"）。 */
void oled_set_datetime(const char *text);

/** 更新状态栏右侧三个图标的显示状态：true=显示图标，false=不显示（留空）。 */
void oled_set_status_icons(bool wifi_ap_active, bool wifi_sta_active, bool bt_active);

/** OLED是否初始化成功。调试/日志场景可用来判断是否需要提示"屏幕不可用"。 */
bool oled_is_ready();
