# База данных

PostgreSQL 13+. Схема лежит в [`sql/schema.sql`](../sql/schema.sql). В Docker Compose она применяется автоматически при первом запуске.

## ER-диаграмма

`NOT NULL` — поле обязательно. `NULL` — поле может быть пустым, и в комментарии сказано, когда это бывает.

```mermaid
erDiagram
    sources ||--o{ log_entries : "загружены в попытке"
    sources ||--o{ parse_errors : "найдены в попытке"

    sources {
        serial id PK "NOT NULL"
        text path "NOT NULL; путь к файлу или <stdin>; не уникален"
        int attempt "NOT NULL; номер попытки загрузки этого path"
        timestamptz started_at "NOT NULL; начало обработки"
        timestamptz finished_at "NULL - идёт или прервана"
        text status "NOT NULL; running / ok / failed"
        int lines_read "NOT NULL"
        int lines_inserted "NOT NULL; новых записей"
        int lines_duplicate "NOT NULL; пропущено дублей"
        int lines_bad "NOT NULL; не разобрано"
        text error "NULL - если ошибки не было"
    }
    log_entries {
        bigint id PK "NOT NULL"
        timestamptz ts "NOT NULL; время события"
        text host "NOT NULL"
        text program "NULL - в строке нет тега программы"
        int pid "NULL - PID не указан"
        text level "NULL - уровень не указан; нижний регистр"
        smallint severity "NULL - нестандартный уровень или нет уровня; 0..7"
        smallint facility "NULL - нет <PRI>"
        text trace_id "NULL - запись не относится к операции"
        text message "NOT NULL"
        int source_id FK "NULL - попытка удалена"
    }
    parse_errors {
        bigint id PK "NOT NULL"
        int source_id FK "NOT NULL; удаляется вместе с попыткой"
        bigint line_no "NOT NULL; номер строки в файле"
        text raw "NOT NULL; исходная строка"
        timestamptz created_at "NOT NULL"
    }
```

## Что изменилось в версии 0.2 и почему

| Было в 0.1 | Стало в 0.2 | Причина |
|---|---|---|
| Справочник `severities` и внешний ключ на него | Справочника нет. `level text` хранит уровень как в логе (`error`, `fatal`, `trace`, `audit`), а `severity smallint` хранит код 0..7, если он известен | Справочник не давал сохранить нестандартный уровень (FATAL, TRACE). Замена такого уровня на `unknown` теряла бы информацию |
| `sev_inferred`: уровень угадывался по словам error/fail в тексте | Поля нет. Уровень берётся только из явной разметки: `<PRI>`, `[ERROR]`, `FATAL:`, `level=` | Угадывание ненадёжно: слово «failed» встречается и в обычных info-сообщениях |
| `sources`: одна строка на файл, `path` UNIQUE, `inode`, `last_offset`, `lines_total`, `updated_at` | `sources`: одна строка на **попытку** импорта, `attempt` — номер попытки, `started_at` / `finished_at`, `status`, счётчики, `error` | Утилита не следит за файлами постоянно: пользователь сам запускает импорт. Поэтому позиция в файле и inode не нужны, а важна история попыток, в том числе неудачных |
| `log_entries.imported_at` | Удалено. Время загрузки берётся из `sources.started_at` по `source_id` | Время дублировалось. Кроме того, «imported_at» у записи и время попытки — разные вещи: попытка может завершиться ошибкой |
| Дубликаты при повторном импорте отсекались только по позиции в файле | Уникальный индекс `log_entries_uniq` и `INSERT ... ON CONFLICT DO NOTHING` | Тот же файл, загруженный ещё раз (или скопированный под другим именем), не искажает статистику |
| — | `trace_id` и представление `v_operations` | Связать записи одной операции, посчитать её длительность, найти самые долгие |

## Уровни (level / severity)

- `level` всегда в нижнем регистре: это гарантирует `CHECK (level = lower(level))`. Синонимы приводятся к одному имени: `ERR` и `Error` становятся `error`, `WARN` становится `warning`.
- Самостоятельные уровни сохраняются под своим именем. `fatal` остаётся `fatal` и получает код 2 (как critical), а `trace` остаётся `trace` и получает код 7 (как debug). Код нужен для сортировки и фильтра `--level error`, где показываются записи «не ниже error».
- Нестандартный уровень (например `audit`) сохраняется как есть, со значением `severity = NULL`. При импорте утилита предупреждает об этом и называет уровень. Найти такие записи можно командой `search --level-is audit`.
- Если уровня в строке нет, то `level = NULL` (в выводе показывается «-»).
- Цвета в БД не хранятся, это задача интерфейса (`ui.c`).

## Защита от дубликатов

```sql
CREATE UNIQUE INDEX log_entries_uniq ON log_entries
    (ts, host, coalesce(program, ''), coalesce(pid, -1), coalesce(level, ''), md5(message));
```

`coalesce` нужен потому, что `NULL` не равен `NULL`. Без него две одинаковые строки без PID не считались бы дублями. `md5(message)` держит индекс компактным при длинных сообщениях.

Загрузка идёт в два шага:
1. `COPY` во временную таблицу `stage`. Это быстро, обычный `COPY` не умеет `ON CONFLICT`.
2. `INSERT INTO log_entries SELECT ... FROM stage ON CONFLICT DO NOTHING`. Число вставленных строк — это новые записи, а разница с числом разобранных строк — дубликаты.

## Попытки импорта

Строка в `sources` создаётся **отдельной транзакцией до начала загрузки**. Загрузка данных идёт во второй транзакции: при ошибке она полностью откатывается, а попытка остаётся в истории со статусом `failed` и текстом ошибки. Историю можно посмотреть командой `logparser sources`.

## Индексы

| Индекс | Тип | Для чего |
|---|---|---|
| `log_entries_uniq` | UNIQUE B-tree | защита от дубликатов |
| `log_entries_ts_brin` | BRIN (ts) | диапазоны по времени; логи пишутся почти по порядку, поэтому индекс крошечный |
| `log_entries_sev_ts` | B-tree (severity, ts) | `search -l error -s 1h` |
| `log_entries_level` | B-tree (level) | `search --level-is fatal`, статистика по уровням |
| `log_entries_prog_ts` | B-tree (program, ts) | `search -p sshd` |
| `log_entries_trace` | частичный B-tree (trace_id, ts) | `trace <id>`, самые долгие операции |
| `log_entries_msg_trgm` | GIN + pg_trgm | `search -g "текст"` (ILIKE '%…%') |

## Очистка

`logparser clear` выполняет `TRUNCATE log_entries, parse_errors, sources RESTART IDENTITY`. Данные удаляются, а таблицы, индексы и представления остаются, поэтому `init` повторять не нужно. Сервер PostgreSQL при этом не трогается.
