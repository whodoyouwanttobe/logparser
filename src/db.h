#ifndef LOGPARSER_DB_H
#define LOGPARSER_DB_H

#include <libpq-fe.h>
#include "parser.h"

/* ---- соединение и транзакции ----
 * Параметры подключения libpq берёт из conninfo ("host=... port=... dbname=...") или,
 * если conninfo = NULL/"", из переменных окружения PGHOST, PGPORT, PGDATABASE, PGUSER,
 * PGPASSWORD. Так можно выбрать нужный сервер PostgreSQL без изменения кода. */
PGconn *db_connect(const char *conninfo);
int db_exec(PGconn *c, const char *sql); /* 0 при успехе */
int db_apply_schema(PGconn *c, const char *path);
void db_print_target(PGconn *c); /* куда подключились: host:port/dbname */

/* ---- попытки импорта (таблица sources) ---- */
typedef struct {
    long long read;
    long long inserted;
    long long duplicate;
    long long bad;
} import_counts;

/* Регистрирует попытку отдельной транзакцией, чтобы она сохранилась даже при ошибке импорта. */
int db_source_start(PGconn *c, const char *path, int *id, int *attempt);
int db_source_finish(PGconn *c, int id, const char *status, const import_counts *n,
                     const char *error);

/* ---- массовая загрузка ----
 * Строки идут через COPY во временную таблицу, затем
 * INSERT ... SELECT ... ON CONFLICT DO NOTHING — так дубликаты отсекает уникальный индекс. */
typedef struct db_copy db_copy;
db_copy *db_copy_begin(PGconn *c); /* вызывать внутри BEGIN */
int db_copy_row(db_copy *cp, const log_entry *e);
int db_copy_end(db_copy *cp); /* освобождает cp; 0 при успехе */
long long db_stage_commit(PGconn *c, int source_id); /* число новых записей или -1 */

int db_insert_parse_error(PGconn *c, int source_id, long long line_no, const char *raw);
int db_clear(PGconn *c); /* удалить все данные, структуру сохранить */

/* ---- запросы для CLI ---- */
typedef struct {
    int max_severity;  /* -1 => любой; иначе severity <= max_severity */
    const char *level; /* точное имя уровня (fatal, trace, ...) или "-" для записей без уровня */
    const char *program;
    const char *host;
    const char *trace_id;
    const char *since; /* "2026-10-10 12:00" или относительное "30m", "2h", "7d" */
    const char *until;
    const char *grep; /* подстрока в message, без учёта регистра */
    int limit;
} search_filter;

/* Колонки результатов db_search и db_trace:
 *   0 ts, 1 host, 2 level, 3 severity, 4 program, 5 pid, 6 trace_id, 7 message, 8 +offset */
PGresult *db_search(PGconn *c, const search_filter *f);
PGresult *db_trace(PGconn *c, const char *trace_id);

PGresult *db_stats_summary(PGconn *c);
PGresult *db_stats_levels(PGconn *c, const char *since);
PGresult *db_stats_programs(PGconn *c, const char *since, int limit);
PGresult *db_stats_hosts(PGconn *c, const char *since, int limit);
PGresult *db_stats_operations(PGconn *c, const char *since, int limit);
PGresult *db_stats_hourly(PGconn *c, int hours);
PGresult *db_sources(PGconn *c, int limit);

int is_relative_time(const char *s);

#endif
