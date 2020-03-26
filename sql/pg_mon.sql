create extension pg_mon;
create extension pg_stat_statements;

set query_monitor.min_duration = 0;
set pg_stat_statements.track = 'all';

create table t (i int, j text);
insert into t values (generate_series(1,10), repeat('bsdshkjd3h', 10));
analyze t;

-- Seq scan query output
select pg_mon_reset();
select pg_stat_statements_reset();
select * from t;
select query, current_expected_rows, current_actual_rows, seq_scans, buckets, frequencies from pg_mon, pg_stat_statements where pg_stat_statements.queryid = pg_mon.queryid and query = 'select * from t';

create index on t(i);
analyze t;
set random_page_cost = 0;

--Index scan output
select pg_mon_reset();
select pg_stat_statements_reset();
select count(*) from t where i < 5;
select query, current_expected_rows, current_actual_rows, seq_scans, index_scans, buckets, frequencies from pg_mon, pg_stat_statements where pg_stat_statements.queryid = pg_mon.queryid and query = 'select count(*) from t where i < $1';

