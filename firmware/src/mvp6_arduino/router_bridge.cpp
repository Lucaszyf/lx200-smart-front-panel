#include "router_bridge.h"
#include "config.h"
#include "watchdog_feed.h"
#include "switch_monitor.h"
#include "transcode_engine.h"
#include "time_sync.h"
#include "oled_display.h"
#include "virtual_handset.h"

#include <Arduino.h>
#include <WiFi.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

static WiFiServer s_tcp_server(WIFI_TCP_PORT);
static WiFiClient s_client;

// 三路UART按角色命名，避免编号和角色对应错（详见config.h的说明）；
// 括号里同时标注UART编号，方便跟原理图/IO分配表交叉核对。
static HardwareSerial s_uart_input_selector(0); // 输入选择器 = UART0：RS232/USB-C/WiFi，A2下承载WiFi透传
static HardwareSerial s_uart_mainboard(1);      // 主板 = UART1
static HardwareSerial s_uart_handset(2);        // 手柄 = UART2

// ⚠️ 重要：这里用到了硬件UART外设0(s_uart_input_selector)。如果"USB CDC On
// Boot"设成了Disabled，Arduino的全局Serial对象也会占用同一个外设0(通过
// GPIO43/44)，会跟这里冲突，串口调试日志会被搞乱、输入选择器通信也会不
// 正常。使用本文件这部分功能前，必须把"USB CDC On Boot"改回Enabled，让
// Serial走Native USB，把外设0真正让给业务使用——这也是固件需求文档原本
// 的设计意图("将3组原生硬件串口全部释放给业务矩阵")。

// 主板UART写入互斥保护 + 手柄活动时间戳（供仲裁使用）
static SemaphoreHandle_t s_mainboard_mutex;
static volatile uint32_t s_handset_last_activity_ms = 0;

// 下行镜像队列（MVP5新增）：容量256字节，装不下就丢弃最旧的不管
// （xQueueSend用0超时，满了直接放弃这次send，不阻塞、不覆盖）
#define MIRROR_QUEUE_LEN 256
static QueueHandle_t s_mirror_queue;

void router_bridge_init()
{
    Serial.println("[router] router_bridge_init() 开始...");

    s_uart_input_selector.begin(INPUT_SELECTOR_BAUD_RATE, SERIAL_8N1,
                                 INPUT_SELECTOR_RX_GPIO, INPUT_SELECTOR_TX_GPIO);
    s_uart_mainboard.begin(MAINBOARD_BAUD_RATE, SERIAL_8N1,
                            MAINBOARD_RX_GPIO, MAINBOARD_TX_GPIO);
    s_uart_handset.begin(HANDSET_BAUD_RATE, SERIAL_8N1,
                          HANDSET_RX_GPIO, HANDSET_TX_GPIO);

    s_mainboard_mutex = xSemaphoreCreateMutex();
    s_mirror_queue = xQueueCreate(MIRROR_QUEUE_LEN, sizeof(uint8_t));

    WiFi.mode(WIFI_AP);
    WiFi.softAP(WIFI_AP_SSID, WIFI_AP_PASSWORD);
    IPAddress ip = WiFi.softAPIP();

    s_tcp_server.begin();
    s_tcp_server.setNoDelay(true); // 关闭Nagle算法，降低控制指令的往返延迟

    // WiFi AP已经起来了，点亮状态栏对应图标；Station和蓝牙目前都还没实现，
    // 保持关闭，等对应功能真正做出来了再点亮（不要让图标点亮暗示了根本不
    // 存在的能力）。
    oled_set_status_icons(true, false, false);

    Serial.printf("[router] AP已启动: SSID=%s IP=%s TCP端口=%d\n",
                  WIFI_AP_SSID, ip.toString().c_str(), WIFI_TCP_PORT);
    Serial.printf("[router] 输入选择器UART初始化完成 (TX=IO%d RX=IO%d 波特率=%d)\n",
                  INPUT_SELECTOR_TX_GPIO, INPUT_SELECTOR_RX_GPIO, INPUT_SELECTOR_BAUD_RATE);
    Serial.printf("[router] 主板UART初始化完成 (TX=IO%d RX=IO%d 波特率=%d)\n",
                  MAINBOARD_TX_GPIO, MAINBOARD_RX_GPIO, MAINBOARD_BAUD_RATE);
    Serial.printf("[router] 手柄UART初始化完成 (TX=IO%d RX=IO%d 波特率=%d)\n",
                  HANDSET_TX_GPIO, HANDSET_RX_GPIO, HANDSET_BAUD_RATE);
}

static void accept_client_if_needed()
{
    if (!s_client || !s_client.connected()) {
        WiFiClient incoming = s_tcp_server.available();
        if (incoming) {
            s_client = incoming;
            s_client.setNoDelay(true);
            Serial.printf("[router] TCP客户端已连接: %s\n", s_client.remoteIP().toString().c_str());
        }
    }
}

// A2：WiFi(TCP) <-> 输入选择器UART 双向原始字节透传，不做协议解析
static void bridge_a2_locked()
{
    while (s_uart_input_selector.available()) {
        uint8_t b = s_uart_input_selector.read();
        if (s_client && s_client.connected()) {
            s_client.write(b);
        }
    }
    if (s_client && s_client.connected()) {
        while (s_client.available()) {
            uint8_t b = s_client.read();
            s_uart_input_selector.write(b);
        }
    }
}

// 非A2状态：输入选择器UART本不该承载有效数据（对应状态下EN2策略见
// switch_monitor.c），这里主动丢弃残留字节，避免切回A2瞬间把陈旧数据
// 一次性甩给客户端。
static void drain_input_selector_when_idle()
{
    while (s_uart_input_selector.available()) {
        s_uart_input_selector.read();
    }
}

static void router_bridge_task(void *arg)
{
    (void)arg;
    watchdog_heartbeat_register(WDT_HB_ROUTER_BRIDGE);

    while (1) {
        accept_client_if_needed();

        if (switch_monitor_get_mode() == PANEL_MODE_A2) {
            bridge_a2_locked();
        } else {
            drain_input_selector_when_idle();
        }

        watchdog_heartbeat_report(WDT_HB_ROUTER_BRIDGE);
        vTaskDelay(pdMS_TO_TICKS(5)); // 轮询间隔，控制转发延迟的同时让出CPU
    }
}

/* ------------------------------------------------------------------------
 * B0/B1/B2：手柄 <-> 主板 双向原始字节直通，不做转码
 * 对应需求5"KP通道最高优先级零延迟转发"——单独任务、比其余任务更高优先级，
 * 轮询间隔也更短，尽量贴近"零延迟"。
 * ---------------------------------------------------------------------- */

static bool is_b_mode(panel_mode_t mode)
{
    return mode == PANEL_MODE_B0 || mode == PANEL_MODE_B1 || mode == PANEL_MODE_B2;
}

// 主板UART写入：持锁保护，短小操作，尽快释放
static void mainboard_write_locked(const uint8_t *data, size_t len)
{
    if (xSemaphoreTake(s_mainboard_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        s_uart_mainboard.write(data, len);
        xSemaphoreGive(s_mainboard_mutex);
    }
}

// 手柄TX -> 主板RX：物理按键扫描码直通，最高优先级，不经过仲裁
static void bridge_handset_to_mainboard()
{
    while (s_uart_handset.available()) {
        uint8_t b = s_uart_handset.read();
        s_handset_last_activity_ms = millis(); // 记录活动时间戳，供转码任务仲裁使用
        mainboard_write_locked(&b, 1);
    }
}

// 主板TX -> 手柄RX：下行画面数据原样转发回手柄屏幕。同时（不阻塞地）把
// 每个字节也送一份进镜像队列，供低优先级的mirror_task异步处理，不拖慢
// 这条转发主路径（需求7的"不阻塞"约束）。
static void bridge_mainboard_to_handset()
{
    while (s_uart_mainboard.available()) {
        uint8_t b = s_uart_mainboard.read();
        s_uart_handset.write(b);
        // 0超时非阻塞发送：队列满了就直接丢弃这个字节，绝不在这条最高优先级
        // 路径上等待，镜像显示可以偶尔丢字节，但转发给手柄本身绝对不能延迟。
        xQueueSend(s_mirror_queue, &b, 0);
    }
}

// 非B模式：手柄/主板UART不该承载有效数据，主动丢弃残留字节。
static void drain_handset_uarts_when_idle()
{
    while (s_uart_handset.available()) { s_uart_handset.read(); }
    while (s_uart_mainboard.available()) { s_uart_mainboard.read(); }
}

static void handset_bridge_task(void *arg)
{
    (void)arg;
    watchdog_heartbeat_register(WDT_HB_HANDSET_BRIDGE);

    while (1) {
        if (is_b_mode(switch_monitor_get_mode())) {
            bridge_handset_to_mainboard();
            bridge_mainboard_to_handset();
        } else {
            drain_handset_uarts_when_idle();
        }

        watchdog_heartbeat_report(WDT_HB_HANDSET_BRIDGE);
        vTaskDelay(pdMS_TO_TICKS(1)); // 轮询间隔比其余任务更短，贴近"零延迟"要求
    }
}

/* ------------------------------------------------------------------------
 * B0/B1/B2：LX200 ASCII指令解析 + 转码 + 仲裁注入主板（需求6）
 *
 * B0/B1：指令来自输入选择器UART（物理RS232/USB-C经开关矩阵送入）
 * B2   ：指令来自TCP客户端（WiFi/BLE协议栈软件回调，不经过任何UART硬件）
 * 两路独立的指令累积器，喂进同一套转码+仲裁注入逻辑。
 * ---------------------------------------------------------------------- */

#define CMD_BUF_MAX 32 // LX200指令通常很短，32字节足够容纳目前覆盖的指令集

typedef struct {
    char buf[CMD_BUF_MAX];
    size_t len;
    bool active; // 是否已经见到起始符':'，还没见到之前忽略所有字节
} cmd_accumulator_t;

static cmd_accumulator_t s_cmd_from_input_selector; // B0/B1用
static cmd_accumulator_t s_cmd_from_tcp;             // B2用

static void cmd_accum_reset(cmd_accumulator_t *acc)
{
    acc->len = 0;
    acc->active = false;
}

// 喂入一个字节；拼出完整指令（以'#'结束）时返回true，acc->buf是以NUL结尾
// 的完整指令字符串；否则返回false（还没收全，或者异常字节被丢弃）。
static bool cmd_accum_feed(cmd_accumulator_t *acc, uint8_t byte)
{
    if (byte == ':') {
        acc->len = 0;
        acc->active = true;
        acc->buf[acc->len++] = (char)byte;
        return false;
    }
    if (!acc->active) {
        return false; // 还没见到起始符，忽略（比如残留的杂散字节）
    }
    if (acc->len >= CMD_BUF_MAX - 1) {
        cmd_accum_reset(acc); // 溢出保护：异常长的输入直接丢弃重置
        return false;
    }
    acc->buf[acc->len++] = (char)byte;
    if (byte == '#') {
        acc->buf[acc->len] = '\0';
        acc->active = false;
        return true;
    }
    return false;
}

// 导出给其他模块（目前是virtual_handset.cpp）复用的单字节注入原语：
// 遵循同一套手柄仲裁规则——手柄最近HANDSET_FRAME_GAP_MS内有活动就失败，
// 不等待不重试，调用方自己决定要不要重试。
bool router_bridge_try_inject_byte(uint8_t b)
{
    if (millis() - s_handset_last_activity_ms < HANDSET_FRAME_GAP_MS) {
        return false;
    }
    mainboard_write_locked(&b, 1);
    return true;
}

// 导出：查询手柄当前是否空闲（供virtual_handset.cpp在宏序列的等待期间
// 高频轮询，满足"<5ms内让出控制权"的要求）。
bool router_bridge_handset_idle()
{
    return (millis() - s_handset_last_activity_ms) >= HANDSET_FRAME_GAP_MS;
}

// 尝试把一条已转码的HEX指令注入主板，逐字节复用router_bridge_try_inject_byte，
// 一旦某个字节因手柄活动被拒绝就整体放弃剩余部分（对应需求6仲裁规则2/3：
// 分批持锁及时释放；检测到手柄新活动立即中止）。
static bool try_inject_hex_command(const uint8_t *hex, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if (!router_bridge_try_inject_byte(hex[i])) {
            if (i > 0) {
                Serial.println("[transcode] 注入过程中检测到手柄活动，中止剩余部分");
            }
            return false;
        }
    }
    return true;
}

// 处理一条已经拼完整的ASCII指令：先看是不是时间/日期同步指令（这类指令
// 只用来同步面板自己的时钟，不转码不注入主板），不是的话再走转码+仲裁注入。
static void process_complete_command(const char *ascii_cmd, const char *source_tag)
{
    if (time_sync_try_handle_command(ascii_cmd)) {
        return; // 时间/日期指令已经被time_sync模块处理完，不再走转码流程
    }

    uint8_t hex[TRANSCODE_MAX_HEX_LEN];
    size_t hex_len = 0;

    bool recognized_with_data = transcode_ascii_to_hex(ascii_cmd, hex, sizeof(hex), &hex_len);

    if (!recognized_with_data) {
        Serial.printf("[transcode] (%s) 指令 \"%s\" 未识别或HEX映射尚未填入，跳过注入\n",
                      source_tag, ascii_cmd);
        return;
    }

    if (try_inject_hex_command(hex, hex_len)) {
        Serial.printf("[transcode] (%s) 指令 \"%s\" 已注入主板 (%u字节)\n",
                      source_tag, ascii_cmd, (unsigned)hex_len);
    } else {
        Serial.printf("[transcode] (%s) 指令 \"%s\" 因手柄活动被跳过/中止\n",
                      source_tag, ascii_cmd);
    }
}

static void transcode_task(void *arg)
{
    (void)arg;
    watchdog_heartbeat_register(WDT_HB_TRANSCODE);
    cmd_accum_reset(&s_cmd_from_input_selector);
    cmd_accum_reset(&s_cmd_from_tcp);

    panel_mode_t prev_mode = PANEL_MODE_UNKNOWN;

    while (1) {
        panel_mode_t mode = switch_monitor_get_mode();

        // 需求9：每次从非B状态切入B0/B1/B2的第一时间，先盲发一次强力复位
        // 序列，把主板拉回已知的菜单基线，再开始处理转码指令，不依赖上次
        // 状态做叠加推演。这个调用是阻塞的（最多5*(80+60)=700ms，或者更早
        // 被手柄活动中止），期间transcode_task本身的指令处理会跟着一起等，
        // 但不影响handset_bridge_task（独立任务，最高优先级，不受影响）。
        if (is_b_mode(mode) && !is_b_mode(prev_mode)) {
            Serial.println("[transcode] 检测到刚进入B模式，先执行强力盲视复位序列...");
            virtual_handset_blind_reset();
        }
        prev_mode = mode;

        if (mode == PANEL_MODE_B0 || mode == PANEL_MODE_B1) {
            while (s_uart_input_selector.available()) {
                uint8_t b = s_uart_input_selector.read();
                if (cmd_accum_feed(&s_cmd_from_input_selector, b)) {
                    process_complete_command(s_cmd_from_input_selector.buf, "输入选择器");
                }
            }
        } else {
            cmd_accum_reset(&s_cmd_from_input_selector);
        }

        if (mode == PANEL_MODE_B2) {
            if (s_client && s_client.connected()) {
                while (s_client.available()) {
                    uint8_t b = s_client.read();
                    if (cmd_accum_feed(&s_cmd_from_tcp, b)) {
                        process_complete_command(s_cmd_from_tcp.buf, "WiFi");
                    }
                }
            }
        } else {
            cmd_accum_reset(&s_cmd_from_tcp);
        }

        watchdog_heartbeat_report(WDT_HB_TRANSCODE);
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

/* ------------------------------------------------------------------------
 * 下行镜像（需求7，MVP5新增，MVP5.1起接入OLED）
 *
 * 布局缺口已解决：OLED改成6行布局，MIRROR1/MIRROR2两行专门留给这里用
 * （详见oled_display.h）。
 *
 * ⚠️ 协议缺口仍然存在：需求7要的是"原厂手柄两行LED屏幕上应当显示的信息"，
 * 这需要知道手柄自己的显示协议（下行字节怎么分成两行文本、有没有清屏/
 * 光标控制字符等）。之前拿到的PDF（Meade_LX200_Classic_Hand_Controller_
 * Key_Codes.pdf）只是按键协议（手柄->主板方向），不是显示协议（主板->
 * 手柄方向），两个是不同的协议文档，需要你另外找/确认。
 *
 * 在协议缺口补上之前，mirror_task 把收到的字节整批打到串口日志，同时把
 * 同一份数据的hex预览推到MIRROR1、可打印字符预览推到MIRROR2——这不是
 * 真正按协议解析出来的两行文本，只是"能看到有数据在动"的过渡方案，等
 * 协议确认后要换成正确解析的内容。
 * ---------------------------------------------------------------------- */

#define MIRROR_LOG_BATCH_MAX 32

static void mirror_task(void *arg)
{
    (void)arg;
    watchdog_heartbeat_register(WDT_HB_MIRROR);

    uint8_t batch[MIRROR_LOG_BATCH_MAX];

    while (1) {
        size_t count = 0;
        uint8_t b;
        // 尽量攒一批再打日志，避免主板每吐1字节就打一行、刷屏刷得太碎；
        // 用短超时等第一个字节，之后不再等待，凑够多少打多少。
        if (xQueueReceive(s_mirror_queue, &b, pdMS_TO_TICKS(50)) == pdTRUE) {
            batch[count++] = b;
            while (count < MIRROR_LOG_BATCH_MAX &&
                   xQueueReceive(s_mirror_queue, &b, 0) == pdTRUE) {
                batch[count++] = b;
            }

            char hex_str[MIRROR_LOG_BATCH_MAX * 3 + 1];
            char ascii_str[MIRROR_LOG_BATCH_MAX + 1];
            size_t hex_pos = 0, ascii_pos = 0;
            for (size_t i = 0; i < count; i++) {
                hex_pos += snprintf(hex_str + hex_pos, sizeof(hex_str) - hex_pos, "%02X ", batch[i]);
                ascii_str[ascii_pos++] = (batch[i] >= 0x20 && batch[i] < 0x7F) ? (char)batch[i] : '.';
            }
            ascii_str[ascii_pos] = '\0';
            Serial.printf("[mirror] 主板下行原始数据(%u字节) hex=[%s] ascii=[%s]\n",
                          (unsigned)count, hex_str, ascii_str);

            // 显示协议还不知道，先把同一份数据的两种预览分别摆在两行镜像上：
            // 第1行=hex，第2行=可打印字符（OLED字库字符集有限，不认识的字符
            // 会显示成空格，不代表数据本身有问题）。等真正的显示协议确认后，
            // 这里要换成按协议正确解析出的两行文本。
            oled_set_mirror_line1(hex_str);
            oled_set_mirror_line2(ascii_str);
        }

        watchdog_heartbeat_report(WDT_HB_MIRROR);
    }
}

void router_bridge_start()
{
    xTaskCreate(router_bridge_task, "router_bridge", 4096, NULL, 8, NULL);
    xTaskCreate(transcode_task, "transcode", 4096, NULL, 9, NULL);
    xTaskCreate(mirror_task, "mirror", 3072, NULL, 4, NULL); // 优先级低，不跟关键路径抢
    // 优先级高于其余任务，对应需求5"手柄通道最高优先级"
    xTaskCreate(handset_bridge_task, "handset_bridge", 4096, NULL, 12, NULL);
}
