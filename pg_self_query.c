/*
 * pg_self_query.c
 *		Extract information about query state from current backend
 *
 * tnx to Postgres Professional for pg_query_state which was a greate example for me!
 *
 *	  contrib/pg_self_query/pg_self_query.c
 * IDENTIFICATION
 */

#include <postgres.h>
//#include "utils/builtins.h"
//#include "utils/memutils.h"
//#include "access/htup_details.h"
#include "catalog/pg_type.h"
#include "funcapi.h"
#include "executor/execParallel.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "nodes/print.h"
#include "pgstat.h"
#include "postmaster/bgworker.h"
#include "storage/ipc.h"
#include "storage/s_lock.h"
#include "storage/spin.h"
#include "storage/procarray.h"
#include "storage/procsignal.h"
//#include "storage/shm_toc.h"
#include "utils/guc.h"
//#include "utils/timestamp.h"

#ifdef PG_MODULE_MAGIC
PG_MODULE_MAGIC;
#endif

#define TIMINIG_OFF_WARNING 1
#define BUFFERS_OFF_WARNING 2

/* Saved hook values in case of unload */
static ExecutorStart_hook_type prev_ExecutorStart = NULL;
static ExecutorRun_hook_type prev_ExecutorRun = NULL;
static ExecutorFinish_hook_type prev_ExecutorFinish = NULL;

void		_PG_init(void);
void		_PG_fini(void);

/* hooks defined in this module */
static void qs_ExecutorStart(QueryDesc *queryDesc, int eflags);
static void qs_ExecutorRun(QueryDesc *queryDesc, ScanDirection direction, uint64 count, bool execute_once);
static void qs_ExecutorFinish(QueryDesc *queryDesc);

/* Global variables */
List 					*QueryDescStack = NIL;
static const char		*be_state_str[] = {						/* BackendState -> string repr */
							"undefined",						 /* STATE_UNDEFINED */
							"idle",								/* STATE_IDLE */
							"active",							/* STATE_RUNNING */
							"idle in transaction",				/* STATE_IDLEINTRANSACTION */
							"fastpath function call",			/* STATE_FASTPATH */
							"idle in transaction (aborted)",	/* STATE_IDLEINTRANSACTION_ABORTED */
							"disabled",							/* STATE_DISABLED */
						};

/*
 * Result status on query state request from asked backend
 */
typedef enum
{
	QUERY_NOT_RUNNING,		/* Backend doesn't execute any query */
	STAT_DISABLED,			/* Collection of execution statistics is disabled */
	QS_RETURNED				/* Backend succesfully returned its query state */
} PG_QS_RequestResult;

/*
 *	Format of stack information
 */
typedef struct
{
	int		length;							/* size of message record, for sanity check */
	PGPROC	*proc;
	PG_QS_RequestResult	result_code;
	int		warnings;						/* bitmap of warnings */
	int		stack_depth;
	char	stack[FLEXIBLE_ARRAY_MEMBER];	/* sequencially laid out stack frames in form of text records */
} stack_msg;


/*
 * Module load callback
 */
void
_PG_init(void)
{
	if (!process_shared_preload_libraries_in_progress)
		return;

	EmitWarningsOnPlaceholders("pg_self_query");

	/* Install hooks */
	prev_ExecutorStart = ExecutorStart_hook;
	ExecutorStart_hook = qs_ExecutorStart;
	prev_ExecutorRun = ExecutorRun_hook;
	ExecutorRun_hook = qs_ExecutorRun;
	prev_ExecutorFinish = ExecutorFinish_hook;
	ExecutorFinish_hook = qs_ExecutorFinish;
}

/*
 * Module unload callback
 */
void
_PG_fini(void)
{
	/* clear global state */
	list_free(QueryDescStack);

	/* Uninstall hooks. */
	ExecutorStart_hook = prev_ExecutorStart;
	ExecutorRun_hook = prev_ExecutorRun;
	ExecutorFinish_hook = prev_ExecutorFinish;
}

/*
 * ExecutorStart hook:
 * 		set up flags to store runtime statistics,
 * 		push current query description in global stack
 */
static void
qs_ExecutorStart(QueryDesc *queryDesc, int eflags)
{
		/* Enable per-node instrumentation */
		if ((eflags & EXEC_FLAG_EXPLAIN_ONLY) == 0)
		{
			queryDesc->instrument_options |= INSTRUMENT_ROWS;
		}

		if (prev_ExecutorStart)
			prev_ExecutorStart(queryDesc, eflags);
		else
			standard_ExecutorStart(queryDesc, eflags);

}

/*
 * ExecutorRun:
 * 		Catch any fatal signals
 */
static void
qs_ExecutorRun(QueryDesc *queryDesc, ScanDirection direction, uint64 count,
			   bool execute_once)
{
	QueryDescStack = lcons(queryDesc, QueryDescStack);
	PG_TRY();
	{
		if (prev_ExecutorRun)
			prev_ExecutorRun(queryDesc, direction, count, execute_once);
		else
			standard_ExecutorRun(queryDesc, direction, count, execute_once);
		QueryDescStack = list_delete_first(QueryDescStack);
	}
	PG_CATCH();
	{
		QueryDescStack = list_delete_first(QueryDescStack);
		PG_RE_THROW();
	}
	PG_END_TRY();
}

/*
 * ExecutorFinish:
 * 		Catch any fatal signals
 */
static void
qs_ExecutorFinish(QueryDesc *queryDesc)
{
	QueryDescStack = lcons(queryDesc, QueryDescStack);
	PG_TRY();
	{
		if (prev_ExecutorFinish)
			prev_ExecutorFinish(queryDesc);
		else
			standard_ExecutorFinish(queryDesc);
		QueryDescStack = list_delete_first(QueryDescStack);

	}
	PG_CATCH();
	{
		QueryDescStack = list_delete_first(QueryDescStack);
		PG_RE_THROW();
	}
	PG_END_TRY();
}

/*
 * Find PgBackendStatus entry
 */
static PgBackendStatus *
search_be_status(int pid)
{
	int beid;

	if (pid <= 0)
		return NULL;

	for (beid = 1; beid <= pgstat_fetch_stat_numbackends(); beid++)
	{
		PgBackendStatus *be_status = pgstat_fetch_stat_beentry(beid);

		if (be_status && be_status->st_procpid == pid)
			return be_status;
	}

	return NULL;
}

/*
 * Structure of stack frame of fucntion call for decomposition
 */
typedef struct
{
	text	*query;
} stack_frame;

/*
 * Structure of stack frame of fucntion call which resulted from analyze of query state
 */
typedef struct
{
	const char	*query;
} stack_frame_analyze;


static stack_msg *
copy_msg(stack_msg *msg)
{
	stack_msg *result = palloc(msg->length);

	memcpy(result, msg, msg->length);
	return result;
}

/*		
  *	Get List of stack_frames as a stack of function calls starting from outermost call.		
  *		Each entry contains only query text.		
  *	Assume extension is enabled and QueryDescStack is not empty		
  */		
 static List *		
 runtime_explain()		
 {		
 	ListCell	    *i;		
 	List			*result = NIL;		

  	Assert(list_length(QueryDescStack) > 0);		

  	/* collect query state outputs of each plan entry of stack */		
 	foreach(i, QueryDescStack)		
 	{		
 		QueryDesc 	*currentQueryDesc = (QueryDesc *) lfirst(i);		
 		stack_frame_analyze	*qs_frame = palloc(sizeof(stack_frame_analyze));		

  		/* save query text */		
 		qs_frame->query = currentQueryDesc->sourceText;		

  		result = lcons(qs_frame, result);		
 	}		

  	return result;		
 }

 /*
 * Compute length of serialized stack frame
 */
static int
serialized_stack_frame_length(stack_frame_analyze *qs_frame)
{
	return 	INTALIGN(strlen(qs_frame->query) + VARHDRSZ);
}

/*
 * Compute overall length of serialized stack of function calls
 */
static int
serialized_stack_length(List *qs_stack)
{
	ListCell 	*i;
	int			result = 0;

	foreach(i, qs_stack)
	{
		stack_frame_analyze *qs_frame = (stack_frame_analyze *) lfirst(i);
		result += serialized_stack_frame_length(qs_frame);
	}

	return result;
}

/*
 * Convert stack_frame_analyze record into serialized text format version
 * 		Increment '*dest' pointer to the next serialized stack frame
 */
static void
serialize_stack_frame(char **dest, stack_frame_analyze *qs_frame)
{
	SET_VARSIZE(*dest, strlen(qs_frame->query) + VARHDRSZ);
	memcpy(VARDATA(*dest), qs_frame->query, strlen(qs_frame->query));
	*dest += INTALIGN(VARSIZE(*dest));
}

/*
 * Convert List of stack_frame_analyze records into serialized structures laid out sequentially
 */
static void
serialize_stack(char *dest, List *qs_stack)
{
	ListCell		*i;
	foreach(i, qs_stack)
	{
		stack_frame_analyze *qs_frame = (stack_frame_analyze *) lfirst(i);
		serialize_stack_frame(&dest, qs_frame);
	}
}

/*
 * Get state of current query.
 */
static stack_msg *
GetQueryState()
{
	List			*qs_stack = runtime_explain();
	int				msglen = sizeof(stack_msg) + serialized_stack_length(qs_stack);
	stack_msg		*msg = palloc(msglen);

	msg->length = msglen;
	msg->proc = MyProc;
	msg->result_code = QS_RETURNED;
	msg->warnings = 0;	
	msg->stack_depth = list_length(qs_stack);
	serialize_stack(msg->stack, qs_stack);

	return msg;
}

static List *
GetCurrentQueryStates()
{
	List			*result = NIL;
	stack_msg		*msg;

	msg = GetQueryState();
	result = lappend(result, copy_msg(msg));
	
	return result;
}



/*
 *	Convert serialized stack frame into stack_frame record
 *		Increment '*src' pointer to the next serialized stack frame
 */
static stack_frame *
deserialize_stack_frame(char **src)
{
	stack_frame *result = palloc(sizeof(stack_frame));
	text		*query = (text *) *src;

	result->query = palloc(VARSIZE(query));
	memcpy(result->query, query, VARSIZE(query));

	*src = (char *) query + INTALIGN(VARSIZE(query));
	return result;
}

/*
 * Convert serialized stack frames into List of stack_frame records
 */
static List *
deserialize_stack(char *src, int stack_depth)
{
	List 	*result = NIL;
	char	*curr_ptr = src;
	int		i;

	for (i = 0; i < stack_depth; i++)
	{
		stack_frame	*frame = deserialize_stack_frame(&curr_ptr);
		//if (i == (stack_depth - 3))
		//{
			result = lappend(result, frame);
		//}
	}

	return result;
}

/*
 * Implementation of pg_self_query function
 */
PG_FUNCTION_INFO_V1(pg_self_query);
Datum
pg_self_query(PG_FUNCTION_ARGS)
{
	typedef struct
	{
		PGPROC 		*proc;
		ListCell 	*frame_cursor;
		int			 frame_index;
		List		*stack;
	} proc_state;

	/* multicall context type */
	typedef struct
	{
		ListCell	*proc_cursor;
		List		*procs;
	} pg_qs_fctx;

	FuncCallContext	*funcctx;
	MemoryContext	oldcontext;
	pg_qs_fctx		*fctx;
#define		N_ATTRS  2
	pid_t			pid = MyProcPid;

	if (SRF_IS_FIRSTCALL())
	{
		PGPROC			*proc;
		stack_msg		*msg;
		List			*msgs;
							
		proc = BackendPidGetProc(pid);

		if (!proc || proc->backendId == InvalidBackendId)
			ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							errmsg("backend with pid=%d not found", pid)));

		msgs = GetCurrentQueryStates();
		
		funcctx = SRF_FIRSTCALL_INIT();
		
		if (list_length(msgs) == 0)
		{
			elog(WARNING, "backend does not reply");
			SRF_RETURN_DONE(funcctx);
		}

		msg = (stack_msg *) linitial(msgs);

		switch (msg->result_code)
		{
			case QUERY_NOT_RUNNING:
				{
					PgBackendStatus	*be_status = search_be_status(pid);
					if (be_status)
						elog(INFO, "state of backend is %s",
								be_state_str[be_status->st_state - STATE_UNDEFINED]);
					else
						elog(INFO, "backend is not running query");
					SRF_RETURN_DONE(funcctx);
				}
			case STAT_DISABLED:
				elog(INFO, "query execution statistics disabled");
				SRF_RETURN_DONE(funcctx);
			case QS_RETURNED:
				{
					TupleDesc	tupdesc;
					ListCell	*i;
					int64		max_calls = 0;

					/* print warnings if exist */
					if (msg->warnings & TIMINIG_OFF_WARNING)
						ereport(WARNING, (errcode(ERRCODE_WARNING),
										errmsg("timing statistics disabled")));
					if (msg->warnings & BUFFERS_OFF_WARNING)
						ereport(WARNING, (errcode(ERRCODE_WARNING),
										errmsg("buffers statistics disabled")));

					oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

					/* save stack of calls and current cursor in multicall context */
					fctx = (pg_qs_fctx *) palloc(sizeof(pg_qs_fctx));
					fctx->procs = NIL;
					foreach(i, msgs)
					{
						List 		*qs_stack;
						stack_msg	*msg = (stack_msg *) lfirst(i);
						proc_state	*p_state = (proc_state *) palloc(sizeof(proc_state));

						if (msg->result_code != QS_RETURNED)
							continue;

						AssertState(msg->result_code == QS_RETURNED);

						qs_stack = deserialize_stack(msg->stack, msg->stack_depth);

						p_state->proc = msg->proc;
						p_state->stack = qs_stack;
						p_state->frame_index = 0;
						p_state->frame_cursor = list_head(qs_stack);

						fctx->procs = lappend(fctx->procs, p_state);
						max_calls += list_length(qs_stack);
					}
					fctx->proc_cursor = list_head(fctx->procs);

					funcctx->user_fctx = fctx;
					funcctx->max_calls = max_calls;

					/* Make tuple descriptor */
					tupdesc = CreateTemplateTupleDesc(N_ATTRS, false);
					TupleDescInitEntry(tupdesc, (AttrNumber) 1, "frame_number", INT4OID, -1, 0);
					TupleDescInitEntry(tupdesc, (AttrNumber) 2, "query_text", TEXTOID, -1, 0);
					funcctx->tuple_desc = BlessTupleDesc(tupdesc);

					MemoryContextSwitchTo(oldcontext);
				}
				break;
		}
	}

	/* restore function multicall context */
	funcctx = SRF_PERCALL_SETUP();
	fctx = funcctx->user_fctx;

	if (funcctx->call_cntr < funcctx->max_calls)
	{
		HeapTuple 	 tuple;
		Datum		 values[N_ATTRS];
		bool		 nulls[N_ATTRS];
		proc_state	*p_state = (proc_state *) lfirst(fctx->proc_cursor);
		stack_frame	*frame = (stack_frame *) lfirst(p_state->frame_cursor);

		/* Make and return next tuple to caller */
		MemSet(values, 0, sizeof(values));
		MemSet(nulls, 0, sizeof(nulls));
		values[0] = Int32GetDatum(p_state->frame_index);
		values[1] = PointerGetDatum(frame->query);

		tuple = heap_form_tuple(funcctx->tuple_desc, values, nulls);

		/* increment cursor */
		p_state->frame_cursor = lnext(p_state->frame_cursor);
		p_state->frame_index++;

		if (p_state->frame_cursor == NULL)
			fctx->proc_cursor = lnext(fctx->proc_cursor);

		SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(tuple));
	}
	else
	{
		SRF_RETURN_DONE(funcctx);
	}
}