#define _GNU_SOURCE
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "db.h"
#include "importer.h"
#include "parser.h"
#include "ui.h"

#define VERSION "0.2.0"

static const char *conninfo = NULL;

static void usage(FILE *out)
{
    fprintf(
        out,
        "logparser " VERSION
        " — консольная утилита для загрузки, хранения, поиска и анализа системных логов.\n"
        "Логи (один или несколько файлов) загружаются в PostgreSQL, дальше по ним можно\n"
        "искать записи, смотреть цепочки операций по trace_id и статистику.\n"
        "\n"
        "Использование: logparser [-d CONNINFO] <команда> [опции]\n"
        "\n"
        "Команды:\n"
        "  init [schema.sql]          создать схему БД (в Docker Compose делается автоматически)\n"
        "  import FILE...             загрузить логи; FILE = '-' — читать stdin.\n"
        "                             Повторная загрузка того же файла не создаёт дубликатов\n"
        "  parse [-n N] FILE          разобрать без БД и показать поля (проверка формата)\n"
        "  search [фильтры]           найти записи\n"
        "      -l, --level LEVEL      уровень не ниже: emergency alert critical error warning\n"
        "                             notice info debug (или код 0..7)\n"
        "      -L, --level-is NAME    точно этот уровень, в т.ч. нестандартный (fatal, trace...);\n"
        "                             '-' — записи без уровня\n"
        "      -p, --program NAME     программа (sshd, kernel, systemd, ...)\n"
        "      -H, --host NAME        хост\n"
        "      -t, --trace ID         записи одной операции (trace_id)\n"
        "      -s, --since TIME       с момента: '2026-10-10 12:00' или 30m, 2h, 7d\n"
        "      -u, --until TIME       до момента\n"
        "      -g, --grep TEXT        подстрока в сообщении (без учёта регистра)\n"
        "      -n, --limit N          сколько последних записей показать (по умолчанию 50)\n"
        "      -f, --format FMT       table | csv\n"
        "  trace ID                   цепочка записей операции с длительностью\n"
        "  stats [-s TIME] [--hours N] [--top N]\n"
        "                             сводка, уровни, топ программ и хостов по ошибкам,\n"
        "                             самые долгие операции, активность по часам\n"
        "  sources [-n N]             история попыток импорта\n"
        "  clear [-y]                 удалить все загруженные данные (структура БД остаётся)\n"
        "\n"
        "Формат строки лога:  [<PRI>]ВРЕМЯ ХОСТ ПРОГРАММА[PID]: СООБЩЕНИЕ\n"
        "  ВРЕМЯ   — 'Oct 10 12:34:56' или ISO 8601 '2026-10-10T12:34:56+07:00'\n"
        "  уровень — только явный: <PRI>, '[ERROR] ...', 'FATAL: ...', 'level=warning'\n"
        "  trace   — поле 'trace_id=abc123' в сообщении (необязательно)\n"
        "\n"
        "Подключение к PostgreSQL (libpq):\n"
        "  переменные PGHOST, PGPORT, PGDATABASE, PGUSER, PGPASSWORD\n"
        "  или -d \"host=localhost port=5432 dbname=logs user=logparser password=...\"\n"
        "  Часовой пояс для времени без зоны: PGTZ (например PGTZ=Asia/Novosibirsk).\n"
        "\n"
        "Примеры:\n"
        "  logparser import samples/syslog.log samples/app.log\n"
        "  logparser search -l error -s 1h\n"
        "  logparser search -p sshd -g 'failed password' -f csv > ssh.csv\n"
        "  logparser trace a1b2c3\n"
        "  logparser stats --top 5\n");
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
    db_print_target(c);
    int rc = db_apply_schema(c, path);
    if (rc == 0) ui_ok("схема применена (%s)", path);
    PQfinish(c);
    return rc == 0 ? 0 : 1;
}

static int cmd_import(int argc, char **argv)
{
    if (argc < 2) {
        ui_error("укажите файл(ы) или '-'");
        return 1;
    }
    PGconn *c = connect_or_die();
    int failed = 0;
    for (int i = 1; i < argc; i++) {
        import_stats st;
        if (import_file(c, argv[i], &st) == 0) {
            ui_print_import(argv[i], &st);
        } else {
            ui_error("%s: импорт отменён, изменения откатены (попытка записана в sources)",
                     argv[i]);
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
                                   {"level-is", required_argument, NULL, 'L'},
                                   {"program", required_argument, NULL, 'p'},
                                   {"host", required_argument, NULL, 'H'},
                                   {"trace", required_argument, NULL, 't'},
                                   {"since", required_argument, NULL, 's'},
                                   {"until", required_argument, NULL, 'u'},
                                   {"grep", required_argument, NULL, 'g'},
                                   {"limit", required_argument, NULL, 'n'},
                                   {"format", required_argument, NULL, 'f'},
                                   {NULL, 0, NULL, 0}};
    search_filter f = {.max_severity = -1, .limit = 50};
    out_format fmt = FMT_TABLE;
    int ch;
    while ((ch = getopt_long(argc, argv, "l:L:p:H:t:s:u:g:n:f:", opts, NULL)) != -1) {
        switch (ch) {
        case 'l':
            f.max_severity = severity_from_name(optarg);
            if (f.max_severity < 0) {
                ui_error("неизвестный уровень '%s'; для нестандартных уровней используйте "
                         "--level-is %s",
                         optarg, optarg);
                return 1;
            }
            break;
        case 'L':
            f.level = optarg;
            break;
        case 'p':
            f.program = optarg;
            break;
        case 'H':
            f.host = optarg;
            break;
        case 't':
            f.trace_id = optarg;
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
        ui_print_entries(r, fmt, 0);
        PQclear(r);
    }
    PQfinish(c);
    return r ? 0 : 1;
}

static int cmd_trace(int argc, char **argv)
{
    if (argc < 2) {
        ui_error("укажите trace_id (список операций: logparser stats)");
        return 1;
    }
    PGconn *c = connect_or_die();
    PGresult *r = db_trace(c, argv[1]);
    if (r) {
        ui_print_trace_header(argv[1], r);
        if (PQntuples(r) > 0) ui_print_entries(r, FMT_TABLE, 1);
        PQclear(r);
    }
    PQfinish(c);
    return r ? 0 : 1;
}

static int cmd_stats(int argc, char **argv)
{
    static struct option opts[] = {{"since", required_argument, NULL, 's'},
                                   {"hours", required_argument, NULL, 'h'},
                                   {"top", required_argument, NULL, 'n'},
                                   {NULL, 0, NULL, 0}};
    const char *since = NULL;
    int hours = 12, top = 5, ch;
    while ((ch = getopt_long(argc, argv, "s:h:n:", opts, NULL)) != -1) {
        if (ch == 's')
            since = optarg;
        else if (ch == 'h')
            hours = atoi(optarg) > 0 ? atoi(optarg) : 12;
        else if (ch == 'n')
            top = atoi(optarg) > 0 ? atoi(optarg) : 5;
        else
            return 1;
    }
    PGconn *c = connect_or_die();
    PGresult *r;
    int rc = 0;
    char title[64];
    if ((r = db_stats_summary(c))) {
        ui_print_summary(r);
        PQclear(r);
    } else
        rc = 1;
    if ((r = db_stats_levels(c, since))) {
        ui_print_levels(r);
        PQclear(r);
    } else
        rc = 1;
    if ((r = db_stats_programs(c, since, top))) {
        snprintf(title, sizeof title, "Топ-%d программ по ошибкам", top);
        ui_print_top(title, "программа", r);
        PQclear(r);
    } else
        rc = 1;
    if ((r = db_stats_hosts(c, since, top))) {
        snprintf(title, sizeof title, "Топ-%d хостов по ошибкам", top);
        ui_print_top(title, "хост", r);
        PQclear(r);
    } else
        rc = 1;
    if ((r = db_stats_operations(c, since, top))) {
        ui_print_operations(r);
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

static int cmd_sources(int argc, char **argv)
{
    int limit = 20, ch;
    while ((ch = getopt(argc, argv, "n:")) != -1) {
        if (ch == 'n')
            limit = atoi(optarg) > 0 ? atoi(optarg) : 20;
        else
            return 1;
    }
    PGconn *c = connect_or_die();
    PGresult *r = db_sources(c, limit);
    if (r) {
        ui_print_sources(r);
        PQclear(r);
    }
    PQfinish(c);
    return r ? 0 : 1;
}

static int cmd_clear(int argc, char **argv)
{
    int yes = argc > 1 && (strcmp(argv[1], "-y") == 0 || strcmp(argv[1], "--yes") == 0);
    PGconn *c = connect_or_die();
    db_print_target(c);
    if (!yes && !ui_confirm("Удалить все загруженные логи, ошибки разбора и историю импорта?")) {
        printf("отменено\n");
        PQfinish(c);
        return 1;
    }
    int rc = db_clear(c);
    if (rc == 0) ui_ok("данные удалены, структура БД сохранена (init повторять не нужно)");
    PQfinish(c);
    return rc == 0 ? 0 : 1;
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
    if (strcmp(cmd, "trace") == 0) return cmd_trace(sub_argc, sub_argv);
    if (strcmp(cmd, "stats") == 0) return cmd_stats(sub_argc, sub_argv);
    if (strcmp(cmd, "sources") == 0) return cmd_sources(sub_argc, sub_argv);
    if (strcmp(cmd, "clear") == 0) return cmd_clear(sub_argc, sub_argv);
    if (strcmp(cmd, "help") == 0) {
        usage(stdout);
        return 0;
    }

    ui_error("неизвестная команда '%s' (см. logparser --help)", cmd);
    return 1;
}
