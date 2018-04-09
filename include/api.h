/* See README */
#ifndef __API_H__INCLUDED__
#define __API_H__INCLUDED__

#define TES_NCHANNELS 2 // TODO: query register server
#define TES_MAX_NCHANNELS 8

/* Server info */
#define TES_INFO_LPORT "55554"
#define TES_INFO_REQ_OK    0 // accepted, reply/action follows
#define TES_INFO_REQ_EINV  1 // malformed request

#define TES_INFO_REQ_PIC "4"
#define TES_INFO_REP_PIC "188888881"

#define TES_INFO_ETYPE_PEAK       1
#define TES_INFO_ETYPE_AREA       2
#define TES_INFO_ETYPE_PULSE      3
#define TES_INFO_ETYPE_TRACE_SGL  4
#define TES_INFO_ETYPE_TRACE_AVG  5
#define TES_INFO_ETYPE_TRACE_DP   6
#define TES_INFO_ETYPE_TRACE_DPTR 7

/* Capture to file */
#define TES_CAP_LPORT "55555"
#define TES_CAP_REQ_OK     0 // accepted or all OK
#define TES_CAP_REQ_EINV   1 // malformed request
#define TES_CAP_REQ_EABORT 2 // file exists (for no-overwrite)
#define TES_CAP_REQ_EPERM  3 // a filename is not allowed
#define TES_CAP_REQ_EFAIL  4 // error initializing
#define TES_CAP_REQ_EWRT   5 // error while writing
#define TES_CAP_REQ_ECONV  6 // error while converting
#define TES_CAP_REQ_EFIN   7 // conversion ok, error deleting data
                             // files or writing stats

#define TES_CAP_REQ_PIC "ss88111"
#define TES_CAP_REP_PIC "18888888"

#define TES_H5_OVRWT_NONE   0 // error if /<RG>/<group> exists
#define TES_H5_OVRWT_RELINK 1 // only move existing group to
                              // /<RG>/overwritten/<group>_<timestamp>
#define TES_H5_OVRWT_FILE   2 // overwrite entire hdf5 file

/* Capture/conversion mode. Keep in mind status requests should default
 * to all 0 and require only a filename and group, and that setting
 * min_ticks or min_events should be enough to indicate capture. */
#define TES_CAP_AUTO     0 // capture and convert unless status
#define TES_CAP_CAPONLY  1 // capture only
#define TES_CAP_CONVONLY 2 // convert only

/* Get average trace */
#define TES_AVGTR_LPORT "55556"
#define TES_AVGTR_REQ_OK    0 // accepted
#define TES_AVGTR_REQ_EINV  1 // malformed request
#define TES_AVGTR_REQ_ETOUT 2 // timeout
#define TES_AVGTR_REQ_EERR  3 // dropped trace
#define TES_AVGTR_REQ_PIC  "4"
#define TES_AVGTR_REP_PIC "1b"
// #define TES_AVGTR_MAXSIZE 65528U

/* Publish MCA histogram */
#define TES_HIST_LPORT "55565"
#include "net/tespkt.h" // defines TES_HIST_MAXSIZE

/* Publish jitter histogram */
#define TES_JITTER_REQ_PIC   "18"
#define TES_JITTER_REP_PIC   TES_JITTER_REQ_PIC
#define TES_JITTER_REP_LPORT "55557"
#define TES_JITTER_PUB_LPORT "55567"
#define TES_JITTER_HDR_LEN    8 // global
#define TES_JITTER_SUBHDR_LEN 8 // per-histogram
#define TES_JITTER_NBINS   1022 // including under-/overflow
#define TES_JITTER_SUBSIZE 4096 // subhdr + nbins*4 bytes
#define TES_JITTER_NHISTS  (TES_NCHANNELS - 1)
#define TES_JITTER_SIZE    (TES_JITTER_HDR_LEN + \
                 TES_JITTER_SUBSIZE*TES_JITTER_NHISTS)

/* Publish raw coincidences */
#define TES_COINC_REQ_PIC   "21"
#define TES_COINC_REP_PIC   TES_COINC_REQ_PIC
#define TES_COINC_REQ_TH_OK    0
#define TES_COINC_REQ_TH_EINV  1 // malformed request
#define TES_COINC_REQ_TH_PIC   "11b"
#define TES_COINC_REP_TH_PIC   "1b"
#define TES_COINC_REP_LPORT    "55558"
#define TES_COINC_REP_TH_LPORT "55559"
#define TES_COINC_PUB_LPORT    "55568"
#define TES_COINC_MAX_PHOTONS 16
#define TES_COINC_MEAS_AREA 0
#define TES_COINC_MEAS_PEAK 1
#define TES_COINC_MEAS_DOTP 2
#define TES_COINC_MAX_WINDOW UINT16_MAX
#define TES_COINC_HDR_LEN  16
#define TES_COINC_MAX_SIZE  (TES_NCHANNELS*256)

#define TES_COINC_TOK_NONE  0 // no event in this channel
#define TES_COINC_TOK_NOISE   \
	(TES_COINC_MAX_PHOTONS+1) // measurement below threshold
#define TES_COINC_TOK_UNKNOWN \
	(TES_COINC_MAX_PHOTONS+2) // an event with no measurement

/*
 * Three most-significant bits are reserved for flags.
 * TES_COINC_HDR_FLAG_* apply to the per-channel info elements in the
 * header.
 * TES_COINC_VEC_FLAG_* apply to the first element of a coincidence
 * vector.
 */
#define TES_COINC_FLAG_MASK  0xE0
#define TES_COINC_HDR_FLAG_HASNOISE    (1 << 7)
#define TES_COINC_VEC_FLAG_UNRESOLVED  (1 << 7)
#define TES_COINC_VEC_FLAG_BAD         (1 << 6)

/* Publish coincidence counters */
#define TES_COINCCOUNT_PUB_PIC "s2888888"
#define TES_COINCCOUNT_REQ_PIC "4"
#define TES_COINCCOUNT_REP_PIC  TES_COINCCOUNT_REQ_PIC
#define TES_COINCCOUNT_REP_LPORT "55560"
#define TES_COINCCOUNT_PUB_LPORT "55570"
#define TES_COINCCOUNT_SEP_SYM   ','
#define TES_COINCCOUNT_SEP_TICKS ':'
#define TES_COINCCOUNT_SYM_NUM   'N' // -> TOK_NUM
#define TES_COINCCOUNT_SYM_NOISE '-' // -> TES_COINC_TOK_NOISE
#define TES_COINCCOUNT_SYM_ANY   'X' // -> TOK_ANY

#endif
