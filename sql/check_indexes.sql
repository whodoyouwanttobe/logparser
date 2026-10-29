-- Диагностика: выполнить на копии БД с характерными данными, после создания актуальной схемы. Не требуется для запуска приложения.
-- Запросы не удаляют пользовательские строки. ANALYZE обновляет статистику планировщика.
ANALYZE sources;
ANALYZE log_entries;
ANALYZE parse_errors;
SELECT indexname, indexdef FROM pg_indexes
WHERE schemaname='public' AND tablename IN ('sources','log_entries','parse_errors')
ORDER BY tablename,indexname;
SELECT relname,indexrelname,idx_scan,pg_size_pretty(pg_relation_size(indexrelid)) AS index_size
FROM pg_stat_user_indexes ORDER BY relname,indexrelname;

EXPLAIN (ANALYZE, BUFFERS)
SELECT * FROM log_entries ORDER BY ts DESC,id DESC LIMIT 50;
EXPLAIN (ANALYZE, BUFFERS)
SELECT * FROM log_entries WHERE trace_id='a1b2c3' ORDER BY ts,id;
EXPLAIN (ANALYZE, BUFFERS)
SELECT coalesce(max(attempt),0)+1 FROM sources WHERE path='samples/sample.log';
-- Наблюдаем цену удаления необязательных индексов на тех же данных:
EXPLAIN (ANALYZE, BUFFERS)
SELECT * FROM log_entries WHERE severity<=3 ORDER BY ts DESC,id DESC LIMIT 50;
EXPLAIN (ANALYZE, BUFFERS)
SELECT * FROM log_entries WHERE program='sshd' ORDER BY ts DESC,id DESC LIMIT 50;
EXPLAIN (ANALYZE, BUFFERS)
SELECT * FROM log_entries WHERE message ILIKE '%failed%' ORDER BY ts DESC,id DESC LIMIT 50;
