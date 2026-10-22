#ifndef LOGPARSER_IMPORTER_H
#define LOGPARSER_IMPORTER_H

#include <libpq-fe.h>
#include "db.h"

#define MAX_ODD_LEVELS 16

typedef struct {
    char name[LEVEL_MAX];
    long long count;
} level_count;

typedef struct {
    int source_id;
    int attempt; /* какая по счёту загрузка этого файла */
    import_counts n; /* прочитано / новых / дубликатов / не разобрано */
    long long no_level;   /* строк без явного уровня */
    long long with_trace; /* строк с trace_id */
    level_count odd[MAX_ODD_LEVELS]; /* нестандартные уровни (сохранены как есть) */
    int odd_n;
} import_stats;

/* path == "-" => stdin. Каждый вызов — новая попытка в таблице sources;
 * повторная загрузка того же файла не создаёт дубликатов. */
int import_file(PGconn *c, const char *path, import_stats *st);

/* Разбор без БД: печатает разобранные поля (для отладки формата). */
int dry_run_file(const char *path, int max_lines);

#endif
