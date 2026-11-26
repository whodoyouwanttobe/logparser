\set ON_ERROR_STOP on
-- Только отдельная тестовая БД. Все строки откатываются; sequence может продвинуться.
BEGIN;
DO $$
DECLARE sid integer; eid bigint; n bigint; marker text;
BEGIN
    marker := 'contract-' || txid_current()::text || '-' || pg_backend_pid()::text;
    INSERT INTO sources(path,attempt) VALUES(marker,1) RETURNING id INTO sid;
    BEGIN
        INSERT INTO sources(path,attempt) VALUES(marker,1);
        RAISE EXCEPTION 'FAIL: duplicate attempt accepted';
    EXCEPTION WHEN unique_violation THEN NULL;
    END;
    INSERT INTO log_entries(ts,host,program,level,severity,message,source_id)
    VALUES('2099-01-01T00:00:00Z',marker,'contract','error',3,'same',sid)
    RETURNING id INTO eid;
    INSERT INTO log_entries(ts,host,program,level,severity,message,source_id)
    VALUES('2099-01-01T00:00:00Z',marker,'contract','error',3,'same',sid)
    ON CONFLICT DO NOTHING;
    GET DIAGNOSTICS n = ROW_COUNT;
    IF n <> 0 THEN RAISE EXCEPTION 'FAIL: duplicate event inserted'; END IF;
    INSERT INTO log_entries(ts,host,program,level,severity,message,source_id)
    VALUES('2099-01-01T00:00:00Z',marker,'contract','error',3,'different',sid);
    SELECT count(*) INTO n FROM log_entries WHERE source_id=sid;
    IF n <> 2 THEN RAISE EXCEPTION 'FAIL: distinct message lost'; END IF;
    BEGIN
        UPDATE log_entries SET severity=8 WHERE id=eid;
        RAISE EXCEPTION 'FAIL: invalid severity accepted';
    EXCEPTION WHEN check_violation THEN NULL;
    END;
    INSERT INTO parse_errors(source_id,line_no,raw) VALUES(sid,1,'bad');
    DELETE FROM sources WHERE id=sid;
    IF EXISTS(SELECT 1 FROM log_entries WHERE source_id=sid)
       OR EXISTS(SELECT 1 FROM parse_errors WHERE source_id=sid) THEN
        RAISE EXCEPTION 'FAIL: cascade left child rows';
    END IF;
    RAISE NOTICE 'PASS: unique attempts, duplicates, distinct messages, severity, cascade';
END $$;
ROLLBACK;
