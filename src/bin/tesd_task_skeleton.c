/*
 * TO DO:
 *  - duplicator/comparator
 */

#include "tesd_tasks.h"

/*
 * 
 */
struct s_data_t
{
};

/* -------------------------------------------------------------- */
/* --------------------------- HELPERS -------------------------- */
/* -------------------------------------------------------------- */


/* -------------------------------------------------------------- */
/* ----------------------------- API ---------------------------- */
/* -------------------------------------------------------------- */

int
task__req_hn (zloop_t* loop, zsock_t* endpoint, void* self_)
{
	dbg_assert (self_ != NULL);

	task_t* self = (task_t*) self_;
	struct s_data_t* data = (struct s_data_t*) self->data;
	
	// int rc = zsock_recv (endpoint, TES__REQ_PIC, );
	/* Would also return -1 if picture contained a pointer (p) or a null
	 * frame (z) but message received did not match this signature; this
	 * is irrelevant in this case; we don't get interrupted, this should
	 * not happen. */
	// assert (rc != -1);
	
	/* Disable polling on the endpoint until the job is done. Wakeup
	 * packet handler. */
	task_activate (self);

	return 0;
}

/*
 *
 */
int
task__pkt_hn (zloop_t* loop, tespkt* pkt, uint16_t flen,
		uint16_t missed, int err, task_t* self)
{
	dbg_assert (self != NULL);
	struct s_data_t* data = (struct s_data_t*) self->data;


	return 0;
}

int
task__init (task_t* self)
{
	assert (self != NULL);

	static struct s_data_t data;

	self->data = &data;
	return 0;
}

int
task__wakeup (task_t* self)
{
	assert (self != NULL);

	return 0;
}

int
task__sleep (task_t* self)
{
	assert (self != NULL);

	return 0;
}

int
task__fin (task_t* self)
{
	assert (self != NULL);

	self->data = NULL;
	return 0;
}
