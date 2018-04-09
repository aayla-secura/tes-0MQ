**A UNIX daemon for reading and saving data from the transition edge sensor
(TES) signal processor.**

The daemon is tested on Linux 4.13.0 and FreeBSD 11.0, and above. It
requires [netmap](https://github.com/luigirizzo/netmap) patched
drivers for the NIC connected to the FPGA.

# API

The server talks to clients over [ØMQ](http://zeromq.org/) sockets.

Some interfaces publish raw (binary data). Others specify a "picture".
These are simply multi-frame messages, with each frame being a string
representation of the binary value. Each character in the picture
is a format specifier (see `zsock_send` and `zsock_recv` from CZMQ).

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

This interface publishes single-frame raw messages, each message
contains one full histogram (MCA stream).

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

This interface publishes single-frame raw messages, each message
contains a 16-byte header followed by all coincidences which occured
between two ticks as one or more vectors.

#### Header

The structure of the header is as follows:

 * `byte [0]`:   number of active channels

 * `byte [1]`:   measurement type, see [Measurement type configuration](#window-and-measurement-type-configuration)

 * `bytes[2:4]`: coincidence window

 * `bytes[4:8]`: number of ticks since last frame

 * `bytes[8:16]`: per-channel info

   There can be at most 8 channels. `byte[b+8]` gives info for
   channel b. Bytes at offset beyond the number of active channels
   are to be discarded. The highest three bits of each byte are
   reserved for flags as follows:
   * `bit [7]`:   **HASNOISE**:
     The lowest threshold for the channel is > 0, meaning a noise
     measurement is possible, see [Coincidence vectors](#coincidence-vectors)
   * `bits[5:7]` : (reserved for future)

#### Coincidence vectors

The vectors are `<number of channels>` bytes long. Each byte is a
token which, after ay flags are masked out, gives the number of
photons in the given channel within the coincidence window. The
maximum number of photons for each channel is the number of set
thresholds for this channel. The maximum number of thresholds that can
be set is 16. Flags are described later. The count tokens are as
follows:

 * 0:    No events occurred in this channel
 * 1-16: That many photons (according to the thresholds) were measured
 * 17:   There was an event, but the measurement was below the
         lowest threshold, i.e. it was noise.
 * 18:   There was an event but it did not contain the configured
         measurement, i.e. no. of photons is unknown.

Values for "noise" and "unknown" are subject to change but they will
be > 16 and < 32.

A coincidence ends when an event arrives with a delay > `<window>`
since the last event.

If more than one event occurs in the same channel during the
coincidence the coincidence will contain more than one vector and is
flagged as **UNRESOLVED**, see [flags](#coincidence-vector-flags).

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

##### Coincidence vector flags
The highest three bits of the first element are reserved for flags as
follows:

 * `bit[7]` **UNRESOLVED**:
   * consecutive events each within less than `<window>` delay from
     the previous, but last one is delayed more than `<window>` since
     first
   * two events in the same channel within the same coincidence
 * `bit[6]` **BAD**:
   measurement is not peak and there are multiple peaks within one of
   the events in the coincidence group
 * `bit[5]` (reserved for future)

### Window and measurement type configuration

Valid requests and replies have a picture of "21".

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

Valid requests have a picture of "11b", replies have a picture of "1b".

#### Message frames in a valid request

1. **Measurement type**

   Any valid measurement type, see [Measurement type configuration](#window-and-measurement-type-configuration)

   The value is read as an **unsigned** int8.

2. **Channel number**

   The value is read as an **unsigned** int8.

3. **Threshold array**

   The value is read as a byte buffer. Maximum size is 16.

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

## COINCIDENCE COUNTER REP+PUB INTERFACE

This interface counts number of coincidences over a configurable
number of ticks, matching them against all of its subscription
patterns (see [Subscription pattern](#subscription-pattern) for the
format of a pattern). For each subscription it publishes multi-frame
messages with a picture "s2888888" as follows:

1. **Pattern**

   Echoing back the subscription pattern. Only clients that subscribed
   to this will receive the whole message.

2. **Coincidence window**

3. **No. of ticks**

4. **No. of resolved coincidences matching the pattern**

5. **No. of resolved coincidences matching the pattern (single peak)**

6. **No. of resolved coincidences**

7. **No. of resolved coincidences (single peak)**

8. **No. of unresolved coincidences**

where "single peak" means the **BAD** flag is not set.

By default pattern counts are synchronized in that they start counting
at the same tick and over the same globally configurable number of
ticks, `gt`. New subscriptions will start receiving counts at the next
batch of `gt` (potentially waiting for `2*gt - 1` tick periods).

The global tick number can be configured by a REQ port, see [Global
tick number configuration](#global-tick-number-configuration).

A subscription may request its own private tick counter, `lt`, in
which case counting begins immediately and counts are published every
`lt` ticks.

#### Subscription pattern

A valid subscription is a string of coma separated tokens, as follows:

   * 0:    No photons (either no events or noise)
   * -:    Noise
   * 1-16: Exactly that many photons were measured
   * N:    Any non-zero number of photons
   * X:    Any (including a missing measurement)

X can be omitted, i.e. consequtive separators assume X, as do missing
tokens at the end (fewer tokens than the number of channels was
given). A missing separator or a total number of separators >= no. of
channels is an invalid pattern and will be discarded.

The token list may optionally be followed by a `:<no. of ticks>` in
which case the pattern will use its own private tick counter. Counting
will begin at the next tick and be published every `lt = <no. of ticks>`
ticks. The tick number cannot be negative (such pattern is invalid).

   * If the tick number is 0 or is missing (but there is a colon) it
     defaults to the global tick number, `gt`, and will change if that
     changes. Counting for the pattern however will start immediately
     and be published every `gt` ticks.

   * If no tick number is given (no colon) the pattern will be
     synchronised with other patterns (that use the global tick
     counter). It will start counting at the next block of `gt` ticks
     and be published every `gt` ticks.

If a pattern is invalid or the maximum number of subscription patterns
has been reached, the task sends out a two-frame message of the form
`<pattern>|<NULL>`, where `<pattern>` is echoing back the subscription
and `<NULL>` is an empty frame.

### Global tick number configuration

Valid requests and replies have a picture of "4".

#### Message frames in a valid request

1. **No. of ticks**

   The value is read as an **unsigned** int32.

#### Message frames in a reply

1. **No. of ticks**

The reply indicates the value after it is set. A request with tick
= 0 is not valid and will return the current setting without change.
Any other request should change the setting and be echoed back. The
new setting will take effect at the start of the next counters.

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
