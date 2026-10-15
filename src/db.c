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
        PQfinish(c);
        return NULL;
    }
    /* служебные NOTICE ("relation already exists") не засоряют вывод */
    PQexec(c, "SET client_min_messages TO warning");
    return c;
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

/* ---------- источники ---------- */

int db_source_lock(PGconn *c, const char *path, source_info *out)
{
    const char *p[1] = {path};
    PGresult *r =
        PQexecParams(c, "INSERT INTO sources(path) VALUES ($1) ON CONFLICT (path) DO NOTHING", 1,
                     NULL, p, NULL, NULL, 0);
    if (PQresultStatus(r) != PGRES_COMMAND_OK) {
        fprintf(stderr, "db: %s", PQerrorMessage(c));
        PQclear(r);
        return -1;
    }
    PQclear(r);

    r = PQexecParams(
        c, "SELECT id, coalesce(inode, -1), last_offset FROM sources WHERE path = $1 FOR UPDATE", 1,
        NULL, p, NULL, NULL, 0);
    if (PQresultStatus(r) != PGRES_TUPLES_OK || PQntuples(r) != 1) {
        fprintf(stderr, "db: %s", PQerrorMessage(c));
        PQclear(r);
        return -1;
    }
    out->id = atoi(PQgetvalue(r, 0, 0));
    out->inode = atoll(PQgetvalue(r, 0, 1));
    out->offset = atoll(PQgetvalue(r, 0, 2));
    PQclear(r);
    return 0;
}

int db_source_update(PGconn *c, int id, long long inode, long long offset, long long added)
{
    char s_id[16], s_ino[24], s_off[24], s_add[24];
    snprintf(s_id, sizeof s_id, "%d", id);
    snprintf(s_ino, sizeof s_ino, "%lld", inode);
    snprintf(s_off, sizeof s_off, "%lld", offset);
    snprintf(s_add, sizeof s_add, "%lld", added);
    const char *p[4] = {s_id, s_ino, s_off, s_add};
    PGresult *r = PQexecParams(c,
                               "UPDATE sources SET inode = $2, last_offset = $3, "
                               "lines_total = lines_total + $4, updated_at = now() WHERE id = $1",
                               4, NULL, p, NULL, NULL, 0);
    int ok = PQresultStatus(r) == PGRES_COMMAND_OK;
    if (!ok) fprintf(stderr, "db: %s", PQerrorMessage(c));
    PQclear(r);
    return ok ? 0 : -1;
}

/* ---------- COPY ---------- */

struct db_copy {
    PGconn *c;
    char *buf;
    size_t len;
    int failed;
};

static int copy_flush(db_copy *cp)
{
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
    if (cp->len + 1 >= COPY_BUF_SIZE) copy_flush(cp);
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
    PGresult *r = PQexec(c, "COPY log_entries (ts, host, program, pid, severity, sev_inferred, "
                            "facility, message, source_id) FROM STDIN");
    if (PQresultStatus(r) != PGRES_COPY_IN) {
        fprintf(stderr, "db: COPY: %s", PQerrorMessage(c));
        PQclear(r);
        return NULL;
    }
    PQclear(r);
    db_copy *cp = calloc(1, sizeof *cp);
    cp->c = c;
    cp->buf = malloc(COPY_BUF_SIZE);
    return cp;
}

int db_copy_row(db_copy *cp, const log_entry *e, int source_id)
{
    char pid[16], sev[4], fac[4], src[16];
    snprintf(pid, sizeof pid, "%d", e->pid);
    snprintf(sev, sizeof sev, "%d", e->severity);
    snprintf(fac, sizeof fac, "%d", e->facility);
    snprintf(src, sizeof src, "%d", source_id);

    copy_field(cp, e->ts, 0);
    copy_field(cp, e->host, 0);
    copy_field(cp, e->program, 0);
    copy_field(cp, e->pid >= 0 ? pid : NULL, 0);
    copy_field(cp, sev, 0);
    copy_field(cp, e->sev_inferred ? "t" : "f", 0);
    copy_field(cp, e->facility >= 0 ? fac : NULL, 0);
    copy_field(cp, e->message, 0);
    copy_field(cp, source_id > 0 ? src : NULL, 1);
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

int db_insert_parse_error(PGconn *c, int source_id, long long line_no, const char *raw)
{
    char s_src[16], s_line[24];
    snprintf(s_src, sizeof s_src, "%d", source_id);
    snprintf(s_line, sizeof s_line, "%lld", line_no);
    const char *p[3] = {source_id > 0 ? s_src : NULL, s_line, raw};
    PGresult *r =
        PQexecParams(c, "INSERT INTO parse_errors(source_id, line_no, raw) VALUES ($1, $2, $3)", 3,
                     NULL, p, NULL, NULL, 0);
    int ok = PQresultStatus(r) == PGRES_COMMAND_OK;
    PQclear(r);
    return ok ? 0 : -1;
}

/* ---------- запросы ---------- */

int is_relative_time(const char *s)
{
    if (!s || !isdigit((unsigned char)*s)) return 0;
    while (isdigit((unsigned char)*s))
        s++;
    return (*s == 's' || *s == 'm' || *s == 'h' || *s == 'd' || *s == 'w') && s[1] == '\0';
}

/* Построитель запроса: значения пользователя идут только через $N (без SQL-инъекций) */
typedef struct {
    char sql[2048];
    const char *params[MAX_PARAMS];
    int n;
} qbuilder;

static void qb_add(qbuilder *q, const char *fmt, const char *value)
{
    char piece[256];
    q->params[q->n] = value;
    q->n++;
    snprintf(piece, sizeof piece, fmt, q->n);
    strncat(q->sql, piece, sizeof q->sql - strlen(q->sql) - 1);
}

static void qb_time(qbuilder *q, const char *op, const char *value)
{
    char fmt[96];
    if (is_relative_time(value))
        snprintf(fmt, sizeof fmt, " AND ts %s now() - $%%d::interval", op);
    else
        snprintf(fmt, sizeof fmt, " AND ts %s $%%d::timestamptz", op);
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

PGresult *db_search(PGconn *c, const search_filter *f)
{
    qbuilder q = {.n = 0};
    char sev[4], lim[16];
    snprintf(sev, sizeof sev, "%d", f->max_severity);
    snprintf(lim, sizeof lim, "%d", f->limit > 0 ? f->limit : 50);

    strcpy(q.sql, "SELECT * FROM ("
                  " SELECT to_char(ts, 'YYYY-MM-DD HH24:MI:SS') AS ts, host,"
                  " severity, coalesce(program, '-') AS program, pid, message, ts AS ts_raw"
                  " FROM log_entries WHERE true");
    if (f->max_severity >= 0) qb_add(&q, " AND severity <= $%d::smallint", sev);
    if (f->program) qb_add(&q, " AND program = $%d", f->program);
    if (f->host) qb_add(&q, " AND host = $%d", f->host);
    if (f->since) qb_time(&q, ">=", f->since);
    if (f->until) qb_time(&q, "<", f->until);
    if (f->grep) qb_add(&q, " AND message ILIKE '%%' || $%d || '%%'", f->grep);
    qb_add(&q, " ORDER BY ts_raw DESC, id DESC LIMIT $%d::int", lim);
    strncat(q.sql, ") t ORDER BY ts_raw", sizeof q.sql - strlen(q.sql) - 1);
    return run(c, &q);
}

PGresult *db_stats_summary(PGconn *c)
{
    qbuilder q = {.n = 0};
    strcpy(q.sql, "SELECT count(*), count(DISTINCT host), count(DISTINCT program),"
                  " coalesce(to_char(min(ts), 'YYYY-MM-DD HH24:MI'), '-'),"
                  " coalesce(to_char(max(ts), 'YYYY-MM-DD HH24:MI'), '-'),"
                  " (SELECT count(*) FROM parse_errors)"
                  " FROM log_entries");
    return run(c, &q);
}

PGresult *db_stats_severity(PGconn *c, const char *since)
{
    qbuilder q = {.n = 0};
    strcpy(q.sql, "SELECT s.code, s.name, count(e.id) FROM severities s"
                  " LEFT JOIN log_entries e ON e.severity = s.code");
    if (since) {
        /* фильтр переносим в условие JOIN, чтобы уровни с нулём тоже остались */
        if (is_relative_time(since))
            qb_add(&q, " AND e.ts >= now() - $%d::interval", since);
        else
            qb_add(&q, " AND e.ts >= $%d::timestamptz", since);
    }
    strcat(q.sql, " GROUP BY s.code, s.name ORDER BY s.code");
    return run(c, &q);
}

PGresult *db_stats_programs(PGconn *c, const char *since, int limit)
{
    qbuilder q = {.n = 0};
    char lim[16];
    snprintf(lim, sizeof lim, "%d", limit);
    strcpy(q.sql, "SELECT coalesce(program, '-'), count(*) FILTER (WHERE severity <= 3),"
                  " count(*) FILTER (WHERE severity = 4), count(*)"
                  " FROM log_entries WHERE true");
    if (since) qb_time(&q, ">=", since);
    qb_add(&q, " GROUP BY 1 ORDER BY 2 DESC, 4 DESC LIMIT $%d::int", lim);
    return run(c, &q);
}

PGresult *db_stats_hourly(PGconn *c, int hours)
{
    qbuilder q = {.n = 0};
    char h[16];
    snprintf(h, sizeof h, "%d", hours);
    /* опорная точка — последняя запись, чтобы график был осмысленным и на старых логах */
    strcpy(q.sql, "WITH last AS (SELECT date_trunc('hour', max(ts)) AS t FROM log_entries),"
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
