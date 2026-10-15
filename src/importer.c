#define _POSIX_C_SOURCE 200809L
#include "importer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

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

int import_file(PGconn *c, const char *path, int full, import_stats *st)
{
    memset(st, 0, sizeof *st);
    int from_stdin = strcmp(path, "-") == 0;

    FILE *f = open_input(path);
    if (!f) return -1;

    struct stat sb = {0};
    if (!from_stdin) fstat(fileno(f), &sb);

    if (db_exec(c, "BEGIN") != 0) goto fail_nofree;

    /* позиция чтения: дочитываем только новое, ротацию определяем по inode */
    source_info src = {.id = 0, .inode = -1, .offset = 0};
    if (!from_stdin) {
        if (db_source_lock(c, path, &src) != 0) goto fail;
        long long ino = (long long)sb.st_ino;
        if (full) {
            src.offset = 0;
        } else if (src.inode != ino || src.offset > (long long)sb.st_size) {
            st->rotated = src.inode >= 0;
            src.offset = 0;
        }
        src.inode = ino;
        if (src.offset > 0 && fseeko(f, (off_t)src.offset, SEEK_SET) != 0) goto fail;
    }
    st->start_offset = src.offset;

    parse_ctx ctx;
    parse_ctx_init(&ctx);

    db_copy *cp = db_copy_begin(c);
    if (!cp) goto fail;

    bad_line *bad = NULL;
    size_t nbad = 0;
    char *line = NULL, *copy = NULL;
    size_t cap = 0, copy_cap = 0;
    ssize_t len;
    long long offset = src.offset;
    log_entry e;
    int copy_failed = 0;

    while ((len = getline(&line, &cap, f)) > 0) {
        /* незавершённую последнюю строку (файл ещё пишется) не трогаем — заберём в следующий раз */
        if (!from_stdin && line[len - 1] != '\n') break;
        offset += len;
        st->lines_read++;

        /* parse_line портит строку, а для parse_errors нужен оригинал */
        if ((size_t)len + 1 > copy_cap) {
            copy_cap = (size_t)len + 1;
            copy = realloc(copy, copy_cap);
        }
        memcpy(copy, line, (size_t)len + 1);

        if (parse_line(&ctx, line, &e) == 0) {
            if (db_copy_row(cp, &e, src.id) != 0) {
                copy_failed = 1;
                break;
            }
            st->lines_ok++;
        } else {
            st->lines_bad++;
            copy[strcspn(copy, "\r\n")] = '\0';
            if (copy[0] && nbad < MAX_SAVED_ERRORS) {
                bad = realloc(bad, (nbad + 1) * sizeof *bad);
                bad[nbad].line_no = st->lines_read;
                bad[nbad].raw = strdup(copy);
                nbad++;
            }
        }
    }
    free(line);
    free(copy);

    int rc = db_copy_end(cp);
    if (copy_failed) rc = -1;

    /* ошибки разбора пишем уже после COPY (во время COPY другие запросы недоступны) */
    for (size_t i = 0; i < nbad; i++) {
        if (rc == 0) db_insert_parse_error(c, src.id, bad[i].line_no, bad[i].raw);
        free(bad[i].raw);
    }
    free(bad);

    if (rc != 0) goto fail;
    if (!from_stdin && db_source_update(c, src.id, src.inode, offset, st->lines_ok) != 0) goto fail;
    if (db_exec(c, "COMMIT") != 0) goto fail_nofree;

    if (!from_stdin) fclose(f);
    return 0;

fail:
    db_exec(c, "ROLLBACK"); /* всё или ничего: частично загруженного файла не бывает */
fail_nofree:
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

    while (getline(&line, &cap, f) > 0 && (max_lines <= 0 || n < max_lines)) {
        n++;
        if (parse_line(&ctx, line, &e) == 0) {
            ok++;
            printf("%-26s %-12s %-8s%s %-16s %6d | %s\n", e.ts, e.host, severity_name(e.severity),
                   e.sev_inferred ? "~" : " ", e.program ? e.program : "-", e.pid, e.message);
        } else {
            printf("!! не разобрана строка %lld\n", n);
        }
    }
    free(line);
    if (f != stdin) fclose(f);
    fprintf(stderr, "разобрано %lld из %lld строк  (~ = уровень определён эвристикой)\n", ok, n);
    return 0;
}
