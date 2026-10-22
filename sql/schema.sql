-- logparser: схема БД (PostgreSQL 13+), версия 0.2
-- Применение:  ./logparser init   или   psql -f sql/schema.sql
-- В Docker Compose применяется автоматически при первом запуске контейнера db.
-- Скрипт идемпотентен: можно запускать повторно.
--
-- Обновление с 0.1.x: структура изменилась несовместимо, базу нужно пересоздать
--   (docker compose down -v  или  DROP SCHEMA public CASCADE; CREATE SCHEMA public;)

DO $$
BEGIN
    IF to_regclass('public.severities') IS NOT NULL THEN
        RAISE EXCEPTION 'В базе схема logparser 0.1 (есть таблица severities). Пересоздайте базу: docker compose down -v или DROP SCHEMA public CASCADE; CREATE SCHEMA public;';
    END IF;
END $$;

CREATE EXTENSION IF NOT EXISTS pg_trgm;   -- быстрый поиск подстроки (ILIKE '%...%')

-- Попытки импорта. Одна строка = один запуск `import` для одного файла.
-- path не уникален: один и тот же файл можно загружать многократно, попытки нумеруются.
CREATE TABLE IF NOT EXISTS sources (
    id               serial      PRIMARY KEY,
    path             text        NOT NULL,                 -- путь к файлу или '<stdin>'
    attempt          integer     NOT NULL DEFAULT 1,       -- номер попытки загрузки этого path
    started_at       timestamptz NOT NULL DEFAULT now(),   -- когда начали обработку
    finished_at      timestamptz,                          -- когда закончили (NULL — идёт или упала)
    status           text        NOT NULL DEFAULT 'running'
                     CHECK (status IN ('running', 'ok', 'failed')),
    lines_read       integer     NOT NULL DEFAULT 0,
    lines_inserted   integer     NOT NULL DEFAULT 0,       -- новых записей
    lines_duplicate  integer     NOT NULL DEFAULT 0,       -- уже были в базе, пропущены
    lines_bad        integer     NOT NULL DEFAULT 0,       -- не разобраны (см. parse_errors)
    error            text                                  -- текст ошибки, если status = 'failed'
);
CREATE INDEX IF NOT EXISTS sources_path ON sources (path, attempt);

-- Записи логов
CREATE TABLE IF NOT EXISTS log_entries (
    id         bigint      GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    ts         timestamptz NOT NULL,
    host       text        NOT NULL,
    program    text,                          -- NULL: в строке нет тега программы
    pid        integer,                       -- NULL: PID не указан
    level      text        CHECK (level = lower(level)),
                                              -- уровень как в логе, нижний регистр: error, fatal, trace...
                                              -- NULL: уровень в строке не указан (не угадываем)
    severity   smallint    CHECK (severity BETWEEN 0 AND 7),
                                              -- код syslog для сортировки/фильтра; NULL: нестандартный уровень
    facility   smallint    CHECK (facility BETWEEN 0 AND 23),  -- NULL: нет <PRI>
    trace_id   text,                          -- NULL: запись не относится к трассируемой операции
    message    text        NOT NULL,
    source_id  integer     REFERENCES sources(id) ON DELETE SET NULL
);

-- Защита от дубликатов: одна и та же строка лога, загруженная повторно, не добавляется.
-- message хешируется (md5), чтобы индекс не зависел от длины сообщения.
CREATE UNIQUE INDEX IF NOT EXISTS log_entries_uniq ON log_entries
    (ts, host, coalesce(program, ''), coalesce(pid, -1), coalesce(level, ''), md5(message));

-- Индексы под запросы CLI
CREATE INDEX IF NOT EXISTS log_entries_ts_brin  ON log_entries USING brin (ts);
CREATE INDEX IF NOT EXISTS log_entries_sev_ts   ON log_entries (severity, ts DESC);
CREATE INDEX IF NOT EXISTS log_entries_level    ON log_entries (level);
CREATE INDEX IF NOT EXISTS log_entries_prog_ts  ON log_entries (program, ts DESC);
CREATE INDEX IF NOT EXISTS log_entries_trace    ON log_entries (trace_id, ts) WHERE trace_id IS NOT NULL;
CREATE INDEX IF NOT EXISTS log_entries_msg_trgm ON log_entries USING gin (message gin_trgm_ops);

-- Строки, которые не удалось разобрать
CREATE TABLE IF NOT EXISTS parse_errors (
    id          bigint      GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    source_id   integer     NOT NULL REFERENCES sources(id) ON DELETE CASCADE,
    line_no     bigint      NOT NULL,          -- номер строки в исходном файле
    raw         text        NOT NULL,
    created_at  timestamptz NOT NULL DEFAULT now()
);

-- Представления для отчётов
CREATE OR REPLACE VIEW v_level_stats AS
SELECT coalesce(level, '-') AS level, min(severity) AS severity, count(*) AS cnt
FROM log_entries
GROUP BY level
ORDER BY min(severity) NULLS LAST, level;

-- Операции (trace_id): начало, конец, длительность, число записей
CREATE OR REPLACE VIEW v_operations AS
SELECT trace_id,
       min(ts)                          AS started,
       max(ts)                          AS finished,
       max(ts) - min(ts)                AS duration,
       count(*)                         AS entries,
       count(*) FILTER (WHERE severity <= 3) AS errors,
       string_agg(DISTINCT coalesce(program, '-'), ',') AS programs
FROM log_entries
WHERE trace_id IS NOT NULL
GROUP BY trace_id;

-- Удаление старых данных (retention). Пример: SELECT purge_logs('30 days');
CREATE OR REPLACE FUNCTION purge_logs(keep interval) RETURNS bigint
LANGUAGE sql AS $$
    WITH d AS (DELETE FROM log_entries WHERE ts < now() - keep RETURNING 1)
    SELECT count(*) FROM d;
$$;
