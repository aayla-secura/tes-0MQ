#ifndef __FPGATASKS_H__INCLUDED__
#define __FPGATASKS_H__INCLUDED__

#include "net/fpgaif_reader.h"
// #define CZMQ_BUILD_DRAFT_API
#include <czmq.h>

typedef struct _task_t task_t;

int  tasks_start (ifring* rxring, zloop_t* c_loop);
int  tasks_read (zloop_t* loop);
void tasks_mute (zloop_t* loop);
int  tasks_wakeup (void);
void tasks_destroy (void);
/* Updates head to the head of the slowest active task.
 * If no active tasks, leaves head unchanged. */
void tasks_get_head (uint32_t*);

#endif
