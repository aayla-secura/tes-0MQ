#include "zbroker.h"
#include <stdlib.h>
// #include <stdio.h>
// TO DO: implement a method to check if a pointer is a valid broker

typedef struct _s_end_t s_end_t;

static void s_end_destroy (s_end_t**);
static void s_set_end (s_end_t* self, zsock_t* reader,
			zloop_reader_fn* handler, zlistx_t* parties);

/* ------------------------------------------------------------------------- */

/* An end type: either a frontend or a backend. As far as we are concerned they
 * are the same, the user chooses how to use them */
struct _s_end_t
{
	zsock_t* reader;
	zloop_reader_fn* handler;
	zlistx_t* parties;
};

/* Structure of the class */
struct _zbroker_t
{
	/* As far as we are concerned the frontend and backend are the same,
	 * user chooses how to use each */
	s_end_t* frontend;
	s_end_t* backend;
};

/* ------------------------------------------------------------------------- */
/* -------------------------------- STATIC --------------------------------- */
/* ------------------------------------------------------------------------- */

/* Set an endpoint's fields */
static void s_set_end (s_end_t* self, zsock_t* reader,
			zloop_reader_fn* handler, zlistx_t* parties)
{
	assert (self);
	self->reader = reader;
	self->handler = handler;
	self->parties = parties;

	return;
}

/* The endpoint destructor */
static void s_end_destroy (s_end_t** self_p)
{
	assert (self_p);
	s_end_t* self = *self_p;

	if (self)
	{
		self->handler = NULL;
		if (self->reader)
			zsock_destroy (&self->reader);
		if (self->parties)
			zlistx_destroy (&self->parties);
		free (self);
		*self_p = NULL;
	}

	return;
}

/* ------------------------------------------------------------------------- */
/* ------------------------------- EXTERNAL -------------------------------- */
/* ------------------------------------------------------------------------- */

/* Set the backend's fields */
void zbroker_set_back (zbroker_t* self, zsock_t* reader,
			zloop_reader_fn* handler, zlistx_t* parties)
{
	assert (self);
	s_set_end (self->backend, reader, handler, parties);

	return;
}

/* Set the frontend's fields */
void zbroker_set_front (zbroker_t* self, zsock_t* reader,
			zloop_reader_fn* handler, zlistx_t* parties)
{
	assert (self);
	s_set_end (self->frontend, reader, handler, parties);

	return;
}

/* Register the frontend's reader with a zloop_t */
int zbroker_front_reader (zbroker_t* self, zloop_t* loop, void* arg)
{
	assert (self);
	s_end_t* frontend = self->frontend;
	assert (frontend);

	return zloop_reader (loop, frontend->reader, frontend->handler, arg);
}

/* Register the backend's reader with a zloop_t */
int zbroker_back_reader (zbroker_t* self, zloop_t* loop, void* arg)
{
	assert (self);
	s_end_t* backend = self->backend;
	assert (backend);

	return zloop_reader (loop, backend->reader, backend->handler, arg);
}

/* Unregister the frontend's reader with a zloop_t */
void zbroker_front_reader_end (zbroker_t* self, zloop_t* loop)
{
	assert (self);
	s_end_t* frontend = self->frontend;
	assert (frontend);

	zloop_reader_end (loop, frontend->reader);
	return;
}

/* Unregister the backend's reader with a zloop_t */
void zbroker_back_reader_end (zbroker_t* self, zloop_t* loop)
{
	assert (self);
	s_end_t* backend = self->backend;
	assert (backend);

	zloop_reader_end (loop, backend->reader);
	return;
}

/* Get the list of parties of the frontend */
zlistx_t* zbroker_front_parties (zbroker_t* self)
{
	assert (self);
	s_end_t* frontend = self->frontend;
	assert (frontend);

	return frontend->parties;
}

/* Get the list of parties of the backend */
zlistx_t* zbroker_back_parties (zbroker_t* self)
{
	assert (self);
	s_end_t* backend = self->backend;
	assert (backend);

	return backend->parties;
}

/* Constructor */
zbroker_t* zbroker_new ()
{
	zbroker_t* self = (zbroker_t*) malloc (sizeof (zbroker_t));
	assert (self);
	self->frontend = (s_end_t*) malloc (sizeof (s_end_t));
	assert (self->frontend);
	self->backend = (s_end_t*) malloc (sizeof (s_end_t));
	assert (self->backend);
	// caller should set the sockets and handlers in the frontends

	return self;
}

/* Destructor */
void zbroker_destroy (zbroker_t** self_p)
{
	assert (self_p);
	zbroker_t* self = *self_p;

	if (self)
	{
		s_end_destroy (&self->frontend);
		s_end_destroy (&self->backend);
		*self_p = NULL;
	}

	return;
}
