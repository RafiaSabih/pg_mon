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
    OUT current_expected_rows float8,
    OUT last_expected_rows float8,
    OUT current_actual_rows float8,
    OUT last_actual_rows float8,
    OUT seq_scans int,
    OUT index_scans int,
    OUT NestedLoopJoin int,
    OUT HashJoin int,
    OUT MergeJoin int,
    OUT buckets float8[],
    OUT frequencies int[]
)
RETURNS SETOF record
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT VOLATILE;

CREATE VIEW pg_mon AS
  SELECT * FROM pg_mon();

GRANT SELECT ON pg_mon TO PUBLIC;
