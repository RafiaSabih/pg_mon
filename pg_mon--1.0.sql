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
    OUT expected_rows float8,
    OUT actual_rows float8,
    OUT is_parallel_query bool,
    OUT update_query bool,
    OUT seq_scans oid[],
    OUT index_scans oid[],
    OUT bitmap_scans oid[],
    OUT other_scans name,
    OUT nested_loop_join_count int,
    OUT hash_join_count int,
    OUT merge_join_count int,
    OUT hist_time_ubounds int[],
    OUT hist_time_freq int[],
    OUT hist_actual_rows_bucket_ubounds int[],
    OUT hist_actual_rows_freq int[],
    OUT hist_est_rows_bucket_ubounds int[],
    OUT hist_est_rows_freq int[]
)
RETURNS SETOF record
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT VOLATILE;

CREATE VIEW pg_mon AS
  SELECT queryid, total_time, first_tuple_time,expected_rows, actual_rows, is_parallel_query,
         update_query,  (SELECT ARRAY
                                        (SELECT relname
                                        FROM pg_class
                                        WHERE oid=ANY(seq_scans))) as seq_scans,
                        (SELECT ARRAY
                                        (SELECT relname
                                        FROM pg_class
                                        WHERE oid=ANY(index_scans))) as index_scans,
                        (SELECT ARRAY
                                        (SELECT relname
                                        FROM pg_class
                                        WHERE oid=ANY(bitmap_scans))) as bitmap_scans,
          other_scans, nested_loop_join_count, hash_join_count, merge_join_count,
          hist_time_ubounds, hist_time_freq, hist_actual_rows_bucket_ubounds, hist_actual_rows_freq,
          hist_est_rows_bucket_ubounds,hist_est_rows_freq
  FROM pg_mon();

COMMENT ON COLUMN pg_mon.total_time IS 'Total time spent in query execution';
COMMENT ON COLUMN pg_mon.first_tuple_time IS 'Time spent in the processing of first tuple only';
COMMENT ON COLUMN pg_mon.expected_rows IS 'Expected number of rows in the current run';
COMMENT ON COLUMN pg_mon.actual_rows IS 'Actual number of rows in the current run';
COMMENT ON COLUMN pg_mon.is_parallel_query IS 'True if query is using any parallel operator';
COMMENT ON COLUMN pg_mon.update_query IS 'True if this is update query';
COMMENT ON COLUMN pg_mon.seq_scans IS 'List of relations using seq scan in the query';
COMMENT ON COLUMN pg_mon.index_scans IS 'List of indexes used in the query';
COMMENT ON COLUMN pg_mon.bitmap_scans IS 'List of bitmap index scans used in the query';
COMMENT ON COLUMN pg_mon.other_scans IS 'Name of any other scan used in the query';
COMMENT ON COLUMN pg_mon.nested_loop_join_count IS 'Count of nested loop joins in the query';
COMMENT ON COLUMN pg_mon.hash_join_count IS 'Count of hash joins in the query';
COMMENT ON COLUMN pg_mon.merge_join_count IS 'Count of merge joins in the query';
COMMENT ON COLUMN pg_mon.hist_time_ubounds IS 'Upper bounds of the histogram buckets for query execution times';
COMMENT ON COLUMN pg_mon.hist_time_freq IS 'Frequency of the respective histogram buckets for query execution times';
COMMENT ON COLUMN pg_mon.hist_actual_rows_bucket_ubounds IS 'Upper bounds of the histogram buckets for the number of actual rows';
COMMENT ON COLUMN pg_mon.hist_actual_rows_freq IS 'Frequency of the respective histogram buckets for number of actual rows in query';
COMMENT ON COLUMN pg_mon.hist_est_rows_bucket_ubounds IS 'Upper bounds of the histogram buckets for the number of estimated rows';
COMMENT ON COLUMN pg_mon.hist_est_rows_freq IS 'Frequency of the respective histogram buckets for number of estimated rows in query';

GRANT SELECT ON pg_mon TO PUBLIC;

CREATE FUNCTION pg_mon_reset()
RETURNS void
AS 'MODULE_PATHNAME'
LANGUAGE C PARALLEL SAFE;
