**A UNIX daemon for reading and saving data from the transition edge sensor
(TES) signal processor.**

The daemon is tested on Linux 4.13.0 and FreeBSD 11.0, and above. It requires
[netmap](https://github.com/luigirizzo/netmap) patched drivers for the NIC
connected to the FPGA.

# API
---

The server accepts client requests on two [ØMQ](http://zeromq.org/) sockets. 

## SERVER INFO REP INTERFACE

This interface accepts requests to reply with and log statistics, such
as bandwidth, missed packets etc.

Valid requests have a picture of "4", replies have a picture of "1888888".

#### Message frames in a valid request

1. **Number of seconds**

   How many seconds to accumulate stats for. It must be non-zero.

   The value is read as an **unsigned** int32.

#### Message frames in a reply

1. **Error status**

 * "0": OK, sending trace

 * "1": invalid request

2. **No. of processed packets**

3. **No. of missed packets**

4. **No. of invalid packets**

5. **No. of ticks**

6. **No. of MCA packets**

7. **No. of trace packets**

## CAPTURE REP INTERFACE

This interface accepts requests to save all received frames, until a given
minimum number of tick frames and a given minimum number of events are seen, to
a file (on the server). It can also return the status of a previous successful
request.

Messages are sent and read via `zsock_send` and `zsock_recv` respectively.
These are simply multi-frame ØMQ messages, with each frame being a string
representation of the value.

Valid requests have a picture of "ss88111", replies have a picture of "18888888".

At the moment we only handle one request at a time. Will block until done.

#### Message frames in a valid request

1. **Filename**

   Basename for the hdf5 file (no extension). It is relative to a hardcoded
   root, leading slashes are ignored. Trailing slashes are not allowed. Missing
   directories are created.

2. **Measurement**

   Name of hdf5 group relative to a hardcoded topmost group. It must be non-empty
   if conversion is to be done (now or at a later request).
   
3. **No. of ticks**

   The server will record all packets (including ethernet header) until at
   least that many ticks are seen.

   The value is read as an **unsigned** int64.

4. **No. of events**

   The server will record all packets (including ethernet header) until at
   least that many non-tick events are seen.

   The value is read as an **unsigned** int64.

5. **Write mode**

 * "0": do not overwrite hdf5 file or measurement group

 * "1": only rename existing measurement group

 * "2": overwrite entire hdf5 file

6. **Asyncronous conversion**

 * "0": reply when hdf5 file is finalized

 * "1": reply when hdf5 conversion begins

7. **Capture mode**

 * "0": auto: capture and convert unless only status is requested

 * "1": capture only: at least one of ticks or events must be given

 * "2": convert only: neither ticks nor events can be given

If neither ticks nor events is given **and** capture mode is auto, the
request is interpreted as a status request and the reply that was sent
previously for this filename is re-sent.

A job will only terminate at receiving a tick, and only if both the minimum
number of ticks and the minimum number of non-tick events has been recorded.

As a consequence of how `zsock_recv` parses arguments, the client may omit
frames corresponding to ignored arguments or arguments which are "0".
Therefore to get a status of a file, only the filename and possibly
measurement is required.

#### Message frames in a reply

Some return codes, namely 1, 4 and 7 may indicate an error in either
capture or conversion. This should be obvious from the time it took to
receive the reply.

1. **Error status**

 * "0": writing finished successfully (in case of write request) or
        status of a valid previous job was read (in case of status request)

 * "1": capture request was not understood or
        conversion request was not understood (capture was successful)
 
 * "2": file exists (in case of a no-overwrite request) or
        file does not exist (in case of status request)

 * "3": filename did not resolve to an allowed path

 * "4": error initializing capture, nothing was written or
        error initializing conversion (capture was successful)

 * "5": error while writing to files, some data was saved

 * "6": error while converting to hdf5 format

 * "7": error while writing stats (capture ok, conversion aborted) or
        error deleting data files (conversion was successful)

2. **No. of ticks written**

   May be less than requested in case of an error during write. *Error status*
   will be "5" in this case.


3. **No. of non-tick events written**


4. **No. of traces written**


5. **No. of histograms written**


6. **No. of frames written**


7. **No. of frames dropped by NIC**


8. **No. of frames dropped by us (invalid)**

## AVERAGE TRACE REP INTERFACE

This interface accepts requests to get the first average trace within
a given time period.

Valid requests have a picture of "4", replies have a picture of "1b".

#### Message frames in a valid request

1. **Timeout**

   Number of seconds to wait for an average trace.

   The value is read as an **unsigned** int32.

#### Message frames in a reply

1. **Error status**

 * "0": OK, sending trace

 * "1": invalid request

 * "2": timeout error

 * "3": trace was corrupt

2. **Trace data**

   Empty in case of timeout error. Otherwise---the full trace.

## HISTOGRAM PUB INTERFACE

This socket publishes ZMQ single-frame messages, each message contains one full
histogram (MCA stream). You can receive these with `zmq_recv` for example.

# INSTALLATION
---

To compile and install the client (`tesc`) and server (`tesd`):

```
make
make install
make clean
```

The default install prefix is `/usr/local`, to change it:

```
make PREFIX=<path> install
```

Note: it is a GNU makefile, so on some systems (e.g. FreeBSD) you need to use
`gmake`.

Both client and server will print usage when given the '-h' option.

#### TEST APPLICATIONS

To compile the test apps do `make test` or alternatively `make all` to compile
everything. Test apps are not installed in the `PREFIX` location.

# TO DO
---

* Write REQ job statistics to a global database such that it can be looked up
  by filename, client IP or time frame.
* Web UI
