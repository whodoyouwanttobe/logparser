#ifndef LOGPARSER_PARSER_H
#define LOGPARSER_PARSER_H

#include <stddef.h>

#define TS_MAX    48
#define LEVEL_MAX 16
#define TRACE_MAX 65

/* Одна разобранная запись. host/program/message указывают внутрь исходной строки
 * (parse_line модифицирует её на месте); ts, level, trace_id — собственные буферы. */
typedef struct {
    char ts[TS_MAX]; /* "YYYY-MM-DD HH:MM:SS[.frac][tz]" — понятно PostgreSQL */
    const char *host;
    const char *program;   /* NULL, если тега нет */
    int pid;               /* -1, если нет */
    char level[LEVEL_MAX]; /* уровень в нижнем регистре, как указан в логе; "" — не указан */
    int severity; /* числовой код 0..7 для известных уровней, -1 — неизвестен/нет */
    int level_known; /* 1 — уровень из стандартного набора (или его синонима) */
    int facility;    /* -1, если нет <PRI> */
    char trace_id[TRACE_MAX]; /* "" — нет trace_id */
    const char *message;
} log_entry;

/* Контекст для BSD-формата, где в метке времени нет года. */
typedef struct {
    int cur_year;
    int cur_month; /* 1..12 */
} parse_ctx;

void parse_ctx_init(parse_ctx *ctx);

/* Разбирает строку лога. Формат строки:
 *   [<PRI>]ВРЕМЯ ХОСТ ПРОГРАММА[PID]: СООБЩЕНИЕ
 * ВРЕМЯ — "Oct 10 12:34:56" (RFC 3164) или ISO 8601 "2026-10-10T12:34:56.123+07:00".
 *
 * Уровень берётся ТОЛЬКО из явной разметки (по словам в тексте он не угадывается):
 *   1) <PRI> в начале строки (стандарт syslog);
 *   2) в начале сообщения: "[ERROR] ...", "<warn> ...", "FATAL: ...", "error: ...";
 *   3) поле "level=warning" в сообщении.
 * Если уровня нет — level = "", severity = -1.
 * trace_id берётся из поля "trace_id=..." (также traceid=, trace-id=, trace=).
 *
 * Возвращает 0 при успехе, -1 если строка не распознана. */
int parse_line(const parse_ctx *ctx, char *line, log_entry *out);

/* Нормализует имя уровня: "ERR" -> "error", "Warn" -> "warning", "FATAL" -> "fatal".
 * В out пишется каноническое имя в нижнем регистре.
 * Возвращает числовой код 0..7 или -1 для нестандартного уровня; *known = 1, если уровень
 * из стандартного набора syslog или его синоним. */
int level_normalize(const char *name, size_t len, char out[LEVEL_MAX], int *known);

/* Для фильтра --level: "error" / "warn" / "3" -> код; -1 при ошибке */
int severity_from_name(const char *name);
const char *severity_name(int code); /* каноническое имя: 3 -> "error" */

#endif
