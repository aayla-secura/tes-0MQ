# Repository layout

```
bin          Compiled binaries (including test apps)
lib          Compiled static libraries
include      Header files for libraries in src/lib and other general headers
src
 +---bin     Main source files for tesd and tesc
 +---lib     Source files for general purpose libs
tests        Source files for test apps
```

# Makefile

There are specific rules for `tesc` and `tesd`.
Will compile everything in `tests/`.
Will compile everything in `src/lib/` as static libraries and link them with `tesc`, `tesd` and every test app.
`tesc` and `tesd` are also dynamically linked with the libraries given in LDLIBS.
Test apps are only linked with additional dynamic libraries if they are present in the filename. At the moment checks are made for czmq, zmq, pthread and pcap.

# Header files

Headers specific to `tesd` are all in `src/bin/`.
Header files for each library in `src/lib/` are in `include/`
Also in `include/` are general purpose headers (e.g. `ansicolors.h`) as well as `api.h`.
`api.h` defines useful macros for clients whishing to talk to the server, it's used by `tesc`.


# Source layout

Line width should be no more than 69 characters! Some people like to vsplit their screen.

## `tesd`

`tesd.c`:
 * Definition of main and the body of the corrdinator.

`tesd_tasks.c`:
 * The glue between the coordinator and tasks. It defines a template for tasks
 * and is responsible for starting, initializing, stopping, dispatching all
 * tasks defined in a static array there. Tasks need only define their own
 * specific client and packet handlers, declare them in `tesd_tasks.h` and add an
 * entry in the task list in `tesd_task.c`.
 * Also, defines functions used by the coordinator to manage tasks in bulk, as
 * well as helpers used by all tasks.
 * See notes at the beginning of `tesd_tasks.c` for more.

`tesd.h`:
 * Common to tasks and coordinator.

`tesd_tasks.h`:
 * Declarations of task related actions used by all tasks.
 *   Definitions are in `tesd_tasks.c`
 * Declarations of task handlers.
 *   Definitions are in `tesd_task_*.c`

`tesd_tasks_coordinator.h`:
 * Declarations of task related actions used by the coordinator
 *   Definitions are in `tesd_tasks.c`

`tesd_task_*.c`:
 * Each task has its own file where it defines its packet and client handlers
 * used by `tesd_tasks.c`

To add a new task:
 * Copy `src/bin/tesd_task_skeleton.c` to `src/bin/tesd_task_<task_name>.c`
 * Declare the handlers in `src/bin/tesd_tasks.h`
 * Define constants useful to the clients in `include/api.h`
 * Add an entry in THE TASK LIST in `src/bin/tesd_tasks.c`

# Comment style

```
/* This is a description of following few lines of code */
```

```
x += 10; /* short explanation */
```

```
/*
 * This is a long description of the function below.
 * Returns foo on success, bar on error.
 */
int func (void)
{
	...
}
```

```
struct
{
	int x; // describe the declared member
}
```

# Speed optimization

In principle prefer splitting long function in several even if some of those
functions are used in one place. If optimization level in makefile is at least
one they will be inlined anyway. In the packet handler of tasks which handle
heavy load, try to avoid as many function calls as possible. For skipping
a block, goto is FINE, period.

Use assert in critical places or if you expect result to change over different runs.
dbg_assert for everything else (consistency check when changing some struct
definition or function call). dbg_assert is a no-op if DEBUG_LEVEL is 0.

Some checks and messages are skipped depending on DEBUG_LEVEL.
Debug levels are defined in src/bin/tesd.h

# Simulation with vale ports

There are some test apps that can read packets from pcap capture, or a plain
server capture, or generate dummy packets; then inject them into a vale ring.
To setup and connect two vale ports with 4 rings each.

```
$ lsmod | grep -q netmap || modprobe netmap
$ vale-ctl -n vi0 -C 1024,1024,4,4
$ vale-ctl -n vi1 -C 1024,1024,4,4
$ vale-ctl -a vale0:vi0
$ vale-ctl -a vale0:vi1
```

The server needs to know the number of rings in the interface, at the moment it
is defined in a macro and not configured on the command line, or determined at
startup. So make sure the number of rings in the vale ports match this.

To run the server as uid 100, gid 1000, on one of the ports:
```
$ tesd -i vale0:vi1 -v -u 1000 -g 100 -U1 -f
```

The test apps default to injecting into vale0:vi0 (can be changed at compile time).

To clean up:
```
$ vale-ctl -d vale0:vi0
$ vale-ctl -d vale0:vi1
$ vale-ctl -r vi0
$ vale-ctl -r vi1
$ modprobe -r netmap
```

# TO DO

* test with big-endian FPGA byte order

* [comments] replace: FIX with XXX
* replace `<type>* <pt>` with `<type> *<pt>`
* remove cast of `malloc`/`mmap`
* remove space after function name?
* when splitting lines, put operator on the new line

* remove unnecessary `dbg_assert (<smth> != NULL)` at start of most functions
* move debugging tespkt functions in `tespkt.h` to `tespkt.c`?
* [`tescap_tx.c`]: inject from fidx + adat and autodetect headers in `*dat` files
* move `tespkt_is_valid` check to coordinator and have result available to threads?

* non-gcc support?
* clean output from make (suppress noise)
* set up autotools and add documentation in `doc/`
* add license
