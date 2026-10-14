#include "oled_display.h"
#include "config.h"
#include "oled_font.h"
#include "status_icons.h"

#include <Arduino.h>
#include <SPI.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static SPIClass *s_spi = NULL;
static SemaphoreHandle_t s_oled_mutex;
static SPISettings s_spi_settings(OLED_SPI_CLOCK_HZ, MSBFIRST, SPI_MODE0);
static bool s_oled_ready = false;

/* ------------------------------------------------------------------------
 * 屏幕布局：6行，用满64px高度
 *
 *   page0(8px)    ：STATUS行（日期时间+图标）
 *   page1(8px)    ：V行
 *   page2(8px)    ：I行
 *   page3(8px)    ：MODE_DESC合并行
 *   page4-5(16px) ：MIRROR1（手柄第1行镜像，大字体）
 *   page6-7(16px) ：MIRROR2（手柄第2行镜像，大字体）
 * 合计 1+1+1+1+2+2 = 8 page，正好用满64px高度
 * ---------------------------------------------------------------------- */
#define LINE_STATUS_PAGE       0
#define LINE_V_PAGE            1
#define LINE_I_PAGE            2
#define LINE_MODE_DESC_PAGE    3
#define LINE_MIRROR1_START     4
#define LINE_MIRROR2_START     6

// 小字体：原始5x7点阵(不拉伸)，V/I/STATUS/MODE_DESC都用这个
#define SMALL_GLYPH_SPACING    1
#define SMALL_PITCH            (FONT_GLYPH_WIDTH + SMALL_GLYPH_SPACING) // 6px
#define UNIT_GAP_PX            4   // V/I行数值和单位之间的额外间隙
static const int k_col_x[3] = {0, 46, 91}; // V/I三列起始x坐标

// 大字体：原始5x7点阵宽高同比例放大2倍(10x14)，MIRROR1/2用这个
#define BIG_GLYPH_WIDTH        (FONT_GLYPH_WIDTH * 2)  // 10px
#define BIG_GLYPH_SPACING      2
#define BIG_PITCH              (BIG_GLYPH_WIDTH + BIG_GLYPH_SPACING) // 12px

// 状态栏图标位置（右侧，从左到右：WiFi AP / WiFi Station / 蓝牙）
#define ICON_AP_X   100
#define ICON_STA_X  110
#define ICON_BT_X   120

// 滚动通用参数
#define SCROLL_STEP_PX          1
#define SCROLL_INTERVAL_MS      25

// MODE_DESC行：超宽时从屏幕水平中点开始滚动（不是常规从最左边露出），
// GAP设成64(半屏宽)是这个"从中点开始"效果成立的关键，详见set_mode_line_locked()
#define MODE_DESC_GAP_PX        64
#define MODE_DESC_MAX_CHARS     40
#define MODE_DESC_MAX_TEXT_PX   (MODE_DESC_MAX_CHARS * SMALL_PITCH)

// MIRROR1/2：常规从右向左连续滚动（不是中点开始），跟之前DESC行的滚动方式一样
#define MIRROR_GAP_PX           40
#define MIRROR_MAX_CHARS        40
#define MIRROR_MAX_TEXT_PX      (MIRROR_MAX_CHARS * BIG_PITCH)

static char s_datetime_text[24] = "SYNC PENDING";
static bool s_icon_ap_active = false;
static bool s_icon_sta_active = false;
static bool s_icon_bt_active = false;

typedef struct {
    char source_text[64];
    uint8_t full_buf[MODE_DESC_MAX_TEXT_PX];
    int text_width_px;
    int offset;
    bool active;
} mode_line_state_t;
static mode_line_state_t s_mode_line;

typedef struct {
    char source_text[64];
    uint8_t full_low[MIRROR_MAX_TEXT_PX];
    uint8_t full_high[MIRROR_MAX_TEXT_PX];
    int text_width_px;
    int offset;
    bool active;
    uint8_t start_page;
} mirror_line_state_t;
static mirror_line_state_t s_mirror1;
static mirror_line_state_t s_mirror2;

/* ------------------------------------------------------------------------
 * 底层SPI收发
 * ---------------------------------------------------------------------- */

static void oled_write_cmd(uint8_t cmd)
{
    digitalWrite(OLED_PIN_DC, LOW);
    digitalWrite(OLED_PIN_CS, LOW);
    s_spi->beginTransaction(s_spi_settings);
    s_spi->transfer(cmd);
    s_spi->endTransaction();
    digitalWrite(OLED_PIN_CS, HIGH);
}

static void oled_write_data(const uint8_t *data, size_t len)
{
    if (len == 0) return;
    digitalWrite(OLED_PIN_DC, HIGH);
    digitalWrite(OLED_PIN_CS, LOW);
    s_spi->beginTransaction(s_spi_settings);
    s_spi->writeBytes(data, len);
    s_spi->endTransaction();
    digitalWrite(OLED_PIN_CS, HIGH);
}

static void oled_set_page_window(uint8_t page)
{
    oled_write_cmd(0x21);
    oled_write_cmd(0x00);
    oled_write_cmd(OLED_WIDTH - 1);
    oled_write_cmd(0x22);
    oled_write_cmd(page);
    oled_write_cmd(page);
}

static void oled_push_one_page(int page, const uint8_t *data)
{
    oled_set_page_window(page);
    oled_write_data(data, OLED_WIDTH);
}

static void oled_push_page_pair(int start_page, const uint8_t *low, const uint8_t *high)
{
    oled_set_page_window(start_page);
    oled_write_data(low, OLED_WIDTH);
    oled_set_page_window(start_page + 1);
    oled_write_data(high, OLED_WIDTH);
}

static void oled_clear_all()
{
    uint8_t zeros[OLED_WIDTH] = {0};
    for (int p = 0; p < OLED_PAGES; p++) {
        oled_set_page_window(p);
        oled_write_data(zeros, OLED_WIDTH);
    }
}

/* ------------------------------------------------------------------------
 * 初始化
 * ---------------------------------------------------------------------- */

static void oled_hw_reset()
{
    Serial.println("[oled] 开始硬复位时序 (RST拉低10ms -> 拉高10ms)...");
    digitalWrite(OLED_PIN_RST, LOW);
    vTaskDelay(pdMS_TO_TICKS(10));
    digitalWrite(OLED_PIN_RST, HIGH);
    vTaskDelay(pdMS_TO_TICKS(10));
    Serial.println("[oled] 硬复位时序完成");
}

static void oled_run_init_sequence()
{
    Serial.println("[oled] 开始发送SSD1306初始化指令序列...");
    oled_write_cmd(0xAE);
    oled_write_cmd(0xD5); oled_write_cmd(0x80);
    oled_write_cmd(0xA8); oled_write_cmd(0x3F);
    oled_write_cmd(0xD3); oled_write_cmd(0x00);
    oled_write_cmd(0x40);
    oled_write_cmd(0x8D); oled_write_cmd(0x14);
    oled_write_cmd(0x20); oled_write_cmd(0x00);
    oled_write_cmd(0xA1);
    oled_write_cmd(0xC8);
    oled_write_cmd(0xDA); oled_write_cmd(0x12);
    oled_write_cmd(0x81); oled_write_cmd(0xCF);
    oled_write_cmd(0xD9); oled_write_cmd(0xF1);
    oled_write_cmd(0xDB); oled_write_cmd(0x40);
    oled_write_cmd(0xA4);
    oled_write_cmd(0xA6);
    oled_write_cmd(0xAF);
    Serial.println("[oled] SSD1306初始化指令序列发送完毕 (display on已发送)");
}

static void scroll_task(void *arg);

void oled_init()
{
    Serial.println("[oled] === oled_init() 开始 ===");

    s_mirror1.start_page = LINE_MIRROR1_START;
    s_mirror2.start_page = LINE_MIRROR2_START;

    s_oled_mutex = xSemaphoreCreateMutex();
    if (s_oled_mutex == NULL) {
        Serial.println("[oled] 错误：xSemaphoreCreateMutex 失败，OLED不可用，但不影响其他模块");
        return;
    }

    Serial.printf("[oled] 配置GPIO: CS=%d DC=%d RST=%d CLK=%d MOSI=%d\n",
                  OLED_PIN_CS, OLED_PIN_DC, OLED_PIN_RST, OLED_PIN_CLK, OLED_PIN_MOSI);
    pinMode(OLED_PIN_DC, OUTPUT);
    pinMode(OLED_PIN_RST, OUTPUT);
    pinMode(OLED_PIN_CS, OUTPUT);
    digitalWrite(OLED_PIN_CS, HIGH);
    Serial.println("[oled] GPIO配置完成");

    Serial.println("[oled] 正在初始化SPI总线 (HSPI)...");
    s_spi = new SPIClass(HSPI);
    if (s_spi == NULL) {
        Serial.println("[oled] 错误：SPIClass 分配失败，OLED不可用，但不影响其他模块");
        return;
    }
    s_spi->begin(OLED_PIN_CLK, -1, OLED_PIN_MOSI, -1);
    Serial.println("[oled] SPI总线初始化完成");

    oled_hw_reset();
    oled_run_init_sequence();
    oled_clear_all();
    Serial.println("[oled] 清屏完成");

    s_oled_ready = true;
    Serial.println("[oled] === oled_init() 成功完成，OLED已就绪 ===");
}

void oled_display_start()
{
    xTaskCreate(scroll_task, "oled_scroll", 2560, NULL, 4, NULL);
}

/* ------------------------------------------------------------------------
 * 字形渲染基础工具
 * ---------------------------------------------------------------------- */

static void stretch_column_2x_vertical(uint8_t orig, uint8_t *out_low, uint8_t *out_high)
{
    uint16_t doubled = 0;
    for (int r = 0; r < 7; r++) {
        if (orig & (1 << r)) {
            doubled |= (0x3 << (r * 2));
        }
    }
    *out_low = doubled & 0xFF;
    *out_high = (doubled >> 8) & 0xFF;
}

static bool is_value_char(char c)
{
    return (c >= '0' && c <= '9') || c == '.';
}

// V/I行专用：数值和单位(V/mA)之间插入额外间隙
static void render_column_small_with_unit_gap(uint8_t *buf, int col_x, const char *text)
{
    size_t len = strlen(text);
    int x = col_x;
    bool gap_inserted = false;

    for (size_t i = 0; i < len; i++) {
        char ch = text[i];
        if (!gap_inserted && i > 0 && is_value_char(text[i - 1]) && !is_value_char(ch)) {
            x += UNIT_GAP_PX;
            gap_inserted = true;
        }
        if (x + FONT_GLYPH_WIDTH > OLED_WIDTH) break;
        int idx = font_index_for_char(ch);
        for (int c = 0; c < FONT_GLYPH_WIDTH; c++) {
            buf[x + c] = k_font5x7[idx][c];
        }
        x += SMALL_PITCH;
    }
}

// 普通小字体渲染：从指定x开始，逐字符按SMALL_PITCH排布，不做单位间隙处理
static void render_column_small(uint8_t *buf, int col_x, const char *text, size_t max_chars)
{
    size_t len = strlen(text);
    if (len > max_chars) len = max_chars;
    for (size_t i = 0; i < len; i++) {
        int base_col = col_x + (int)i * SMALL_PITCH;
        if (base_col + FONT_GLYPH_WIDTH > OLED_WIDTH) break;
        int idx = font_index_for_char(text[i]);
        for (int c = 0; c < FONT_GLYPH_WIDTH; c++) {
            buf[base_col + c] = k_font5x7[idx][c];
        }
    }
}

/* ------------------------------------------------------------------------
 * STATUS行：日期时间(左) + 三个图标(右)
 * ---------------------------------------------------------------------- */

static void render_status_locked()
{
    uint8_t buf[OLED_WIDTH] = {0};
    render_column_small(buf, 0, s_datetime_text, 16);

    if (s_icon_ap_active) {
        memcpy(buf + ICON_AP_X, k_icon_wifi_ap, ICON_WIDTH);
    }
    if (s_icon_sta_active) {
        memcpy(buf + ICON_STA_X, k_icon_wifi_sta, ICON_WIDTH);
    }
    if (s_icon_bt_active) {
        memcpy(buf + ICON_BT_X, k_icon_bt, ICON_WIDTH);
    }

    oled_push_one_page(LINE_STATUS_PAGE, buf);
}

/* ------------------------------------------------------------------------
 * V/I行：三列对齐，小字体
 * ---------------------------------------------------------------------- */

static void render_telemetry_locked(const char *v[3], const char *i[3])
{
    uint8_t buf_v[OLED_WIDTH] = {0};
    uint8_t buf_i[OLED_WIDTH] = {0};

    for (int col = 0; col < 3; col++) {
        render_column_small_with_unit_gap(buf_v, k_col_x[col], v[col]);
        render_column_small_with_unit_gap(buf_i, k_col_x[col], i[col]);
    }

    oled_push_one_page(LINE_V_PAGE, buf_v);
    oled_push_one_page(LINE_I_PAGE, buf_i);
}

/* ------------------------------------------------------------------------
 * MODE_DESC行：小字体，超宽时从屏幕水平中点开始向左滚动
 * ---------------------------------------------------------------------- */

static void build_mode_line_buffer(const char *text)
{
    memset(s_mode_line.full_buf, 0, sizeof(s_mode_line.full_buf));
    size_t len = strlen(text);
    if (len > MODE_DESC_MAX_CHARS) len = MODE_DESC_MAX_CHARS;

    for (size_t i = 0; i < len; i++) {
        int idx = font_index_for_char(text[i]);
        int base_col = (int)i * SMALL_PITCH;
        for (int c = 0; c < FONT_GLYPH_WIDTH; c++) {
            if (base_col + c < MODE_DESC_MAX_TEXT_PX) {
                s_mode_line.full_buf[base_col + c] = k_font5x7[idx][c];
            }
        }
    }
    s_mode_line.text_width_px = (int)len * SMALL_PITCH;
}

static void render_mode_line_window_locked()
{
    uint8_t win[OLED_WIDTH];
    int virtual_width = s_mode_line.text_width_px + MODE_DESC_GAP_PX;
    for (int x = 0; x < OLED_WIDTH; x++) {
        int vcol = (x + s_mode_line.offset) % virtual_width;
        win[x] = (vcol < s_mode_line.text_width_px) ? s_mode_line.full_buf[vcol] : 0;
    }
    oled_push_one_page(LINE_MODE_DESC_PAGE, win);
}

static void render_mode_line_static_locked()
{
    uint8_t win[OLED_WIDTH] = {0};
    int copy_len = s_mode_line.text_width_px;
    if (copy_len > OLED_WIDTH) copy_len = OLED_WIDTH;
    memcpy(win, s_mode_line.full_buf, copy_len);
    oled_push_one_page(LINE_MODE_DESC_PAGE, win);
}

static void set_mode_line_locked(const char *text)
{
    if (strcmp(s_mode_line.source_text, text) == 0) {
        return;
    }
    strncpy(s_mode_line.source_text, text, sizeof(s_mode_line.source_text) - 1);
    s_mode_line.source_text[sizeof(s_mode_line.source_text) - 1] = '\0';

    build_mode_line_buffer(text);

    if (s_mode_line.text_width_px <= OLED_WIDTH) {
        s_mode_line.active = false;
        s_mode_line.offset = 0;
        render_mode_line_static_locked();
    } else {
        s_mode_line.active = true;
        // 关键点：offset初始值设成text_width_px（配合GAP=64，也就是半屏宽），
        // 使得第一帧画面里 x=[0,63] 落在"间隙区"(空白)，x=[64,127] 正好对应
        // 文字从头开始的前64px——视觉效果就是文字从屏幕水平中点冒出来，
        // 前面（左半屏）一开始是清空的，之后随每一帧滚动继续往左走。
        s_mode_line.offset = s_mode_line.text_width_px;
        render_mode_line_window_locked();
    }
}

/* ------------------------------------------------------------------------
 * MIRROR1/MIRROR2：大字体，常规从右向左连续滚动
 * ---------------------------------------------------------------------- */

static void build_mirror_buffer(mirror_line_state_t *m, const char *text)
{
    memset(m->full_low, 0, sizeof(m->full_low));
    memset(m->full_high, 0, sizeof(m->full_high));

    size_t len = strlen(text);
    if (len > MIRROR_MAX_CHARS) len = MIRROR_MAX_CHARS;

    for (size_t i = 0; i < len; i++) {
        int idx = font_index_for_char(text[i]);
        int base_col = (int)i * BIG_PITCH;
        for (int c = 0; c < FONT_GLYPH_WIDTH; c++) {
            uint8_t lo, hi;
            stretch_column_2x_vertical(k_font5x7[idx][c], &lo, &hi);
            int out0 = base_col + c * 2;
            int out1 = out0 + 1;
            if (out0 < MIRROR_MAX_TEXT_PX) { m->full_low[out0] = lo; m->full_high[out0] = hi; }
            if (out1 < MIRROR_MAX_TEXT_PX) { m->full_low[out1] = lo; m->full_high[out1] = hi; }
        }
    }
    m->text_width_px = (int)len * BIG_PITCH;
}

static void render_mirror_window_locked(mirror_line_state_t *m)
{
    uint8_t win_low[OLED_WIDTH];
    uint8_t win_high[OLED_WIDTH];
    int virtual_width = m->text_width_px + MIRROR_GAP_PX;

    for (int x = 0; x < OLED_WIDTH; x++) {
        int vcol = (x + m->offset) % virtual_width;
        if (vcol < m->text_width_px) {
            win_low[x] = m->full_low[vcol];
            win_high[x] = m->full_high[vcol];
        } else {
            win_low[x] = 0;
            win_high[x] = 0;
        }
    }
    oled_push_page_pair(m->start_page, win_low, win_high);
}

static void render_mirror_static_locked(mirror_line_state_t *m)
{
    uint8_t win_low[OLED_WIDTH] = {0};
    uint8_t win_high[OLED_WIDTH] = {0};
    int copy_len = m->text_width_px;
    if (copy_len > OLED_WIDTH) copy_len = OLED_WIDTH;
    memcpy(win_low, m->full_low, copy_len);
    memcpy(win_high, m->full_high, copy_len);
    oled_push_page_pair(m->start_page, win_low, win_high);
}

static void set_mirror_line_locked(mirror_line_state_t *m, const char *text)
{
    if (strcmp(m->source_text, text) == 0) {
        return;
    }
    strncpy(m->source_text, text, sizeof(m->source_text) - 1);
    m->source_text[sizeof(m->source_text) - 1] = '\0';

    build_mirror_buffer(m, text);
    m->offset = 0;

    if (m->text_width_px <= OLED_WIDTH) {
        m->active = false;
        render_mirror_static_locked(m);
    } else {
        m->active = true;
        render_mirror_window_locked(m);
    }
}

/* ------------------------------------------------------------------------
 * 后台滚动任务：MODE_DESC + MIRROR1 + MIRROR2 三条独立滚动，各自按需推进
 * ---------------------------------------------------------------------- */

static void scroll_task(void *arg)
{
    (void)arg;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(SCROLL_INTERVAL_MS));
        if (!s_oled_ready) continue;
        if (xSemaphoreTake(s_oled_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            if (s_mode_line.active) {
                int vw = s_mode_line.text_width_px + MODE_DESC_GAP_PX;
                s_mode_line.offset = (s_mode_line.offset + SCROLL_STEP_PX) % vw;
                render_mode_line_window_locked();
            }
            if (s_mirror1.active) {
                int vw = s_mirror1.text_width_px + MIRROR_GAP_PX;
                s_mirror1.offset = (s_mirror1.offset + SCROLL_STEP_PX) % vw;
                render_mirror_window_locked(&s_mirror1);
            }
            if (s_mirror2.active) {
                int vw = s_mirror2.text_width_px + MIRROR_GAP_PX;
                s_mirror2.offset = (s_mirror2.offset + SCROLL_STEP_PX) % vw;
                render_mirror_window_locked(&s_mirror2);
            }
            xSemaphoreGive(s_oled_mutex);
        }
    }
}

/* ------------------------------------------------------------------------
 * 对外API：OLED未就绪时全部静默直接返回
 * ---------------------------------------------------------------------- */

void oled_set_telemetry(const char *v0, const char *v1, const char *v2,
                         const char *i0, const char *i1, const char *i2)
{
    if (!s_oled_ready) return;
    if (xSemaphoreTake(s_oled_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        Serial.println("[oled] oled_mutex 获取超时，跳过本次遥测刷新");
        return;
    }
    const char *v[3] = {v0, v1, v2};
    const char *i[3] = {i0, i1, i2};
    render_telemetry_locked(v, i);
    xSemaphoreGive(s_oled_mutex);
}

void oled_set_mode_line(const char *text)
{
    if (!s_oled_ready) return;
    if (xSemaphoreTake(s_oled_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return;
    set_mode_line_locked(text);
    xSemaphoreGive(s_oled_mutex);
}

void oled_set_mirror_line1(const char *text)
{
    if (!s_oled_ready) return;
    if (xSemaphoreTake(s_oled_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return;
    set_mirror_line_locked(&s_mirror1, text);
    xSemaphoreGive(s_oled_mutex);
}

void oled_set_mirror_line2(const char *text)
{
    if (!s_oled_ready) return;
    if (xSemaphoreTake(s_oled_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return;
    set_mirror_line_locked(&s_mirror2, text);
    xSemaphoreGive(s_oled_mutex);
}

void oled_set_datetime(const char *text)
{
    if (!s_oled_ready) return;
    if (xSemaphoreTake(s_oled_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return;
    strncpy(s_datetime_text, text, sizeof(s_datetime_text) - 1);
    s_datetime_text[sizeof(s_datetime_text) - 1] = '\0';
    render_status_locked();
    xSemaphoreGive(s_oled_mutex);
}

void oled_set_status_icons(bool wifi_ap_active, bool wifi_sta_active, bool bt_active)
{
    if (!s_oled_ready) return;
    if (xSemaphoreTake(s_oled_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return;
    s_icon_ap_active = wifi_ap_active;
    s_icon_sta_active = wifi_sta_active;
    s_icon_bt_active = bt_active;
    render_status_locked();
    xSemaphoreGive(s_oled_mutex);
}

void oled_show_boot_message(const char *msg)
{
    if (!s_oled_ready) return;
    if (xSemaphoreTake(s_oled_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return;
    const char *blank[3] = {"", "", ""};
    render_telemetry_locked(blank, blank);
    set_mode_line_locked(msg);
    set_mirror_line_locked(&s_mirror1, "");
    set_mirror_line_locked(&s_mirror2, "");
    render_status_locked();
    xSemaphoreGive(s_oled_mutex);
}

bool oled_is_ready()
{
    return s_oled_ready;
}
