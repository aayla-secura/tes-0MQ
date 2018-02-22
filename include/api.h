/* See README */
#ifndef __API_H__INCLUDED__
#define __API_H__INCLUDED__

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

#define TES_CAP_REQ_PIC  "ss88111"
#define TES_CAP_REP_PIC "18888888"

#define TES_H5_OVRWT_NONE   0 /* error if /<RG>/<group> exists */
#define TES_H5_OVRWT_RELINK 1 /* only move existing group to
                              * /<RG>/overwritten/<group>_<timestamp> */
#define TES_H5_OVRWT_FILE   2 /* overwrite entire hdf5 file */

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

/* Publish histogram */
#define TES_HIST_LPORT "55565"

#endif