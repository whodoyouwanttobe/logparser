#define _POSIX_C_SOURCE 200809L
#include "db.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define COPY_BUF_SIZE (64 * 1024)
#define MAX_PARAMS    16

/* ---------- соединение ---------- */

PGconn *db_connect(const char *conninfo)
{
    PGconn *c = PQconnectdb(conninfo ? conninfo : "");
    if (PQstatus(c) != CONNECTION_OK) {
        fprintf(stderr, "db: не удалось подключиться: %s", PQerrorMessage(c));
        fprintf(stderr, "db: проверьте PGHOST/PGPORT/PGDATABASE/PGUSER/PGPASSWORD "
                        "или запущен ли PostgreSQL (docker compose up -d)\n");
        PQfinish(c);
        return NULL;
    }
    /* служебные NOTICE ("relation already exists") не засоряют вывод */
    PQclear(PQexec(c, "SET client_min_messages TO warning"));
    return c;
}

void db_print_target(PGconn *c)
{
    const char *host = PQhost(c);
    printf("подключено: %s:%s/%s (пользователь %s)\n", host && *host ? host : "localhost",
           PQport(c), PQdb(c), PQuser(c));
}

int db_exec(PGconn *c, const char *sql)
{
    PGresult *r = PQexec(c, sql);
    ExecStatusType st = PQresultStatus(r);
    int ok = (st == PGRES_COMMAND_OK || st == PGRES_TUPLES_OK);
    if (!ok) fprintf(stderr, "db: %s", PQerrorMessage(c));
    PQclear(r);
    return ok ? 0 : -1;
}

int db_apply_schema(PGconn *c, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        perror(path);
        return -1;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    char *sql = malloc((size_t)sz + 1);
    if (!sql) {
        fclose(f);
        return -1;
    }
    size_t rd = fread(sql, 1, (size_t)sz, f);
    sql[rd] = '\0';
    fclose(f);
    int rc = db_exec(c, sql); /* simple query protocol выполняет несколько операторов сразу */
    free(sql);
    return rc;
}

/* ---------- параметризованный запрос без результата ---------- */

static int exec_params(PGconn *c, const char *sql, int n, const char *const *params)
{
    PGresult *r = PQexecParams(c, sql, n, NULL, params, NULL, NULL, 0);
    ExecStatusType st = PQresultStatus(r);
    int ok = st == PGRES_COMMAND_OK || st == PGRES_TUPLES_OK;
    if (!ok) fprintf(stderr, "db: %s", PQerrorMessage(c));
    PQclear(r);
    return ok ? 0 : -1;
}

/* ---------- попытки импорта ---------- */

int db_source_start(PGconn *c, const char *path, int *id, int *attempt)
{
    /* Короткая отдельная транзакция: MAX+1 нельзя вычислять конкурентно.
     * Блокировка действует только при регистрации, не на время чтения файла. */
    if (db_exec(c, "BEGIN") != 0) return -1;
    if (db_exec(c, "LOCK TABLE sources IN SHARE ROW EXCLUSIVE MODE") != 0) {
        db_exec(c, "ROLLBACK");
        return -1;
    }
    const char *p[1] = {path};
    PGresult *r =
        PQexecParams(c,
                     "INSERT INTO sources(path, attempt) "
                     "SELECT $1, coalesce(max(attempt), 0) + 1 FROM sources WHERE path = $1 "
                     "RETURNING id, attempt",
                     1, NULL, p, NULL, NULL, 0);
    if (PQresultStatus(r) != PGRES_TUPLES_OK || PQntuples(r) != 1) {
        fprintf(stderr, "db: %s", PQerrorMessage(c));
        PQclear(r);
        db_exec(c, "ROLLBACK");
        return -1;
    }
    *id = atoi(PQgetvalue(r, 0, 0));
    *attempt = atoi(PQgetvalue(r, 0, 1));
    PQclear(r);
    if (db_exec(c, "COMMIT") != 0) {
        db_exec(c, "ROLLBACK");
        return -1;
    }
    return 0;
}

int db_source_finish(PGconn *c, int id, const char *status, const import_counts *n,
                     const char *error)
{
    char s_id[16], s_rd[24], s_in[24], s_dup[24], s_bad[24];
    snprintf(s_id, sizeof s_id, "%d", id);
    snprintf(s_rd, sizeof s_rd, "%lld", n->read);
    snprintf(s_in, sizeof s_in, "%lld", n->inserted);
    snprintf(s_dup, sizeof s_dup, "%lld", n->duplicate);
    snprintf(s_bad, sizeof s_bad, "%lld", n->bad);
    const char *p[7] = {s_id, status, s_rd, s_in, s_dup, s_bad, error};
    return exec_params(c,
                       "UPDATE sources SET status = $2, finished_at = now(), lines_read = $3, "
                       "lines_inserted = $4, lines_duplicate = $5, lines_bad = $6, error = $7 "
                       "WHERE id = $1",
                       7, p);
}

/* ---------- COPY во временную таблицу ---------- */

struct db_copy {
    PGconn *c;
    char *buf;
    size_t len;
    int failed;
};

static int copy_flush(db_copy *cp)
{
    if (cp->failed) return -1;
    if (cp->len == 0) return 0;
    if (PQputCopyData(cp->c, cp->buf, (int)cp->len) != 1) {
        fprintf(stderr, "db: COPY: %s", PQerrorMessage(cp->c));
        cp->failed = 1;
        return -1;
    }
    cp->len = 0;
    return 0;
}

static void copy_putc(db_copy *cp, char ch)
{
    if (cp->failed) return;
    if (cp->len + 1 >= COPY_BUF_SIZE && copy_flush(cp) != 0) return;
    cp->buf[cp->len++] = ch;
}

static void copy_puts(db_copy *cp, const char *s)
{
    while (*s)
        copy_putc(cp, *s++);
}

/* текстовый формат COPY: экранируем \, TAB, \n, \r; NULL = \N */
static void copy_field(db_copy *cp, const char *s, int last)
{
    if (!s) {
        copy_puts(cp, "\\N");
    } else {
        for (; *s; s++) {
            switch (*s) {
            case '\\':
                copy_puts(cp, "\\\\");
                break;
            case '\t':
                copy_puts(cp, "\\t");
                break;
            case '\n':
                copy_puts(cp, "\\n");
                break;
            case '\r':
                copy_puts(cp, "\\r");
                break;
            default:
                copy_putc(cp, *s);
            }
        }
    }
    copy_putc(cp, last ? '\n' : '\t');
}

db_copy *db_copy_begin(PGconn *c)
{
    if (db_exec(c, "CREATE TEMP TABLE stage (ts timestamptz, host text, program text, pid int, "
                   "level text, severity smallint, facility smallint, trace_id text, "
                   "message text) ON COMMIT DROP") != 0)
        return NULL;

    PGresult *r = PQexec(c, "COPY stage FROM STDIN");
    if (PQresultStatus(r) != PGRES_COPY_IN) {
        fprintf(stderr, "db: COPY: %s", PQerrorMessage(c));
        PQclear(r);
        return NULL;
    }
    PQclear(r);
    db_copy *cp = calloc(1, sizeof *cp);
    if (cp) cp->buf = malloc(COPY_BUF_SIZE);
    if (!cp || !cp->buf) {
        free(cp);
        PQputCopyEnd(c, "out of memory");
        while ((r = PQgetResult(c)) != NULL)
            PQclear(r);
        return NULL;
    }
    cp->c = c;
    return cp;
}

int db_copy_row(db_copy *cp, const log_entry *e)
{
    char pid[16], sev[4], fac[4];
    snprintf(pid, sizeof pid, "%d", e->pid);
    snprintf(sev, sizeof sev, "%d", e->severity);
    snprintf(fac, sizeof fac, "%d", e->facility);

    copy_field(cp, e->ts, 0);
    copy_field(cp, e->host, 0);
    copy_field(cp, e->program, 0);
    copy_field(cp, e->pid >= 0 ? pid : NULL, 0);
    copy_field(cp, e->level[0] ? e->level : NULL, 0);
    copy_field(cp, e->severity >= 0 ? sev : NULL, 0);
    copy_field(cp, e->facility >= 0 ? fac : NULL, 0);
    copy_field(cp, e->trace_id[0] ? e->trace_id : NULL, 0);
    copy_field(cp, e->message, 1);
    return cp->failed ? -1 : 0;
}

int db_copy_end(db_copy *cp)
{
    int rc = 0;
    copy_flush(cp);
    if (PQputCopyEnd(cp->c, cp->failed ? "client error" : NULL) != 1) rc = -1;
    PGresult *r;
    while ((r = PQgetResult(cp->c)) != NULL) {
        if (PQresultStatus(r) != PGRES_COMMAND_OK) {
            fprintf(stderr, "db: COPY: %s", PQerrorMessage(cp->c));
            rc = -1;
        }
        PQclear(r);
    }
    if (cp->failed) rc = -1;
    free(cp->buf);
    free(cp);
    return rc;
}

long long db_stage_commit(PGconn *c, int source_id)
{
    char src[16];
    snprintf(src, sizeof src, "%d", source_id);
    const char *p[1] = {src};
    PGresult *r =
        PQexecParams(c,
                     "INSERT INTO log_entries (ts, host, program, pid, level, severity, "
                     "facility, trace_id, message, source_id) "
                     "SELECT ts, host, program, pid, level, severity, facility, trace_id, "
                     "message, $1::int FROM stage ON CONFLICT DO NOTHING",
                     1, NULL, p, NULL, NULL, 0);
    if (PQresultStatus(r) != PGRES_COMMAND_OK) {
        fprintf(stderr, "db: %s", PQerrorMessage(c));
        PQclear(r);
        return -1;
    }
    long long n = atoll(PQcmdTuples(r));
    PQclear(r);
    return n;
}

int db_insert_parse_error(PGconn *c, int source_id, long long line_no, const char *raw)
{
    char s_src[16], s_line[24];
    snprintf(s_src, sizeof s_src, "%d", source_id);
    snprintf(s_line, sizeof s_line, "%lld", line_no);
    const char *p[3] = {s_src, s_line, raw};
    return exec_params(c, "INSERT INTO parse_errors(source_id, line_no, raw) VALUES ($1, $2, $3)",
                       3, p);
}

int db_clear(PGconn *c)
{
    return db_exec(c, "TRUNCATE log_entries, parse_errors, sources RESTART IDENTITY");
}

/* Возвращает число удалённых попыток, а не событий. */
long long db_purge(PGconn *c, const char *keep)
{
    const char *p[1] = {keep};
    PGresult *r = PQexecParams(c, "SELECT purge_logs($1::interval)", 1, NULL, p, NULL, NULL, 0);
    if (PQresultStatus(r) != PGRES_TUPLES_OK || PQntuples(r) != 1) {
        fprintf(stderr, "db: %s", PQerrorMessage(c));
        PQclear(r);
        return -1;
    }
    long long n = atoll(PQgetvalue(r, 0, 0));
    PQclear(r);
    return n;
}

/* ---------- построитель запросов ---------- */

int is_relative_time(const char *s)
{
    if (!s || !isdigit((unsigned char)*s)) return 0;
    while (isdigit((unsigned char)*s))
        s++;
    return (*s == 's' || *s == 'm' || *s == 'h' || *s == 'd' || *s == 'w') && s[1] == '\0';
}

/* значения пользователя идут только через $N (без SQL-инъекций) */
typedef struct {
    char sql[4096];
    const char *params[MAX_PARAMS];
    int n;
} qbuilder;

static void qb_raw(qbuilder *q, const char *s)
{
    strncat(q->sql, s, sizeof q->sql - strlen(q->sql) - 1);
}

static void qb_add(qbuilder *q, const char *fmt, const char *value)
{
    char piece[256];
    q->params[q->n] = value;
    q->n++;
    snprintf(piece, sizeof piece, fmt, q->n);
    qb_raw(q, piece);
}

static void qb_time(qbuilder *q, const char *col, const char *op, const char *value)
{
    char fmt[128];
    if (is_relative_time(value))
        snprintf(fmt, sizeof fmt, " AND %s %s now() - $%%d::interval", col, op);
    else
        snprintf(fmt, sizeof fmt, " AND %s %s $%%d::timestamptz", col, op);
    qb_add(q, fmt, value);
}

static PGresult *run(PGconn *c, const qbuilder *q)
{
    PGresult *r = PQexecParams(c, q->sql, q->n, NULL, q->params, NULL, NULL, 0);
    if (PQresultStatus(r) != PGRES_TUPLES_OK) {
        fprintf(stderr, "db: %s", PQerrorMessage(c));
        PQclear(r);
        return NULL;
    }
    return r;
}

#define ENTRY_COLS                                                                                 \
    "to_char(ts, 'YYYY-MM-DD HH24:MI:SS') AS ts_s, host, coalesce(level, '-') AS level, "          \
    "severity, coalesce(program, '-') AS program, pid, coalesce(trace_id, '') AS trace_id, "       \
    "message"

PGresult *db_search(PGconn *c, const search_filter *f)
{
    qbuilder q = {.n = 0};
    char sev[4], lim[16];
    snprintf(sev, sizeof sev, "%d", f->max_severity);
    snprintf(lim, sizeof lim, "%d", f->limit > 0 ? f->limit : 50);

    qb_raw(&q, "SELECT * FROM (SELECT " ENTRY_COLS ", '' AS offset_s, ts, id "
               "FROM log_entries WHERE true");
    if (f->max_severity >= 0) qb_add(&q, " AND severity <= $%d::smallint", sev);
    if (f->level && strcmp(f->level, "-") == 0)
        qb_raw(&q, " AND level IS NULL");
    else if (f->level)
        qb_add(&q, " AND level = lower($%d)", f->level);
    if (f->program) qb_add(&q, " AND program = $%d", f->program);
    if (f->host) qb_add(&q, " AND host = $%d", f->host);
    if (f->trace_id) qb_add(&q, " AND trace_id = $%d", f->trace_id);
    if (f->since) qb_time(&q, "ts", ">=", f->since);
    if (f->until) qb_time(&q, "ts", "<", f->until);
    if (f->grep) qb_add(&q, " AND message ILIKE '%%' || $%d || '%%'", f->grep);
    qb_add(&q, " ORDER BY ts DESC, id DESC LIMIT $%d::int", lim);
    qb_raw(&q, ") t ORDER BY ts, id");
    return run(c, &q);
}

PGresult *db_trace(PGconn *c, const char *trace_id)
{
    qbuilder q = {.n = 0};
    qb_raw(&q, "SELECT " ENTRY_COLS ", "
               "'+' || to_char(ts - min(ts) OVER (), 'HH24:MI:SS.MS') AS offset_s "
               "FROM log_entries WHERE trace_id = $1 ORDER BY ts, id");
    q.params[0] = trace_id;
    q.n = 1;
    return run(c, &q);
}

PGresult *db_stats_summary(PGconn *c)
{
    qbuilder q = {.n = 0};
    qb_raw(&q, "SELECT count(*), count(DISTINCT host), count(DISTINCT program),"
               " coalesce(to_char(min(ts), 'YYYY-MM-DD HH24:MI'), '-'),"
               " coalesce(to_char(max(ts), 'YYYY-MM-DD HH24:MI'), '-'),"
               " (SELECT count(DISTINCT raw) FROM parse_errors),"
               " count(DISTINCT trace_id),"
               " count(*) FILTER (WHERE level IS NULL)"
               " FROM log_entries");
    return run(c, &q);
}

/* level, severity, count — включая нестандартные уровни и записи без уровня */
PGresult *db_stats_levels(PGconn *c, const char *since)
{
    qbuilder q = {.n = 0};
    qb_raw(&q, "SELECT coalesce(level, '-'), min(severity), count(*) FROM log_entries WHERE true");
    if (since) qb_time(&q, "ts", ">=", since);
    qb_raw(&q, " GROUP BY level ORDER BY min(severity) NULLS LAST, level NULLS LAST");
    return run(c, &q);
}

/* программа, ошибок (severity <= 3), предупреждений, всего */
PGresult *db_stats_programs(PGconn *c, const char *since, int limit)
{
    qbuilder q = {.n = 0};
    char lim[16];
    snprintf(lim, sizeof lim, "%d", limit);
    qb_raw(&q, "SELECT coalesce(program, '-'), count(*) FILTER (WHERE severity <= 3),"
               " count(*) FILTER (WHERE severity = 4), count(*) FROM log_entries WHERE true");
    if (since) qb_time(&q, "ts", ">=", since);
    qb_add(&q, " GROUP BY 1 ORDER BY 2 DESC, 4 DESC, 1 LIMIT $%d::int", lim);
    return run(c, &q);
}

PGresult *db_stats_hosts(PGconn *c, const char *since, int limit)
{
    qbuilder q = {.n = 0};
    char lim[16];
    snprintf(lim, sizeof lim, "%d", limit);
    qb_raw(&q, "SELECT host, count(*) FILTER (WHERE severity <= 3),"
               " count(*) FILTER (WHERE severity = 4), count(*) FROM log_entries WHERE true");
    if (since) qb_time(&q, "ts", ">=", since);
    qb_add(&q, " GROUP BY 1 ORDER BY 2 DESC, 4 DESC, 1 LIMIT $%d::int", lim);
    return run(c, &q);
}

/* самые долгие операции: trace_id, начало, длительность, записей, ошибок, программы */
PGresult *db_stats_operations(PGconn *c, const char *since, int limit)
{
    qbuilder q = {.n = 0};
    char lim[16];
    snprintf(lim, sizeof lim, "%d", limit);
    qb_raw(&q, "SELECT trace_id, to_char(min(ts), 'YYYY-MM-DD HH24:MI:SS'),"
               " to_char(max(ts) - min(ts), 'HH24:MI:SS.MS'),"
               " count(*), count(*) FILTER (WHERE severity <= 3),"
               " string_agg(DISTINCT coalesce(program, '-'), ',')"
               " FROM log_entries WHERE trace_id IS NOT NULL");
    if (since) qb_time(&q, "ts", ">=", since);
    qb_add(&q,
           " GROUP BY trace_id ORDER BY max(ts) - min(ts) DESC, count(*) DESC"
           " LIMIT $%d::int",
           lim);
    return run(c, &q);
}

PGresult *db_stats_hourly(PGconn *c, int hours)
{
    qbuilder q = {.n = 0};
    char h[16];
    snprintf(h, sizeof h, "%d", hours);
    /* опорная точка — последняя запись, чтобы график был осмысленным и на старых логах */
    qb_raw(&q, "WITH last AS (SELECT date_trunc('hour', max(ts)) AS t FROM log_entries),"
               " hours AS (SELECT generate_series(last.t - ($1::int - 1) * interval '1 hour',"
               "  last.t, interval '1 hour') AS h FROM last)"
               " SELECT to_char(hours.h, 'MM-DD HH24:00'),"
               "  count(e.id) FILTER (WHERE e.severity <= 3),"
               "  count(e.id) FILTER (WHERE e.severity = 4),"
               "  count(e.id)"
               " FROM hours LEFT JOIN log_entries e"
               "  ON e.ts >= hours.h AND e.ts < hours.h + interval '1 hour'"
               " GROUP BY hours.h ORDER BY hours.h");
    q.params[0] = h;
    q.n = 1;
    return run(c, &q);
}

/* история попыток импорта */
PGresult *db_sources(PGconn *c, int limit)
{
    qbuilder q = {.n = 0};
    char lim[16];
    snprintf(lim, sizeof lim, "%d", limit);
    qb_add(&q,
           "SELECT * FROM (SELECT id, path, attempt, to_char(started_at, 'YYYY-MM-DD HH24:MI:SS'),"
           " status, lines_read, lines_inserted, lines_duplicate, lines_bad,"
           " coalesce(error, '') FROM sources ORDER BY id DESC LIMIT $%d::int) t ORDER BY id",
           lim);
    return run(c, &q);
}
