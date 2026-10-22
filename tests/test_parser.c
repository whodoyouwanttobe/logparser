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

static void test_bsd_with_pid_and_pri(void)
{
    char line[] = "<36>Oct 10 08:05:12 srv01 sshd[2301]: Failed password for root\n";
    log_entry e;
    CHECK(parse_line(&ctx, line, &e) == 0);
    CHECK(STREQ(e.ts, "2026-10-10 08:05:12"));
    CHECK(STREQ(e.host, "srv01"));
    CHECK(STREQ(e.program, "sshd"));
    CHECK(e.pid == 2301);
    CHECK(STREQ(e.message, "Failed password for root"));
    CHECK(e.facility == 4); /* 36 = 4*8 + 4 */
    CHECK(e.severity == 4);
    CHECK(STREQ(e.level, "warning"));
}

static void test_no_guessing_by_words(void)
{
    /* слова error/failed в тексте НЕ делают запись ошибкой */
    char line[] = "Oct 10 08:00:00 srv01 app[1]: Connection failed, error counter reset";
    log_entry e;
    CHECK(parse_line(&ctx, line, &e) == 0);
    CHECK(e.level[0] == '\0');
    CHECK(e.severity == -1);
}

static void test_bracket_level(void)
{
    char line[] = "2026-10-10T10:00:00+07:00 api01 shop[1]: [WARN] trace_id=a1b2c3 slow";
    log_entry e;
    CHECK(parse_line(&ctx, line, &e) == 0);
    CHECK(STREQ(e.level, "warning")); /* синоним приведён к одному имени */
    CHECK(e.severity == 4);
    CHECK(STREQ(e.trace_id, "a1b2c3"));
}

static void test_fatal_kept_as_is(void)
{
    char line[] = "2026-10-10T10:01:09+07:00 api01 shop[1]: FATAL: checkout aborted";
    log_entry e;
    CHECK(parse_line(&ctx, line, &e) == 0);
    CHECK(STREQ(e.level, "fatal")); /* не заменяем на critical и не на unknown */
    CHECK(e.severity == 2);
    CHECK(e.level_known == 1);
}

static void test_unknown_level_kept(void)
{
    char line[] = "2026-10-10T10:03:00+07:00 w01 billing[7]: [AUDIT] invoice exported";
    log_entry e;
    CHECK(parse_line(&ctx, line, &e) == 0);
    CHECK(STREQ(e.level, "audit"));
    CHECK(e.severity == -1);
    CHECK(e.level_known == 0);
}

static void test_level_field_and_lowercase_prefix(void)
{
    char a[] = "2026-10-10T10:15:00+07:00 srv02 dockerd[900]: level=warning msg=\"x\"";
    char b[] = "Oct 10 11:00:00 srv01 sshd[9]: error: kex_exchange_identification";
    char c[] = "Oct 10 11:00:00 srv01 systemd[1]: app.service: Failed with result";
    log_entry e;
    CHECK(parse_line(&ctx, a, &e) == 0);
    CHECK(STREQ(e.level, "warning"));
    CHECK(parse_line(&ctx, b, &e) == 0);
    CHECK(STREQ(e.level, "error"));
    CHECK(parse_line(&ctx, c, &e) == 0);
    CHECK(e.level[0] == '\0'); /* "app.service:" — не уровень */
}

static void test_trace_variants(void)
{
    char a[] = "2026-10-10T10:00:00Z h p[1]: [INFO] traceId=AB-12.x done";
    char b[] = "2026-10-10T10:00:00Z h p[1]: [INFO] no trace here, mytrace_id=zz";
    log_entry e;
    CHECK(parse_line(&ctx, a, &e) == 0);
    CHECK(STREQ(e.trace_id, "AB-12.x"));
    CHECK(parse_line(&ctx, b, &e) == 0);
    CHECK(e.trace_id[0] == '\0'); /* ключ должен начинаться на границе слова */
}

static void test_bsd_single_digit_day_no_pid(void)
{
    char line[] = "Oct  1 00:00:01 host kernel: [ 1.0] usb connected";
    log_entry e;
    CHECK(parse_line(&ctx, line, &e) == 0);
    CHECK(STREQ(e.ts, "2026-10-01 00:00:01"));
    CHECK(STREQ(e.program, "kernel"));
    CHECK(e.pid == -1);
    CHECK(e.level[0] == '\0'); /* "[ 1.0]" — не уровень */
}

static void test_previous_year(void)
{
    parse_ctx jan = {.cur_year = 2027, .cur_month = 1};
    char line[] = "Dec 31 23:59:59 h app: bye";
    log_entry e;
    CHECK(parse_line(&jan, line, &e) == 0);
    CHECK(STREQ(e.ts, "2026-12-31 23:59:59"));
}

static void test_iso_and_journalctl(void)
{
    char a[] = "2026-10-10T10:15:00.123456+07:00 srv02 dockerd[900]: started";
    char b[] = "2026-10-10T10:20:00+0700 srv02 systemd[1]: Reached target";
    log_entry e;
    CHECK(parse_line(&ctx, a, &e) == 0);
    CHECK(STREQ(e.ts, "2026-10-10T10:15:00.123456+07:00"));
    CHECK(parse_line(&ctx, b, &e) == 0);
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
    CHECK(severity_from_name("fatal") == 2);
    CHECK(severity_from_name("7") == 7);
    CHECK(severity_from_name("audit") == -1);
    CHECK(STREQ(severity_name(3), "error"));
}

int main(void)
{
    test_bsd_with_pid_and_pri();
    test_no_guessing_by_words();
    test_bracket_level();
    test_fatal_kept_as_is();
    test_unknown_level_kept();
    test_level_field_and_lowercase_prefix();
    test_trace_variants();
    test_bsd_single_digit_day_no_pid();
    test_previous_year();
    test_iso_and_journalctl();
    test_no_tag();
    test_garbage();
    test_severity_names();

    printf("%d/%d проверок прошло\n", total - failures, total);
    return failures ? 1 : 0;
}
