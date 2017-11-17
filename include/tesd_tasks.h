#ifndef __FPGATASKS_H__INCLUDED__
#define __FPGATASKS_H__INCLUDED__

#include "net/tesif_reader.h"
// #define CZMQ_BUILD_DRAFT_API
#include <czmq.h>

typedef struct _task_t task_t;

/*
 * Start the tasks and if c_loop is not NULL, register a generic reader for
 * each task.
 * Returns 0 on success, -1 on error.
 */
int  tasks_start (ifdesc* ifd, zloop_t* c_loop);
/*
 * Register a generic reader with the loop. The reader will listen to all tasks
 * and terminate the loop when a task dies. This is called by tasks_start if
 * a non-NULL zloop_t* is passed.
 */
int  tasks_read (zloop_t* loop);
/*
 * Deregister the reader of each task with the loop.
 */
void tasks_mute (zloop_t* loop);
/*
 * Sends a wake up signal to all tasks waiting for more packets.
 */
int  tasks_wakeup (void);
/*
 * Asks each task to terminate and cleans up.
 */
void tasks_destroy (void);
/*
 * For each ring, returns the head of the slowest active task.
 * If no active tasks, returns NULL.
 */
uint32_t* tasks_get_heads (void);

#endif
