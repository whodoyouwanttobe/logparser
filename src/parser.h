#ifndef LOGPARSER_PARSER_H
#define LOGPARSER_PARSER_H

#include <stddef.h>

#define TS_MAX 48

/* Одна разобранная запись. Строковые поля указывают внутрь исходной строки
 * (parse_line модифицирует её на месте), кроме ts — он нормализован в буфер. */
typedef struct {
    char ts[TS_MAX]; /* "YYYY-MM-DD HH:MM:SS[.frac][tz]" — понятно PostgreSQL */
    const char *host;
    const char *program; /* NULL, если тега нет */
    int pid;             /* -1, если нет */
    int severity;        /* 0..7 */
    int sev_inferred;    /* 1 — уровень угадан эвристикой */
    int facility;        /* -1, если нет <PRI> */
    const char *message;
} log_entry;

/* Контекст для BSD-формата, где в метке времени нет года. */
typedef struct {
    int cur_year;
    int cur_month; /* 1..12 */
} parse_ctx;

void parse_ctx_init(parse_ctx *ctx);

/* Разбирает строку syslog. Поддерживаемые форматы:
 *   [<PRI>]Oct 10 12:34:56 host prog[pid]: message          (RFC 3164, /var/log/syslog)
 *   [<PRI>]2026-10-10T12:34:56.123+07:00 host prog[pid]: msg (rsyslog ISO, journalctl -o short-iso)
 * Возвращает 0 при успехе, -1 если строка не распознана. */
int parse_line(const parse_ctx *ctx, char *line, log_entry *out);

/* "err" / "warning" / "3" -> код; -1 при ошибке */
int severity_from_name(const char *name);
const char *severity_name(int code);

#endif
