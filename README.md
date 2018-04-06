**A UNIX daemon for reading and saving data from the transition edge sensor
(TES) signal processor.**

The daemon is tested on Linux 4.13.0 and FreeBSD 11.0, and above. It
requires [netmap](https://github.com/luigirizzo/netmap) patched
drivers for the NIC connected to the FPGA.

# API

The server talks to clients over [ØMQ](http://zeromq.org/) sockets. 

## PACKET INFO REP INTERFACE

This interface accepts requests to reply with and log statistics, such
as bandwidth, missed packets etc.

Valid requests have a picture of "4", replies have a picture of
"188888881".

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

6. **No. of MCA headers**

7. **No. of trace headers**

8. **No. of other events**

   This is the number of (non-tick, non-trace) events. Each non-tick,
   non-trace event frame contains several of those.

9. **Event types seen**

   This is a bitmask of event types seen:

     bit 1: peak

     bit 2: area

     bit 3: pulse

     bit 4: trace single

     bit 5: trace average

     bit 6: dot-product

     bit 7: trace dot-product

   If 0, no events were seen (only ticks and possibly MCAs).

## CAPTURE REP INTERFACE

This interface accepts requests to save all received frames, until
a given minimum number of tick frames and a given minimum number of
events are seen, to a file (on the server). It can also return the
status of a previous successful request.

Messages are sent and read via `zsock_send` and `zsock_recv`
respectively. These are simply multi-frame ØMQ messages, with each
frame being a string representation of the value.

Valid requests have a picture of "ss88111", replies have a picture of
"18888888".

At the moment we only handle one request at a time. Will block until
done.

#### Message frames in a valid request

1. **Filename**

   Basename for the hdf5 file (no extension). It is relative to
   a hardcoded root, leading slashes are ignored. Trailing slashes are
   not allowed. Missing directories are created.

2. **Measurement**

   Name of hdf5 group relative to a hardcoded topmost group. It must
   be non-empty if conversion is to be done (now or at a later
   request).
   
3. **No. of ticks**

   The server will record all packets (including ethernet header)
   until at least that many ticks are seen.

   The value is read as an **unsigned** int64.

4. **No. of events**

   The server will record all packets (including ethernet header)
   until at least that many non-tick events are seen.

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
previously for this filename is resent.

A job will only terminate at receiving a tick, and only if both the
minimum number of ticks and the minimum number of non-tick events has
been recorded.

As a consequence of how `zsock_recv` parses arguments, the client may
omit frames corresponding to ignored arguments or arguments which are
"0". Therefore to get a status of a file, only the filename and
possibly measurement is required.

#### Message frames in a reply

Some return codes, namely 1, 4 and 7 may indicate an error in either
capture or conversion. This should be obvious from the time it took to
receive the reply.

1. **Error status**

 * "0": writing finished successfully (in case of write request) or
        status of a valid previous job was read (in case of status
        request)

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

## MCA HISTOGRAM PUB INTERFACE

This interface publishes single-frame messages, each message contains
one full histogram (MCA stream).

## JITTER HISTOGRAM REP+PUB INTERFACE

This interface publishes single-frame messages, each message contains
one full histogram, representing the jitter between two channels and
constructed over a configurable number of ticks. The histogram has
a fixed number of bins, each of width 1. The reference channel is also
configurable. Each point in the histogram corresponds to a frame of
the non-reference channel. If it falls in the positive bin range, it
gives the delay (in units of 4ns) since the last reference channel
frame. If it falls in the negative bin range, it gives the delay (in
units of 4ns) until the next reference channel frame. There is only
one entry for each non-reference frame: the smallest absolute value of
the two possibilities being taken. (i.e. if the delay since the last
reference is smaller than the delay before the next reference, the
positive entry is taken, and vice versa).

### Configuration

Reference channel and number of ticks to accumulate over is configured
by sending a message to the REP socket. Valid requests have a picture
of "18", replies have a picture of "1".

#### Message frames in a valid request

1. **Reference channel**

   The value is read as an **unsigned** int8.

2. **Ticks**

   The value is read as an **unsigned** int64.

#### Message frames in a reply

1. **Set reference channel**

2. **Set ticks**

The reply indicates the values after they are set. A request with
ticks = 0 OR reference channel > no. of channels is not valid and will
return the current setting without change. Any other request should
change the settings and be echoed back. The new settings will take
effect at the next histogram.

## RAW COINCIDENCE REP+REP+PUB INTERFACE

This interface publishes single-frame messages, each message contains
all coincidences which occured between two ticks as one or more
vectors. The vectors are `<number of channels>` bytes long. Each byte
gives the number of photons in the given channel within the
coincidence window. The maximum number is 16. 0 means no events
occured in this channel. Two special tokens can also appear (values
subject to change, but they will be > 16 and < 32:

 * 17: There was an event, but the measurement was below the
   threshold, i.e. it was noise.
 * 18: There was an event but it did not contain the configured
   measurement, i.e. no. of photons is unknown.

A coincidence ends when an event arrives with a delay > `<window>`
since the last event.

If more than one event occurs in the same channel during the
coincidence the coincidence will contain more than one vector and is
flagged as **UNRESOLVED** (see flags below).

If the total delay since the start of the coincidence exceeds
`<window>` all vectors in the coincidence are also flagged as
**UNRESOLVED**. For example:

```
no. of channels = 2; window > 10
channel  delay  photons
  0        10      1
  1        10      2       ==>  UNRESOLVED [1, 2]
  1        10      1       ==>  UNRESOLVED [0, 1]

no. of channels = 4; window > 10 && window < 40
channel  delay  photons
  0        10      1
  1        10      2
  2        10      1
  3        10      3       ==>  UNRESOLVED [1, 2, 1, 3]
```

A vector of all zeroes (after flags are masked out) indicates a tick.
The last tick before the start of a coincidence may be joined with the
coincidence vector (as a flag). This is enabled/disabled at compile
time.

The highest two (or three) bits of the first element are reserved for
flags as follows:

 * `bit[7]` **UNRESOLVED** (coincidence vector):
   * consecutive events each within less than `<window>` delay from
     the previous, but last one is delayed more than `<window>` since
     first two events in the same channel within the same coincidence
 * `bit[7]` **UNRESOLVED** (tick vector):
   tick occured during the previous coincidence (there may be
   multiple consecutive tick vectors with this flag, but never an
   unresolved tick following a resolved one with no coincidences in
   between
 * `bit[6]` **BAD** (coincidence vector):
   measurement is not peak and there are multiple peaks within one of
   the events in the coincidence group

If ticks are joined with the coincidence the UNRESOLVED flag will not
be applied to the coincidence vector with the tick flag set, even if
that tick occured during the coincidence. It will only be set for the
extra tick vectors before that coincidence, if any of them also
occured during the coincidence. Furthermore, if ticks are joined with
coincidences, an additional flag is used:

 * `bit[5]` **TICK** (coincidence vector):
   coincidence is first after a tick
 * `bit[5]` **TICK** (tick vector):
   if n > 1 ticks occured between coincidences, there'd be n - 1
   tick vectors with this flag

### Window/measurement type configuration

#### Message frames in a valid request

1. **Coindidence window**

   In units of 4ns. The value is read as an **unsigned** int16.

2. **Measurement type**

 * "0": area

 * "1": peak height

 * "2": dot product

   The value is read as an **unsigned** int8.

#### Message frames in a reply

1. **Coindidence window**

2. **Measurement type**

The reply indicates the values after they are set. A request with
window = 0 OR channel > no. of channels is not valid and will return
the current setting without change. Any other request should change
the settings and be echoed back. The new settings will take effect at
the next tick.

### Threshold configuration

#### Message frames in a valid request

1. **Measurement type**

   Any valid measurement type, see measurement type configuration.

   The value is read as an **unsigned** int8.

2. **Channel number**

   The value is read as an **unsigned** int8.

3. **Threshold array**

   The value is read as a byte buffer.

#### Message frames in a reply

1. **Error status**

 * "0": OK, sending trace

 * "1": invalid request

2. **Threshold array**

If measurement or channel is invalid, threshold array will be empty.
Otherwise it will indicate the full array (length = maximum no. of
photons x 4 bytes) after setting. If the given array was invalid (or
missing, the threshold is unchanged). In a valid threshold array every
element is greater than the previous, but there may be trailing zeroes
indicating unset thresholds (the highest photon number in the stream
for this channel will be the number of set threshold elements).

# INSTALLATION

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

Note: it is a GNU makefile, so on some systems (e.g. FreeBSD) you need
to use `gmake`.

Both client and server will print usage when given the '-h' option.

#### TEST APPLICATIONS

To compile the test apps do `make tests` or alternatively `make all`
to compile everything. Test apps are not installed in the `PREFIX`
location.

# TO DO

 * Write REQ job statistics to a global database such that it can be
   looked up by filename, client IP or time frame.
