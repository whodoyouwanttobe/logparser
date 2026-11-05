#include "view.h"
#include "db.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum { PAGE = 8, LIMIT = 100 };

/* Logs are data: never send embedded terminal commands to the screen. */
static int field(const char *s, int limit)
{
    const unsigned char *p = (const unsigned char *)s;
    int width = 0;
    while (*p && limit-- > 0) {
        unsigned char ch = *p++;
        ++width;
        if (ch < 32 || ch == 127) {
            putchar(' ');
            continue;
        }
        if (ch == 0xc2 && *p >= 0x80 && *p <= 0x9f) {
            ++p;
            putchar(' ');
            continue;
        }
        putchar(ch);
        while ((*p & 0xc0) == 0x80)
            putchar(*p++);
    }
    if (*p) {
        fputs("…", stdout);
        ++width;
    }
    return width;
}

static void column(const char *text, int width)
{
    int used = field(text, width - 1);
    while (used++ < width)
        putchar(' ');
    fputs("  ", stdout);
}

int view_logs(PGconn *c)
{
    search_filter f = {.max_severity = -1, .limit = LIMIT};
    PGresult *rows = db_search(c, &f), *trace = NULL;
    char line[256], query[256] = "", trace_id[256] = "";
    const char *notice = "";
    int page = 0, saved = 0, rc = 0;
    while (rows) {
        PGresult *r = trace ? trace : rows;
        int n = PQntuples(r), pages = n ? (n - 1) / PAGE + 1 : 1;
        if (isatty(STDOUT_FILENO) && getenv("TERM") && strcmp(getenv("TERM"), "dumb"))
            fputs("\033[2J\033[H", stdout);
        printf("LogParser  |  %s", trace ? "Операция " : "Журнал");
        if (trace)
            field(trace_id, 80);
        else {
            printf("  |  %s", f.max_severity == 3 ? "ошибки" : "все уровни");
            if (*query) {
                fputs("  |  поиск: ", stdout);
                field(query, 50);
            }
        }
        printf("\nЗаписей: %d  |  Страница %d/%d\n\n", n, page + 1, pages);
        fputs("    №   ", stdout);
        column(trace ? "ОТ НАЧАЛА" : "ВРЕМЯ", trace ? 14 : 20);
        column("УРОВЕНЬ", 10);
        column("ПРОГРАММА", 16);
        puts("СООБЩЕНИЕ");
        for (int i = page * PAGE; i < n && i < (page + 1) * PAGE; ++i) {
            int error = !PQgetisnull(r, i, 3) && atoi(PQgetvalue(r, i, 3)) <= 3;
            printf("%s  %2d   ", error ? "!" : " ", i - page * PAGE + 1);
            column(PQgetvalue(r, i, trace ? 8 : 0), trace ? 14 : 20);
            column(PQgetvalue(r, i, 2), 10);
            column(PQgetvalue(r, i, 4), 16);
            field(PQgetvalue(r, i, 7), 90);
            putchar('\n');
        }
        if (!n) puts("Записей не найдено.");
        printf("\n%s\nn/p страницы | q выход | %s\n> ", notice,
               trace ? "b назад"
                     : "номер: открыть операцию | e: ошибки/все | /текст: поиск | /: сброс");
        fflush(stdout);
        notice = "";
        if (!fgets(line, sizeof line, stdin)) break;
        if (!strchr(line, '\n') && !feof(stdin)) {
            int ch;
            while ((ch = getchar()) != '\n' && ch != EOF) {}
            notice = "Слишком длинная команда.";
            continue;
        }
        line[strcspn(line, "\r\n")] = 0;
        if (!strcmp(line, "q")) break;
        if (!strcmp(line, "n")) {
            if (page + 1 < pages) ++page;
            continue;
        }
        if (!strcmp(line, "p")) {
            if (page) --page;
            continue;
        }
        if (trace && !strcmp(line, "b")) {
            PQclear(trace);
            trace = NULL;
            page = saved;
            continue;
        }
        if (!trace && (!strcmp(line, "e") || line[0] == '/')) {
            if (line[0] == 'e')
                f.max_severity = f.max_severity == 3 ? -1 : 3;
            else {
                strcpy(query, line + 1);
                f.grep = *query ? query : NULL;
            }
            PQclear(rows);
            rows = db_search(c, &f);
            page = 0;
            continue;
        }
        if (!trace && line[0] >= '1' && line[0] <= '8' && !line[1]) {
            int i = page * PAGE + line[0] - '1';
            if (i >= n) {
                notice = "Такой строки нет.";
                continue;
            }
            const char *id = PQgetvalue(rows, i, 6);
            if (!*id) {
                notice = "У этой записи нет trace_id.";
                continue;
            }
            snprintf(trace_id, sizeof trace_id, "%s", id);
            trace = db_trace(c, id);
            if (!trace) {
                rc = 1;
                break;
            }
            saved = page;
            page = 0;
            continue;
        }
        notice = "Выбери команду из подсказки.";
    }
    if (!rows) rc = 1;
    PQclear(trace);
    PQclear(rows);
    return rc;
}
