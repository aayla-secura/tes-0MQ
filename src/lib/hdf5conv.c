/*
 * TO DO:
 *  - open all data files at once, and if async is false, send message then
 *  - delete data files after conversion
 */

#include "hdf5conv.h"
#include "common.h" /* is_daemon */
#include "daemon_ng.h"

#define INIT_TIMEOUT 5 /* in seconds */
#define ROOT_GROUP "capture"
#define NODELETE_TMP /* TO DO */
#define DATATYPE H5T_NATIVE_UINT_LEAST8
/* #define DATATYPE H5T_NATIVE_UINT_FAST8 */
/* #define DATATYPE H5T_NATIVE_UINT8 */

#define LOGID "[HDF5CONV] "

struct s_creq_data_t
{
	struct hdf5_conv_req_t creq;
	hid_t group_id;
	hid_t file_id;
};

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
		s_msg (0, LOG_ERR, -1,
			"%sCould not check if group %s exists",
			LOGID, group);
		return -1;
	}
	else if (gstat > 0 && ! ovrwt)
	{ /* open the group */
		g_id = H5Gopen (l_id, group, H5P_DEFAULT);	
		if (g_id < 0)
		{
			s_msg (0, LOG_ERR, -1,
				"%sCould not open group %s", LOGID, group);
			return -1;
		}
	}
	else
	{ /* create the group */
		if (gstat > 0)
		{ /* delete it first */
			herr_t err = H5Ldelete (l_id, group, H5P_DEFAULT);	
			if (err < 0)
			{
				s_msg (0, LOG_ERR, -1,
					"%sCould not delete group %s",
					LOGID, group);
				return -1;
			}
		}

		g_id = H5Gcreate (l_id, group, H5P_DEFAULT,
			H5P_DEFAULT, H5P_DEFAULT);
		if (g_id < 0)
		{
			s_msg (0, LOG_ERR, -1,
				"%sCould not create group %s", LOGID, group);
			return -1;
		}

	}

	return g_id;
}

/*
 * Open filename, mmap it and close the file descriptor.
 * Saves the address of the mapping in ddesc->buffer (overwriting filename
 * pointer).
 * If ddesc->length < 0 or extends beyond EOF, calculates length and
 * saves it.
 * Returns 0 on success, -1 on error.
 */
static int
s_map_file (struct hdf5_dset_desc_t* ddesc, hid_t g_id)
{
	assert (ddesc != NULL);
	assert (ddesc->filename != NULL);
	assert (ddesc->offset >= 0);
	assert (ddesc->length != 0);

	/* Open the data file. */
	int fd = open (ddesc->filename, O_RDONLY);
	if (fd == -1)
	{
		s_msg (errno, LOG_ERR, -1,
			"%sCould not open data file %s",
			LOGID, ddesc->filename);
		return -1;
	}

	off_t fsize = lseek (fd, 0, SEEK_END);
	if (fsize == (off_t)-1)
	{
		s_msg (errno, LOG_ERR, -1,
			"%sCould not seek to end of file %s",
			LOGID, ddesc->filename);
		close (fd);
		return -1;
	}
	
	/* Check length. */
	if (ddesc->offset >= fsize)
	{ /* nothing to read from file */
		ddesc->length = 0;
		close (fd);
		return 0;
	}

	ssize_t maxlength = fsize - ddesc->offset;
	if (ddesc->length < 0 || ddesc->length > maxlength)
		ddesc->length = maxlength;

	/* mmap from BOF, since mmap requires the offset be a multiple of
	 * page size. */
	assert (ddesc->length > 0);
	void* data = mmap (NULL, ddesc->offset + ddesc->length,
			PROT_READ, MAP_PRIVATE, fd, 0);
	if (data == (void*)-1)
	{
		s_msg (errno, LOG_ERR, -1,
			"%sCould not mmap file %s",
			LOGID, ddesc->filename);
		close (fd);
		return -1;
	}

	/* Store the address. Remember to add offset when reading and
	 * unmap offset+length (though unmapping is not necessary). */
	ddesc->buffer = data;

	close (fd);
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
	assert (ddesc->length >= 0);
	s_msg (0, LOG_DEBUG, -1,
		"%sCreating dataset %s", LOGID, ddesc->dname);

	/* Create the datasets. */
	hsize_t length[1] = {ddesc->length};
	hid_t dspace = H5Screate_simple (1, length, NULL);
	if (dspace < 0)
	{
		s_msg (0, LOG_ERR, -1,
			"%sCould not create dataspace", LOGID);
		return -1;
	}
	hid_t dset = H5Dcreate (g_id, ddesc->dname, DATATYPE,
		dspace, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
	if (dset < 0)
	{
		s_msg (0, LOG_ERR, -1,
			"%sCould not create dataset %s",
			LOGID, ddesc->dname);
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
	assert (ddesc->buffer != (void*)-1);
	herr_t err = H5Dwrite (dset, DATATYPE, H5S_ALL,
		H5S_ALL, H5P_DEFAULT, ddesc->buffer + ddesc->offset);
	if (err < 0)
	{
		s_msg (0, LOG_ERR, -1,
			"%sCould not write to dataset %s",
			LOGID, ddesc->dname);
		H5Dclose (dset);
		H5Sclose (dspace);
		return -1;
	}

	H5Dclose (dset);
	H5Sclose (dspace);

	return 0;
}

static int
s_hdf5_init (void* creq_data_)
{
	assert (creq_data_ != NULL);

	struct s_creq_data_t* creq_data = (struct s_creq_data_t*)creq_data_;
	struct hdf5_conv_req_t* creq = &creq_data->creq;

	/* Check if hdf5 file exists. */
	hid_t f_id;
	int fok = access (creq->filename, F_OK);
	if (fok == 0 && ! creq->ovrwt)
	{ /* open it for writing */
		s_msg (0, LOG_DEBUG, -1,
			"%sOpening existing hdf5 file: %s",
			LOGID, creq->filename);
		f_id = H5Fopen (creq->filename, H5F_ACC_RDWR, H5P_DEFAULT);
	}
	else
	{ /* create it */
		if (fok == -1 && errno != ENOENT)
		{ /* the calling process ensures the filepath should be in an
		   * existing directory (same as the capture files) */
			s_msg (errno, LOG_ERR, -1,
			 	"%sUnexpected error accessing hdf5 file %s",
				LOGID, creq->filename);
			return -1;
		}
		s_msg (0, LOG_DEBUG, -1,
			"%s%s hdf5 file: %s", LOGID,
			(fok ? "Creating" : "Overwriting"), creq->filename);
		f_id = H5Fcreate (creq->filename, H5F_ACC_TRUNC,
			H5P_DEFAULT, H5P_DEFAULT);
	}
	if (f_id < 0)
	{
		s_msg (0, LOG_ERR, -1,
			"%sCould not %s hdf5 file %s", LOGID,
			((fok == 0 && ! creq->ovrwt) ? "open" : "create"),
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
		struct hdf5_dset_desc_t* ddesc = creq->datasets + d;
		if (ddesc == NULL ||
			ddesc->dname == NULL ||
			strlen (ddesc->dname) == 0 ||
			ddesc->filename == NULL ||
			(ddesc->offset < 0 && ddesc->length <=0) )
		{
			s_msg (0, LOG_ERR, -1, "Invalid request");
			H5Gclose (cg_id);
			H5Fclose (f_id);
			return -1;
		}
		if (ddesc->length == 0 ||
			ddesc->offset < 0)
		{
			/* if length is 0, offset doesn't matter; */
			/* if copying from buffer (offset < 0), offset should
			 * be reset to 0 */
			ddesc->offset = 0;
			continue;
		}

		int rc = s_map_file (ddesc, cg_id);
		if (rc != 0)
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

static int
s_hdf5_write (void* creq_data_)
{
	assert (creq_data_ != NULL);

	struct s_creq_data_t* creq_data = (struct s_creq_data_t*)creq_data_;
	/* Create the datasets. */
	int rc = 0;
	for (int d = 0; d < creq_data->creq.num_dsets; d++)
	{
		struct hdf5_dset_desc_t* ddesc =
			creq_data->creq.datasets + d;
		int rc = s_create_dset (ddesc, creq_data->group_id);
		if (rc != 0)
			break;
	}

	/* Close the hdf5 file. */
	H5Gclose (creq_data->group_id);
	H5Fclose (creq_data->file_id);

	return rc;
}

/*
 * Open/create creq->filename, create/overwrite group
 * ROOT_GROUP/creq->group; for each dataset mmap the file if needed and
 * call s_create_dset.
 * Returns 0 on success, -1 on error.
 */
int
hdf5_conv (const struct hdf5_conv_req_t* creq)
{
	if (creq == NULL ||
		creq->filename == NULL ||
		strlen (creq->filename) == 0 ||
		creq->group == NULL ||
		creq->datasets == NULL ||
		creq->num_dsets == 0)
	{
		s_msg (0, LOG_ERR, -1, "Invalid request");
		return -1;
	}

	/* Copy the request so we don't clobber the filename/buffer pointer in
	 * any of the struct hdf5_dset_desc_t's */
	struct s_creq_data_t creq_data;
	memset (&creq_data, 0, sizeof (creq_data));
	memcpy (&creq_data.creq, creq, sizeof (creq_data.creq));

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

	return rc;
}
