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
#include "storage/spin.h"

#include "tcop/utility.h"


Datum		pg_mon(PG_FUNCTION_ARGS);
Datum		pg_mon_reset(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(pg_mon);
PG_FUNCTION_INFO_V1(pg_mon_reset);

PG_MODULE_MAGIC;

/* GUC variables */
static bool CONFIG_PLAN_INFO_IMMEDIATE = false;
static bool CONFIG_PLAN_INFO_DISABLE = false;
static bool CONFIG_LOG_NEW_QUERY = true;
static int MON_HT_SIZE = 5000;

#define MON_COLS  20
#define NUMBUCKETS 30
#define ROWNUMBUCKETS 20
#define MAX_TABLES 30

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
        Oid seq_scans[MAX_TABLES];
        Oid index_scans[MAX_TABLES];
        Oid bitmap_scans[MAX_TABLES];
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
        slock_t		mutex;
}mon_rec;

/* Current nesting depth of ExecutorRun calls */
static int	nesting_level = 0;

extern void _PG_init(void);
extern void _PG_fini(void);

/* LWlock to mange the reading and writing the hash table. */
LWLock	   *mon_lock;

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
static ProcessUtility_hook_type prev_ProcessUtility = NULL;
static void pgmon_ExecutorStart(QueryDesc *queryDesc, int eflags);

#if PG_VERSION_NUM < 160000
static void ER_hook(QueryDesc *queryDesc, ScanDirection direction,\
                    uint64 count, bool execute_once);
#define _ER_hook \
    static void ER_hook(QueryDesc *queryDesc, ScanDirection direction,\
                                    uint64 count, bool execute_once)
#else
static void ER_hook(QueryDesc *queryDesc, ScanDirection direction,
                    uint64 count);
#define _ER_hook \
     static void ER_hook(QueryDesc *queryDesc, ScanDirection direction,\
                        uint64 count)
#endif
static void pgmon_ExecutorFinish(QueryDesc *queryDesc);
static void pgmon_ExecutorEnd(QueryDesc *queryDesc);
static void pgmon_plan_store(QueryDesc *queryDesc);
static void pgmon_exec_store(QueryDesc *queryDesc);
#if PG_VERSION_NUM < 130000
static void PU_hook(PlannedStmt *pstmt, const char *queryString,
						   ProcessUtilityContext context, ParamListInfo params,
						   QueryEnvironment *queryEnv,
						   DestReceiver *dest, char *completionTag);
#define _PU_HOOK \
    static void PU_hook(PlannedStmt *pstmt, const char *queryString,\
						   ProcessUtilityContext context, ParamListInfo params, \
						   QueryEnvironment *queryEnv, \
						   DestReceiver *dest, char *completionTag)
#define _prev_hook \
        prev_ProcessUtility(pstmt, queryString, context, params, queryEnv, dest, completionTag)
#define _standard_ProcessUtility \
        standard_ProcessUtility(pstmt, queryString, context, params, queryEnv, dest, completionTag)
#elif PG_VERSION_NUM >= 130000 && PG_VERSION_NUM < 140000
static void PU_hook(PlannedStmt *pstmt, const char *queryString,
									ProcessUtilityContext context, ParamListInfo params,
									QueryEnvironment *queryEnv,
									DestReceiver *dest, QueryCompletion *qc);

#define _PU_HOOK \
    static void PU_hook(PlannedStmt *pstmt, const char *queryString,\
						   ProcessUtilityContext context, ParamListInfo params, \
						   QueryEnvironment *queryEnv, \
						   DestReceiver *dest, QueryCompletion *qc)
#define _prev_hook \
        prev_ProcessUtility(pstmt, queryString, context, params, queryEnv, dest, qc)
#define _standard_ProcessUtility \
        standard_ProcessUtility(pstmt, queryString, context, params, queryEnv, dest, qc)
#else
static void PU_hook(PlannedStmt *pstmt, const char *queryString,
									bool readOnlyTree,
                                    ProcessUtilityContext context, ParamListInfo params,
									QueryEnvironment *queryEnv,
                                    DestReceiver *dest, QueryCompletion *qc);
#define _PU_HOOK \
    static void PU_hook(PlannedStmt *pstmt, const char *queryString, bool readOnlyTree, \
						   ProcessUtilityContext context, ParamListInfo params, \
						   QueryEnvironment *queryEnv, \
						   DestReceiver *dest, QueryCompletion *qc)
#define _prev_hook \
        prev_ProcessUtility(pstmt, queryString, readOnlyTree, context, params, queryEnv, dest, qc)
#define _standard_ProcessUtility \
        standard_ProcessUtility(pstmt, queryString, readOnlyTree, context, params, queryEnv, dest, qc)
#endif
/* Saved hook values in case of unload */
static shmem_startup_hook_type prev_shmem_startup_hook = NULL;
static void shmem_shutdown(int code, Datum arg);

static void plan_tree_traversal(QueryDesc *query, Plan *plan, mon_rec *entry);
static void update_histogram(volatile mon_rec *entry, AddHist);
static void pg_mon_reset_internal(void);
static mon_rec * create_or_get_entry(mon_rec temp_entry, int64 queryId, QueryDesc *queryDesc);
static void scan_info(Plan *subplan, mon_rec *entry, QueryDesc *queryDesc);
static const char * scan_string(NodeTag type);

/* Hash table in the shared memory */
static HTAB *mon_ht;

/* Bucket boundaries for the histogram in ms, from 5 ms to 1 minute */
static int64 bucket_bounds[NUMBUCKETS] = {
                                1, 5, 10, 15, 20, 25, 30, 40, 50, 60, 70, 80,
                                90, 100, 200, 300, 400, 500, 600, 700, 1000,
                                2000, 3000, 5000, 7000, 10000, 20000, 30000,
                                50000, 60000
                                };

static int64 row_bucket_bounds[ROWNUMBUCKETS] = {
                                        1, 5, 10, 50, 100, 200, 300, 400, 500,
                                        1000, 2000, 3000, 4000, 5000, 10000,
                                        30000, 50000, 70000, 100000, 1000000
                                        };

/*
 * Keep a temporary record to store the plan information of the
 * current query
 */
static mon_rec temp_entry;
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
        mon_ht = ShmemInitHash("mon_hash", MON_HT_SIZE, MON_HT_SIZE,
                                &info, HASH_ELEM);
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
        if (!process_shared_preload_libraries_in_progress)
		    return;

        DefineCustomIntVariable("pg_mon.max_statements",
                                "Sets the maximum number of statements tracked by pg_mon.",
                                NULL,
                                &MON_HT_SIZE,
                                5000,
                                100,
                                INT_MAX,
                                PGC_POSTMASTER,
                                0,
                                NULL,
                                NULL,
                                NULL);
        DefineCustomBoolVariable("pg_mon.plan_info_immediate",
                                                            "Populate the plan time information immediately after planning phase.",
                                                            NULL,
                                                            &CONFIG_PLAN_INFO_IMMEDIATE,
                                                            CONFIG_PLAN_INFO_IMMEDIATE,
                                                            PGC_SUSET,
                                                            0,
                                                            NULL,
                                                            NULL,
                                                            NULL);
        DefineCustomBoolVariable("pg_mon.plan_info_disable",
                                                            "Skip plan time information.",
                                                            NULL,
                                                            &CONFIG_PLAN_INFO_DISABLE,
                                                            CONFIG_PLAN_INFO_DISABLE,
                                                            PGC_SUSET,
                                                            0,
                                                            NULL,
                                                            NULL,
                                                            NULL);
        DefineCustomBoolVariable("pg_mon.log_new_query",
                                                            "Log the new query.",
                                                            NULL,
                                                            &CONFIG_LOG_NEW_QUERY,
                                                            CONFIG_LOG_NEW_QUERY,
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
        ExecutorStart_hook = pgmon_ExecutorStart;
        prev_ExecutorRun = ExecutorRun_hook;
        ExecutorRun_hook = ER_hook;
        prev_ExecutorFinish = ExecutorFinish_hook;
        ExecutorFinish_hook = pgmon_ExecutorFinish;
        prev_ExecutorEnd = ExecutorEnd_hook;
        ExecutorEnd_hook = pgmon_ExecutorEnd;
        prev_ProcessUtility = ProcessUtility_hook;
	    ProcessUtility_hook = PU_hook;
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
        ProcessUtility_hook = prev_ProcessUtility;
}


/*
 * ExecutorStart hook: start up logging if needed
 */
static void
pgmon_ExecutorStart(QueryDesc *queryDesc, int eflags)
{
    if (prev_ExecutorStart)
                prev_ExecutorStart(queryDesc, eflags);
        else
                standard_ExecutorStart(queryDesc, eflags);

    if (queryDesc->plannedstmt->queryId != UINT64CONST(0) && nesting_level == 0)
    {
       /*
        * Set up to track total elapsed time in ExecutorRun.Make sure the space
        * is allocated in the per-query context so it will go away at ExecutorEnd.
        */
        if(queryDesc->totaltime == NULL)
        {
            /*
             * We need to be in right memory context before allocating. Similar
             * to how it is done in other places, e.g. in pg_stat_statements,
             * auto_explain, etc.
             * https://github.com/postgres/postgres/blob/5f28b21eb3c5c2fb72c24608bc686acd7c9b113c/contrib/pg_stat_statements/pg_stat_statements.c#L1021
             */
            MemoryContext oldcxt;

            oldcxt = MemoryContextSwitchTo(queryDesc->estate->es_query_cxt);
            #if PG_VERSION_NUM < 140000
                queryDesc->totaltime = InstrAlloc(1, INSTRUMENT_ALL);
            #else
                queryDesc->totaltime = InstrAlloc(1, INSTRUMENT_ALL, false);
            #endif

            MemoryContextSwitchTo(oldcxt);
        }
        if (queryDesc->planstate->instrument == NULL)
        {
            /*
             * We need to be in right memory context before allocating. Similar
             * to how it is done in other places, e.g. ExecInitNode
             * https://github.com/postgres/postgres/blob/5f28b21eb3c5c2fb72c24608bc686acd7c9b113c/src/backend/executor/execProcnode.c#L397
             */
            MemoryContext oldcxt;

            oldcxt = MemoryContextSwitchTo(queryDesc->estate->es_query_cxt);
            #if PG_VERSION_NUM < 140000
                queryDesc->planstate->instrument = InstrAlloc(1, INSTRUMENT_ALL);
            #else
                queryDesc->planstate->instrument = InstrAlloc(1, INSTRUMENT_ALL, false);
            #endif

            MemoryContextSwitchTo(oldcxt);
        }

        queryDesc->instrument_options |= INSTRUMENT_ROWS;

        memset(&temp_entry, 0, sizeof(mon_rec));
        temp_entry.queryid = queryDesc->plannedstmt->queryId;

        /* Add the bucket boundaries for the entry */
        memcpy(temp_entry.query_time_buckets, bucket_bounds, sizeof(bucket_bounds));
        memcpy(temp_entry.actual_row_buckets, row_bucket_bounds, sizeof(row_bucket_bounds));
        memcpy(temp_entry.est_row_buckets, row_bucket_bounds, sizeof(row_bucket_bounds));

        if (!CONFIG_PLAN_INFO_DISABLE)
            pgmon_plan_store(queryDesc);
    }
}

/*
 * ExecutorRun hook: all we need do is track nesting depth
 */
_ER_hook
{
        nesting_level++;
        PG_TRY();
        {
                if (prev_ExecutorRun)
                        prev_ExecutorRun(queryDesc, direction, count, execute_once);
                else
                        standard_ExecutorRun(queryDesc, direction, count, execute_once);
#if PG_VERSION_NUM < 130000
                nesting_level--;
#endif
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
pgmon_ExecutorFinish(QueryDesc *queryDesc)
{
    if (queryDesc->planstate->instrument && nesting_level == 0)
    {
       temp_entry.first_tuple_time = queryDesc->planstate->instrument->firsttuple * 1000;
    }
    nesting_level++;

    PG_TRY();
    {
            if (prev_ExecutorFinish)
                    prev_ExecutorFinish(queryDesc);
            else
                    standard_ExecutorFinish(queryDesc);
#if PG_VERSION_NUM < 130000
            nesting_level--;
#endif
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
pgmon_ExecutorEnd(QueryDesc *queryDesc)
{
    uint64		queryId = queryDesc->plannedstmt->queryId;

    if (queryId != UINT64CONST(0) && queryDesc->totaltime && nesting_level == 0)
    {
            /*
             * Make sure stats accumulation is done.
             * (Note: it's okay if several levels of hook all do this.)
             */
            InstrEndLoop(queryDesc->totaltime);
            InstrEndLoop(queryDesc->planstate->instrument);

            /* Save query information */
            pgmon_exec_store(queryDesc);
    }

    if (prev_ExecutorEnd)
            prev_ExecutorEnd(queryDesc);
    else
            standard_ExecutorEnd(queryDesc);
}

/*
 * ProcessUtility hook
 */
_PU_HOOK
{
    if (CONFIG_LOG_NEW_QUERY)
    {
        switch (nodeTag(pstmt->utilityStmt))
        {
            case T_AlterRoleStmt:
                break;
            default:
                ereport(LOG, (errmsg("new query registered from pg_mon"), errhint("%s", queryString)));
                break;
        }
    }
    if (prev_ProcessUtility)
    {
        _prev_hook;
    }
    else
    {
        _standard_ProcessUtility;
    }
}

static void
pgmon_plan_store(QueryDesc *queryDesc)
{
        mon_rec  *entry = NULL;
        volatile mon_rec *e;

        /* Safety check... */
        if (!mon_ht)
                return;

        Assert(queryDesc != NULL);

        if (!CONFIG_PLAN_INFO_DISABLE)
        {
            plan_tree_traversal(queryDesc, queryDesc->plannedstmt->planTree, &temp_entry);
            temp_entry.current_expected_rows = queryDesc->planstate->plan->plan_rows;
        }

        /*
         * If plan information is to be provided immediately, then take the
         * lock here to update the information in hash table.
         */
        if (CONFIG_PLAN_INFO_IMMEDIATE && !CONFIG_PLAN_INFO_DISABLE)
        {
            LWLockAcquire(mon_lock, LW_SHARED);
            entry = create_or_get_entry(temp_entry, temp_entry.queryid, queryDesc);

            e = (volatile mon_rec *) entry;
            SpinLockAcquire(&e->mutex);
            update_histogram(e, EST_ROWS);
            SpinLockRelease(&e->mutex);
            LWLockRelease(mon_lock);
        }
}

static void
pgmon_exec_store(QueryDesc *queryDesc)
{
        mon_rec  *entry = NULL;
        volatile mon_rec *e;
        int64	queryId = queryDesc->plannedstmt->queryId;
        bool is_present = false;
        int i, j;

        Assert(queryDesc!= NULL);

        /* Safety check... */
        if (!mon_ht)
                return;

        LWLockAcquire(mon_lock, LW_SHARED);
        entry = create_or_get_entry(temp_entry, queryId, queryDesc);

        e = (volatile mon_rec *) entry;
        SpinLockAcquire(&e->mutex);

        e->current_total_time = queryDesc->totaltime->total * 1000; //(in msec)
        e->first_tuple_time = temp_entry.first_tuple_time;
        update_histogram(e, QUERY_TIME);
        e->current_actual_rows = queryDesc->totaltime->ntuples;
        update_histogram(e, ACTUAL_ROWS);

        /*
         * If planning info is not already updated then only update
         * estimated rows histogram.
         */
        if (!CONFIG_PLAN_INFO_IMMEDIATE)
            update_histogram(e, EST_ROWS);

        /*
         * If this query is already present in the hash table, then update the
         * plan information of the query also. If the seq_scans, indexes, etc.
         * used by the query are different from the previous view then add
         * them to the entry here.
         * However, if the number of seq_scans or index_scans has reached
         * more than MAX_TABLES, then silently exit without adding.
         *
         * O(tables^2) in current loops may need sort aftert testing.
         */
        if (!CONFIG_PLAN_INFO_DISABLE)
        {
            for (j = 0; j < MAX_TABLES && temp_entry.seq_scans[j] != 0; j++)
            {
                for (i = 0; i < MAX_TABLES && entry->seq_scans[i] != 0; i++)
                {
                    if (temp_entry.seq_scans[j] == entry->seq_scans[i])
                    {
                        is_present = true;
                        break;
                    }
                }
                if (!is_present && i < MAX_TABLES)
                {
                    entry->seq_scans[i] = temp_entry.seq_scans[j];
                }
                is_present = false;
            }

            for (j = 0; j < MAX_TABLES && temp_entry.index_scans[j] != 0; j++)
            {
                for (i = 0; i < MAX_TABLES && entry->index_scans[i] != 0; i++)
                {
                    if (temp_entry.index_scans[j] == entry->index_scans[i])
                    {
                        is_present = true;
                        break;
                    }
                }
                if (!is_present && i < MAX_TABLES)
                {
                    entry->index_scans[i] = temp_entry.index_scans[j];
                }
                is_present = false;
            }

            for (j = 0; j < MAX_TABLES && temp_entry.bitmap_scans[j] != 0; j++)
            {
                for (i = 0; i < MAX_TABLES && entry->bitmap_scans[i] != 0; i++)
                {
                    if (temp_entry.bitmap_scans[j] == entry->bitmap_scans[i])
                    {
                        is_present = true;
                        break;
                    }
                }
                if (!is_present && i < MAX_TABLES)
                {
                    entry->bitmap_scans[i] = temp_entry.bitmap_scans[j];
                }
                is_present = false;
            }
            if (entry->NestedLoopJoin < temp_entry.NestedLoopJoin)
                entry->NestedLoopJoin = temp_entry.NestedLoopJoin;
            if (entry->HashJoin < temp_entry.HashJoin)
                entry->HashJoin = temp_entry.HashJoin;
            if (entry->MergeJoin < temp_entry.MergeJoin)
                entry->MergeJoin = temp_entry.MergeJoin;
        }

        SpinLockRelease(&e->mutex);
        LWLockRelease(mon_lock);
}

/*
 * Find the required hash table entry if not found then copy the
 * contents of temp_entry, otherwise return the entry. The caller should have
 * shared lock on hash_table which could be upgraded to exclusive mode, if new
 * entry has to be added.
 */
static mon_rec * create_or_get_entry(mon_rec temp_entry, int64 queryId, QueryDesc *queryDesc)
{
    mon_rec *entry = NULL;
    bool found = false;

    entry = (mon_rec *) hash_search(mon_ht, &queryId, HASH_FIND, &found);

    if (!entry)
    {
        LWLockRelease(mon_lock);
        LWLockAcquire(mon_lock, LW_EXCLUSIVE);
       /*
        * Check if the number of entries are exceeding the limit. Currently,
        * we are handling this case by resetting the pg_mon view, but could be
        * dealt more elegantly later, e.g. as in pg_stat_statetments remove
        * the least used entries, etc.
        */
        if (hash_get_num_entries(mon_ht) >= MON_HT_SIZE)
        {
            pg_mon_reset_internal();
        }

        entry = (mon_rec *) hash_search(mon_ht, &queryId, HASH_ENTER, &found);

        if (!found)
        {
            *entry = temp_entry;
            SpinLockInit(&entry->mutex);

            /* Since this is a new query,  log the query text */
            if (CONFIG_LOG_NEW_QUERY)
            {
                ereport(LOG, (errmsg("new query registered from pg_mon"), errhint("%s", queryDesc->sourceText)));
            }
        }
    }

    return entry;
}

static void
plan_tree_traversal(QueryDesc *queryDesc, Plan *plan_node, mon_rec *entry)
{
#if PG_VERSION_NUM < 140000
    ModifyTable *mplan;
    ListCell *p;
#endif

    /* Iterate through the plan to find all the required nodes*/
            if (plan_node != NULL)
            {
                switch(plan_node->type)
                {
                    case T_SeqScan:
                    case T_IndexScan:
                    case T_IndexOnlyScan:
                    case T_BitmapIndexScan:
                        scan_info(plan_node, entry, queryDesc);
                        break;
                    case T_FunctionScan:
                    case T_SampleScan:
                    case T_TidScan:
                    case T_SubqueryScan:
                    case T_ValuesScan:
                    case T_TableFuncScan:
                    case T_CteScan:
                    case T_NamedTuplestoreScan:
                    case T_WorkTableScan:
                    case T_ForeignScan:
                    case T_CustomScan:
                        namestrcpy(&entry->other_scan, scan_string(plan_node->type));
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
#if PG_VERSION_NUM < 140000
                        mplan =(ModifyTable *)plan_node;
                        foreach (p, mplan->plans){
                            Plan *subplan = (Plan *) lfirst (p);
                            if (subplan != NULL){
                                scan_info(subplan, entry, queryDesc);
                            }
                        }
#endif
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

static void
scan_info(Plan *subplan, mon_rec *entry, QueryDesc *queryDesc)
{
    bool found = false;
    IndexScan *idx;
    BitmapIndexScan *bidx;
    Scan *scan;
    RangeTblEntry *rte;
    Index relid;
    int i = 0;

    switch(subplan->type)
    {
        case T_SeqScan:
            scan = (Scan *)subplan;
            relid = scan->scanrelid;
            rte = rt_fetch(relid, queryDesc->plannedstmt->rtable);
            for (i = 0; i < MAX_TABLES && entry->seq_scans[i] > 0;
                i++)
            {
                if (entry->seq_scans[i] == rte->relid)
                {
                    found = true;
                    break;
                }
            }
            if (!found && i < MAX_TABLES)
                entry->seq_scans[i] = rte->relid;
            break;
        case T_IndexScan:
        case T_IndexOnlyScan:
            idx = (IndexScan *)subplan;
            for (i = 0; i < MAX_TABLES && entry->index_scans[i] > 0;
                i++)
            {
                if (entry->index_scans[i] == idx->indexid)
                {
                    found = true;
                    break;
                }
            }
            if (!found && i < MAX_TABLES)
                entry->index_scans[i] = idx->indexid;
            break;
        case T_BitmapIndexScan:
            bidx = (BitmapIndexScan *)subplan;
            for (i = 0; i < MAX_TABLES && entry->bitmap_scans[i] > 0;
                i++)
            {
                if (entry->bitmap_scans[i] == bidx->indexid)
                {
                    found = true;
                    break;
                }
            }
            if (!found && i < MAX_TABLES)
                entry->bitmap_scans[i] = bidx->indexid;
            break;
        default:
            break;
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
                int			i = 0, n, idx = 0, last_fill_bucket = 0;
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

                if (!entry->ModifyTable && entry->seq_scans[0] == 0)
                    nulls[i++] = true;
                else
                {
                    Datum	   *datums = (Datum *) palloc(MAX_TABLES * sizeof(Datum));
                    ArrayType  *arry;
                    int n = 0, idx = 0;
                    for (n = 0; n < MAX_TABLES && entry->seq_scans[n] != 0; n++)
                            datums[idx++] = ObjectIdGetDatum(entry->seq_scans[n]);
                    arry = construct_array(datums, idx, OIDOID, sizeof(Oid), false, 'i');
                    values[i++] = PointerGetDatum(arry);
                }
                if (!entry->ModifyTable && entry->index_scans[0] == 0)
                    nulls[i++] = true;
                else
                {
                    Datum	   *datums = (Datum *) palloc(MAX_TABLES * sizeof(Datum));
                    ArrayType  *arry;
                    int n = 0, idx = 0;
                    for (n = 0; n < MAX_TABLES && entry->index_scans[n] != 0; n++)
                            datums[idx++] = ObjectIdGetDatum(entry->index_scans[n]);
                    arry = construct_array(datums, idx, OIDOID, sizeof(Oid), false, 'i');
                    values[i++] = PointerGetDatum(arry);
                }
                if (!entry->ModifyTable && entry->bitmap_scans[0] == 0)
                    nulls[i++] = true;
                else
                {
                    Datum	   *datums = (Datum *) palloc(MAX_TABLES * sizeof(Datum));
                    ArrayType  *arry;
                    int n = 0, idx = 0;
                    for (n = 0; n < MAX_TABLES && entry->bitmap_scans[n] != 0; n++)
                            datums[idx++] = ObjectIdGetDatum(entry->bitmap_scans[n]);
                    arry = construct_array(datums, idx, OIDOID, sizeof(Oid), false, 'i');
                    values[i++] = PointerGetDatum(arry);
                }
                values[i++] = NameGetDatum(&entry->other_scan);
                values[i++] = Int32GetDatum(entry->NestedLoopJoin);
                values[i++] = Int32GetDatum(entry->HashJoin);
                values[i++] = Int32GetDatum(entry->MergeJoin);

                for (n = NUMBUCKETS-1; n >= 0; n--)
                {
                    if (entry->query_time_freq[n] > 0)
                    {
                        last_fill_bucket = n;
                        break;
                    }
                }
                for (n = 0; n <= last_fill_bucket; n++)
                {
                    numdatums[idx++] = Int64GetDatum(entry->query_time_buckets[n]);
                }
                arry = construct_array(numdatums, idx, INT4OID, sizeof(int), true, 'i');
                values[i++] = PointerGetDatum(arry);

                for (n = 0, idx = 0; n <= last_fill_bucket; n++)
                {
                     numdatums[idx++] = Int64GetDatum(entry->query_time_freq[n]);
                }
                arry = construct_array(numdatums, idx, INT4OID, sizeof(int), true, 'i');
                values[i++] = PointerGetDatum(arry);
                numdatums = NULL;
                arry = NULL;
                last_fill_bucket = 0;

                for (n = ROWNUMBUCKETS-1; n >= 0; n--)
                {
                    if (entry->actual_row_freq[n] > 0)
                    {
                        last_fill_bucket = n;
                        break;
                    }
                }

                for (n = 0, idx = 0; n <= last_fill_bucket; n++)
                {
                    rownumdatums[idx++] = Int64GetDatum(entry->actual_row_buckets[n]);
                }
                arry = construct_array(rownumdatums, idx, INT4OID, sizeof(int), true, 'i');
                values[i++] = PointerGetDatum(arry);

                for (n = 0, idx = 0; n <= last_fill_bucket; n++)
                {
                    rownumdatums[idx++] = Int64GetDatum(entry->actual_row_freq[n]);
                }
                arry = construct_array(rownumdatums, idx, INT4OID, sizeof(int), true, 'i');
                values[i++] = PointerGetDatum(arry);
                last_fill_bucket = 0;

                for (n = ROWNUMBUCKETS-1; n >= 0; n--)
                {
                    if (entry->est_row_freq[n] > 0)
                    {
                        last_fill_bucket = n;
                        break;
                    }
                }
                for (n = 0, idx = 0; n <= last_fill_bucket; n++)
                {
                    rownumdatums[idx++] = Int64GetDatum(entry->est_row_buckets[n]);
                }
                arry = construct_array(rownumdatums, idx, INT4OID, sizeof(int), true, 'i');
                values[i++] = PointerGetDatum(arry);

                for (n = 0, idx = 0; n <= last_fill_bucket; n++)
                {
                    rownumdatums[idx++] = Int64GetDatum(entry->est_row_freq[n]);
                }
                arry = construct_array(rownumdatums, idx, INT4OID, sizeof(int), true, 'i');
                values[i++] = PointerGetDatum(arry);

                tuplestore_putvalues(tupstore, tupdesc, values, nulls);
        }

        LWLockRelease(mon_lock);

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
    LWLockAcquire(mon_lock, LW_EXCLUSIVE);
    pg_mon_reset_internal();
    LWLockRelease(mon_lock);

    PG_RETURN_VOID();
}

static void
pg_mon_reset_internal()
{
    HASH_SEQ_STATUS status;
    mon_rec *entry;

    hash_seq_init(&status, mon_ht);
    while ((entry = hash_seq_search(&status)) != NULL)
    {
        hash_search(mon_ht, &entry->queryid, HASH_REMOVE, NULL);
    }
}

/* Update the histogram for the current query */
static void
update_histogram(volatile mon_rec *entry, AddHist value)
{
    int i;
    if (value == QUERY_TIME)
    {
        float8 val = entry->current_total_time;
        /*
         * if the last value of bucket is more than the current time,
         * then increase the bucket boundary.
         */
        if (val > entry->query_time_buckets[NUMBUCKETS-1])
        {
            entry->query_time_buckets[NUMBUCKETS-1] = val;
            entry->query_time_freq[NUMBUCKETS-1]++;
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

        /*
         * if the last value of bucket is more than the current time,
         * then increase the bucket boundary.
         */
        if (val > entry->actual_row_buckets[ROWNUMBUCKETS-1])
        {
            entry->actual_row_buckets[ROWNUMBUCKETS-1] = val;
            entry->actual_row_freq[ROWNUMBUCKETS-1]++;
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

        /*
         * if the last value of bucket is more than the current time,
         * then increase the bucket boundary
         */
        if (val > entry->est_row_buckets[ROWNUMBUCKETS-1])
        {
            entry->est_row_buckets[ROWNUMBUCKETS-1] = val;
            entry->est_row_freq[ROWNUMBUCKETS-1]++;
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
}

static const char *
scan_string(NodeTag type){

    switch(type){
        case T_FunctionScan:
            return "T_FunctionScan";
        case T_SampleScan:
            return "T_SampleScan";
        case T_TidScan:
            return "T_TidScan";
        case T_SubqueryScan:
            return "T_SubqueryScan";
        case T_ValuesScan:
            return "T_ValuesScan";
        case T_TableFuncScan:
            return "T_TableFuncScan";
        case T_CteScan:
            return "T_CteScan";
        case T_NamedTuplestoreScan:
            return "T_NamedTuplestoreScan";
        case T_WorkTableScan:
            return "T_WorkTableScan";
        case T_ForeignScan:
            return "T_ForeignScan";
        case T_CustomScan:
            return "T_CustomScan";
        default:
            return "";
    }
}