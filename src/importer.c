#define _POSIX_C_SOURCE 200809L
#include "importer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "db.h"
#include "parser.h"

#define MAX_SAVED_ERRORS 1000

typedef struct {
    long long line_no;
    char *raw;
} bad_line;

static FILE *open_input(const char *path)
{
    if (strcmp(path, "-") == 0) return stdin;
    FILE *f = fopen(path, "r");
    if (!f) perror(path);
    return f;
}

static void count_odd_level(import_stats *st, const char *level)
{
    for (int i = 0; i < st->odd_n; i++) {
        if (strcmp(st->odd[i].name, level) == 0) {
            st->odd[i].count++;
            return;
        }
    }
    if (st->odd_n < MAX_ODD_LEVELS) {
        snprintf(st->odd[st->odd_n].name, LEVEL_MAX, "%s", level);
        st->odd[st->odd_n].count = 1;
        st->odd_n++;
    }
}

int import_file(PGconn *c, const char *path, import_stats *st)
{
    memset(st, 0, sizeof *st);
    int from_stdin = strcmp(path, "-") == 0;

    FILE *f = open_input(path);
    if (!f) return -1;

    /* попытка фиксируется отдельно и до начала загрузки: в истории останется и неудачная */
    if (db_source_start(c, from_stdin ? "<stdin>" : path, &st->source_id, &st->attempt) != 0) {
        if (!from_stdin) fclose(f);
        return -1;
    }
    const char *error = "ошибка загрузки в БД";

    if (db_exec(c, "BEGIN") != 0) goto fail;

    db_copy *cp = db_copy_begin(c);
    if (!cp) goto fail;

    parse_ctx ctx;
    parse_ctx_init(&ctx);

    bad_line *bad = NULL;
    size_t nbad = 0;
    char *line = NULL, *copy = NULL;
    size_t cap = 0, copy_cap = 0;
    ssize_t len;
    log_entry e;
    int copy_failed = 0;
    long long parsed = 0;

    while ((len = getline(&line, &cap, f)) > 0) {
        st->n.read++;

        /* parse_line портит строку, а для parse_errors нужен оригинал */
        if ((size_t)len + 1 > copy_cap) {
            copy_cap = (size_t)len + 1;
            copy = realloc(copy, copy_cap);
        }
        memcpy(copy, line, (size_t)len + 1);

        if (parse_line(&ctx, line, &e) == 0) {
            if (db_copy_row(cp, &e) != 0) {
                copy_failed = 1;
                break;
            }
            parsed++;
            if (!e.level[0])
                st->no_level++;
            else if (!e.level_known)
                count_odd_level(st, e.level);
            if (e.trace_id[0]) st->with_trace++;
        } else {
            copy[strcspn(copy, "\r\n")] = '\0';
            if (copy[0] == '\0') continue; /* пустые строки не считаем ошибкой */
            st->n.bad++;
            if (nbad < MAX_SAVED_ERRORS) {
                bad = realloc(bad, (nbad + 1) * sizeof *bad);
                bad[nbad].line_no = st->n.read;
                bad[nbad].raw = strdup(copy);
                nbad++;
            }
        }
    }
    free(line);
    free(copy);

    int rc = db_copy_end(cp);
    if (copy_failed) rc = -1;

    if (rc == 0) {
        long long inserted = db_stage_commit(c, st->source_id);
        if (inserted < 0) {
            rc = -1;
        } else {
            st->n.inserted = inserted;
            st->n.duplicate = parsed - inserted;
        }
    }

    /* ошибки разбора пишем после COPY (во время COPY другие запросы недоступны) */
    for (size_t i = 0; i < nbad; i++) {
        if (rc == 0 && db_insert_parse_error(c, st->source_id, bad[i].line_no, bad[i].raw) != 0)
            rc = -1;
        free(bad[i].raw);
    }
    free(bad);

    if (rc != 0) goto fail;
    if (db_exec(c, "COMMIT") != 0) goto fail;

    db_source_finish(c, st->source_id, "ok", &st->n, NULL);
    if (!from_stdin) fclose(f);
    return 0;

fail:;
    /* текст ошибки PostgreSQL сохраняем в sources.error, чтобы было видно, почему упало */
    char errbuf[512];
    snprintf(errbuf, sizeof errbuf, "%s", PQerrorMessage(c));
    errbuf[strcspn(errbuf, "\n")] = '\0';
    if (errbuf[0]) error = errbuf;
    db_exec(c, "ROLLBACK"); /* всё или ничего: частично загруженного файла не бывает */
    st->n.inserted = st->n.duplicate = 0;
    db_source_finish(c, st->source_id, "failed", &st->n, error);
    if (!from_stdin) fclose(f);
    return -1;
}

int dry_run_file(const char *path, int max_lines)
{
    FILE *f = open_input(path);
    if (!f) return -1;

    parse_ctx ctx;
    parse_ctx_init(&ctx);
    char *line = NULL;
    size_t cap = 0;
    long long n = 0, ok = 0;
    log_entry e;

    while ((max_lines <= 0 || n < max_lines) && getline(&line, &cap, f) > 0) {
        n++;
        if (parse_line(&ctx, line, &e) == 0) {
            ok++;
            printf("%-26s %-8s %-9s %-14s %6d %-10s | %s\n", e.ts, e.host,
                   e.level[0] ? e.level : "-", e.program ? e.program : "-", e.pid,
                   e.trace_id[0] ? e.trace_id : "-", e.message);
        } else {
            printf("!! строка %lld не разобрана\n", n);
        }
    }
    free(line);
    if (f != stdin) fclose(f);
    fprintf(stderr, "разобрано %lld из %lld строк\n", ok, n);
    return 0;
}
