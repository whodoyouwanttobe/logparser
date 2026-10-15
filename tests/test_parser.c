/* Unit-тесты парсера. Запуск: make test */
#include <stdio.h>
#include <string.h>

#include "parser.h"

static int failures = 0, total = 0;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        total++;                                                                                   \
        if (!(cond)) {                                                                             \
            failures++;                                                                            \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                        \
        }                                                                                          \
    } while (0)

#define STREQ(a, b) ((a) && strcmp((a), (b)) == 0)

static parse_ctx ctx = {.cur_year = 2026, .cur_month = 10};

static void test_bsd_with_pid(void)
{
    char line[] = "Oct 10 08:05:12 srv01 sshd[2301]: Failed password for root\n";
    log_entry e;
    CHECK(parse_line(&ctx, line, &e) == 0);
    CHECK(STREQ(e.ts, "2026-10-10 08:05:12"));
    CHECK(STREQ(e.host, "srv01"));
    CHECK(STREQ(e.program, "sshd"));
    CHECK(e.pid == 2301);
    CHECK(STREQ(e.message, "Failed password for root"));
    CHECK(e.severity == 3 && e.sev_inferred == 1);
}

static void test_bsd_single_digit_day_no_pid(void)
{
    char line[] = "Oct  1 00:00:01 host kernel: [ 1.0] usb connected";
    log_entry e;
    CHECK(parse_line(&ctx, line, &e) == 0);
    CHECK(STREQ(e.ts, "2026-10-01 00:00:01"));
    CHECK(STREQ(e.program, "kernel"));
    CHECK(e.pid == -1);
    CHECK(e.severity == 6);
}

static void test_previous_year(void)
{
    /* в январе читаем декабрьский лог — год должен быть прошлым */
    parse_ctx jan = {.cur_year = 2027, .cur_month = 1};
    char line[] = "Dec 31 23:59:59 h app: bye";
    log_entry e;
    CHECK(parse_line(&jan, line, &e) == 0);
    CHECK(STREQ(e.ts, "2026-12-31 23:59:59"));
}

static void test_pri(void)
{
    char line[] = "<11>Oct 10 10:00:05 srv02 myapp[5000]: all good actually";
    log_entry e;
    CHECK(parse_line(&ctx, line, &e) == 0);
    CHECK(e.facility == 1); /* 11 = 1*8 + 3 */
    CHECK(e.severity == 3);
    CHECK(e.sev_inferred == 0); /* взято из PRI, а не угадано */
}

static void test_iso(void)
{
    char line[] = "2026-10-10T10:15:00.123456+07:00 srv02 dockerd[900]: level=warning msg=x";
    log_entry e;
    CHECK(parse_line(&ctx, line, &e) == 0);
    CHECK(STREQ(e.ts, "2026-10-10T10:15:00.123456+07:00"));
    CHECK(STREQ(e.program, "dockerd"));
    CHECK(e.severity == 4);
}

static void test_journalctl_tz_without_colon(void)
{
    char line[] = "2026-10-10T10:20:00+0700 srv02 systemd[1]: Reached target";
    log_entry e;
    CHECK(parse_line(&ctx, line, &e) == 0);
    CHECK(STREQ(e.ts, "2026-10-10T10:20:00+0700"));
}

static void test_no_tag(void)
{
    char line[] = "Oct 10 09:00:00 host just some text: with colon";
    log_entry e;
    CHECK(parse_line(&ctx, line, &e) == 0);
    CHECK(e.program == NULL);
    CHECK(STREQ(e.message, "just some text: with colon"));
}

static void test_garbage(void)
{
    char a[] = "это не лог";
    char b[] = "";
    char c[] = "Oct 99 10:00:00 host x: y";
    char d[] = "Oct 10 10:00:00";
    log_entry e;
    CHECK(parse_line(&ctx, a, &e) == -1);
    CHECK(parse_line(&ctx, b, &e) == -1);
    CHECK(parse_line(&ctx, c, &e) == -1);
    CHECK(parse_line(&ctx, d, &e) == -1);
}

static void test_severity_names(void)
{
    CHECK(severity_from_name("err") == 3);
    CHECK(severity_from_name("ERROR") == 3);
    CHECK(severity_from_name("warning") == 4);
    CHECK(severity_from_name("7") == 7);
    CHECK(severity_from_name("nope") == -1);
}

int main(void)
{
    test_bsd_with_pid();
    test_bsd_single_digit_day_no_pid();
    test_previous_year();
    test_pri();
    test_iso();
    test_journalctl_tz_without_colon();
    test_no_tag();
    test_garbage();
    test_severity_names();

    printf("%d/%d проверок прошло\n", total - failures, total);
    return failures ? 1 : 0;
}
