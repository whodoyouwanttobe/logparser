# Прототип базы данных

PostgreSQL 13+. Полная схема лежит в [`sql/schema.sql`](../sql/schema.sql).

## ER-диаграмма

```mermaid
erDiagram
    sources ||--o{ log_entries : "откуда"
    sources ||--o{ parse_errors : "откуда"
    severities ||--o{ log_entries : "уровень"

    sources {
        serial id PK
        text path UK "путь к файлу"
        bigint inode "для обнаружения ротации"
        bigint last_offset "до какого байта прочитано"
        bigint lines_total
        timestamptz updated_at
    }
    log_entries {
        bigint id PK
        timestamptz ts "время события"
        text host
        text program "sshd, kernel, ..."
        int pid
        smallint severity FK "0..7"
        boolean sev_inferred "уровень угадан?"
        smallint facility
        text message
        int source_id FK
        timestamptz imported_at
    }
    severities {
        smallint code PK
        text name "emerg..debug"
    }
    parse_errors {
        bigint id PK
        int source_id FK
        bigint line_no
        text raw "исходная строка"
    }
```

## Таблицы

| Таблица | Зачем нужна |
|---|---|
| `log_entries` | основные данные: одна строка лога соответствует одной записи |
| `sources` | файлы-источники и позиция чтения в каждом. Повторный `import` дочитывает только новые строки. Если сменился `inode`, значит прошла ротация логов, и файл читается с начала |
| `severities` | справочник уровней по RFC 5424: 0 emerg … 7 debug |
| `parse_errors` | строки, которые не удалось разобрать. Данные не теряются, формат можно доработать позже |

## Индексы и запросы, которые они ускоряют

| Индекс | Тип | Запрос |
|---|---|---|
| `log_entries_ts_brin` | BRIN (ts) | диапазоны по времени. Логи пишутся почти по порядку, поэтому индекс занимает килобайты |
| `log_entries_sev_ts` | B-tree (severity, ts) | `search -l err -s 1h` |
| `log_entries_prog_ts` | B-tree (program, ts) | `search -p sshd` |
| `log_entries_msg_trgm` | GIN + pg_trgm | `search -g "failed password"`, то есть `ILIKE '%…%'` |

## Представления и функции

- `v_severity_stats`: количество записей по уровням.
- `v_errors_hourly`: ошибки и предупреждения по часам.
- `purge_logs('30 days')`: удаляет записи старше заданного срока (retention).

## Загрузка

Данные загружаются через `COPY ... FROM STDIN`, это в разы быстрее построчных `INSERT`.
Импорт одного файла выполняется в **одной транзакции**: либо файл загружен целиком, либо не загружено ничего.
На время импорта строка в `sources` блокируется (`SELECT ... FOR UPDATE`), поэтому два параллельных импорта одного файла не создадут дублей.

## Что можно сделать дальше

- Партиционировать `log_entries` по месяцам (`PARTITION BY RANGE (ts)`): старые данные удаляются через `DROP PARTITION` мгновенно.
- Вынести `host` и `program` в справочники, если объём вырастет до десятков миллионов строк.
- Добавить материализованные представления для тяжёлой статистики.
