#ifndef __HDF5CONV_H__INCLUDED__
#define __HDF5CONV_H__INCLUDED__

#include <stdint.h>
#include <czmq_prelude.h> // bool type

#ifdef linux
// is it there only on Debian?
#  include <hdf5/serial/hdf5.h>
#else
#  include <hdf5.h>
#endif

/*
 * If offset is negative, copy directly from buffer. Length must be
 * positive.

 * If offset >= 0, open <filename> and mmap it.
 * If length is negative or extends beyond EOF, copy until EOF starting
 * at <offset>.
 * If offset extends beyond EOF, file will be closed and the dataset will
 * be empty.
 * If length is 0, the file will not be opened at all and the dataset
 * will be empty.
 */
struct hdf5_dset_desc_t
{
	char*   dname;  /* dataset name */
	off_t   offset; /* from beginning of file */
	ssize_t length; /* how many bytes to copy to dataset */
	union
	{
		char* filename; /* /path/to/<datafile> */
		void* buffer;   /* address of data */
	};
};

struct hdf5_conv_req_t
{
	char* filename;    /* /path/to/<hdf5file> */
	char* group;       /* group name under ROOT_GROUP */
	struct hdf5_dset_desc_t* datasets;
	uint8_t num_dsets; /* how many elements in datasets array */
	bool ovrwt;        /* overwrite entire hdf5 file */
	bool async;        /* return after opening files,
			    * convert in background */
};

int hdf5_conv (const struct hdf5_conv_req_t* creq);

#endif
