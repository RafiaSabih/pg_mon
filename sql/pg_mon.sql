create extension pg_mon;
create extension pg_stat_statements;
set pg_mon.log_query = false;

create table t (i int, j text);
create table t2 (i int, j text);
insert into t values (generate_series(1,10), repeat('test_pg_mon', 10));
insert into t2 values (generate_series(1,10), repeat('test_pg_mon', 10));
analyze t;
analyze t2;

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
select * from t where i < 5;
select expected_rows, actual_rows, seq_scans, index_scans, hist_time_ubounds, hist_time_freq from pg_mon where index_scans IS NOT NULL;

--When query changes scan method
set enable_indexscan = 'off';
select * from t where i < 5;
select expected_rows, actual_rows, index_scans, bitmap_scans, hist_time_ubounds, hist_time_freq from pg_mon where index_scans IS NOT NULL and bitmap_scans IS NOT NULL;

set enable_bitmapscan = 'off';
select * from t where i < 5;
select expected_rows, actual_rows, seq_scans, index_scans, bitmap_scans, hist_time_ubounds, hist_time_freq from pg_mon where seq_scans IS NOT NULL and index_scans IS NOT NULL;

--Scan information for update statements
set enable_indexscan = 'on';
set enable_bitmapscan = 'on';
update t set i = 11 where i = 1;
select seq_scans,  index_scans, update_query from pg_mon where update_query = true;
select pg_mon_reset();
select pg_stat_statements_reset();

set enable_indexscan = 'off';
set enable_bitmapscan = 'off';
update t set i = 1;
select seq_scans, index_scans, update_query from pg_mon where update_query = true;

--Scan information for delete statements
select pg_mon_reset();
select pg_stat_statements_reset();
set enable_indexscan = 'on';
set enable_bitmapscan = 'on';
delete from t where i < 5;
select seq_scans, index_scans, update_query from pg_mon where update_query = true;
select pg_mon_reset();
select pg_stat_statements_reset();

set enable_indexscan = 'off';
set enable_bitmapscan = 'off';
delete from t;
select seq_scans, index_scans, update_query from pg_mon where update_query = true;
insert into t values (generate_series(1,10), repeat('bsdshkjd3h', 10));

--Join output
select pg_mon_reset();
select pg_stat_statements_reset();
select * from t, t2 where t.i = t2.i;
select expected_rows, actual_rows, seq_scans, index_scans, hash_join_count, hist_time_ubounds, hist_time_freq from pg_mon where hash_join_count > 0;

set enable_hashjoin = 'off';
select pg_mon_reset();
select pg_stat_statements_reset();
select * from t, t2 where t.i = t2.i;
select expected_rows, actual_rows, seq_scans, index_scans, merge_join_count, hist_time_ubounds, hist_time_freq from pg_mon where merge_join_count > 0;

set enable_mergejoin = 'off';
select pg_mon_reset();
select pg_stat_statements_reset();
select * from t, t2 where t.i = t2.i;
select expected_rows, actual_rows, seq_scans, index_scans, nested_loop_join_count, hist_time_ubounds, hist_time_freq from pg_mon where nested_loop_join_count > 0;

-- Get plan information right after planning phase
set pg_mon.plan_info_immediate = 'true';
select pg_mon_reset();
select pg_stat_statements_reset();
select * from t, t2 where t.i = t2.i;
select expected_rows, actual_rows, seq_scans, index_scans, nested_loop_join_count, hist_time_ubounds, hist_time_freq from pg_mon where nested_loop_join_count > 0;

-- Cover the path for skipping plan time information
set pg_mon.plan_info_disable = 'true';
select pg_stat_statements_reset();
select pg_mon_reset();
select * from t, t2 where t.i = t2.i;
select expected_rows, actual_rows, seq_scans, index_scans, nested_loop_join_count, hist_time_ubounds, hist_time_freq from pg_mon where expected_rows = 0 and actual_rows = 10;

--Cleanup
drop table t;
drop table t2;
drop extension pg_stat_statements;
drop extension pg_mon;