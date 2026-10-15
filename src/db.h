#ifndef LOGPARSER_DB_H
#define LOGPARSER_DB_H

#include <libpq-fe.h>
#include "parser.h"

/* ---- соединение и транзакции ---- */
PGconn *db_connect(const char *conninfo); /* NULL/"" => переменные PGHOST, PGDATABASE, ... */
int db_exec(PGconn *c, const char *sql); /* 0 при успехе */
int db_apply_schema(PGconn *c, const char *path);

/* ---- источники ---- */
typedef struct {
    int id;
    long long inode;
    long long offset;
} source_info;

int db_source_lock(PGconn *c, const char *path,
                   source_info *out); /* создаёт при отсутствии, SELECT ... FOR UPDATE */
int db_source_update(PGconn *c, int id, long long inode, long long offset, long long added);

/* ---- массовая загрузка через COPY (в разы быстрее INSERT) ---- */
typedef struct db_copy db_copy;
db_copy *db_copy_begin(PGconn *c);
int db_copy_row(db_copy *cp, const log_entry *e, int source_id);
int db_copy_end(db_copy *cp); /* освобождает cp; 0 при успехе */

int db_insert_parse_error(PGconn *c, int source_id, long long line_no, const char *raw);

/* ---- запросы для CLI ---- */
typedef struct {
    int max_severity; /* -1 => любой */
    const char *program;
    const char *host;
    const char *since; /* "2026-10-10 12:00" или относительное "1h", "2d" */
    const char *until;
    const char *grep; /* подстрока в message, без учёта регистра */
    int limit;
} search_filter;

PGresult *db_search(PGconn *c, const search_filter *f);
PGresult *db_stats_severity(PGconn *c, const char *since);
PGresult *db_stats_programs(PGconn *c, const char *since, int limit);
PGresult *db_stats_hourly(PGconn *c, int hours);
PGresult *db_stats_summary(PGconn *c);

int is_relative_time(const char *s);

#endif
