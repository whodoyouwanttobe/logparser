#ifndef LOGPARSER_UI_H
#define LOGPARSER_UI_H

#include <libpq-fe.h>
#include "importer.h"

typedef enum { FMT_TABLE, FMT_CSV } out_format;

void ui_init(void); /* включает цвет, если stdout — терминал и не задан NO_COLOR */

void ui_print_search(PGresult *r, out_format fmt);
void ui_print_summary(PGresult *r);
void ui_print_severity(PGresult *r);
void ui_print_programs(PGresult *r);
void ui_print_hourly(PGresult *r);
void ui_print_import(const char *path, const import_stats *st);

void ui_error(const char *fmt, ...);
void ui_ok(const char *fmt, ...);

#endif
