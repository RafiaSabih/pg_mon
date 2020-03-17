# pg_mon

PostgreSQL extension to save query information

The goal of this extension is to to provide some plan level details of the
executed queries. Instead of dumping the whole explain or analyze plan into
log files, this extension extracts important information from the query plans
viz scan and join types, number of planned and actual rows and total time of
query and save them in a convenient view.


How to install the extension:

1. Run make and make install in the pg_mon folder
2. Add pg_mon to shared_preload_libraries
3. Run ' create extension pg_mon; ' in client


How to use the extension:

- anytime you may view the contents of the view using ' select * from pg_mon; '
- Some configuration parameters related to the extension are,
    - query_monitor.min_duration - minimum duration (in ms) of the queries to be
                                   monitored via this extension. Setting it to -1
                                   disables the extension. Setting it to 0 will
                                   monitor all the queries. Default value is 1 second.
    - query_monitor.timing - boolean to set if we want to monitor also the execution time
                             of the queries. Default value is true. Note that monitoring
                             queries for execution time is likely to have some execution overheads.
    - query_monitor.nested_statements - boolean guc to set if monitor also the nested statement.
                                        Default value is set to true. This is particularly useful
                                        when monitoring queries executed in function calls, etc.

Important information available via the extension:

 - indexes - name of the index(es) used by the query
 - seq_scan - name of the relation(s) using seq scan in the query
 - query time histogram - timing of all the runs of a query are summarised in a histogram.
                          This is serial based equi-depth histogram. this information is
                          available via two columns of the view,        
    - buckets - This contains the upper bound of the buckets, since this is serial histogram
                    lower bound can be taken as the end of the pervious bucket.
    - frequencies - This contains the corresponding frequencies of the buckets.
             In the current version, the number of histogram buckets is fixed to ten.
 - joins information - The view also contains columns for each of the three join methods, and
                        value in them shows the total number of joins of the corresponding type
                        in the query.
 - number of rows - This view also gives the information of total number of estimated and actual
                     rows in the current and last run of the query.
