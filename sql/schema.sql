-- logparser: схема БД (PostgreSQL 13+)
-- Применение:  psql -d logs -f sql/schema.sql   или   ./logparser init
-- Скрипт идемпотентен: можно запускать повторно.

CREATE EXTENSION IF NOT EXISTS pg_trgm;   -- быстрый поиск подстроки (ILIKE '%...%')

-- Справочник уровней важности (RFC 5424)
CREATE TABLE IF NOT EXISTS severities (
    code  smallint PRIMARY KEY CHECK (code BETWEEN 0 AND 7),
    name  text NOT NULL UNIQUE
);
INSERT INTO severities(code, name) VALUES
    (0,'emerg'),(1,'alert'),(2,'crit'),(3,'err'),
    (4,'warning'),(5,'notice'),(6,'info'),(7,'debug')
ON CONFLICT DO NOTHING;

-- Источники (файлы). Хранят позицию чтения => повторный import дочитывает только новое.
-- inode нужен, чтобы заметить ротацию (logrotate): новый inode => читаем файл с начала.
CREATE TABLE IF NOT EXISTS sources (
    id           serial PRIMARY KEY,
    path         text   NOT NULL UNIQUE,
    inode        bigint,
    last_offset  bigint NOT NULL DEFAULT 0,
    lines_total  bigint NOT NULL DEFAULT 0,
    updated_at   timestamptz NOT NULL DEFAULT now()
);

-- Основная таблица событий
CREATE TABLE IF NOT EXISTS log_entries (
    id            bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    ts            timestamptz NOT NULL,
    host          text        NOT NULL,
    program       text,
    pid           integer,
    severity      smallint    NOT NULL REFERENCES severities(code),
    sev_inferred  boolean     NOT NULL DEFAULT true,  -- true: уровень угадан по тексту, false: взят из <PRI>
    facility      smallint    CHECK (facility BETWEEN 0 AND 23),
    message       text        NOT NULL,
    source_id     integer     REFERENCES sources(id) ON DELETE SET NULL,
    imported_at   timestamptz NOT NULL DEFAULT now()
);

-- Индексы под реальные запросы CLI:
--  * BRIN по времени: логи пишутся почти по порядку, индекс крошечный и быстрый для диапазонов
CREATE INDEX IF NOT EXISTS log_entries_ts_brin   ON log_entries USING brin (ts);
--  * фильтр "уровень не ниже X за период"
CREATE INDEX IF NOT EXISTS log_entries_sev_ts    ON log_entries (severity, ts DESC);
--  * фильтр по программе (sshd, kernel, systemd ...)
CREATE INDEX IF NOT EXISTS log_entries_prog_ts   ON log_entries (program, ts DESC);
--  * поиск подстроки в сообщении
CREATE INDEX IF NOT EXISTS log_entries_msg_trgm  ON log_entries USING gin (message gin_trgm_ops);

-- Строки, которые не удалось разобрать (не теряем данные, разбираемся потом)
CREATE TABLE IF NOT EXISTS parse_errors (
    id          bigint GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    source_id   integer REFERENCES sources(id) ON DELETE CASCADE,
    line_no     bigint,
    raw         text NOT NULL,
    created_at  timestamptz NOT NULL DEFAULT now()
);

-- Представления для отчётов
CREATE OR REPLACE VIEW v_severity_stats AS
SELECT s.code, s.name, count(e.id) AS cnt
FROM severities s
LEFT JOIN log_entries e ON e.severity = s.code
GROUP BY s.code, s.name
ORDER BY s.code;

CREATE OR REPLACE VIEW v_errors_hourly AS
SELECT date_trunc('hour', ts) AS hour,
       count(*) FILTER (WHERE severity <= 3) AS errors,
       count(*) FILTER (WHERE severity = 4)  AS warnings,
       count(*)                              AS total
FROM log_entries
GROUP BY 1;

-- Очистка старых данных (retention). Пример: SELECT purge_logs('30 days');
CREATE OR REPLACE FUNCTION purge_logs(keep interval) RETURNS bigint
LANGUAGE sql AS $$
    WITH d AS (DELETE FROM log_entries WHERE ts < now() - keep RETURNING 1)
    SELECT count(*) FROM d;
$$;
