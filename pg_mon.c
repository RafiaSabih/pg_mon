/* -------------------------------------------------------------------------
 *
 * pg_mon.c
 *
 * Copyright (c) 2010-2019, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *		contrib/pg_mon/pg_mon.c
 * -------------------------------------------------------------------------
 */
#include <postgres.h>

#include <limits.h>

#include <miscadmin.h>
#include "storage/lwlock.h"
#include "storage/ipc.h"
#include "storage/shmem.h"
#if PG_VERSION_NUM >= 100000
#include "utils/hashutils.h"
#endif
#include "utils/hsearch.h"
#include "utils/memutils.h"
#include "utils/tuplestore.h"
#include "utils/timestamp.h"
#include "utils/acl.h"
#include "funcapi.h"

#include "access/parallel.h"
#include "commands/explain.h"
#include "executor/instrument.h"
#include "jit/jit.h"
#include "utils/guc.h"

#include "nodes/plannodes.h"
#include "catalog/pg_type_d.h"
#include "mb/pg_wchar.h"

Datum		pg_mon(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(pg_mon);

PG_MODULE_MAGIC;

/* GUC variables */
static int	query_monitor_min_duration = 1000; /* set -1 for disable, in MS */
static bool     query_monitor_timing = true;
static bool     query_monitor_nested_statements = true;

#define MON_COLS  13
#define MON_HT_SIZE       1024

#define NUMBUCKETS 10
#define HIST_THRESH 1.001
#define BUCKET_THRESH 0.1
/*
 * Record for a query.
 */
typedef struct mon_rec
{
        int64 queryid;
        double current_total_time;
        double current_expected_rows;
        double last_expected_rows;
        double current_actual_rows;
        double last_actual_rows;
        int seq_scans ;
        int index_scans;
        int NestedLoopJoin ;
        int HashJoin;
        int MergeJoin;
        float8   buckets[NUMBUCKETS];
        int freq[NUMBUCKETS];
}mon_rec;

/* Current nesting depth of ExecutorRun calls */
static int	nesting_level = 0;

extern void _PG_init(void);
extern void _PG_fini(void);

/* LWlock to mange the reading and writing the hash table. */
#if PG_VERSION_NUM < 90400
LWLockId	mon_lock;
#else
LWLock	   *mon_lock;
#endif

/* Saved hook values in case of unload */
static ExecutorStart_hook_type prev_ExecutorStart = NULL;
static ExecutorRun_hook_type prev_ExecutorRun = NULL;
static ExecutorFinish_hook_type prev_ExecutorFinish = NULL;
static ExecutorEnd_hook_type prev_ExecutorEnd = NULL;

static void explain_ExecutorStart(QueryDesc *queryDesc, int eflags);
static void explain_ExecutorRun(QueryDesc *queryDesc,
                                                                ScanDirection direction,
                                                                uint64 count, bool execute_once);
static void explain_ExecutorFinish(QueryDesc *queryDesc);
static void explain_ExecutorEnd(QueryDesc *queryDesc);
static void pgmon_plan_store(QueryDesc *queryDesc);
static void pgmon_exec_store(QueryDesc *queryDesc);

/* Saved hook values in case of unload */
static shmem_startup_hook_type prev_shmem_startup_hook = NULL;
static void shmem_shutdown(int code, Datum arg);

static void plan_tree_traversal(Plan *plan_node, mon_rec *entry);
static mon_rec * create_histogram(mon_rec *entry);
static void sort_hist(double *buckets, int *freq, int len);
static void swapd(double *xp, double *yp);
static void swapi(int *xp, int *yp);
static bool inbetween(double low, double val, double high);
static bool require_repartition(double *buckets, int *freq);

/* Hash table in the shared memory */
static HTAB *mon_ht;

/*
 * shmem_startup hook: allocate and attach to shared memory,
 */
static void
shmem_startup(void)
{
        HASHCTL		info;

        if (prev_shmem_startup_hook)
                prev_shmem_startup_hook();

        mon_ht = NULL;

        /*
         * Create or attach to the shared memory state, including hash table
         */
        LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);

        memset(&info, 0, sizeof(info));
        info.keysize = sizeof(uint32);
        info.entrysize = sizeof(mon_rec);
#if PG_VERSION_NUM > 100000
        info.hash = uint32_hash;

        mon_ht = ShmemInitHash("mon_hash", MON_HT_SIZE, MON_HT_SIZE,
                                &info, HASH_ELEM | HASH_FUNCTION);
#else
        mon_ht = ShmemInitHash("mon_hash", MON_HT_SIZE, MON_HT_SIZE,
                                &info, HASH_ELEM);
#endif
#if PG_VERSION_NUM < 90600
        mon_lock = LWLockAssign();
#else
        mon_lock = &(GetNamedLWLockTranche("mon_lock"))->lock;
#endif
        LWLockRelease(AddinShmemInitLock);

        /*
         * If we're in the postmaster (or a standalone backend...), set up a shmem
         * exit hook to dump the statistics to disk.
         */
        if (!IsUnderPostmaster)
                on_shmem_exit(shmem_shutdown, (Datum) 0);
}

/*
 * shmem_shutdown hook
 *
 * Note: we don't bother with acquiring lock, because there should be no
 * other processes running when this is called.
 */
static void
shmem_shutdown(int code, Datum arg)
{
        mon_ht = NULL;

        return;
}

/*
 * Estimate shared memory space needed.
 */
static Size
qmon_memsize(void)
{
        return hash_estimate_size(MON_HT_SIZE, sizeof(mon_rec));

}

/*
 * Module Load Callback
 */
void
_PG_init(void)
{
        /* Define custom GUC variables. */
        DefineCustomIntVariable("query_monitor.min_duration",
                                                        "Sets the minimum execution time above which plans will be logged.",
                                                        "Zero prints all plans. -1 turns this feature off.",
                                                        &query_monitor_min_duration,
                                                        query_monitor_min_duration,
                                                        -1, INT_MAX,
                                                        PGC_SUSET,
                                                        GUC_UNIT_MS,
                                                        NULL,
                                                        NULL,
                                                        NULL);

        DefineCustomBoolVariable("query_monitor.nested_statements",
                                                         "Monitor nested statements.",
                                                         NULL,
                                                         &query_monitor_nested_statements,
                                                         query_monitor_nested_statements,
                                                         PGC_SUSET,
                                                         0,
                                                         NULL,
                                                         NULL,
                                                         NULL);

        DefineCustomBoolVariable("query_monitor.timing",
                                                         "Collect timing data, not just row counts.",
                                                         NULL,
                                                         &query_monitor_timing,
                                                         query_monitor_timing,
                                                         PGC_SUSET,
                                                         0,
                                                         NULL,
                                                         NULL,
                                                         NULL);

        /*
         * Request additional shared resources.  (These are no-ops if we're not in
         * the postmaster process.)  We'll allocate or attach to the shared
         * resources in *_shmem_startup().
         */
        RequestAddinShmemSpace(qmon_memsize());
#if PG_VERSION_NUM < 90600
        RequestAddinLWLocks(1);
#else
        RequestNamedLWLockTranche("mon_lock", 1);
#endif

        /* Install Hooks */
        prev_shmem_startup_hook = shmem_startup_hook;
        shmem_startup_hook = shmem_startup;
        prev_ExecutorStart = ExecutorStart_hook;
        ExecutorStart_hook = explain_ExecutorStart;
        prev_ExecutorRun = ExecutorRun_hook;
        ExecutorRun_hook = explain_ExecutorRun;
        prev_ExecutorFinish = ExecutorFinish_hook;
        ExecutorFinish_hook = explain_ExecutorFinish;
        prev_ExecutorEnd = ExecutorEnd_hook;
        ExecutorEnd_hook = explain_ExecutorEnd;
}

/*
 * Module unload callback
 */
void
_PG_fini(void)
{
        /* Uninstall hooks. */
        shmem_startup_hook = prev_shmem_startup_hook;
        ExecutorStart_hook = prev_ExecutorStart;
        ExecutorRun_hook = prev_ExecutorRun;
        ExecutorFinish_hook = prev_ExecutorFinish;
        ExecutorEnd_hook = prev_ExecutorEnd;

}


/*
 * ExecutorStart hook: start up logging if needed
 */
static void
explain_ExecutorStart(QueryDesc *queryDesc, int eflags)
{
        if (query_monitor_min_duration >= 0)
        {
                /* Enable per-node instrumentation iff log_analyze is required. */
                if (query_monitor_timing)
                    queryDesc->instrument_options |= INSTRUMENT_TIMER;
                else
                    queryDesc->instrument_options |= INSTRUMENT_ROWS;
        }

        if (prev_ExecutorStart)
                prev_ExecutorStart(queryDesc, eflags);
        else
                standard_ExecutorStart(queryDesc, eflags);

        if (query_monitor_min_duration >= 0)
        {
            pgmon_plan_store(queryDesc);
                /*
                 * Set up to track total elapsed time in ExecutorRun.  Make sure the
                 * space is allocated in the per-query context so it will go away at
                 * ExecutorEnd.
                 */
                if (queryDesc->totaltime == NULL)
                {
                        MemoryContext oldcxt;

                        oldcxt = MemoryContextSwitchTo(queryDesc->estate->es_query_cxt);
                        queryDesc->totaltime = InstrAlloc(1, INSTRUMENT_ALL);
                        MemoryContextSwitchTo(oldcxt);
                }
        }
}

/*
 * ExecutorRun hook: all we need do is track nesting depth
 */
static void
explain_ExecutorRun(QueryDesc *queryDesc, ScanDirection direction,
                                        uint64 count, bool execute_once)
{
        nesting_level++;
        PG_TRY();
        {
                if (prev_ExecutorRun)
                        prev_ExecutorRun(queryDesc, direction, count, execute_once);
                else
                        standard_ExecutorRun(queryDesc, direction, count, execute_once);
        }
        PG_FINALLY();
        {
                nesting_level--;
        }
        PG_END_TRY();
}

/*
 * ExecutorFinish hook: all we need do is track nesting depth
 */
static void
explain_ExecutorFinish(QueryDesc *queryDesc)
{
        nesting_level++;
        PG_TRY();
        {
                if (prev_ExecutorFinish)
                        prev_ExecutorFinish(queryDesc);
                else
                        standard_ExecutorFinish(queryDesc);
        }
        PG_FINALLY();
        {
                nesting_level--;
        }
        PG_END_TRY();
}

/*
 * ExecutorEnd hook: log results if needed
 */
static void
explain_ExecutorEnd(QueryDesc *queryDesc)
{
        if (queryDesc->totaltime && query_monitor_min_duration >= 0)
        {
                double		msec;

                /*
                 * Make sure stats accumulation is done.  (Note: it's okay if several
                 * levels of hook all do this.)
                 */
                InstrEndLoop(queryDesc->totaltime);

                /* Save query information if duration is exceeded. */
                msec = queryDesc->totaltime->total * 1000;
                if (msec >= query_monitor_min_duration)
                        pgmon_exec_store(queryDesc);
        }

        if (prev_ExecutorEnd)
                prev_ExecutorEnd(queryDesc);
        else
                standard_ExecutorEnd(queryDesc);
}

static void
pgmon_plan_store(QueryDesc *queryDesc)
{
        mon_rec  *entry, *temp_entry;
        int64	queryId =  queryDesc->plannedstmt->queryId;
        bool    found = false;

        Assert(queryDesc!= NULL);

        /* Safety check... */
        if (!mon_ht)
                return;

        /* Keep a temporary record to store the plan information of current query */
        temp_entry = (mon_rec *) palloc(sizeof(mon_rec));
        memset(&temp_entry->queryid, 0 , sizeof(mon_rec));
        temp_entry->queryid = queryId;
        plan_tree_traversal(queryDesc->plannedstmt->planTree, temp_entry);
        temp_entry->current_expected_rows = queryDesc->planstate->plan->plan_rows;

        /* Lookup the hash table entry and create one if not found. */
        LWLockAcquire(mon_lock, LW_EXCLUSIVE);

        entry = (mon_rec *) hash_search(mon_ht, &queryId, HASH_ENTER_NULL, &found);

        /* Create new entry, if not present */
        if (!found)
        {
            /* Update the plan information for the entry*/
            *entry = *temp_entry;
            memset(entry->buckets, 0, NUMBUCKETS * sizeof(double));
            memset(entry->freq, 0, NUMBUCKETS * sizeof(int));
            LWLockRelease(mon_lock);
        }
        else
        {
            entry->last_expected_rows = entry->current_expected_rows;
            entry->last_actual_rows = entry->current_actual_rows;
            LWLockRelease(mon_lock);

            /* Check if there are any changes in the plan now */
            if (entry->HashJoin != temp_entry->HashJoin ||
                entry->MergeJoin != temp_entry->MergeJoin ||
                entry->NestedLoopJoin != temp_entry->NestedLoopJoin ||
                entry->seq_scans != temp_entry->seq_scans ||
                entry->index_scans != temp_entry->index_scans)

                 ereport(NOTICE,
                        (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                        errmsg("query repeated but there are changes in plan")));
        }
        pfree(temp_entry);
}

static void
pgmon_exec_store(QueryDesc *queryDesc)
{
        mon_rec  *entry;
        int64	queryId = queryDesc->plannedstmt->queryId;
        bool    found = false;

        Assert(queryDesc!= NULL);

        /* Safety check... */
        if (!mon_ht)
                return;

        /* Lookup the hash table entry and create one if not found. */
        LWLockAcquire(mon_lock, LW_EXCLUSIVE);

        entry = (mon_rec *) hash_search(mon_ht, &queryId, HASH_ENTER_NULL, &found);

        if (entry->current_total_time != 0)
        {
           entry->last_actual_rows = entry->current_actual_rows;
        }

        entry->current_total_time = queryDesc->totaltime->total * 1000.0;
        entry->current_actual_rows = queryDesc->totaltime->ntuples;

        entry = create_histogram(entry);

        LWLockRelease(mon_lock);
}

void
plan_tree_traversal(Plan *plan_node, mon_rec *entry)
{
    /* Iterate through the plan to find all the required nodes*/
            if (plan_node != NULL)
            {
                switch(plan_node->type)
                {
                    case T_SeqScan:
                        entry->seq_scans++;
                        break;
                    case T_IndexScan:
                    case T_IndexOnlyScan:
                    case T_BitmapIndexScan:
                    case T_BitmapHeapScan:
                        entry->index_scans++;
                        break;
                    case T_NestLoop:
                        entry->NestedLoopJoin++;
                        break;
                    case T_MergeJoin:
                        entry->MergeJoin++;
                        break;
                    case T_HashJoin:
                        entry->HashJoin++;
                        break;
                    default:
                        break;
                }
                plan_tree_traversal(plan_node->lefttree, entry);
                plan_tree_traversal(plan_node->righttree, entry);
            }
}
/*
 * This is called when user requests the pg_mon view.
 */
Datum
pg_mon(PG_FUNCTION_ARGS)
{
        ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
        TupleDesc	tupdesc;
        Tuplestorestate *tupstore;
        MemoryContext per_query_ctx;
        MemoryContext oldcontext;
        HASH_SEQ_STATUS status;
        mon_rec *entry;

        /* hash table must exist already */
        if (!mon_ht)
                ereport(ERROR,
                                (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                                 errmsg("pg_mon must be loaded via shared_preload_libraries")));

        /* Switch into long-lived context to construct returned data structures */
        per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
        oldcontext = MemoryContextSwitchTo(per_query_ctx);

        /* Build a tuple descriptor for our result type */
        if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
                elog(ERROR, "return type must be a row type");

        tupstore = tuplestore_begin_heap(true, false, work_mem);

        MemoryContextSwitchTo(oldcontext);

        LWLockAcquire(mon_lock, LW_SHARED);

        hash_seq_init(&status, mon_ht);
        while ((entry = hash_seq_search(&status)) != NULL)
        {
                Datum		values[MON_COLS];
                bool		nulls[MON_COLS] = {0};
                int			i = 0;

                memset(values, 0, sizeof(values));
                memset(nulls, 0, sizeof(nulls));

                values[i++] = Int64GetDatum(entry->queryid);
                values[i++] = Float8GetDatumFast(entry->current_total_time);
                values[i++] = Float8GetDatumFast(entry->current_expected_rows);
                if (entry->last_expected_rows == 0)
                    nulls[i++] = true;
                else
                    values[i++] = Float8GetDatumFast(entry->last_expected_rows);

                values[i++] = Float8GetDatumFast(entry->current_actual_rows);

                if (entry->last_actual_rows == 0)
                    nulls[i++] = true;
                else
                    values[i++] = Float8GetDatumFast(entry->last_actual_rows);

                if (entry->seq_scans == 0)
                    nulls[i++] = true;
                else
                    values[i++] = Int32GetDatum(entry->seq_scans);

                if (entry->index_scans == 0)
                    nulls[i++] = true;
                else
                    values[i++] = Int32GetDatum(entry->index_scans);

                if (entry->NestedLoopJoin == 0)
                    nulls[i++] = true;
                else
                    values[i++] = Int32GetDatum(entry->NestedLoopJoin);

                if (entry->HashJoin == 0)
                    nulls[i++] = true;
                else
                    values[i++] = Int32GetDatum(entry->HashJoin);

                if (entry->MergeJoin == 0)
                    nulls[i++] = true;
                else
                    values[i++] = Int32GetDatum(entry->MergeJoin);

                if(entry->buckets == NULL)
                    nulls[i++] = true;
                else
                {
                    Datum	   *numdatums = (Datum *) palloc(NUMBUCKETS * sizeof(Datum));
                    ArrayType  *arry;
                    int n, idx=0;
                    for (n = 0; n < NUMBUCKETS && entry->buckets[n] > 0; n++)
                            numdatums[idx++] = Float8GetDatum(entry->buckets[n]);
                    arry = construct_array(numdatums, idx, FLOAT8OID, sizeof(float8), true, 'd');
                    values[i++] = PointerGetDatum(arry);
                }

                if(entry->freq == NULL)
                    nulls[i++] = true;
                else
                {
                    Datum	   *numdatums = (Datum *) palloc(NUMBUCKETS * sizeof(Datum));
                    ArrayType  *arry;
                    int n, idx=0;
                    for (n = 0; n < NUMBUCKETS && entry->freq[n] > 0; n++)
                            numdatums[idx++] = Int8GetDatum(entry->freq[n]);
                    arry = construct_array(numdatums, idx, INT4OID, sizeof(int), true, 'i');
                    values[i++] = PointerGetDatum(arry);
                }

                tuplestore_putvalues(tupstore, tupdesc, values, nulls);
        }

        LWLockRelease(mon_lock);

        /* clean up and return the tuplestore */
        tuplestore_donestoring(tupstore);

        rsinfo->returnMode = SFRM_Materialize;
        rsinfo->setResult = tupstore;
        rsinfo->setDesc = tupdesc;

        return (Datum) 0;
}

static mon_rec * create_histogram(mon_rec *entry){

    int i;
    float8 val = entry->current_total_time;
    bool done = false;

    /* Case 0: This is the first bucket to be created*/
    if (entry->buckets[0] == 0){
        entry->buckets[0] = entry->current_total_time;
        entry->freq[0] = 1;
    }
    else{
        /* Find the next empty bucket */
        for (i = 0; entry->buckets[i] != 0; i++)
        {
            /* Case 1: find if the current time fits in available buckets */
            if ((1.0 - BUCKET_THRESH) * entry->buckets[i] <= val && val <= (1 + BUCKET_THRESH) *entry->buckets[i])
            {
                entry->freq[i]++;
                done = true;
            }
        }
        if (!done && i < NUMBUCKETS)
        {
            /* Case 2: fill the next empty bucket with the current time */
            entry->buckets[i] = val;
            entry->freq[i] = 1;
            done = true;
            sort_hist(entry->buckets, entry->freq, i+1);
        }
        else if (!done)
        {
            /* Case 3: find in which bucket the current value goes */
            for (i = 1; i < NUMBUCKETS ; i++)
            {
                if (inbetween(entry->buckets[i-1], val, entry->buckets[i]))
                {
                    entry->freq[i]++;
                    done = true;
                }
            }
            if (!done && val > entry->buckets[NUMBUCKETS -1])
            {
                /* Case 4: when value is beyond the last bucket, extend the bucket */
                entry->buckets[NUMBUCKETS -1] = val;
                entry->freq[NUMBUCKETS -1]++;
                done = true;
            }
            if (require_repartition(entry->buckets, entry->freq))
            {
                /* Case 6: repartition the histogram */
            }
        }
    }

    return entry;
}

static bool require_repartition(float8 *buckets, int *freq)
{
    int i, sum = 0, total = 0;
    for (i = 0; i < NUMBUCKETS; i++)
    {
        total += freq[i];
    }
    for (i = 0; i < NUMBUCKETS; i++)
    {
        sum += (freq[i] - (freq[i]/total));
    }
    if (sum < HIST_THRESH)
        return true;
    return false;
}

static bool inbetween(float8 low, float8 val, float8 high){
    if(val * BUCKET_THRESH < low)
        return false;
    if(val * BUCKET_THRESH < high && val > low)
        return true;
    return false;
}

static void sort_hist(float8 *buckets, int *freq, int len){
    int i, j;
    for (i = 0; i < len-1; i++){
        // Last i elements are already in place
        for (j = 0; j < len-i-1; j++)
            if (buckets[j] > buckets[j+1])
            {
                 swapd(&buckets[j], &buckets[j+1]);
                 swapi(&freq[j], &freq[j+1]);
            }
    }
}

static void swapd(float8 *xp, float8 *yp)
{
    double temp = *xp;
    *xp = *yp;
    *yp = temp;
}
void swapi(int *xp, int *yp)
{
    double temp = *xp;
    *xp = *yp;
    *yp = temp;
}