#define _POSIX_C_SOURCE 200809L
#include "ui.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "parser.h"

static int use_color = 0;

#define C_RESET  (use_color ? "\033[0m" : "")
#define C_BOLD   (use_color ? "\033[1m" : "")
#define C_DIM    (use_color ? "\033[2m" : "")
#define C_RED    (use_color ? "\033[31m" : "")
#define C_YELLOW (use_color ? "\033[33m" : "")
#define C_GREEN  (use_color ? "\033[32m" : "")
#define C_CYAN   (use_color ? "\033[36m" : "")
#define C_BGRED  (use_color ? "\033[1;41;97m" : "")

void ui_init(void)
{ use_color = isatty(STDOUT_FILENO) && getenv("NO_COLOR") == NULL; }

/* printf("%-Ns") считает байты, а кириллица в UTF-8 — 2 байта на символ.
 * Печатаем строку и добиваем пробелами по числу символов. */
static void upad(const char *s, int width, int right)
{
    int chars = 0;
    for (const char *p = s; *p; p++)
        if (((unsigned char)*p & 0xC0) != 0x80) chars++;
    if (right)
        for (int i = chars; i < width; i++)
            putchar(' ');
    fputs(s, stdout);
    if (!right)
        for (int i = chars; i < width; i++)
            putchar(' ');
}

/* цвет по числовому коду; для уровня без кода (NULL) — без цвета */
static const char *sev_color(const char *sev)
{
    if (!sev || !*sev) return "";
    int s = atoi(sev);
    if (s <= 2) return C_BGRED;
    if (s == 3) return C_RED;
    if (s == 4) return C_YELLOW;
    if (s == 5) return C_CYAN;
    if (s == 7) return C_DIM;
    return "";
}

static void vmsg(FILE *out, const char *color, const char *prefix, const char *fmt, va_list ap)
{
    int tty = isatty(fileno(out)) && getenv("NO_COLOR") == NULL;
    fprintf(out, "%s%s%s ", tty ? color : "", prefix, tty ? "\033[0m" : "");
    vfprintf(out, fmt, ap);
    fputc('\n', out);
}

void ui_error(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vmsg(stderr, "\033[31m", "ошибка:", fmt, ap);
    va_end(ap);
}

void ui_warn(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vmsg(stderr, "\033[33m", "внимание:", fmt, ap);
    va_end(ap);
}

void ui_ok(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vmsg(stdout, "\033[32m", "✔", fmt, ap);
    va_end(ap);
}

int ui_confirm(const char *question)
{
    printf("%s [y/N] ", question);
    fflush(stdout);
    char buf[16];
    if (!fgets(buf, sizeof buf, stdin)) return 0;
    return buf[0] == 'y' || buf[0] == 'Y' || strncmp(buf, "д", 2) == 0 || strncmp(buf, "Д", 2) == 0;
}

/* CSV: поле в кавычках, кавычки удваиваются */
static void csv_field(const char *s, int last)
{
    putchar('"');
    for (; *s; s++) {
        if (*s == '"') putchar('"');
        putchar(*s);
    }
    putchar('"');
    putchar(last ? '\n' : ',');
}

/* колонки: 0 ts, 1 host, 2 level, 3 severity, 4 program, 5 pid, 6 trace_id, 7 message, 8 offset */
void ui_print_entries(PGresult *r, out_format fmt, int with_offset)
{
    int n = PQntuples(r);
    if (fmt == FMT_CSV) {
        printf("ts,host,level,program,pid,trace_id,message\n");
        for (int i = 0; i < n; i++) {
            csv_field(PQgetvalue(r, i, 0), 0);
            csv_field(PQgetvalue(r, i, 1), 0);
            csv_field(PQgetvalue(r, i, 2), 0);
            csv_field(PQgetvalue(r, i, 4), 0);
            csv_field(PQgetvalue(r, i, 5), 0);
            csv_field(PQgetvalue(r, i, 6), 0);
            csv_field(PQgetvalue(r, i, 7), 1);
        }
        return;
    }

    if (n == 0) {
        printf("%sничего не найдено%s\n", C_DIM, C_RESET);
        return;
    }
    int any_trace = 0;
    for (int i = 0; i < n; i++)
        if (*PQgetvalue(r, i, 6)) any_trace = 1;

    fputs(C_BOLD, stdout);
    upad(with_offset ? "СМЕЩЕНИЕ" : "ВРЕМЯ", with_offset ? 15 : 21, 0);
    upad("ХОСТ", 10, 0);
    upad("УРОВЕНЬ", 10, 0);
    upad("ПРОГРАММА", 20, 0);
    if (any_trace && !with_offset) upad("TRACE", 10, 0);
    printf("СООБЩЕНИЕ%s\n", C_RESET);

    for (int i = 0; i < n; i++) {
        char prog[64];
        if (PQgetisnull(r, i, 5))
            snprintf(prog, sizeof prog, "%s", PQgetvalue(r, i, 4));
        else
            snprintf(prog, sizeof prog, "%s[%s]", PQgetvalue(r, i, 4), PQgetvalue(r, i, 5));
        const char *col = PQgetisnull(r, i, 3) ? "" : sev_color(PQgetvalue(r, i, 3));

        if (with_offset)
            printf("%s%-13.13s%s  ", C_DIM, PQgetvalue(r, i, 8), C_RESET);
        else
            printf("%s%-19s%s  ", C_DIM, PQgetvalue(r, i, 0), C_RESET);
        printf("%-8.8s  %s%-8.8s%s  %-18.18s  ", PQgetvalue(r, i, 1), col, PQgetvalue(r, i, 2),
               C_RESET, prog);
        if (any_trace && !with_offset) printf("%s%-8.8s%s  ", C_CYAN, PQgetvalue(r, i, 6), C_RESET);
        printf("%s\n", PQgetvalue(r, i, 7));
    }
    printf("%s— найдено: %d%s\n", C_DIM, n, C_RESET);
}

void ui_print_trace_header(const char *trace_id, PGresult *r)
{
    int n = PQntuples(r);
    if (n == 0) {
        printf("%sоперация trace_id=%s не найдена%s\n", C_DIM, trace_id, C_RESET);
        return;
    }
    int errors = 0;
    for (int i = 0; i < n; i++)
        if (!PQgetisnull(r, i, 3) && atoi(PQgetvalue(r, i, 3)) <= 3) errors++;
    printf("%sОперация %s%s: %s … %s, длительность %s, записей %d, ошибок %s%d%s\n\n", C_BOLD,
           trace_id, C_RESET, PQgetvalue(r, 0, 0), PQgetvalue(r, n - 1, 0),
           PQgetvalue(r, n - 1, 8) + 1, n, errors ? C_RED : "", errors, C_RESET);
}

static void bar(long long v, long long max, int width, const char *color)
{
    int w = max > 0 ? (int)((v * width + max - 1) / max) : 0;
    fputs(color, stdout);
    for (int i = 0; i < w; i++)
        fputs("█", stdout);
    fputs(C_RESET, stdout);
    for (int i = w; i < width; i++)
        putchar(' ');
}

void ui_print_summary(PGresult *r)
{
    printf("%sОбщая сводка%s\n", C_BOLD, C_RESET);
    printf("  записей:            %s\n", PQgetvalue(r, 0, 0));
    printf("  хостов / программ:  %s / %s\n", PQgetvalue(r, 0, 1), PQgetvalue(r, 0, 2));
    printf("  операций (trace):   %s\n", PQgetvalue(r, 0, 6));
    printf("  период:             %s … %s\n", PQgetvalue(r, 0, 3), PQgetvalue(r, 0, 4));
    printf("  без уровня:         %s\n", PQgetvalue(r, 0, 7));
    printf("  не разобрано:       %s (уникальных строк)\n\n", PQgetvalue(r, 0, 5));
}

/* level, severity, count */
void ui_print_levels(PGresult *r)
{
    int n = PQntuples(r);
    long long max = 0, total = 0;
    for (int i = 0; i < n; i++) {
        long long v = atoll(PQgetvalue(r, i, 2));
        if (v > max) max = v;
        total += v;
    }
    printf("%sПо уровням%s  %s(«-» — уровень не указан в логе)%s\n", C_BOLD, C_RESET, C_DIM,
           C_RESET);
    if (n == 0) printf("  %sнет данных%s\n", C_DIM, C_RESET);
    for (int i = 0; i < n; i++) {
        const char *sev = PQgetisnull(r, i, 1) ? NULL : PQgetvalue(r, i, 1);
        long long v = atoll(PQgetvalue(r, i, 2));
        int s = sev ? atoi(sev) : 6;
        printf("  %s", sev_color(sev));
        upad(PQgetvalue(r, i, 0), 10, 0);
        printf("%s ", C_RESET);
        bar(v, max, 30, s <= 3 ? C_RED : s == 4 ? C_YELLOW : C_GREEN);
        printf(" %7lld  %5.1f%%\n", v, total ? 100.0 * (double)v / (double)total : 0.0);
    }
    putchar('\n');
}

/* имя, ошибок, предупреждений, всего */
void ui_print_top(const char *title, const char *col, PGresult *r)
{
    int n = PQntuples(r);
    printf("%s%s%s\n", C_BOLD, title, C_RESET);
    printf("  %s", C_DIM);
    upad(col, 20, 0);
    upad("ошибок", 9, 1);
    upad("предупр.", 10, 1);
    upad("всего", 9, 1);
    printf("%s\n", C_RESET);
    for (int i = 0; i < n; i++) {
        long long errs = atoll(PQgetvalue(r, i, 1));
        printf("  %-20.20s %s%8lld%s %9s %8s\n", PQgetvalue(r, i, 0), errs ? C_RED : "", errs,
               C_RESET, PQgetvalue(r, i, 2), PQgetvalue(r, i, 3));
    }
    putchar('\n');
}

/* trace_id, начало, длительность, записей, ошибок, программы */
void ui_print_operations(PGresult *r)
{
    int n = PQntuples(r);
    printf("%sСамые долгие операции (trace_id)%s\n", C_BOLD, C_RESET);
    if (n == 0) {
        printf("  %sв логах нет trace_id%s\n\n", C_DIM, C_RESET);
        return;
    }
    printf("  %s", C_DIM);
    upad("trace_id", 14, 0);
    upad("начало", 21, 0);
    upad("длительность", 15, 0);
    upad("записей", 9, 1);
    upad("ошибок", 8, 1);
    printf("  программы%s\n", C_RESET);
    for (int i = 0; i < n; i++) {
        long long errs = atoll(PQgetvalue(r, i, 4));
        printf("  %s%-14.14s%s%-21s%-15s%7s %s%7lld%s  %s\n", C_CYAN, PQgetvalue(r, i, 0), C_RESET,
               PQgetvalue(r, i, 1), PQgetvalue(r, i, 2), PQgetvalue(r, i, 3), errs ? C_RED : "",
               errs, C_RESET, PQgetvalue(r, i, 5));
    }
    putchar('\n');
}

void ui_print_hourly(PGresult *r)
{
    int n = PQntuples(r);
    long long max = 0;
    for (int i = 0; i < n; i++) {
        long long v = atoll(PQgetvalue(r, i, 3));
        if (v > max) max = v;
    }
    printf("%sАктивность по часам%s  (%s█%s ошибки  %s█%s предупреждения  █ остальное)\n", C_BOLD,
           C_RESET, C_RED, C_RESET, C_YELLOW, C_RESET);
    const int W = 40;
    for (int i = 0; i < n; i++) {
        long long e = atoll(PQgetvalue(r, i, 1));
        long long w = atoll(PQgetvalue(r, i, 2));
        long long t = atoll(PQgetvalue(r, i, 3));
        int we = max ? (int)((e * W + max - 1) / max) : 0;
        int ww = max ? (int)((w * W + max - 1) / max) : 0;
        int wt = max ? (int)((t * W + max - 1) / max) : 0;
        if (we + ww > wt) wt = we + ww;
        printf("  %s  ", PQgetvalue(r, i, 0));
        fputs(C_RED, stdout);
        for (int k = 0; k < we; k++)
            fputs("█", stdout);
        fputs(C_YELLOW, stdout);
        for (int k = 0; k < ww; k++)
            fputs("█", stdout);
        fputs(C_RESET, stdout);
        for (int k = we + ww; k < wt; k++)
            fputs("█", stdout);
        for (int k = wt; k < W; k++)
            putchar(' ');
        printf(" %lld", t);
        if (e) printf(" %s(%lld err)%s", C_RED, e, C_RESET);
        putchar('\n');
    }
}

/* id, path, attempt, started, status, read, inserted, duplicate, bad, error */
void ui_print_sources(PGresult *r)
{
    int n = PQntuples(r);
    if (n == 0) {
        printf("%sимпортов ещё не было%s\n", C_DIM, C_RESET);
        return;
    }
    fputs(C_BOLD, stdout);
    upad("№", 5, 0);
    upad("ФАЙЛ", 26, 0);
    upad("ПОП.", 6, 0);
    upad("НАЧАЛО", 21, 0);
    upad("СТАТУС", 9, 0);
    upad("новых", 7, 1);
    upad("дублей", 8, 1);
    upad("ошибок", 8, 1);
    printf("%s\n", C_RESET);
    for (int i = 0; i < n; i++) {
        const char *status = PQgetvalue(r, i, 4);
        const char *col = strcmp(status, "ok") == 0       ? C_GREEN
                          : strcmp(status, "failed") == 0 ? C_RED
                                                          : C_YELLOW;
        printf("%-5s%-26.26s%-6s%-21s%s%-9s%s%7s %7s %7s", PQgetvalue(r, i, 0), PQgetvalue(r, i, 1),
               PQgetvalue(r, i, 2), PQgetvalue(r, i, 3), col, status, C_RESET, PQgetvalue(r, i, 6),
               PQgetvalue(r, i, 7), PQgetvalue(r, i, 8));
        if (*PQgetvalue(r, i, 9)) printf("  %s%s%s", C_RED, PQgetvalue(r, i, 9), C_RESET);
        putchar('\n');
    }
}

void ui_print_import(const char *path, const import_stats *st)
{
    ui_ok("%s (попытка %d): новых %lld, дубликатов пропущено %lld, не разобрано %lld "
          "(прочитано строк: %lld)",
          path, st->attempt, st->n.inserted, st->n.duplicate, st->n.bad, st->n.read);
    fflush(stdout);
    if (st->with_trace) printf("  %sстрок с trace_id: %lld%s\n", C_DIM, st->with_trace, C_RESET);
    if (st->no_level)
        ui_warn("без указанного уровня строк: %lld — сохранены с уровнем «-»", st->no_level);
    for (int i = 0; i < st->odd_n; i++)
        ui_warn("нестандартный уровень «%s» (строк: %lld) — сохранён как есть, без числового кода; "
                "фильтр: search --level-is %s",
                st->odd[i].name, st->odd[i].count, st->odd[i].name);
}
