#ifndef __ZBROKER_H__INCLUDED__
#define __ZBROKER_H__INCLUDED__

#include <czmq.h>

typedef struct _zbroker_t zbroker_t;

void zbroker_set_back (zbroker_t*, zsock_t*, zloop_reader_fn*, zlistx_t*);
void zbroker_set_front (zbroker_t*, zsock_t*, zloop_reader_fn*, zlistx_t*);
int zbroker_front_reader (zbroker_t*, zloop_t*, void*);
int zbroker_back_reader (zbroker_t*, zloop_t*, void*);
void zbroker_front_reader_end (zbroker_t*, zloop_t*);
void zbroker_back_reader_end (zbroker_t*, zloop_t*);
zlistx_t* zbroker_front_parties (zbroker_t*);
zlistx_t* zbroker_back_parties (zbroker_t*);
zbroker_t* zbroker_new ();
void zbroker_destroy (zbroker_t**);

#endif
