/*
 * Declarations of task related actions used by all tasks.
 * Definitions are in tesd_tasks.c
 *
 * Also declarations of task handlers.
 * Definitions are in tesd_task_*.c
 */

#ifndef __TESD_TASKS_H__INCLUDED__
#define __TESD_TASKS_H__INCLUDED__

#include "tesd.h"
#include "net/tesif_reader.h"

/* From netmap_user.h */
#define likely(x)   __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

#if __BYTE_ORDER == TES_BYTE_ORDER
#  define htofs
#  define htofl
#else
#  define htofs bswap16
#  define htofl bswap32
#endif

/* Signals for communicating between coordinator and task threads */
#define SIG_INIT   0 /* task -> coordinator thread when ready */
#define SIG_STOP   1 /* coordinator -> task when shutting down */
#define SIG_DIED   2 /* task -> coordinator when error */
#define SIG_WAKEUP 3 /* coordinator -> task when new packets */

/* Return codes for task's socket handlers */
#define TASK_SLEEP  1
#define TASK_ERROR -1

/* Shorthand */
typedef struct _task_t task_t;
typedef struct _task_endpoint_t task_endp_t;

typedef int (task_data_fn)(task_t*);
typedef int (task_pkt_fn)(zloop_t*, tespkt*,
		uint16_t, uint16_t, int, task_t*);

struct _task_endpoint_t
{
	zloop_reader_fn* handler;
	const char* addresses;      // comma-separated
	zsock_t*    sock;
	const int   type;           // one of ZMQ_*
	bool        automute;       // s_task_(de)activate will
	                            // enable/disable handler
};

struct _task_t
{
	zloop_t*      loop;
	task_pkt_fn*  pkt_handler;
	task_data_fn* data_init;    // initialize data
	task_data_fn* data_fin;     // cleanup data
	void*         data;         // task-specific
	zactor_t*     shim;         // coordinator's end of the pipe,
	                            // signals sent on behalf of
	                            // coordinator go here
#define MAX_FRONTENDS 16
	task_endp_t frontends[MAX_FRONTENDS];
	int         id;             // the task ID
	tes_ifdesc* ifd;            // netmap interface
	uint32_t    heads[NUM_RINGS]; // per-ring task's head
	uint16_t    nrings;         // number of rings <= NUM_RINGS
	uint16_t    prev_fseq;      // previous frame sequence
	uint16_t    prev_pseq_mca;  // previous MCA protocol sequence
	uint16_t    prev_pseq_tr;   // previous trace protocol sequence
	bool        autoactivate;   // s_task_shim will activate task
	bool        just_activated; // first packet after activation
	bool        error;          // internal, see DEV NOTES
	bool        busy;           // internal, see DEV NOTES
	bool        active;         // internal, see DEV NOTES
#ifdef ENABLE_FULL_DEBUG
	struct
	{
		uint64_t wakeups;
		uint64_t wakeups_inactive; // woken up when inactive
		uint64_t wakeups_false;    // woken up when no new packets
		uint64_t rings_dispatched;
		struct
		{
			uint64_t rcvd_in[NUM_RINGS];
			uint64_t missed;
		} pkts;
	} dbg_stats;
#endif
};

/*
 * Synchronizes the task's head with the ring's head and sets active
 * to true. If the task handles one client at a time, disables
 * reading the client_handler.
 */
void task_activate (task_t* self);

/*
 * Deactivates the task and, if the task handles one client at
 * a time, enables reading the client_handler.
 * Returns 0 on success, TASK_ERROR on error.
 */
int  task_deactivate (task_t* self);

/* ------------------------ TASK HANDLERS ----------------------- */

/* Server info */
zloop_reader_fn task_info_req_hn;
task_pkt_fn     task_info_pkt_hn;
task_data_fn    task_info_init;
task_data_fn    task_info_fin;

/* Capture to file */
zloop_reader_fn task_cap_req_hn;
task_pkt_fn     task_cap_pkt_hn;
task_data_fn    task_cap_init;
task_data_fn    task_cap_fin;

/* Get average trace */
zloop_reader_fn task_avgtr_req_hn;
task_pkt_fn     task_avgtr_pkt_hn;
task_data_fn    task_avgtr_init;
task_data_fn    task_avgtr_fin;

/* Publish histogram */
zloop_reader_fn task_hist_sub_hn;
task_pkt_fn     task_hist_pkt_hn;
task_data_fn    task_hist_init;
task_data_fn    task_hist_fin;

#endif
