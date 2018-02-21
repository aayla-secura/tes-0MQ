/*
 * TO DO:
 *  - delete data files after conversion
 */

#include "hdf5conv.h"
#include "api.h"
#include "daemon_ng.h"

#ifdef linux
// is it there only on Debian?
#  include <hdf5/serial/hdf5.h>
#else
#  include <hdf5.h>
#endif

#include <time.h>
#include <sys/mman.h>
#include <errno.h>
#include <assert.h>

#define INIT_TIMEOUT 5 /* in seconds */
#define ROOT_GROUP  "capture"     /* relative to file */
#define OVRWT_GROUP "overwritten" /* relative to file */
#define NODELETE_TMP
#define DATATYPE H5T_NATIVE_UINT_LEAST8
/* #define DATATYPE H5T_NATIVE_UINT_FAST8 */
/* #define DATATYPE H5T_NATIVE_UINT8 */

static hid_t s_get_grp (hid_t lid, const char* group, bool create);
static hid_t s_crt_grp (hid_t lid, const char* group,
	hid_t bkp_lid, const char* bkpgroup);
static int s_map_file (struct hdf5_dset_desc_t* ddesc, hid_t gid);
static int s_create_dset (const struct hdf5_dset_desc_t* ddesc,
		hid_t gid);
static int s_hdf5_init   (void* creq_data_);
static int s_hdf5_write  (void* creq_data_);

struct s_creq_data_t
{
	struct hdf5_conv_req_t* creq;
	hid_t group_id;
	hid_t file_id;
};

/* -------------------------------------------------------------- */
/* --------------------------- HELPERS -------------------------- */
/* -------------------------------------------------------------- */

/*
 * Open or create <group> relative to lid.
 * If create is true and group does not exists, it is created.
 * If create is false and group does not exists, we return 0.
 * Returns positive group id on success, 0 if nothing done, -1 on
 * error.
 */
static hid_t
s_get_grp (hid_t lid, const char* group, bool create)
{
	assert (group != NULL);
	assert (lid > 0);

	/* Check if group exists. */
	htri_t gstat = H5Lexists (lid, group, H5P_DEFAULT);
	if (gstat < 0)
	{
		logmsg (0, LOG_ERR,
			"Could not check if group %s exists",
			group);
		return -1;
	}
	else if (gstat == 0 && ! create)
	{ /* nothing to do */
		logmsg (0, LOG_INFO,
			"Group %s does not exist", group);
		return 0;
	}

	/* Open or create it. */
	hid_t gid = -1;

	logmsg (0, LOG_DEBUG,
		"%s group %s",
		(gstat > 0) ? "Opening" : "Creating",
		group);

	if (gstat > 0)
		gid = H5Gopen (lid, group, H5P_DEFAULT);    
	else
		gid = H5Gcreate (lid, group, H5P_DEFAULT,
			H5P_DEFAULT, H5P_DEFAULT);

	if (gid < 0)
	{
		logmsg (0, LOG_ERR,
			"Could not %s group %s",
			(gstat > 0) ? "open" : "create",
			group);
		return -1;
	}

	return gid;
}

/*
 * Create <group> relative to lid.
 * If bkp_lid > 0 and group exists, it is renamed to bkpgroup
 * under bkp_lid.
 * If bkp_lid <= 0 and group exists, we return 0.
 * Returns positive group id on success, 0 if nothing done, -1 on
 * error.
 */
static hid_t
s_crt_grp (hid_t lid, const char* group, hid_t bkp_lid,
	const char* bkpgroup)
{
	assert (group != NULL);
	assert (lid > 0);

	/* Check if group exists. */
	hid_t gid = s_get_grp (lid, group, 0);
	if (gid < 0)
		return -1;

	bool exists = 0;
	if (gid > 0)
	{
		exists = 1;
		H5Gclose (gid); /* we don't need it */
	}

	if (exists)
	{
		if (bkp_lid <= 0)
		{ /* no backup given */
			logmsg (0, LOG_INFO,
				"Group %s exists", group);
			return 0;
		}

		/* Rename the group. */
		assert (strlen (bkpgroup) > 0);

		logmsg (0, LOG_DEBUG,
			"Renaming group %s to %s",
			group, bkpgroup);

		herr_t err = 0;
#ifdef ENABLE_FULL_DEBUG
		/* Make sure backup destination does not exist. */
		gid = s_get_grp (bkp_lid, bkpgroup, 0);
		if (gid > 0);
		{
			H5Gclose (gid);
			err = -1;
		}
#endif
		/* Copy, then delete to prevent data loss. */
		if (err >= 0)
			err = H5Lcopy (lid, group, bkp_lid, bkpgroup,
				H5P_DEFAULT, H5P_DEFAULT);
		if (err >= 0)
			err = H5Ldelete (lid, group, H5P_DEFAULT);
#ifdef ENABLE_FULL_DEBUG
		/* Make sure the old link is gone. */
		gid = s_get_grp (lid, group, 0);
		if (gid > 0);
		{
			H5Gclose (gid);
			err = -1;
		}
#endif
		if (err < 0)
		{
			logmsg (0, LOG_ERR,
				"Cannot move group %s to %s",
				group, bkpgroup);
			return -1;
		}
	}

	/* Create the group. */
	gid = H5Gcreate (lid, group, H5P_DEFAULT,
		H5P_DEFAULT, H5P_DEFAULT);

	if (gid < 0)
	{
		logmsg (0, LOG_ERR,
			"Could not create group %s",
			group);
		return -1;
	}

	return gid;
}

/*
 * Open dataset file, mmap it and close the file descriptor.
 * Saves the address of the mapping in ddesc->buffer.
 * If ddesc->offset < 0 (refers to EOF), recalculates offset w.r.t.
 * BOF and saves it.
 * If ddesc->length < 0 or extends beyond EOF, calculates length and
 * saves it.
 * If length == 0 or offset extends beyond EOF, ddesc->buffer will
 * be NULL.
 * On success buffer may be NULL if dataset should be empty, in
 * which case length is ensured to be 0. Otherwise length is ensured
 * to be positive and offset---to be non-negative.
 * Returns TES_CAP_REQ_*
 */
static int
s_map_file (struct hdf5_dset_desc_t* ddesc, hid_t gid)
{
#ifdef ENABLE_FULL_DEBUG
	/* sleep (1); */
#endif
	assert (ddesc != NULL);
	assert (ddesc->filename != NULL);
	assert (ddesc->buffer == NULL);
	
	if (ddesc->length == 0)
		return TES_CAP_REQ_OK;

	/* Open the data file. */
	int fd = open (ddesc->filename, O_RDONLY);
	if (fd == -1)
	{
		logmsg (errno, LOG_ERR,
			"Could not open data file %s",
			ddesc->filename);
		return TES_CAP_REQ_EFAIL;
	}

	/* Get its size. */
	off_t fsize = lseek (fd, 0, SEEK_END);
	if (fsize == (off_t)-1)
	{
		logmsg (errno, LOG_ERR,
			"Could not seek to end of file %s",
			ddesc->filename);
		close (fd);
		return TES_CAP_REQ_EFAIL;
	}
	
	/* Check offset. */
	if (ddesc->offset < 0)
		ddesc->offset += fsize;
	if (fsize == 0 || ddesc->offset >= fsize || ddesc->offset < 0)
	{ /* file empty or abs(offset) too large */
		ddesc->length = 0;
		close (fd);
		return TES_CAP_REQ_OK;
	}

	assert (ddesc->offset >= 0);
	assert (fsize > 0);
	ssize_t maxlength = fsize - ddesc->offset;
	assert (maxlength > 0);
	if (ddesc->length < 0 || ddesc->length > maxlength)
		ddesc->length = maxlength;

	/* mmap from BOF, since mmap requires the offset be a multiple
	 * of page size. */
	assert (ddesc->length > 0);
	void* data = mmap (NULL, ddesc->offset + ddesc->length,
		PROT_READ, MAP_PRIVATE, fd, 0);
	close (fd);
	if (data == (void*)-1)
	{
		logmsg (errno, LOG_ERR,
			"Could not mmap file %s",
			ddesc->filename);
		return TES_CAP_REQ_EFAIL;
	}

	/* Store the address. Remember to add offset when reading and
	 * unmap offset+length (though unmapping is not necessary). */
	ddesc->buffer = data;

	return TES_CAP_REQ_OK;
}

/*
 * Write data given in ddesc as a dataset inside group gid.
 * Returns TES_CAP_REQ_*
 */
static int
s_create_dset (const struct hdf5_dset_desc_t* ddesc, hid_t gid)
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
		return TES_CAP_REQ_ECONV;
	}
	hid_t dset = H5Dcreate (gid, ddesc->dsetname, DATATYPE,
		dspace, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
	if (dset < 0)
	{
		logmsg (0, LOG_ERR,
			"Could not create dataset %s",
			ddesc->dsetname);
		H5Sclose (dspace);
		return TES_CAP_REQ_ECONV;
	}

	/* Check if dataset is empty. */
	if (ddesc->length == 0)
	{
		H5Dclose (dset);
		H5Sclose (dspace);
		return TES_CAP_REQ_OK;
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

	return (err < 0 ? TES_CAP_REQ_ECONV : TES_CAP_REQ_OK);
}

/*
 * Open the hdf5 file, create the groups.
 * Open all dataset files and mmap them (calls s_map_file).
 * On success, save the file and dataset group ids in creq_data_.
 * Returns TES_CAP_REQ_*
 */
static int
s_hdf5_init (void* creq_data_)
{
	assert (creq_data_ != NULL);

	struct s_creq_data_t* creq_data =
		(struct s_creq_data_t*)creq_data_;
	struct hdf5_conv_req_t* creq = creq_data->creq;
	bool ovrwt = (creq->ovrwtmode == TES_H5_OVRWT_FILE);

	/* Check if hdf5 file exists. */
	hid_t fid;
	int fok = access (creq->filename, F_OK);
	if (fok == 0 && ! ovrwt)
	{ /* open it for writing */
		logmsg (0, LOG_DEBUG,
			"Opening existing hdf5 file: %s",
			creq->filename);
		fid = H5Fopen (creq->filename, H5F_ACC_RDWR, H5P_DEFAULT);
	}
	else
	{ /* create/overwrite it */
		if (fok == 0)
		{ /* unlink it first */
			assert (ovrwt);
			int rc = unlink (creq->filename);
			if (rc == -1)
			{
				logmsg (errno, LOG_ERR,
					"Cannot delete hdf5 file %s",
					creq->filename);
				return TES_CAP_REQ_EFAIL;
			}
		}
		logmsg (0, LOG_DEBUG,
			"%s hdf5 file: %s",
			(fok ? "Creating" : "Overwriting"),
			creq->filename);
		fid = H5Fcreate (creq->filename, H5F_ACC_TRUNC,
			H5P_DEFAULT, H5P_DEFAULT);
	}
	if (fid < 0)
	{
		logmsg (0, LOG_ERR,
			"Could not %s hdf5 file %s",
			(fok == 0 && ! ovrwt) ? "open" : "create",
			creq->filename);
		return TES_CAP_REQ_EFAIL;
	}

	/* Open the root group. */
	hid_t root_gid = s_get_grp (fid, ROOT_GROUP, 1);
	if (root_gid < 0)
	{
		H5Fclose (fid);
		return TES_CAP_REQ_EFAIL;
	}
	assert (root_gid > 0);

	hid_t ovrwt_gid = -1;
	char bkpgroup[PATH_MAX] = {0};
	if (creq->ovrwtmode == TES_H5_OVRWT_RELINK)
	{
		/* Open the overwrite group. */
		ovrwt_gid = s_get_grp (fid, OVRWT_GROUP, 1);
		if (ovrwt_gid < 0)
		{
			H5Gclose (root_gid);
			H5Fclose (fid);
			return TES_CAP_REQ_EFAIL;
		}
		assert (ovrwt_gid > 0);

		/* Generate a name. */
		time_t tnow = time (NULL);
		if (tnow == (time_t)-1)
		{
			logmsg (0, LOG_ERR, "Could not get current time");
			H5Gclose (root_gid);
			H5Gclose (ovrwt_gid);
			H5Fclose (fid);
			return TES_CAP_REQ_EFAIL;
		}
		int rc = snprintf (bkpgroup, PATH_MAX, "%s_%lu",
			creq->group, tnow);
		if (rc < 0 || rc >= PATH_MAX)
		{
			if (rc >= PATH_MAX)
				logmsg (0, LOG_ERR,
					"Group name too long, cannot append timestamp");
			else
				logmsg (errno, LOG_ERR,
					"Cannot write to stack buffer");
			H5Gclose (root_gid);
			H5Gclose (ovrwt_gid);
			H5Fclose (fid);
			return TES_CAP_REQ_EFAIL;
		}
	}

	/* Create the client requested subgroup. */
	hid_t client_gid;
	client_gid = s_crt_grp (root_gid, creq->group,
		ovrwt_gid, bkpgroup);
	H5Gclose (root_gid); /* not needed anymore */
	if (ovrwt_gid > 0)
		H5Gclose (ovrwt_gid); /* not needed anymore */
	if (client_gid <= 0)
	{
		H5Fclose (fid);
		return (client_gid == 0 ? TES_CAP_REQ_EABORT : TES_CAP_REQ_EFAIL);
	}
	assert (client_gid > 0);

	/* mmap files. */
	for (int d = 0; d < creq->num_dsets; d++)
	{
		struct hdf5_dset_desc_t* ddesc = &creq->dsets[d];
		if (ddesc->filename == NULL)
			continue;

		int rc = s_map_file (ddesc, client_gid);
		if (rc != TES_CAP_REQ_OK)
		{
			H5Gclose (client_gid);
			H5Fclose (fid);
			return rc;
		}
	}

	/* Save the file and group id. */
	creq_data->file_id = fid;
	creq_data->group_id = client_gid;
	return TES_CAP_REQ_OK;
}

/*
 * Create all datasets (calls s_create_dset).
 * Returns TES_CAP_REQ_*
 */
static int
s_hdf5_write (void* creq_data_)
{
	assert (creq_data_ != NULL);

	struct s_creq_data_t* creq_data =
		(struct s_creq_data_t*)creq_data_;

	/* Create the datasets. */
	int rc = 0;
	for (int d = 0; d < creq_data->creq->num_dsets; d++)
	{
		struct hdf5_dset_desc_t* ddesc =
			&creq_data->creq->dsets[d];
		int rc = s_create_dset (ddesc, creq_data->group_id);
		if (rc != TES_CAP_REQ_OK)
			break;
	}

	/* Close the hdf5 file. */
	H5Gclose (creq_data->group_id);
	H5Fclose (creq_data->file_id);

	return rc;
}

/* -------------------------------------------------------------- */
/* ----------------------------- API ---------------------------- */
/* -------------------------------------------------------------- */

int
hdf5_conv (struct hdf5_conv_req_t* creq)
{
	if (creq == NULL ||
		creq->filename == NULL ||
		strlen (creq->filename) == 0 ||
		creq->group == NULL ||
		strlen (creq->group) == 0 ||
		creq->dsets == NULL ||
		creq->ovrwtmode > 2 ||
		creq->num_dsets == 0)
	{
		logmsg (0, LOG_ERR, "Invalid request");
		return TES_CAP_REQ_EINV;
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
			return TES_CAP_REQ_EINV;
		}
	}

	struct s_creq_data_t creq_data = {
		.creq = creq,
		.group_id = -1,
		.file_id = -1,
		};

	/* If operating in asynchronous mode, fork here before opening
	 * the files and signal parent before starting copy. */
	int status = TES_CAP_REQ_OK;
	if (creq->async)
	{
		int rc = fork_and_run (s_hdf5_init, s_hdf5_write,
			&creq_data, INIT_TIMEOUT);
		if (rc == -1)
			status = TES_CAP_REQ_EFAIL;
	}
	else
	{
		status = s_hdf5_init (&creq_data);
		if (status == TES_CAP_REQ_OK)
			status = s_hdf5_write (&creq_data);
	}

	/* Unmap data and unlink files. */
	for (int d = 0; d < creq->num_dsets; d++)
	{
		struct hdf5_dset_desc_t* ddesc = &creq->dsets[d];
		if (ddesc->filename == NULL)
			continue;

		/* mmapped by us */
		if (ddesc->buffer != NULL)
		{
			munmap (ddesc->buffer,
				ddesc->offset + ddesc->length);
			ddesc->buffer = NULL;
		}
#ifndef NODELETE_TMP
		if (status == TES_CAP_REQ_OK)
		{
			int rc = unlink (ddesc->filename)
			if (rc == -1)
				status = TES_CAP_REQ_EFIN;
		}
#endif
	}

	/* Unlink files. */
	return status;
}
