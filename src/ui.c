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

static const char *sev_color(int sev)
{
    if (sev <= 2) return C_BGRED;
    if (sev == 3) return C_RED;
    if (sev == 4) return C_YELLOW;
    if (sev == 5) return C_CYAN;
    if (sev == 7) return C_DIM;
    return "";
}

void ui_error(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "%sошибка:%s ", isatty(STDERR_FILENO) ? "\033[31m" : "",
            isatty(STDERR_FILENO) ? "\033[0m" : "");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

void ui_ok(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    printf("%s✔%s ", C_GREEN, C_RESET);
    vprintf(fmt, ap);
    putchar('\n');
    va_end(ap);
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

/* колонки результата db_search: ts, host, severity, program, pid, message */
void ui_print_search(PGresult *r, out_format fmt)
{
    int n = PQntuples(r);
    if (fmt == FMT_CSV) {
        printf("ts,host,severity,program,pid,message\n");
        for (int i = 0; i < n; i++) {
            csv_field(PQgetvalue(r, i, 0), 0);
            csv_field(PQgetvalue(r, i, 1), 0);
            csv_field(severity_name(atoi(PQgetvalue(r, i, 2))), 0);
            csv_field(PQgetvalue(r, i, 3), 0);
            csv_field(PQgetvalue(r, i, 4), 0);
            csv_field(PQgetvalue(r, i, 5), 1);
        }
        return;
    }

    if (n == 0) {
        printf("%sничего не найдено%s\n", C_DIM, C_RESET);
        return;
    }
    fputs(C_BOLD, stdout);
    upad("ВРЕМЯ", 21, 0);
    upad("ХОСТ", 14, 0);
    upad("УРОВЕНЬ", 9, 0);
    upad("ПРОГРАММА", 24, 0);
    printf("СООБЩЕНИЕ%s\n", C_RESET);
    for (int i = 0; i < n; i++) {
        int sev = atoi(PQgetvalue(r, i, 2));
        char prog[64];
        if (PQgetisnull(r, i, 4))
            snprintf(prog, sizeof prog, "%s", PQgetvalue(r, i, 3));
        else
            snprintf(prog, sizeof prog, "%s[%s]", PQgetvalue(r, i, 3), PQgetvalue(r, i, 4));
        printf("%s%-19s%s  %-12.12s  %s%-7s%s  %-22.22s  %s\n", C_DIM, PQgetvalue(r, i, 0), C_RESET,
               PQgetvalue(r, i, 1), sev_color(sev), severity_name(sev), C_RESET, prog,
               PQgetvalue(r, i, 5));
    }
    printf("%s— найдено: %d%s\n", C_DIM, n, C_RESET);
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
    printf("  записей:          %s\n", PQgetvalue(r, 0, 0));
    printf("  хостов / программ: %s / %s\n", PQgetvalue(r, 0, 1), PQgetvalue(r, 0, 2));
    printf("  период:           %s … %s\n", PQgetvalue(r, 0, 3), PQgetvalue(r, 0, 4));
    printf("  не разобрано:     %s строк\n\n", PQgetvalue(r, 0, 5));
}

void ui_print_severity(PGresult *r)
{
    int n = PQntuples(r);
    long long max = 0, total = 0;
    for (int i = 0; i < n; i++) {
        long long v = atoll(PQgetvalue(r, i, 2));
        if (v > max) max = v;
        total += v;
    }
    printf("%sПо уровням важности%s\n", C_BOLD, C_RESET);
    for (int i = 0; i < n; i++) {
        int code = atoi(PQgetvalue(r, i, 0));
        long long v = atoll(PQgetvalue(r, i, 2));
        printf("  %s%-8s%s ", sev_color(code), PQgetvalue(r, i, 1), C_RESET);
        bar(v, max, 30, code <= 3 ? C_RED : code == 4 ? C_YELLOW : C_GREEN);
        printf(" %7lld  %5.1f%%\n", v, total ? 100.0 * (double)v / (double)total : 0.0);
    }
    putchar('\n');
}

void ui_print_programs(PGresult *r)
{
    int n = PQntuples(r);
    printf("%sТоп программ по ошибкам%s\n", C_BOLD, C_RESET);
    printf("  %s", C_DIM);
    upad("программа", 20, 0);
    upad("err+", 9, 1);
    upad("warning", 9, 1);
    upad("всего", 9, 1);
    printf("%s\n", C_RESET);
    for (int i = 0; i < n; i++) {
        long long errs = atoll(PQgetvalue(r, i, 1));
        printf("  %-20.20s %s%8lld%s %8s %8s\n", PQgetvalue(r, i, 0), errs ? C_RED : "", errs,
               C_RESET, PQgetvalue(r, i, 2), PQgetvalue(r, i, 3));
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

void ui_print_import(const char *path, const import_stats *st)
{
    ui_ok("%s: загружено %lld, не разобрано %lld (прочитано строк: %lld)%s", path, st->lines_ok,
          st->lines_bad, st->lines_read,
          st->rotated ? " — обнаружена ротация, файл прочитан заново"
                      : (st->start_offset > 0 ? " — дочитаны только новые строки" : ""));
}
