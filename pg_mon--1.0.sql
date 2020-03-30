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
    OUT current_total_time float8,
    OUT first_tuple_time float8,
    OUT current_expected_rows float8,
    OUT last_expected_rows float8,
    OUT current_actual_rows float8,
    OUT last_actual_rows float8,
    OUT is_parallel_query bool,
    OUT modify_table bool,
    OUT seq_scans name[],
    OUT index_scans name[],
    OUT other_scans name,
    OUT nested_loop_join int,
    OUT hash_join int,
    OUT merge_join int,
    OUT buckets float8[],
    OUT frequencies int[]
)
RETURNS SETOF record
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT VOLATILE;

CREATE VIEW pg_mon AS
  SELECT * FROM pg_mon();

GRANT SELECT ON pg_mon TO PUBLIC;

CREATE FUNCTION pg_mon_reset()
RETURNS void
AS 'MODULE_PATHNAME'
LANGUAGE C PARALLEL SAFE;
