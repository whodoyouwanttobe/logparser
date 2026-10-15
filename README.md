# logparser

Консольный парсер системных логов на чистом C с хранением в PostgreSQL.
Читает `/var/log/syslog`, `auth.log` и вывод `journalctl`, раскладывает строки на поля и складывает их в БД. Потом по этим данным можно искать события и строить статистику прямо в терминале.

## Возможности

- Форматы: классический syslog (RFC 3164), ISO 8601 (rsyslog, `journalctl -o short-iso`), префикс `<PRI>`.
- Уровень важности берётся из `<PRI>`, а если его нет, определяется по тексту: error, fail, warn, panic и т.д.
- Инкрементальный импорт: повторный запуск дочитывает только новые строки и замечает ротацию логов.
- Быстрая загрузка через `COPY`, импорт выполняется в одной транзакции.
- Нераспознанные строки сохраняются в отдельную таблицу.
- Поиск по уровню, программе, хосту, периоду (`-s 1h`) и тексту; вывод таблицей или в CSV.
- Статистика: распределение по уровням, топ программ по ошибкам, график по часам.

## Сборка

Нужны `gcc`, `make` и `libpq-dev` (Ubuntu/Debian: `sudo apt install build-essential libpq-dev postgresql`).

```bash
make          # собрать ./logparser
make test     # unit-тесты парсера (база данных не нужна)
```

## Быстрый старт

```bash
createdb logs
export PGDATABASE=logs PGTZ=Asia/Novosibirsk

./logparser init                              # создать схему
./logparser import samples/sample.log         # загрузить пример
./logparser search -l err                     # все ошибки
./logparser search -p sshd -g failed -s 1d    # неудачные входы по SSH за сутки
./logparser stats                             # сводка
journalctl -o short-iso --since today | ./logparser import -
```

`./logparser --help` выводит полный список команд и опций.

## Архитектура

```
main.c  ──►  ui.c                (фронтенд: команды и вывод)
   │
   └──►  importer.c  ──►  parser.c   (бэкенд: чтение и разбор)
              │
              └──►  db.c  ──►  PostgreSQL   (БД: схема в sql/schema.sql)
```

| Модуль | Ответственность |
|---|---|
| `parser.c` | разбор одной строки в структуру `log_entry`, без зависимостей |
| `importer.c` | чтение файла или stdin, позиция чтения, ротация, транзакция |
| `db.c` | весь код, работающий с libpq: подключение, COPY, параметризованные запросы |
| `ui.c` | таблицы, цвета, гистограммы, CSV |
| `main.c` | разбор аргументов и запуск команд |

Подробнее: [docs/database.md](docs/database.md) (схема БД), [CONTRIBUTING.md](CONTRIBUTING.md) (код-стайл и Git Flow).

## Команда

| Участник | Роль |
|---|---|
| Александр | бэкенд: парсер, импорт |
| loha | база данных: схема, слой `db.c` |
| Руслан | фронтенд: CLI, вывод |
