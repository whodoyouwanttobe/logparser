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

/* Канонические имена кодов syslog (RFC 5424) */
static const char *SEV_NAMES[8] = {"emergency", "alert",  "critical", "error",
                                   "warning",   "notice", "info",     "debug"};

/* Сопоставление обозначений уровней разных программ с кодом syslog.
 * canonical — как сохраняем в БД. Синонимы с тем же смыслом (err, warn) приводим к одному
 * имени; самостоятельные уровни (fatal, trace) сохраняем под своим именем, чтобы не терять
 * информацию, но даём им код для сортировки и фильтра --level. */
static const struct {
    const char *name;
    const char *canonical;
    int code;
} LEVELS[] = {
    {"emergency", "emergency", 0}, {"emerg", "emergency", 0},   {"panic", "emergency", 0},
    {"alert", "alert", 1},         {"critical", "critical", 2}, {"crit", "critical", 2},
    {"fatal", "fatal", 2},         {"error", "error", 3},       {"err", "error", 3},
    {"warning", "warning", 4},     {"warn", "warning", 4},      {"notice", "notice", 5},
    {"info", "info", 6},           {"information", "info", 6},  {"informational", "info", 6},
    {"debug", "debug", 7},         {"trace", "trace", 7},
};
#define LEVELS_N (sizeof LEVELS / sizeof LEVELS[0])

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
    return (code >= 0 && code <= 7) ? SEV_NAMES[code] : "-";
}

int level_normalize(const char *name, size_t len, char out[LEVEL_MAX], int *known)
{
    size_t n = len < LEVEL_MAX - 1 ? len : LEVEL_MAX - 1;
    for (size_t i = 0; i < n; i++)
        out[i] = (char)tolower((unsigned char)name[i]);
    out[n] = '\0';

    for (size_t i = 0; i < LEVELS_N; i++) {
        if (strcmp(out, LEVELS[i].name) == 0) {
            snprintf(out, LEVEL_MAX, "%s", LEVELS[i].canonical);
            *known = 1;
            return LEVELS[i].code;
        }
    }
    *known = 0;
    return -1; /* нестандартный уровень: сохраняем как есть, без кода */
}

int severity_from_name(const char *name)
{
    if (!name || !*name) return -1;
    if (isdigit((unsigned char)name[0]) && name[1] == '\0')
        return (name[0] - '0') <= 7 ? name[0] - '0' : -1;
    char buf[LEVEL_MAX];
    int known;
    int code = level_normalize(name, strlen(name), buf, &known);
    return known ? code : -1;
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

static size_t alpha_len(const char *s)
{
    size_t n = 0;
    while (isalpha((unsigned char)s[n]))
        n++;
    return n;
}

static int all_upper(const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (!isupper((unsigned char)s[i])) return 0;
    return 1;
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

/* Явная разметка уровня в начале сообщения:
 *   [ERROR] ...   <warn> ...   — любое слово из букв в скобках
 *   FATAL: ...                 — слово ЗАГЛАВНЫМИ с двоеточием
 *   error: ...                 — известное имя уровня с двоеточием (так пишет, например, sshd)
 * Возвращает длину имени уровня и его начало в *name, либо 0. */
static size_t level_prefix(const char *msg, const char **name)
{
    if (*msg == '[' || *msg == '<') {
        char close = *msg == '[' ? ']' : '>';
        size_t n = alpha_len(msg + 1);
        if (n > 0 && n < LEVEL_MAX && msg[1 + n] == close) {
            *name = msg + 1;
            return n;
        }
        return 0;
    }
    size_t n = alpha_len(msg);
    if (n == 0 || n >= LEVEL_MAX || msg[n] != ':') return 0;
    if (all_upper(msg, n)) {
        *name = msg;
        return n;
    }
    char buf[LEVEL_MAX];
    int known;
    level_normalize(msg, n, buf, &known);
    if (known) {
        *name = msg;
        return n;
    }
    return 0;
}

/* регистронезависимый поиск "key=" на границе слова; возвращает указатель на значение */
static const char *find_field(const char *msg, const char *key)
{
    size_t kl = strlen(key);
    for (const char *p = msg; *p; p++) {
        if ((p == msg || !isalnum((unsigned char)p[-1])) && strncasecmp(p, key, kl) == 0 &&
            p[kl] == '=')
            return p + kl + 1;
    }
    return NULL;
}

static void extract_level_field(const char *msg, log_entry *e)
{
    const char *v = find_field(msg, "level");
    if (!v) return;
    if (*v == '"') v++;
    size_t n = alpha_len(v);
    if (n == 0 || n >= LEVEL_MAX) return;
    e->severity = level_normalize(v, n, e->level, &e->level_known);
}

static void extract_trace_id(const char *msg, log_entry *e)
{
    static const char *KEYS[] = {"trace_id", "traceid", "trace-id", "trace"};
    for (size_t k = 0; k < sizeof KEYS / sizeof KEYS[0]; k++) {
        const char *v = find_field(msg, KEYS[k]);
        if (!v) continue;
        if (*v == '"') v++;
        size_t n = 0;
        while (n < TRACE_MAX - 1 && (isalnum((unsigned char)v[n]) || v[n] == '-' || v[n] == '_' ||
                                     v[n] == '.' || v[n] == ':'))
            n++;
        if (n == 0) continue;
        memcpy(e->trace_id, v, n);
        e->trace_id[n] = '\0';
        return;
    }
}

/* ---------- основной разбор ---------- */

int parse_line(const parse_ctx *ctx, char *line, log_entry *e)
{
    /* обрезаем \n, \r, хвостовые пробелы */
    size_t n = strlen(line);
    while (n && (line[n - 1] == '\n' || line[n - 1] == '\r' || line[n - 1] == ' '))
        line[--n] = '\0';
    if (n == 0) return -1;

    int pri_sev = -1;
    e->facility = -1;
    e->severity = -1;
    e->level_known = 0;
    e->level[0] = '\0';
    e->trace_id[0] = '\0';
    e->pid = -1;
    e->program = NULL;

    char *p = (char *)parse_pri(line, &e->facility, &pri_sev);

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

    /* уровень: <PRI> -> префикс сообщения -> поле level=; по словам в тексте НЕ угадываем */
    const char *lv;
    size_t lvn;
    if (pri_sev >= 0) {
        e->severity = pri_sev;
        e->level_known = 1;
        snprintf(e->level, LEVEL_MAX, "%s", SEV_NAMES[pri_sev]);
    } else if ((lvn = level_prefix(e->message, &lv)) > 0) {
        e->severity = level_normalize(lv, lvn, e->level, &e->level_known);
    } else {
        extract_level_field(e->message, e);
    }

    extract_trace_id(e->message, e);
    return 0;
}
