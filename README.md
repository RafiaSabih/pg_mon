# pg_mon

PostgreSQL extension to enhance query monitoring

In this extension we solve a couple of important monitoring issues, viz, better
execution time monitoring by histograms and saving important query plan
information in shared hash table which is available via view --pg_mon.

- For better understanding of query instances, it saves execution time
   histograms. To enhance the experience more, there are also histograms for
   estimated and actual number of rows.
- An in-memory hash-table stores the important information from query plans,
   viz, name of the tables using sequential scans, name of indexes used in the
   query, number and types of different join methods in the query.

How to install the extension

1. Run make and make install in the pg_mon folder
2. Add pg_mon to shared_preload_libraries
3. Run 'create extension pg_mon;' in client

How to use the extension

- anytime you may view the contents of the view using 'select * from pg_mon; '
- Some configuration parameters related to the extension are,
  - pg_mon.enable_timing - boolean to set if we want to monitor also the execution
                     time of the queries. Default value is true. Note that
                     monitoring queries for execution time is likely to have
                     some execution overheads.

  - pg_mon.min_duration - minimum duration (in ms) of the queries to be
                            monitored. Setting it to -1 effectively disables
                            the extension. Setting it to 0 will monitor all the
                            queries. Default value is 0.
                            Note that if the extension is enabled timing is set
                            to true and min_duration is also anything above or
                            equal to  0, then even though the queries executed
                            on the server are less than min_duration,
                            the performance overhead for logging timing will
                            still be there. With this parameter one can only
                            control what to save in the respective view.

  - pg_mon.plan_info_immediate - boolean guc to set if plan time information should
                                 be made available immediately after planning phase.
                                 By default this is set to false. There might be
                                 locking overhead incurred by setting this to true.
  - pg_mon.nested_statements - boolean guc to set if monitor also the nested
                                 statement. Default value is set to true.
                                 This is particularly useful when monitoring
                                 queries executed in function calls, etc.

Important information available via the extension

- indexes - name of the index(es) used by the query
- seq_scan - name of the relation(s) using seq scan in the query
- join information - The view also contains columns for each of the three
                       join methods, and value in them shows the total number
                       of joins of the corresponding type in the query.
- query time histogram - timing of all the runs of a query are summarized in a
                          histogram. This is split between two columns,
  - buckets - This contains the upper bound of the buckets, since this is
                serial histogram lower bound can be taken as the end of the
                pervious bucket.
  - frequencies - This contains the corresponding frequencies of the buckets.

    In the current version, the number of histogram buckets is fixed to twenty.
    Also the upper bounds of the buckets is fixed to the following,
    {1, 5, 10, 15, 20, 25, 30, 40, 50, 60, 70, 80, 90, 100, 200, 300, 400, 500,
    600, 700, 1000, 2000, 3000, 5000, 7000, 10000, 20000, 30000, 50000, 60000}

    Similarly, the histograms for the actual and estimated number of rows is
    also present in the view. The upper bounds for the buckets in these
    histograms are, {1, 5, 10, 50, 100, 200, 300, 400, 500, 1000, 2000, 3000,
                    4000, 5000, 10000, 30000, 50000, 70000, 100000, 1000000}.
