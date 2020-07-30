create extension pg_mon;
create extension pg_stat_statements;

set pg_stat_statements.track = 'all';

create table t (i int, j text);
insert into t values (generate_series(1,10), repeat('bsdshkjd3h', 10));
analyze t;

-- Seq scan query output
select pg_mon_reset();
select pg_stat_statements_reset();
select * from t;
select expected_rows, actual_rows, seq_scans, hist_time_ubounds, hist_time_freq from pg_mon where seq_scans IS NOT NULL;

create index on t(i);
analyze t;
set random_page_cost = 0;

--Index scan output
select pg_mon_reset();
select pg_stat_statements_reset();
select count(*) from t where i < 5;
select expected_rows, actual_rows, seq_scans, index_scans, hist_time_ubounds, hist_time_freq from pg_mon where index_scans IS NOT NULL;
