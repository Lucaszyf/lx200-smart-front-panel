#include "transcode_engine.h"
#include <string.h>
#include <stdbool.h>

/* ============================================================================
 * 指令识别：O(1)——按指令关键字符直接switch跳转，不做字符串遍历比较。
 * 左侧的LX200 ASCII指令都是Meade LX200公开协议的标准指令，可以直接确认。
 * ========================================================================= */

typedef enum {
    CMD_UNKNOWN = 0,
    CMD_MOVE_NORTH, CMD_MOVE_SOUTH, CMD_MOVE_EAST, CMD_MOVE_WEST,
    CMD_HALT_NORTH, CMD_HALT_SOUTH, CMD_HALT_EAST, CMD_HALT_WEST, CMD_HALT_ALL,
    CMD_RATE_GUIDE, CMD_RATE_CENTER, CMD_RATE_FIND, CMD_RATE_SLEW,
    CMD_COUNT
} lx200_cmd_id_t;

// 期望格式 ":" + 1~2个特征字符 + "#"（或更长，视具体指令而定）。
// 目前只覆盖了移动/停止/速率这一类跟"手柄按键"直接对应的指令子集——
// 这是MVP4先要跑通的范围，坐标查询/对准等指令不在这里，属于后续需求。
static lx200_cmd_id_t identify_command(const char *cmd)
{
    if (cmd == NULL || cmd[0] != ':') return CMD_UNKNOWN;

    switch (cmd[1]) {
        case 'M': // 移动指令：:Mn# :Ms# :Me# :Mw#
            switch (cmd[2]) {
                case 'n': return CMD_MOVE_NORTH;
                case 's': return CMD_MOVE_SOUTH;
                case 'e': return CMD_MOVE_EAST;
                case 'w': return CMD_MOVE_WEST;
                default:  return CMD_UNKNOWN;
            }
        case 'Q': // 停止指令：:Qn# :Qs# :Qe# :Qw# :Q#（全部停止，无方向字符）
            switch (cmd[2]) {
                case 'n': return CMD_HALT_NORTH;
                case 's': return CMD_HALT_SOUTH;
                case 'e': return CMD_HALT_EAST;
                case 'w': return CMD_HALT_WEST;
                case '#': return CMD_HALT_ALL;
                default:  return CMD_UNKNOWN;
            }
        case 'R': // 速率指令：:RG#(guide) :RC#(center) :RM#(find) :RS#(slew/max)
            switch (cmd[2]) {
                case 'G': return CMD_RATE_GUIDE;
                case 'C': return CMD_RATE_CENTER;
                case 'M': return CMD_RATE_FIND;
                case 'S': return CMD_RATE_SLEW;
                default:  return CMD_UNKNOWN;
            }
        default:
            return CMD_UNKNOWN;
    }
}

/* ============================================================================
 * HEX映射表——来自 Meade_LX200_Classic_Hand_Controller_Key_Codes.pdf，
 * 且已按更正版资料修正过3处笔误（S/E/W的Key-Up码，详见下方）。
 *
 * 该文档给出的是标准手柄串口协议：1起始位+8数据位(LSB在前)+1停止位，最高位
 * (bit7)按下(Key Down)恒为0、松开(Key Up)恒为1。方向键(N/S/E/W)的按下/松开
 * 码，正好对应LX200 ASCII的"开始移动"/"停止移动"指令：
 *   :Mn# (开始北移) <-> N键按下(Key Down)
 *   :Qn# (停止北移) <-> N键松开(Key Up)
 *   S/E/W同理
 *
 * ⚠️ 【重要更正】最初拿到的PDF里S/E/W三个键的Key-Up码有笔误，已按更正版
 * 资料改正：
 *   S Key-Up: 0xC3 -> 0xD3（原值是笔误）
 *   E Key-Up: 0xD5 -> 0xC5（原值是笔误）
 *   W Key-Up: 0xB7 -> 0xD7（原值是笔误，而且原值正好和数字键7的Key-Up
 *             撞车，这正是当初怀疑"W和7松开码重复是不是笔误"的那个疑点，
 *             现在确认了：确实是笔误，不是巧合，改成0xD7后不再撞车）
 *
 * 速率指令(:RG#/:RC#/:RM#/:RS#)的HEX码：官方按键码文档本身没有单独列出
 * "调速键"，但手柄上SLEW/FIND/CNTR/GUIDE和数字7/4/1/0是同一个物理按键
 * 复用的（按下/松开的HEX码完全相同，只是主板根据当前菜单上下文决定按下
 * 时该解释成数字输入还是速率选择），已确认对应关系：
 *   :RS#(SLEW速度) <-> 数字"7"键　:RM#(FIND速度) <-> 数字"4"键
 *   :RC#(CNTR速度) <-> 数字"1"键　:RG#(GUIDE速度) <-> 数字"0"键
 * 速率选择是"点按一下"的离散动作（不是方向键那种"按住不放"），所以这里
 * 用按下+松开一次性作为一条完整的HEX序列注入，不像方向键那样按下/松开
 * 分别对应两条不同的ASCII指令(:Mn#/:Qn#)。
 * ========================================================================= */

static const uint8_t k_hex_move_north[] = {0x4E}; // N键按下(Key Down)
static const uint8_t k_hex_move_south[] = {0x53}; // S键按下
static const uint8_t k_hex_move_east[]  = {0x45}; // E键按下
static const uint8_t k_hex_move_west[]  = {0x57}; // W键按下

static const uint8_t k_hex_halt_north[] = {0xCE}; // N键松开(Key Up)
static const uint8_t k_hex_halt_south[] = {0xD3}; // S键松开（已更正，原0xC3是笔误）
static const uint8_t k_hex_halt_east[]  = {0xC5}; // E键松开（已更正，原0xD5是笔误）
static const uint8_t k_hex_halt_west[]  = {0xD7}; // W键松开（已更正，原0xB7是笔误）

// :Q#（全部停止）在真实手柄上并没有对应的单一按键——手柄本身只有方向键，
// "全部停止"是"所有当前按着的方向键都松开"这件事的结果，不是某个专门的键。
// 这里的处理方式是：依次发送全部4个方向的"松开"码，效果上等同于同时放开
// 所有方向键。如果主板协议不接受连续发送多个松开码，或者希望换一种处理
// 方式，需要你确认。
static const uint8_t k_hex_halt_all[] = {0xCE, 0xD3, 0xC5, 0xD7}; // N/S/E/W 松开码依次发送

// 速率指令：复用数字键0/1/4/7的按下+松开码，一次性发送完整的一次"点按"
static const uint8_t k_hex_rate_guide[]  = {0x30, 0xB0}; // GUIDE = 数字"0"键 点按
static const uint8_t k_hex_rate_center[] = {0x31, 0xB1}; // CNTR  = 数字"1"键 点按
static const uint8_t k_hex_rate_find[]   = {0x34, 0xB4}; // FIND  = 数字"4"键 点按
static const uint8_t k_hex_rate_slew[]   = {0x37, 0xB7}; // SLEW  = 数字"7"键 点按

typedef struct {
    const uint8_t *hex;
    size_t len;
    bool has_real_data; // true=下面是真实数据，可以注入；false=占位符，禁止注入
} hex_mapping_t;

// 数组顺序必须跟 lx200_cmd_id_t 枚举顺序完全一致（下标对应），
// 这里用位置初始化而不是C99的[index]=形式（本文件按C++编译，不支持后者）。
static const hex_mapping_t k_hex_table[CMD_COUNT] = {
    /* CMD_UNKNOWN     */ { NULL, 0, false },
    /* CMD_MOVE_NORTH  */ { k_hex_move_north, sizeof(k_hex_move_north), true },
    /* CMD_MOVE_SOUTH  */ { k_hex_move_south, sizeof(k_hex_move_south), true },
    /* CMD_MOVE_EAST   */ { k_hex_move_east,  sizeof(k_hex_move_east),  true },
    /* CMD_MOVE_WEST   */ { k_hex_move_west,  sizeof(k_hex_move_west),  true },
    /* CMD_HALT_NORTH  */ { k_hex_halt_north, sizeof(k_hex_halt_north), true },
    /* CMD_HALT_SOUTH  */ { k_hex_halt_south, sizeof(k_hex_halt_south), true },
    /* CMD_HALT_EAST   */ { k_hex_halt_east,  sizeof(k_hex_halt_east),  true },
    /* CMD_HALT_WEST   */ { k_hex_halt_west,  sizeof(k_hex_halt_west),  true },
    /* CMD_HALT_ALL    */ { k_hex_halt_all,   sizeof(k_hex_halt_all),   true },
    /* CMD_RATE_GUIDE  */ { k_hex_rate_guide,  sizeof(k_hex_rate_guide),  true },
    /* CMD_RATE_CENTER */ { k_hex_rate_center, sizeof(k_hex_rate_center), true },
    /* CMD_RATE_FIND   */ { k_hex_rate_find,   sizeof(k_hex_rate_find),   true },
    /* CMD_RATE_SLEW   */ { k_hex_rate_slew,   sizeof(k_hex_rate_slew),   true },
};

bool transcode_ascii_to_hex(const char *ascii_cmd, uint8_t *hex_out,
                            size_t hex_out_cap, size_t *hex_len_out)
{
    if (hex_len_out) *hex_len_out = 0;

    lx200_cmd_id_t id = identify_command(ascii_cmd);
    if (id == CMD_UNKNOWN || id >= CMD_COUNT) {
        return false; // 指令未识别
    }

    const hex_mapping_t *entry = &k_hex_table[id];
    if (!entry->has_real_data) {
        return false; // 指令已识别，但还没有真实HEX映射数据，不能注入
    }

    if (entry->len > hex_out_cap) {
        return false; // 输出缓冲区不够大（正常不应发生，防御性检查）
    }

    memcpy(hex_out, entry->hex, entry->len);
    if (hex_len_out) *hex_len_out = entry->len;
    return true;
}
