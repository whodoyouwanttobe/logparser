#define _GNU_SOURCE
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "db.h"
#include "importer.h"
#include "parser.h"
#include "ui.h"

#define VERSION "0.1.0"

static const char *conninfo = NULL;

static void usage(FILE *out)
{
    fprintf(out,
            "logparser " VERSION " — парсер системных логов с хранением в PostgreSQL\n"
            "\n"
            "Использование: logparser [-d CONNINFO] <команда> [опции]\n"
            "\n"
            "Команды:\n"
            "  init [schema.sql]          создать/обновить схему БД\n"
            "  import [--full] FILE...    загрузить логи (FILE = '-' читает stdin)\n"
            "  parse [-n N] FILE          разобрать без БД и показать поля (отладка)\n"
            "  search [фильтры]           найти записи\n"
            "      -l, --level LEVEL      уровень не ниже: emerg alert crit err warning notice "
            "info debug\n"
            "      -p, --program NAME     программа (sshd, kernel, systemd, ...)\n"
            "      -H, --host NAME        хост\n"
            "      -s, --since TIME       с момента: '2026-10-10 12:00' или 30m, 2h, 7d\n"
            "      -u, --until TIME       до момента\n"
            "      -g, --grep TEXT        подстрока в сообщении (без учёта регистра)\n"
            "      -n, --limit N          сколько последних записей показать (по умолчанию 50)\n"
            "      -f, --format FMT       table | csv\n"
            "  stats [-s TIME] [--hours N] сводка, уровни, топ программ, график по часам\n"
            "\n"
            "Подключение: -d \"host=localhost dbname=logs user=...\" или переменные PGHOST, "
            "PGDATABASE, PGUSER, PGPASSWORD.\n"
            "Часовой пояс BSD-логов без зоны берётся из PGTZ (например PGTZ=Asia/Novosibirsk).\n"
            "\n"
            "Примеры:\n"
            "  logparser init\n"
            "  logparser import /var/log/syslog /var/log/auth.log\n"
            "  journalctl -o short-iso --since today | logparser import -\n"
            "  logparser search -l err -s 1h\n"
            "  logparser search -p sshd -g 'failed password' -f csv > ssh.csv\n"
            "  logparser stats -s 1d\n");
}

static PGconn *connect_or_die(void)
{
    PGconn *c = db_connect(conninfo);
    if (!c) exit(2);
    return c;
}

/* ---------- команды ---------- */

static int cmd_init(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "sql/schema.sql";
    PGconn *c = connect_or_die();
    int rc = db_apply_schema(c, path);
    if (rc == 0) ui_ok("схема применена (%s)", path);
    PQfinish(c);
    return rc == 0 ? 0 : 1;
}

static int cmd_import(int argc, char **argv)
{
    static struct option opts[] = {{"full", no_argument, NULL, 'F'}, {NULL, 0, NULL, 0}};
    int full = 0, ch;
    while ((ch = getopt_long(argc, argv, "F", opts, NULL)) != -1) {
        if (ch == 'F')
            full = 1;
        else
            return 1;
    }
    if (optind >= argc) {
        ui_error("укажите файл(ы) или '-'");
        return 1;
    }

    PGconn *c = connect_or_die();
    int failed = 0;
    for (int i = optind; i < argc; i++) {
        import_stats st;
        if (import_file(c, argv[i], full, &st) == 0) {
            ui_print_import(argv[i], &st);
        } else {
            ui_error("%s: импорт отменён, изменения откатены", argv[i]);
            failed = 1;
        }
    }
    PQfinish(c);
    return failed;
}

static int cmd_parse(int argc, char **argv)
{
    int max = 20, ch;
    while ((ch = getopt(argc, argv, "n:")) != -1) {
        if (ch == 'n')
            max = atoi(optarg);
        else
            return 1;
    }
    if (optind >= argc) {
        ui_error("укажите файл");
        return 1;
    }
    return dry_run_file(argv[optind], max) == 0 ? 0 : 1;
}

static int cmd_search(int argc, char **argv)
{
    static struct option opts[] = {{"level", required_argument, NULL, 'l'},
                                   {"program", required_argument, NULL, 'p'},
                                   {"host", required_argument, NULL, 'H'},
                                   {"since", required_argument, NULL, 's'},
                                   {"until", required_argument, NULL, 'u'},
                                   {"grep", required_argument, NULL, 'g'},
                                   {"limit", required_argument, NULL, 'n'},
                                   {"format", required_argument, NULL, 'f'},
                                   {NULL, 0, NULL, 0}};
    search_filter f = {.max_severity = -1, .limit = 50};
    out_format fmt = FMT_TABLE;
    int ch;
    while ((ch = getopt_long(argc, argv, "l:p:H:s:u:g:n:f:", opts, NULL)) != -1) {
        switch (ch) {
        case 'l':
            f.max_severity = severity_from_name(optarg);
            if (f.max_severity < 0) {
                ui_error("неизвестный уровень '%s'", optarg);
                return 1;
            }
            break;
        case 'p':
            f.program = optarg;
            break;
        case 'H':
            f.host = optarg;
            break;
        case 's':
            f.since = optarg;
            break;
        case 'u':
            f.until = optarg;
            break;
        case 'g':
            f.grep = optarg;
            break;
        case 'n':
            f.limit = atoi(optarg);
            break;
        case 'f':
            if (strcmp(optarg, "csv") == 0)
                fmt = FMT_CSV;
            else if (strcmp(optarg, "table") != 0) {
                ui_error("формат: table | csv");
                return 1;
            }
            break;
        default:
            return 1;
        }
    }
    PGconn *c = connect_or_die();
    PGresult *r = db_search(c, &f);
    if (r) {
        ui_print_search(r, fmt);
        PQclear(r);
    }
    PQfinish(c);
    return r ? 0 : 1;
}

static int cmd_stats(int argc, char **argv)
{
    static struct option opts[] = {{"since", required_argument, NULL, 's'},
                                   {"hours", required_argument, NULL, 'h'},
                                   {NULL, 0, NULL, 0}};
    const char *since = NULL;
    int hours = 24, ch;
    while ((ch = getopt_long(argc, argv, "s:h:", opts, NULL)) != -1) {
        if (ch == 's')
            since = optarg;
        else if (ch == 'h')
            hours = atoi(optarg) > 0 ? atoi(optarg) : 24;
        else
            return 1;
    }
    PGconn *c = connect_or_die();
    PGresult *r;
    int rc = 0;
    if ((r = db_stats_summary(c))) {
        ui_print_summary(r);
        PQclear(r);
    } else
        rc = 1;
    if ((r = db_stats_severity(c, since))) {
        ui_print_severity(r);
        PQclear(r);
    } else
        rc = 1;
    if ((r = db_stats_programs(c, since, 10))) {
        ui_print_programs(r);
        PQclear(r);
    } else
        rc = 1;
    if ((r = db_stats_hourly(c, hours))) {
        ui_print_hourly(r);
        PQclear(r);
    } else
        rc = 1;
    PQfinish(c);
    return rc;
}

/* ---------- точка входа ---------- */

int main(int argc, char **argv)
{
    ui_init();

    int i = 1;
    while (i < argc && argv[i][0] == '-') {
        if ((strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--db") == 0) && i + 1 < argc) {
            conninfo = argv[i + 1];
            i += 2;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(stdout);
            return 0;
        } else if (strcmp(argv[i], "--version") == 0) {
            puts("logparser " VERSION);
            return 0;
        } else {
            ui_error("неизвестная опция %s", argv[i]);
            return 1;
        }
    }
    if (i >= argc) {
        usage(stderr);
        return 1;
    }

    const char *cmd = argv[i];
    int sub_argc = argc - i;
    char **sub_argv = argv + i;
    optind = 1;

    if (strcmp(cmd, "init") == 0) return cmd_init(sub_argc, sub_argv);
    if (strcmp(cmd, "import") == 0) return cmd_import(sub_argc, sub_argv);
    if (strcmp(cmd, "parse") == 0) return cmd_parse(sub_argc, sub_argv);
    if (strcmp(cmd, "search") == 0) return cmd_search(sub_argc, sub_argv);
    if (strcmp(cmd, "stats") == 0) return cmd_stats(sub_argc, sub_argv);
    if (strcmp(cmd, "help") == 0) {
        usage(stdout);
        return 0;
    }

    ui_error("неизвестная команда '%s' (см. logparser --help)", cmd);
    return 1;
}
