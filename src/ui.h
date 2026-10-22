#ifndef LOGPARSER_UI_H
#define LOGPARSER_UI_H

#include <libpq-fe.h>
#include "importer.h"

typedef enum { FMT_TABLE, FMT_CSV } out_format;

/* Цвета — забота интерфейса, а не БД: в базе хранится только уровень (level/severity). */
void ui_init(void); /* включает цвет, если stdout — терминал и не задан NO_COLOR */

void ui_print_entries(PGresult *r, out_format fmt, int with_offset);
void ui_print_trace_header(const char *trace_id, PGresult *r);
void ui_print_summary(PGresult *r);
void ui_print_levels(PGresult *r);
void ui_print_top(const char *title, const char *col, PGresult *r);
void ui_print_operations(PGresult *r);
void ui_print_hourly(PGresult *r);
void ui_print_sources(PGresult *r);
void ui_print_import(const char *path, const import_stats *st);

int ui_confirm(const char *question); /* [y/N]; 1 — да */

void ui_error(const char *fmt, ...);
void ui_warn(const char *fmt, ...);
void ui_ok(const char *fmt, ...);

#endif
