/*
 * Create an hdf5 file with the following structure:
 *
 * /capture
 * {
 *         /<measurement>
 *         {
 *                 /<dataset>
 *                 /<dataset>
 *                 /<dataset>
 *                 ...
 *         }
 * }
 *
 * Each dataset corresponds to a file (or part of a file).
 * Measurement group and dataset files and names are given in
 * a struct hdf5_conv_req_t.
 */

#ifndef __HDF5CONV_H__INCLUDED__
#define __HDF5CONV_H__INCLUDED__

#include <stdint.h>
#include <czmq_prelude.h> // bool type

/*
 * Exactly one of filename and buffer must be set.
 *
 * 1) If filename is NULL, copy directly from buffer.
 *    Length and offset must then be non-negative.
 *
 * 2) If buffer is NULL, open filename and mmap it. Will close the
 *    file, unmap and nullify buffer at the end.
 *    If offset < 0 it is takes with respect to EOF.
 *    If length < 0 or extends beyond EOF, copy until EOF.
 *    If length == 0 or offset extends beyond EOF, the dataset will
 *    be empty.
 *    The value of offset and length after the call is unspecified.
 */
struct hdf5_dset_desc_t
{
	char*   dsetname; /* dataset name */
	off_t   offset;   /* from beginning of file */
	ssize_t length;   /* how many bytes to copy to dataset */
	char*   filename; /* /path/to/<datafile> */
	void*   buffer;   /* address of mmapped data */
};

struct hdf5_conv_req_t
{
	char*   filename;  /* /path/to/<hdf5file> */
	char*   group;     /* group name under <root_group> */
	struct  hdf5_dset_desc_t* dsets; /* an array of datasets */
	uint8_t num_dsets; /* how many elements in datasets array */
	bool    ovrwt;     /* overwrite entire hdf5 file */
	bool    async;     /* return after opening files,
	                    * convert in background */
};

/*
 * Open/create creq->filename, create/overwrite group
 * <root_group>/creq->group. For each dataset mmap the file.
 * <root_group> is currently "capture".
 * Returns 0 on success, -1 on error.
 */
int hdf5_conv (struct hdf5_conv_req_t* creq);

#endif
