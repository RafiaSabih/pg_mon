# pg_mon

PostgreSQL extension to enhance query monitoring

In this extension we solve a couple of important monitoring issues, viz, better
execution time monitoring by histograms and saving important query plan
information in shared hash table which is available via view --pg_mon. The
information available via the view is as follows,

| queryid |  total_time  | first_tuple_time | expected_rows | actual rows | is_parallel_query | update_query | seq_scans | index_scans | bitmap_scans | other_scans | nested_loop_join_count | merge_join_count | hash_join_count | hist_time_ubounds | hist_time_freq | hist_actual_rows_bucket_ubounds | hist_actual_rows_bucket_freq | hist_est_rows_bucket_ubounds | hist_est_rows_bucket_freq |
|---------|--------------|------------------|---------------|-------------|-------------------|--------------|-----------|-------------|--------------|-------------|------------------------|------------------|-----------------|-------------------|----------------|---------------------------------|------------------------------|------------------------------|---------------------------|
|         |              |                  |               |             |                   |              |           |             |              |             |                        |                  |                 |                   |                |                                 |                              |                              |                           |

- For better monitoring of query instances, it saves execution time
   histograms. The columns corresponding to this information in the view are,

    | hist_time_ubounds | hist_time_freq |
    |-------------------|----------------|
    |                   |                |

   To enhance the experience more, there are also histograms for
   estimated and actual number of rows, this is available in  columns,

    | hist_actual_rows_bucket_ubounds | hist_actual_rows_bucket_freq | hist_est_rows_bucket_ubound | hist_est_rows_bucket_freq |
    |---------------------------------|------------------------------|-----------------------------|---------------------------|
    |                                 |                              |                             |                           |

- An in-memory hash-table stores the important information from query plans,
   viz, name of the tables using sequential scans, name of indexes used in the
   query, number and types of different join methods in the query.

## Installation

1. Run make and make install in the pg_mon folder
2. Add pg_mon to shared_preload_libraries
3. Run 'create extension pg_mon;' in client

## How to use the extension

- once enabled, it keeps on saving the information of all the running queries.
- one may view the contents of the view using 'select * from pg_mon; '
- pg_mon.plan_info_immediate -   boolean guc to set if plan time information should
                                 be made available immediately after planning phase.
                                 By default this is set to false. There might be
                                 locking overhead incurred by setting this to true.
- pg_mon.plan_info_disable - another boolean GUC to skip saving any information regarding
                             the query plan, like scans, joins, etc.

More details on the information available via the extension

- seq_scan - name of the relation(s) using seq scan in the query
- indexes - name of the index(es) used by the query. The indexes used by
            index scan or index only access methods are in index_scans attribute.
            However, ones using bitmap_scans are listed under the attribute bitmap_scans.
- join information - The view also contains columns for each of the three
                       join methods, and value in them shows the total number
                       of joins of the corresponding type in the query.
After every run, it checks if there are any new scans used in the latest run
of the query and add them to the entry. Join information is corresponding to the
latest run of the query.

- query time histogram - timing of all the runs of a query are summarized in a
                          histogram. This is split between two columns,
  - hist_time_ubounds - This contains the upper bound of the buckets, since this is
              serial histogram lower bound can be taken as the end of the
              pervious bucket.
  - hist_time_freq - This contains the corresponding frequencies of the buckets.

    In the current version, the number of histogram buckets is fixed to thirty.
    Also the upper bounds of the buckets is fixed to the following,
    {1, 5, 10, 15, 20, 25, 30, 40, 50, 60, 70, 80, 90, 100, 200, 300, 400, 500,
    600, 700, 1000, 2000, 3000, 5000, 7000, 10000, 20000, 30000, 50000, 60000}

    Similarly, the histograms for the actual and estimated number of rows is
    also present in the view. The upper bounds for the buckets in these
    histograms are, {1, 5, 10, 50, 100, 200, 300, 400, 500, 1000, 2000, 3000,
                    4000, 5000, 10000, 30000, 50000, 70000, 100000, 1000000}.
