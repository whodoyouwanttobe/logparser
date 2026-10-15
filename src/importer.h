#ifndef LOGPARSER_IMPORTER_H
#define LOGPARSER_IMPORTER_H

#include <libpq-fe.h>

typedef struct {
    long long lines_read;
    long long lines_ok;
    long long lines_bad;
    long long start_offset; /* с какого байта начали (для инкрементального импорта) */
    int rotated; /* 1 — файл сменился (новый inode), читали с начала */
} import_stats;

/* path == "-" => stdin (без запоминания позиции).
 * full != 0  => игнорировать сохранённую позицию и читать файл целиком. */
int import_file(PGconn *c, const char *path, int full, import_stats *st);

/* Разбор без БД: печатает разобранные поля (для отладки формата). */
int dry_run_file(const char *path, int max_lines);

#endif
