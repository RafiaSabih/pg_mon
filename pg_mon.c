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
#include "utils/hsearch.h"
#include "utils/memutils.h"
#include "utils/tuplestore.h"
#include "funcapi.h"
#include "commands/explain.h"
#include "executor/instrument.h"
#include "utils/guc.h"

#include "nodes/plannodes.h"
#if PG_VERSION_NUM >= 130000
#include "common/hashfn.h"
#include "catalog/pg_type_d.h"
#else
#include "utils/hashutils.h"
#include "catalog/pg_type.h"
#endif
#include "mb/pg_wchar.h"
#include "utils/rel.h"
#include "utils/lsyscache.h"
#include "utils/builtins.h"
#include "nodes/plannodes.h"
#include "parser/parsetree.h"


Datum		pg_mon(PG_FUNCTION_ARGS);
Datum		pg_mon_reset(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(pg_mon);
PG_FUNCTION_INFO_V1(pg_mon_reset);

PG_MODULE_MAGIC;

/* GUC variables */
static int	min_duration = 1000; /* set -1 for disable, in MS */
static bool     timing = true;
static bool     nested_statements = true;

#define MON_COLS  20
#define MON_HT_SIZE       1024

#define NUMBUCKETS 30
#define ROWNUMBUCKETS 20
#define MAX_TABLES  30

/*
 * Record for a query.
 */
typedef struct mon_rec
{
        int64 queryid;
        double current_total_time;
        double first_tuple_time;
        double current_expected_rows;
        double current_actual_rows;
        bool is_parallel;
        bool ModifyTable;
        NameData seq_scans[MAX_TABLES];
        NameData index_scans[MAX_TABLES];
        NameData bitmap_scans[MAX_TABLES];
        NameData other_scan;
        int NestedLoopJoin ;
        int HashJoin;
        int MergeJoin;
        int64 query_time_buckets[NUMBUCKETS];
        int64 query_time_freq[NUMBUCKETS];
        int64 actual_row_buckets[ROWNUMBUCKETS];
        int64 actual_row_freq[ROWNUMBUCKETS];
        int64 est_row_buckets[ROWNUMBUCKETS];
        int64 est_row_freq[ROWNUMBUCKETS];
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

typedef enum AddHist{
            QUERY_TIME,
            ACTUAL_ROWS,
            EST_ROWS
} AddHist;

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
static void pgmon_save_firsttuple(QueryDesc *queryDesc);
/* Saved hook values in case of unload */
static shmem_startup_hook_type prev_shmem_startup_hook = NULL;
static void shmem_shutdown(int code, Datum arg);

static void plan_tree_traversal(QueryDesc *query, Plan *plan, mon_rec *entry);
static mon_rec * create_histogram(mon_rec *entry, AddHist);

/* Hash table in the shared memory */
static HTAB *mon_ht;

/* Bucket boundaries for the histogram in ms, from 5 ms to 1 minute */
int bucket_bounds[NUMBUCKETS] = {1, 5, 10, 15, 20, 25, 30, 40, 50, 60, 70, 80, 90, 100, 200, 300, 400, 500, 600, 700, 1000, 2000, 3000, 5000, 7000, 10000, 20000, 30000, 50000, 60000};

int row_bucket_bounds[ROWNUMBUCKETS] = {1, 5, 10, 50, 100, 200, 300, 400, 500, 1000, 2000, 3000, 4000, 5000, 10000, 30000, 50000, 70000, 100000, 1000000};
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
    mon_lock = &(GetNamedLWLockTranche("mon_lock"))->lock;
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
        DefineCustomIntVariable("pg_mon.min_duration",
                                                        "Sets the minimum execution time above which plans will be logged.",
                                                        "Zero prints all plans. -1 turns this feature off.",
                                                        &min_duration,
                                                        min_duration,
                                                        -1, INT_MAX,
                                                        PGC_SUSET,
                                                        GUC_UNIT_MS,
                                                        NULL,
                                                        NULL,
                                                        NULL);

        DefineCustomBoolVariable("pg_mon.nested_statements",
                                                         "Monitor nested statements.",
                                                         NULL,
                                                         &nested_statements,
                                                         nested_statements,
                                                         PGC_SUSET,
                                                         0,
                                                         NULL,
                                                         NULL,
                                                         NULL);

        DefineCustomBoolVariable("pg_mon.timing",
                                                         "Collect timing data, not just row counts.",
                                                         NULL,
                                                         &timing,
                                                         timing,
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
        RequestNamedLWLockTranche("mon_lock", 1);

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
        if (min_duration >= 0)
        {
                /* Enable per-node instrumentation iff log_analyze is required. */
                if (timing)
                    queryDesc->instrument_options |= INSTRUMENT_TIMER;
                else
                    queryDesc->instrument_options |= INSTRUMENT_ROWS;
        }

        if (prev_ExecutorStart)
                prev_ExecutorStart(queryDesc, eflags);
        else
                standard_ExecutorStart(queryDesc, eflags);

        if (min_duration >= 0)
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
#if PG_VERSION_NUM < 130000
        PG_CATCH();
	{
		nesting_level--;
		PG_RE_THROW();
	}
#else
        PG_FINALLY();
        {
                nesting_level--;
        }
#endif
        PG_END_TRY();
}

/*
 * ExecutorFinish hook: all we need do is track nesting depth
 */
static void
explain_ExecutorFinish(QueryDesc *queryDesc)
{
        nesting_level++;
        pgmon_save_firsttuple(queryDesc);
        PG_TRY();
        {
                if (prev_ExecutorFinish)
                        prev_ExecutorFinish(queryDesc);
                else
                        standard_ExecutorFinish(queryDesc);
        }
#if PG_VERSION_NUM < 130000
        PG_CATCH();
	{
		nesting_level--;
		PG_RE_THROW();
	}
#else
        PG_FINALLY();
        {
                nesting_level--;
        }
#endif
        PG_END_TRY();
}

/*
 * ExecutorEnd hook: log results if needed
 */
static void
explain_ExecutorEnd(QueryDesc *queryDesc)
{
        if (queryDesc->totaltime && min_duration >= 0)
        {
                double		msec;

                /*
                 * Make sure stats accumulation is done.  (Note: it's okay if several
                 * levels of hook all do this.)
                 */
                InstrEndLoop(queryDesc->totaltime);

                /* Save query information if duration is exceeded. */
                msec = queryDesc->totaltime->total * 1000;
                if (msec >= min_duration)
                        pgmon_exec_store(queryDesc);
        }

        if (prev_ExecutorEnd)
                prev_ExecutorEnd(queryDesc);
        else
                standard_ExecutorEnd(queryDesc);
}

/* Save the time taken by processing of first tuple */
static void
pgmon_save_firsttuple(QueryDesc *queryDesc)
{
    mon_rec  *entry;
    int64	queryId =  queryDesc->plannedstmt->queryId;
    bool    found = false;

    Assert(queryDesc!= NULL);

    /* Safety check... */
    if (!mon_ht)
            return;

    /* Lookup the hash table entry and create one if not found. */
    LWLockAcquire(mon_lock, LW_EXCLUSIVE);

    entry = (mon_rec *) hash_search(mon_ht, &queryId, HASH_ENTER_NULL, &found);

     /* Save the first tuple time for the entry */
    if (found)
    {
        entry->first_tuple_time = queryDesc->planstate->instrument->firsttuple * 1000;
        LWLockRelease(mon_lock);
    }
    else
    {
        LWLockRelease(mon_lock);
        return;
    }

}

static void
pgmon_plan_store(QueryDesc *queryDesc)
{
        Assert(queryDesc != NULL);

        mon_rec  *entry, *temp_entry;
        int64	queryId =  queryDesc->plannedstmt->queryId;
        bool    found = false;
        int i;

        /* Safety check... */
        if (!mon_ht)
                return;

        /* Keep a temporary record to store the plan information of current query */
        temp_entry = (mon_rec *) palloc(sizeof(mon_rec));
        memset(&temp_entry->queryid, 0 , sizeof(mon_rec));
        temp_entry->queryid = queryId;
        temp_entry->ModifyTable = false;
        temp_entry->is_parallel = false;
        temp_entry->MergeJoin = 0;
        temp_entry->NestedLoopJoin = 0;
        temp_entry->HashJoin = 0;
        namestrcpy(&temp_entry->other_scan, "");

        plan_tree_traversal(queryDesc, queryDesc->plannedstmt->planTree, temp_entry);

        temp_entry->current_expected_rows = queryDesc->planstate->plan->plan_rows;

        /* Lookup the hash table entry and create one if not found. */

        // TODO use double check, entry/found can be checked without lock
        /*
        entry=search...
        if not entry
          lock exclusive
          entry = search ...
          if not entry
            create
          release lock
        */
        LWLockAcquire(mon_lock, LW_EXCLUSIVE);
        entry = (mon_rec *) hash_search(mon_ht, &queryId, HASH_ENTER_NULL, &found);

        // TODO fix found
        /* Create new entry, if not present */
        if (!found)
        {
            /* Update the plan information for the entry*/
            *entry = *temp_entry;
            for (i = 0; i < NUMBUCKETS; i++)
                entry->query_time_buckets[i] = bucket_bounds[i];

            for (i = 0; i < ROWNUMBUCKETS; i++)
                entry->actual_row_buckets[i] = row_bucket_bounds[i];

            for (i = 0; i < ROWNUMBUCKETS; i++)
                entry->est_row_buckets[i] = row_bucket_bounds[i];

            memset(entry->query_time_freq, 0, NUMBUCKETS * sizeof(int));
            memset(entry->actual_row_freq, 0, ROWNUMBUCKETS * sizeof(int));
            memset(entry->est_row_freq, 0, ROWNUMBUCKETS * sizeof(int));
        }

        entry = create_histogram(entry, EST_ROWS);
        LWLockRelease(mon_lock);

        pfree(temp_entry);
}

static void
pgmon_exec_store(QueryDesc *queryDesc)
{
        mon_rec  *entry;
        int64	queryId = queryDesc->plannedstmt->queryId;
        bool    found = false;

        Assert(queryDesc != NULL);

        /* Safety check... */
        if (!mon_ht)
                return;

        // TODO not clear why we lock here, dont create anything and dont check anything
        /* Lookup the hash table entry and create one if not found. */
        LWLockAcquire(mon_lock, LW_EXCLUSIVE);

        entry = (mon_rec *) hash_search(mon_ht, &queryId, HASH_ENTER_NULL, &found);

        entry->current_total_time = queryDesc->totaltime->total * 1000; //(time in seconds)
        entry->current_actual_rows = queryDesc->totaltime->ntuples;

        entry = create_histogram(entry, QUERY_TIME);
        entry = create_histogram(entry, ACTUAL_ROWS);

        LWLockRelease(mon_lock);
}

// TODO Future work unnest with stack? vs recursive call? compiler?
static void
plan_tree_traversal(QueryDesc *queryDesc, Plan *plan_node, mon_rec *entry)
{
    const char *relname;
    int i;
    IndexScan *idx;
    BitmapIndexScan *bidx;
    Scan *scan;
    RangeTblEntry *rte;
    Index relid;
    /* Iterate through the plan to find all the required nodes*/
            if (plan_node != NULL)
            {
                switch(plan_node->type)
                {
                    case T_SeqScan:
                        scan = (Scan *)plan_node;
                        relid = scan->scanrelid;
                        rte = rt_fetch(relid, queryDesc->plannedstmt->rtable);
                        relname = get_rel_name(rte->relid);
                        for (i = 0; i < MAX_TABLES && strcmp(entry->seq_scans[i].data, "") != 0; i++);
                        namestrcpy(&entry->seq_scans[i], relname);
                        break;
                    case T_IndexScan:
                    case T_IndexOnlyScan:
                        idx = (IndexScan *)plan_node;
                        scan = &(idx->scan);
                        relname = get_rel_name(idx->indexid);
                        for (i = 0; i < MAX_TABLES && strcmp(entry->index_scans[i].data, "") != 0; i++);
                        namestrcpy(&entry->index_scans[i], relname);
                        break;
                    case T_BitmapIndexScan:
                    case T_BitmapHeapScan:
                        bidx = (BitmapIndexScan *)plan_node;
                        scan = &(bidx->scan);
                        relname = get_rel_name(bidx->indexid);
                        for (i = 0; i < MAX_TABLES && strcmp(entry->bitmap_scans[i].data, "") != 0; i++);
                        namestrcpy(&entry->bitmap_scans[i], relname);
                        break;
                    case T_FunctionScan:
                        namestrcpy(&entry->other_scan, "T_FunctionScan");
                        break;
                    case T_SampleScan:
                        namestrcpy(&entry->other_scan, "T_SampleScan");
                        break;
                    case T_TidScan:
                        namestrcpy(&entry->other_scan, "T_TidScan");
                        break;
                    case T_SubqueryScan:
                        namestrcpy(&entry->other_scan, "T_SubqueryScan");
                        break;
                    case T_ValuesScan:
                        namestrcpy(&entry->other_scan, "T_ValuesScan");
                        break;
                    case T_TableFuncScan:
                        namestrcpy(&entry->other_scan, "T_TableFuncScan");
                        break;
                    case T_CteScan:
                        namestrcpy(&entry->other_scan, "T_CteScan");
                        break;
                    case T_NamedTuplestoreScan:
                        namestrcpy(&entry->other_scan, "T_NamedTuplestoreScan");
                        break;
                    case T_WorkTableScan:
                        namestrcpy(&entry->other_scan, "T_WorkTableScan");
                        break;
                    case T_ForeignScan:
                        namestrcpy(&entry->other_scan, "T_ForeignScan");
                        break;
                    case T_CustomScan:
                        namestrcpy(&entry->other_scan, "T_CustomScan");
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
                    case T_Gather:
                    case T_GatherMerge:
                        entry->is_parallel = true;
                        break;
                    case T_ModifyTable:
                        entry->ModifyTable = true;
                        break;
                    default:
                        break;
                }
                if (plan_node->lefttree)
                    plan_tree_traversal(queryDesc, plan_node->lefttree, entry);
                if (plan_node->righttree)
                    plan_tree_traversal(queryDesc, plan_node->righttree, entry);
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
                Datum	   *numdatums = (Datum *) palloc(NUMBUCKETS * sizeof(Datum));
                Datum	   *rownumdatums = (Datum *) palloc(ROWNUMBUCKETS * sizeof(Datum));
                Datum		values[MON_COLS];
                bool		nulls[MON_COLS] = {0};
                int			i = 0, n, idx = 0;
                ArrayType  *arry = NULL;

                memset(values, 0, sizeof(values));
                memset(nulls, 0, sizeof(nulls));

                values[i++] = Int64GetDatum(entry->queryid);
                values[i++] = Float8GetDatumFast(entry->current_total_time);
                values[i++] = Float8GetDatumFast(entry->first_tuple_time);
                values[i++] = Float8GetDatumFast(entry->current_expected_rows);
                values[i++] = Float8GetDatumFast(entry->current_actual_rows);
                values[i++] = BoolGetDatum(entry->is_parallel);
                values[i++] = BoolGetDatum(entry->ModifyTable);

                if (!entry->ModifyTable && strcmp(entry->seq_scans[0].data, "") == 0)
                    nulls[i++] = true;
                else
                {
                    Datum	   *numdatums = (Datum *) palloc(MAX_TABLES * sizeof(Datum));
                    ArrayType  *arry;
                    int n, idx = 0;
                    for (n = 0; n < MAX_TABLES && strcmp(entry->seq_scans[n].data, "") != 0; n++)
                            numdatums[idx++] = NameGetDatum(&entry->seq_scans[n]);
                    arry = construct_array(numdatums, idx, NAMEOID, NAMEDATALEN, false, 'c');
                    values[i++] = PointerGetDatum(arry);
                }
                if (!entry->ModifyTable && strcmp(entry->index_scans[0].data, "") == 0)
                    nulls[i++] = true;
                else
                {
                    Datum	   *numdatums = (Datum *) palloc(MAX_TABLES * sizeof(Datum));
                    ArrayType  *arry;
                    int n, idx = 0;
                    for (n = 0; n < MAX_TABLES && strcmp(entry->index_scans[n].data, "") != 0; n++)
                            numdatums[idx++] = NameGetDatum(&entry->index_scans[n]);
                    arry = construct_array(numdatums, idx, NAMEOID, NAMEDATALEN, false, 'c');
                    values[i++] = PointerGetDatum(arry);
                }
                if (!entry->ModifyTable && strcmp(entry->bitmap_scans[0].data, "") == 0)
                    nulls[i++] = true;
                else
                {
                    Datum	   *numdatums = (Datum *) palloc(MAX_TABLES * sizeof(Datum));
                    ArrayType  *arry;
                    int n, idx = 0;
                    for (n = 0; n < MAX_TABLES && strcmp(entry->bitmap_scans[n].data, "") != 0; n++)
                            numdatums[idx++] = NameGetDatum(&entry->bitmap_scans[n]);
                    arry = construct_array(numdatums, idx, NAMEOID, NAMEDATALEN, false, 'c');
                    values[i++] = PointerGetDatum(arry);
                }
                values[i++] = NameGetDatum(&entry->other_scan);
                values[i++] = Int32GetDatum(entry->NestedLoopJoin);
                values[i++] = Int32GetDatum(entry->HashJoin);
                values[i++] = Int32GetDatum(entry->MergeJoin);

                for (n = 0; n < NUMBUCKETS; n++)
                {
                    if (entry->query_time_freq[n] > 0)
                        numdatums[idx++] = Int64GetDatum(entry->query_time_buckets[n]);
                }
                arry = construct_array(numdatums, idx, INT4OID, sizeof(int), true, 'i');
                values[i++] = PointerGetDatum(arry);

                for (n = 0, idx = 0; n < NUMBUCKETS; n++)
                {
                    if (entry->query_time_freq[n] > 0)
                        numdatums[idx++] = Int64GetDatum(entry->query_time_freq[n]);
                }
                arry = construct_array(numdatums, idx, INT4OID, sizeof(int), true, 'i');
                values[i++] = PointerGetDatum(arry);
                numdatums = NULL;
                arry = NULL;

                for (n = 0, idx = 0; n < ROWNUMBUCKETS; n++)
                {
                    if (entry->actual_row_freq[n] > 0)
                        rownumdatums[idx++] = Int64GetDatum(entry->actual_row_buckets[n]);
                }
                arry = construct_array(rownumdatums, idx, INT4OID, sizeof(int), true, 'i');
                values[i++] = PointerGetDatum(arry);

                for (n = 0, idx = 0; n < ROWNUMBUCKETS; n++)
                {
                    if (entry->actual_row_freq[n] > 0)
                        rownumdatums[idx++] = Int64GetDatum(entry->actual_row_freq[n]);
                }
                arry = construct_array(rownumdatums, idx, INT4OID, sizeof(int), true, 'i');
                values[i++] = PointerGetDatum(arry);

                for (n = 0, idx = 0; n < ROWNUMBUCKETS; n++)
                {
                    if (entry->est_row_freq[n] > 0)
                        rownumdatums[idx++] = Int64GetDatum(entry->est_row_buckets[n]);
                }
                arry = construct_array(rownumdatums, idx, INT4OID, sizeof(int), true, 'i');
                values[i++] = PointerGetDatum(arry);

                for (n = 0, idx = 0; n < ROWNUMBUCKETS; n++)
                {
                    if (entry->est_row_freq[n] > 0)
                        rownumdatums[idx++] = Int64GetDatum(entry->est_row_freq[n]);
                }
                arry = construct_array(rownumdatums, idx, INT4OID, sizeof(int), true, 'i');
                values[i++] = PointerGetDatum(arry);

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

/*
 * Reset query information.
 */
Datum
pg_mon_reset(PG_FUNCTION_ARGS)
{
    HASH_SEQ_STATUS status;
    mon_rec *entry;

    LWLockAcquire(mon_lock, LW_EXCLUSIVE);
    hash_seq_init(&status, mon_ht);
    while ((entry = hash_seq_search(&status)) != NULL)
    {
        hash_search(mon_ht, &entry->queryid, HASH_REMOVE, NULL);
    }
    LWLockRelease(mon_lock);

    PG_RETURN_VOID();
}

/* Create the histogram for the current query */
static mon_rec * create_histogram(mon_rec *entry, AddHist value)
{
    int i;
    if (value == QUERY_TIME)
    {
        float8 val = entry->current_total_time;
        /* if the last value of bucket is more than the current time, then increase the bucket boundary */
        if (val > entry->query_time_buckets[NUMBUCKETS-1])
        {
            entry->query_time_buckets[NUMBUCKETS-1] = val;
            entry->query_time_freq[NUMBUCKETS-1]++;
            return entry;
        }

        /* Find the matching bucket */
        for (i = 0; i < NUMBUCKETS; i++)
        {
            if (val <= entry->query_time_buckets[i])
            {
                entry->query_time_freq[i]++;
                break;
            }
        }
    }
    else if (value == ACTUAL_ROWS)
    {
        float8 val = entry->current_actual_rows;

        /* if the last value of bucket is more than the current time, then increase the bucket boundary */
        if (val > entry->actual_row_buckets[ROWNUMBUCKETS-1])
        {
            entry->actual_row_buckets[ROWNUMBUCKETS-1] = val;
            entry->actual_row_freq[ROWNUMBUCKETS-1]++;
            return entry;
        }

        /* Find the matching bucket */
        for (i = 0; i < ROWNUMBUCKETS; i++)
        {
            if (val <= entry->actual_row_buckets[i])
            {
                entry->actual_row_freq[i]++;
                break;
            }
        }
    }
    else if (value == EST_ROWS)
    {
        float8 val = entry->current_expected_rows;

        /* if the last value of bucket is more than the current time, then increase the bucket boundary */
        if (val > entry->est_row_buckets[ROWNUMBUCKETS-1])
        {
            entry->est_row_buckets[ROWNUMBUCKETS-1] = val;
            entry->est_row_freq[ROWNUMBUCKETS-1]++;
            return entry;
        }

        /* Find the matching bucket */
        for (i = 0; i < ROWNUMBUCKETS; i++)
        {
            if (val <= entry->est_row_buckets[i])
            {
                entry->est_row_freq[i]++;
                break;
            }
        }
    }

    return entry;
}
