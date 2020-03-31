/* contrib/pg_mon/pg_mon--1.0--1.1.sql */
/*
 * Author:  rsabih
 * Created: Dec 6, 2019
 */

-- complain if script is sourced in psql, rather than via ALTER EXTENSION
\echo Use "CREATE EXTENSION pg_mon" to load this file. \quit

/* Now define */
CREATE FUNCTION pg_mon(
    OUT queryid int8,
    OUT total_time float8,
    OUT first_tuple_time float8,
    OUT expected_rows_current float8,
    OUT expected_rows_lastrun float8,
    OUT actual_rows_current float8,
    OUT actual_rows_lastrun float8,
    OUT is_parallel_query bool,
    OUT update_query bool,
    OUT seq_scans name[],
    OUT index_scans name[],
    OUT other_scans name,
    OUT nested_loop_join_count int,
    OUT hash_join_count int,
    OUT merge_join_count int,
    OUT hist_buckets_ubounds float8[],
    OUT hist_freq int[]
)
RETURNS SETOF record
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT VOLATILE;

CREATE VIEW pg_mon AS
  SELECT * FROM pg_mon();

COMMENT ON COLUMN pg_mon.total_time IS 'Total time spent in query execution';
COMMENT ON COLUMN pg_mon.first_tuple_time IS 'Time spent in the processing of first tuple only';
COMMENT ON COLUMN pg_mon.expected_rows_current IS 'Expected number of rows in the current run';
COMMENT ON COLUMN pg_mon.expected_rows_lastrun IS 'Expected number of rows in the last run';
COMMENT ON COLUMN pg_mon.actual_rows_current IS 'Actual number of rows in the current run';
COMMENT ON COLUMN pg_mon.actual_rows_lastrun IS 'Actual number of rows in the last run';
COMMENT ON COLUMN pg_mon.is_parallel_query IS 'True if query is using any parallel operator';
COMMENT ON COLUMN pg_mon.update_query IS 'True if this is update query';
COMMENT ON COLUMN pg_mon.seq_scans IS 'List of relations using seq scan in the query';
COMMENT ON COLUMN pg_mon.index_scans IS 'List of indexes used in the query';
COMMENT ON COLUMN pg_mon.other_scans IS 'Name of any other scan used in the query';
COMMENT ON COLUMN pg_mon.nested_loop_join_count IS 'Count of nested loop joins in the query';
COMMENT ON COLUMN pg_mon.hash_join_count IS 'Count of hash joins in the query';
COMMENT ON COLUMN pg_mon.merge_join_count IS 'Count of merge joins in the query';
COMMENT ON COLUMN pg_mon.hist_buckets_ubounds IS 'Upper bounds of the histogram buckets for query execution times';
COMMENT ON COLUMN pg_mon.hist_freq IS 'Frequency of the respective histogram buckets for query execution times';

GRANT SELECT ON pg_mon TO PUBLIC;

CREATE FUNCTION pg_mon_reset()
RETURNS void
AS 'MODULE_PATHNAME'
LANGUAGE C PARALLEL SAFE;
