#include "time_sync.h"

#include <Arduino.h>
#include <time.h>
#include <sys/time.h>
#include <stdio.h>
#include <string.h>

static bool s_has_time = false;
static bool s_has_date = false;
static struct tm s_tm; // tm_year是"距1900年"，tm_mon是0-11，其余字段含义标准

static void apply_and_sync_locked()
{
    if (!(s_has_time && s_has_date)) {
        return; // 日期和时间都齐了才真正下发，避免用默认0值(1900年之类)污染时钟
    }
    time_t t = mktime(&s_tm);
    struct timeval tv;
    tv.tv_sec = t;
    tv.tv_usec = 0;
    settimeofday(&tv, NULL);
    Serial.printf("[time_sync] 时钟已同步: %04d-%02d-%02d %02d:%02d:%02d\n",
                  s_tm.tm_year + 1900, s_tm.tm_mon + 1, s_tm.tm_mday,
                  s_tm.tm_hour, s_tm.tm_min, s_tm.tm_sec);
}

bool time_sync_try_handle_command(const char *ascii_cmd)
{
    if (ascii_cmd == NULL || ascii_cmd[0] != ':' || strlen(ascii_cmd) < 3) {
        return false;
    }

    // :SLHH:MM:SS#  设置本地时间（LX200标准指令）
    if (ascii_cmd[1] == 'S' && ascii_cmd[2] == 'L') {
        int hh = 0, mm = 0, ss = 0;
        if (sscanf(ascii_cmd + 3, "%2d:%2d:%2d", &hh, &mm, &ss) == 3) {
            s_tm.tm_hour = hh;
            s_tm.tm_min = mm;
            s_tm.tm_sec = ss;
            s_has_time = true;
            Serial.printf("[time_sync] 收到设置时间指令: %02d:%02d:%02d\n", hh, mm, ss);
            apply_and_sync_locked();
        } else {
            Serial.printf("[time_sync] :SL#指令格式无法解析: %s\n", ascii_cmd);
        }
        return true; // 不管解析成功与否，这条指令都归时间同步模块处理，不再往下传
    }

    // :SCMM/DD/YY#  设置日期（LX200标准指令）
    if (ascii_cmd[1] == 'S' && ascii_cmd[2] == 'C') {
        int mo = 0, dd = 0, yy = 0;
        if (sscanf(ascii_cmd + 3, "%2d/%2d/%2d", &mo, &dd, &yy) == 3) {
            s_tm.tm_mon = mo - 1;
            s_tm.tm_mday = dd;
            // 两位年份：00-69按20xx处理，70-99按19xx处理（几乎不会真的用到后者，
            // 只是遵循常见的两位年份世纪判定惯例，不是瞎猜）
            s_tm.tm_year = (yy < 70 ? 2000 + yy : 1900 + yy) - 1900;
            s_has_date = true;
            Serial.printf("[time_sync] 收到设置日期指令: 20%02d-%02d-%02d\n", yy, mo, dd);
            apply_and_sync_locked();
        } else {
            Serial.printf("[time_sync] :SC#指令格式无法解析: %s\n", ascii_cmd);
        }
        return true;
    }

    return false; // 不是时间/日期指令，调用方继续按正常流程处理
}

void time_sync_get_display_string(char *out, size_t out_cap)
{
    if (!(s_has_time && s_has_date)) {
        snprintf(out, out_cap, "SYNC PENDING");
        return;
    }
    time_t now = time(NULL);
    struct tm *lt = localtime(&now);
    strftime(out, out_cap, "%Y-%m-%d %H:%M", lt);
}

bool time_sync_is_synced()
{
    return s_has_time && s_has_date;
}
