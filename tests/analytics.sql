-- Run with psql -X -v ON_ERROR_STOP=1 -f tests/analytics.sql in a TEST database.
BEGIN;
CREATE TEMP TABLE test_owner AS
WITH s AS (INSERT INTO sources(path,attempt) VALUES ('__analytics_contract__',1) RETURNING id)
SELECT id FROM s;
INSERT INTO log_entries(ts,host,program,trace_id,severity,message,source_id)
SELECT '2026-11-01 00:00:00+00'::timestamptz + sec * interval '1 second',
       '__contract__',svc,tr,sev,msg,id
FROM test_owner CROSS JOIN (VALUES
 (0,'db','a',3,'SQLSTATE=08006'),(1,'db','a',3,'SQLSTATE=08006'),(2,'db','a',2,'SQLSTATE=08006'),
 (300,'db','a',3,'SQLSTATE=08006'),(301,'db','a',3,'SQLSTATE=08006'),
 (302,'db','a',6,'SQLSTATE=08006'),(303,'db','a',3,'SQLSTATE=080060'),
 (304,'other','a',3,'SQLSTATE=08006'),
 (10,'api','yes',3,'Inventory service unavailable'),(70,'api','yes',2,'Checkout aborted after retries'),
 (71,'api','late',3,'Inventory service unavailable'),(132,'api','late',2,'Checkout aborted after retries'),
 (20,'api','a',2,'Checkout aborted after retries'),(21,'api','a',3,'Inventory service unavailable'),
 (22,'api','b',2,'Checkout aborted after retries')
) AS t(sec,svc,tr,sev,msg);
DO $$ BEGIN
 IF (SELECT count(*) FROM v_analytics WHERE host='__contract__') <> 2
 OR NOT EXISTS (SELECT FROM v_analytics WHERE host='__contract__' AND rule='DB_LINK' AND n=3)
 OR NOT EXISTS (SELECT FROM v_analytics WHERE host='__contract__' AND rule='CHECKOUT_CHAIN' AND trace_id='yes' AND n=2)
 THEN RAISE EXCEPTION 'analytics contract failed'; END IF;
END $$;
ROLLBACK;
