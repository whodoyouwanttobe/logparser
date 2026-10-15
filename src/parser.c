#define _POSIX_C_SOURCE 200809L
#include "parser.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

static const char *MONTHS[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                 "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

static const char *SEV_NAMES[8] = {"emerg",   "alert",  "crit", "err",
                                   "warning", "notice", "info", "debug"};

void parse_ctx_init(parse_ctx *ctx)
{
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    ctx->cur_year = tm.tm_year + 1900;
    ctx->cur_month = tm.tm_mon + 1;
}

const char *severity_name(int code)
{
    return (code >= 0 && code <= 7) ? SEV_NAMES[code] : "?";
}

int severity_from_name(const char *name)
{
    if (!name || !*name) return -1;
    if (isdigit((unsigned char)name[0]) && name[1] == '\0')
        return (name[0] - '0') <= 7 ? name[0] - '0' : -1;
    for (int i = 0; i < 8; i++)
        if (strcasecmp(name, SEV_NAMES[i]) == 0) return i;
    if (strcasecmp(name, "error") == 0) return 3;
    if (strcasecmp(name, "warn") == 0) return 4;
    return -1;
}

/* ---------- вспомогательные ---------- */

static int all_digits(const char *s, int n)
{
    for (int i = 0; i < n; i++)
        if (!isdigit((unsigned char)s[i])) return 0;
    return 1;
}

static const char *skip_spaces(const char *s)
{
    while (*s == ' ' || *s == '\t')
        s++;
    return s;
}

/* регистронезависимый поиск подстроки в первых limit символах */
static int ci_contains(const char *hay, const char *needle, size_t limit)
{
    size_t nl = strlen(needle);
    for (size_t i = 0; hay[i] && i < limit; i++) {
        size_t j = 0;
        while (j < nl && hay[i + j] && tolower((unsigned char)hay[i + j]) == needle[j])
            j++;
        if (j == nl) return 1;
    }
    return 0;
}

/* <PRI>: facility*8 + severity */
static const char *parse_pri(const char *s, int *facility, int *severity)
{
    if (*s != '<') return s;
    const char *p = s + 1;
    int v = 0, n = 0;
    while (isdigit((unsigned char)*p) && n < 3) {
        v = v * 10 + (*p - '0');
        p++;
        n++;
    }
    if (n == 0 || *p != '>' || v > 191) return s;
    *facility = v / 8;
    *severity = v % 8;
    return p + 1;
}

/* "Oct 10 12:34:56" / "Oct  1 12:34:56" */
static const char *parse_bsd_ts(const parse_ctx *ctx, const char *s, char *out)
{
    int mon = -1;
    for (int i = 0; i < 12; i++)
        if (strncmp(s, MONTHS[i], 3) == 0 && s[3] == ' ') {
            mon = i + 1;
            break;
        }
    if (mon < 0) return NULL;

    int day, hh, mm, ss, used = 0;
    if (sscanf(s + 4, "%2d %2d:%2d:%2d%n", &day, &hh, &mm, &ss, &used) != 4) return NULL;
    if (day < 1 || day > 31 || hh > 23 || mm > 59 || ss > 60) return NULL;

    /* года в формате нет: если месяц "из будущего" — это прошлый год (лог декабря, читаем в январе)
     */
    int year = ctx->cur_year;
    if (mon > ctx->cur_month + 1) year--;

    snprintf(out, TS_MAX, "%04d-%02d-%02d %02d:%02d:%02d", year, mon, day, hh, mm, ss);
    return s + 4 + used;
}

/* 2026-10-10T12:34:56[.ffffff][Z|+07:00|+0700] */
static const char *parse_iso_ts(const char *s, char *out)
{
    if (!(all_digits(s, 4) && s[4] == '-' && all_digits(s + 5, 2) && s[7] == '-' &&
          all_digits(s + 8, 2) && (s[10] == 'T' || s[10] == ' ') && all_digits(s + 11, 2) &&
          s[13] == ':' && all_digits(s + 14, 2) && s[16] == ':' && all_digits(s + 17, 2)))
        return NULL;

    const char *p = s + 19;
    if (*p == '.' || *p == ',') {
        p++;
        while (isdigit((unsigned char)*p))
            p++;
    }
    if (*p == 'Z')
        p++;
    else if (*p == '+' || *p == '-') {
        if (!all_digits(p + 1, 2)) return NULL;
        p += 3;
        if (*p == ':') p++;
        if (all_digits(p, 2)) p += 2;
    }
    if (*p != ' ' && *p != '\t') return NULL;

    size_t len = (size_t)(p - s);
    if (len >= TS_MAX) return NULL;
    memcpy(out, s, len);
    out[len] = '\0';
    if (out[10] == ' ') out[10] = 'T';
    if (out[19] == ',') out[19] = '.';
    return p;
}

/* Эвристика уровня, если в строке нет <PRI> (обычный /var/log/syslog) */
static int guess_severity(const char *prog, const char *msg)
{
    const size_t L = 256;
    if (ci_contains(msg, "panic", L) || ci_contains(msg, "emerg", L)) return 0;
    if (ci_contains(msg, "critical", L) || ci_contains(msg, "crit:", L) ||
        ci_contains(msg, "out of memory", L) || ci_contains(msg, "oom-kill", L))
        return 2;
    if (ci_contains(msg, "error", L) || ci_contains(msg, "err:", L) ||
        ci_contains(msg, "fail", L) || ci_contains(msg, "segfault", L) ||
        ci_contains(msg, "fatal", L))
        return 3;
    if (ci_contains(msg, "warn", L) || ci_contains(msg, "invalid user", L) ||
        ci_contains(msg, "denied", L) || ci_contains(msg, "timeout", L) ||
        ci_contains(msg, "timed out", L))
        return 4;
    if (ci_contains(msg, "notice", L)) return 5;
    if (ci_contains(msg, "debug", L) || (prog && ci_contains(prog, "debug", 64))) return 7;
    return 6;
}

/* ---------- основной разбор ---------- */

int parse_line(const parse_ctx *ctx, char *line, log_entry *e)
{
    /* обрезаем \n, \r, хвостовые пробелы */
    size_t n = strlen(line);
    while (n && (line[n - 1] == '\n' || line[n - 1] == '\r' || line[n - 1] == ' '))
        line[--n] = '\0';
    if (n == 0) return -1;

    e->facility = -1;
    e->severity = -1;
    e->pid = -1;
    e->program = NULL;

    char *p = (char *)parse_pri(line, &e->facility, &e->severity);

    const char *after = parse_iso_ts(p, e->ts);
    if (!after) after = parse_bsd_ts(ctx, p, e->ts);
    if (!after) return -1;
    p = (char *)skip_spaces(after);

    /* host */
    char *host = p;
    while (*p && *p != ' ')
        p++;
    if (p == host || *p == '\0') return -1;
    *p++ = '\0';
    e->host = host;
    p = (char *)skip_spaces(p);

    /* tag: prog[pid]: или prog: (тег без пробелов, разумной длины) */
    char *tag = p, *q = p;
    while (*q && *q != ':' && *q != '[' && *q != ' ' && (q - tag) < 128)
        q++;
    if (q > tag && *q == '[') {
        char *pid_start = q + 1, *pid_end = pid_start;
        while (isdigit((unsigned char)*pid_end))
            pid_end++;
        if (*pid_end == ']' && pid_end[1] == ':' && pid_end > pid_start) {
            *q = '\0';
            *pid_end = '\0';
            e->pid = atoi(pid_start);
            e->program = tag;
            p = pid_end + 2;
        }
    } else if (q > tag && *q == ':' && (q[1] == ' ' || q[1] == '\0')) {
        *q = '\0';
        e->program = tag;
        p = q + 1;
    }
    /* иначе — тега нет, всё остальное это сообщение */

    e->message = skip_spaces(p);

    if (e->severity < 0) {
        e->severity = guess_severity(e->program, e->message);
        e->sev_inferred = 1;
    } else {
        e->sev_inferred = 0;
    }
    return 0;
}
