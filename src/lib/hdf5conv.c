/*
 * TO DO:
 *  - delete data files after conversion
 */

#include "hdf5conv.h"
#include "daemon_ng.h"

#ifdef linux
// is it there only on Debian?
#  include <hdf5/serial/hdf5.h>
#else
#  include <hdf5.h>
#endif

#include <sys/mman.h>
#include <errno.h>
#include <assert.h>

#define INIT_TIMEOUT 5 /* in seconds */
#define ROOT_GROUP "capture"
#define NODELETE_TMP /* TO DO */
#define DATATYPE H5T_NATIVE_UINT_LEAST8
/* #define DATATYPE H5T_NATIVE_UINT_FAST8 */
/* #define DATATYPE H5T_NATIVE_UINT8 */

static hid_t s_get_grp  (hid_t l_id, const char* group, bool ovrwt);
static int   s_map_file (struct hdf5_dset_desc_t* ddesc, hid_t g_id);
static int   s_create_dset (const struct hdf5_dset_desc_t* ddesc, hid_t g_id);
static int   s_hdf5_init   (void* creq_data_);
static int   s_hdf5_write  (void* creq_data_);

struct s_creq_data_t
{
	struct hdf5_conv_req_t* creq;
	hid_t group_id;
	hid_t file_id;
};

/* ------------------------------------------------------------------------- */
/* -------------------------------- HELPERS -------------------------------- */
/* ------------------------------------------------------------------------- */

/*
 * Open or create <group> relative to l_id. If ovrwt is true and group
 * exists, it is deleted first.
 * Returns group id on success, -1 on error.
 */
static hid_t
s_get_grp (hid_t l_id, const char* group, bool ovrwt)
{
	hid_t g_id;

	/* Check if group exists. */
	htri_t gstat = H5Lexists (l_id, group, H5P_DEFAULT);
	if (gstat < 0)
	{
		logmsg (0, LOG_ERR,
			"Could not check if group %s exists",
			group);
		return -1;
	}
	else if (gstat > 0 && ! ovrwt)
	{ /* open the group */
		g_id = H5Gopen (l_id, group, H5P_DEFAULT);	
	}
	else
	{ /* create the group */
		if (gstat > 0)
		{ /* delete it first */
			herr_t err = H5Ldelete (l_id, group, H5P_DEFAULT);	
			if (err < 0)
			{
				logmsg (0, LOG_ERR,
					"Could not delete group %s",
					group);
				return -1;
			}
		}

		g_id = H5Gcreate (l_id, group, H5P_DEFAULT,
			H5P_DEFAULT, H5P_DEFAULT);

	}
	if (g_id < 0)
	{
		logmsg (0, LOG_ERR,
			"Could not %s group %s",
			(gstat > 0 && ! ovrwt) ? "open" : "create",
			group);
		return -1;
	}

	return g_id;
}

/*
 * Open dataset file, mmap it and close the file descriptor.
 * Saves the address of the mapping in ddesc->buffer.
 * If ddesc->offset < 0 (refers to EOF), recalculates offset w.r.t. BOF
 * and saves it.
 * If ddesc->length < 0 or extends beyond EOF, calculates length and
 * saves it.
 * If length == 0 or offset extends beyond EOF, ddesc->buffer will be
 * NULL.
 * On success buffer may be NULL if dataset should be empty, in which case
 * length is ensured to be 0. Otherwise length is ensured to be positive and
 * offset---to be non-negative.
 * Returns 0 on success, -1 on error.
 */
static int
s_map_file (struct hdf5_dset_desc_t* ddesc, hid_t g_id)
{
#ifdef ENABLE_FULL_DEBUG
	/* sleep (1); */
#endif
	assert (ddesc != NULL);
	assert (ddesc->filename != NULL);
	assert (ddesc->buffer == NULL);
	
	if (ddesc->length == 0)
		return 0;

	/* Open the data file. */
	int fd = open (ddesc->filename, O_RDONLY);
	if (fd == -1)
	{
		logmsg (errno, LOG_ERR,
			"Could not open data file %s",
			ddesc->filename);
		return -1;
	}

	/* Get its size. */
	off_t fsize = lseek (fd, 0, SEEK_END);
	if (fsize == (off_t)-1)
	{
		logmsg (errno, LOG_ERR,
			"Could not seek to end of file %s",
			ddesc->filename);
		close (fd);
		return -1;
	}
	
	/* Check offset. */
	if (ddesc->offset < 0)
		ddesc->offset += fsize;
	if (fsize == 0 || ddesc->offset >= fsize || ddesc->offset < 0)
	{ /* file empty or abs(offset) too large */
		ddesc->length = 0;
		close (fd);
		return 0;
	}

	assert (ddesc->offset >= 0);
	assert (fsize > 0);
	ssize_t maxlength = fsize - ddesc->offset;
	assert (maxlength > 0);
	if (ddesc->length < 0 || ddesc->length > maxlength)
		ddesc->length = maxlength;

	/* mmap from BOF, since mmap requires the offset be a multiple of
	 * page size. */
	assert (ddesc->length > 0);
	void* data = mmap (NULL, ddesc->offset + ddesc->length,
			PROT_READ, MAP_PRIVATE, fd, 0);
	close (fd);
	if (data == (void*)-1)
	{
		logmsg (errno, LOG_ERR,
			"Could not mmap file %s",
			ddesc->filename);
		return -1;
	}

	/* Store the address. Remember to add offset when reading and
	 * unmap offset+length (though unmapping is not necessary). */
	ddesc->buffer = data;

	return 0;
}

/*
 * Write data given in ddesc as a dataset inside group g_id.
 * Returns 0 on success, -1 on error.
 */
static int
s_create_dset (const struct hdf5_dset_desc_t* ddesc, hid_t g_id)
{
#ifdef ENABLE_FULL_DEBUG
	/* sleep (1); */
#endif
	assert (ddesc != NULL);
	assert (ddesc->dsetname != NULL);
	assert (ddesc->buffer != (void*)-1);
	if (ddesc->buffer == NULL)
		assert (ddesc->length == 0);
	else
		assert (ddesc->length > 0);

	logmsg (0, LOG_DEBUG,
		"Creating dataset %s", ddesc->dsetname);

	/* Create the datasets. */
	hsize_t length[1] = {ddesc->length};
	hid_t dspace = H5Screate_simple (1, length, NULL);
	if (dspace < 0)
	{
		logmsg (0, LOG_ERR,
			"Could not create dataspace");
		return -1;
	}
	hid_t dset = H5Dcreate (g_id, ddesc->dsetname, DATATYPE,
		dspace, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
	if (dset < 0)
	{
		logmsg (0, LOG_ERR,
			"Could not create dataset %s",
			ddesc->dsetname);
		H5Sclose (dspace);
		return -1;
	}

	/* Check if dataset is empty. */
	if (ddesc->length == 0)
	{
		H5Dclose (dset);
		H5Sclose (dspace);
		return 0;
	}

	/* Write the data. */
	assert (ddesc->buffer != NULL);
 	assert (ddesc->offset >= 0);
	herr_t err = H5Dwrite (dset, DATATYPE, H5S_ALL,
		H5S_ALL, H5P_DEFAULT, ddesc->buffer + ddesc->offset);
	if (err < 0)
	{
		logmsg (0, LOG_ERR,
			"Could not write to dataset %s",
			ddesc->dsetname);
	}

	H5Dclose (dset);
	H5Sclose (dspace);

	return (err < 0 ? -1 : 0);
}

/*
 * Open the hdf5 file, create the groups if needed.
 * Open all dataset files and mmap them (calls s_map_file).
 * On success, save the file and dataset group ids in creq_data_.
 * Returns 0 on success, -1 on error.
 */
static int
s_hdf5_init (void* creq_data_)
{
	assert (creq_data_ != NULL);

	struct s_creq_data_t* creq_data = (struct s_creq_data_t*)creq_data_;
	struct hdf5_conv_req_t* creq = creq_data->creq;

	/* Check if hdf5 file exists. */
	hid_t f_id;
	int fok = access (creq->filename, F_OK);
	if (fok == 0 && ! creq->ovrwt)
	{ /* open it for writing */
		logmsg (0, LOG_DEBUG,
			"Opening existing hdf5 file: %s",
			creq->filename);
		f_id = H5Fopen (creq->filename, H5F_ACC_RDWR, H5P_DEFAULT);
	}
	else
	{ /* create it */
		if (fok == -1 && errno != ENOENT)
		{ /* the calling process ensures the filepath should be in an
		   * existing directory (same as the capture files) */
			logmsg (errno, LOG_ERR,
			 	"Unexpected error accessing hdf5 file %s",
				creq->filename);
			return -1;
		}
		logmsg (0, LOG_DEBUG,
			"%s hdf5 file: %s",
			(fok ? "Creating" : "Overwriting"), creq->filename);
		f_id = H5Fcreate (creq->filename, H5F_ACC_TRUNC,
			H5P_DEFAULT, H5P_DEFAULT);
	}
	if (f_id < 0)
	{
		logmsg (0, LOG_ERR,
			"Could not %s hdf5 file %s",
			(fok == 0 && ! creq->ovrwt) ? "open" : "create",
			creq->filename);
		return -1;
	}

	/* (Re-)create the group(s). */
	/* root group */
	hid_t rg_id = s_get_grp (f_id, ROOT_GROUP, 0);
	if (rg_id < 0)
	{
		H5Fclose (f_id);
		return -1;
	}

	/* client requested subgroup */
	hid_t cg_id;
	if (strlen (creq->group) == 0)
	{
		cg_id = rg_id;
	}
	else
	{
		cg_id = s_get_grp (rg_id, creq->group, 1);
		H5Gclose (rg_id);
		if (cg_id < 0)
		{
			H5Fclose (f_id);
			return -1;
		}
	}

	/* mmap files. */
	for (int d = 0; d < creq->num_dsets; d++)
	{
		struct hdf5_dset_desc_t* ddesc = &creq->dsets[d];
		if (ddesc->filename == NULL)
			continue;

		int rc = s_map_file (ddesc, cg_id);
		if (rc == -1)
		{
			H5Gclose (cg_id);
			H5Fclose (f_id);
			return -1;
		}
	}

	/* Save the file and group id. */
	creq_data->file_id = f_id;
	creq_data->group_id = cg_id;
	return 0;
}

/*
 * Create all datasets (calls s_create_dset).
 * Returns 0 on success, -1 on error.
 */
static int
s_hdf5_write (void* creq_data_)
{
	assert (creq_data_ != NULL);

	struct s_creq_data_t* creq_data = (struct s_creq_data_t*)creq_data_;

	/* Create the datasets. */
	int rc = 0;
	for (int d = 0; d < creq_data->creq->num_dsets; d++)
	{
		struct hdf5_dset_desc_t* ddesc =
			&creq_data->creq->dsets[d];
		int rc = s_create_dset (ddesc, creq_data->group_id);
		if (rc == -1)
			break;
	}

	/* Close the hdf5 file. */
	H5Gclose (creq_data->group_id);
	H5Fclose (creq_data->file_id);

	return rc;
}

/* ------------------------------------------------------------------------- */
/* ---------------------------------- API ---------------------------------- */
/* ------------------------------------------------------------------------- */

int
hdf5_conv (struct hdf5_conv_req_t* creq)
{
	if (creq == NULL ||
		creq->filename == NULL ||
		strlen (creq->filename) == 0 ||
		creq->group == NULL ||
		creq->dsets == NULL ||
		creq->num_dsets == 0)
	{
		logmsg (0, LOG_ERR, "Invalid request");
		return -1;
	}
	for (int d = 0; d < creq->num_dsets; d++)
	{
		struct hdf5_dset_desc_t* ddesc = &creq->dsets[d];
		if (ddesc == NULL ||
			ddesc->dsetname == NULL ||
			strlen (ddesc->dsetname) == 0 ||
			( ddesc->filename != NULL &&
				ddesc->buffer != NULL ) ||
			( ddesc->filename == NULL &&
				ddesc->buffer == NULL ) ||
			( ddesc->buffer != NULL &&
				( ddesc->offset < 0 ||
					 ddesc->length < 0 ) ) )
		{
			logmsg (0, LOG_ERR, "Invalid request");
			return -1;
		}
	}

	struct s_creq_data_t creq_data = {
		.creq = creq,
		.group_id = -1,
		.file_id = -1,
		};

	/* If operating in asynchronous mode, fork here before opening
	 * the files and signal parent before starting copy. */
	int rc = -1;
	if (creq->async)
	{
		rc = fork_and_run (s_hdf5_init, s_hdf5_write,
				&creq_data, INIT_TIMEOUT);
	}
	else
	{
		rc = s_hdf5_init (&creq_data);
		if (rc == 0)
			rc = s_hdf5_write (&creq_data);
	}

	/* Unmap data. */
	for (int d = 0; d < creq->num_dsets; d++)
	{
		struct hdf5_dset_desc_t* ddesc = &creq->dsets[d];
		if (ddesc->filename != NULL)
		{ /* mmapped by us */
			if (ddesc->buffer != NULL)
			{
				munmap (ddesc->buffer,
						ddesc->offset + ddesc->length);
				ddesc->buffer = NULL;
			}
		}
	}
	return rc;
}
