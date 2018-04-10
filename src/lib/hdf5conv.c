#include "hdf5conv.h"
#include "daemon_ng.h"

#ifdef linux
// is it there only on Debian?
#  include <hdf5/serial/hdf5.h>
#else
#  include <hdf5.h>
#endif

#include <string.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <syslog.h>
#include <errno.h>
#include <assert.h>

#define INIT_TIMEOUT 5 /* in seconds */
#define ROOT_GROUP  "capture"     /* relative to file */
#define OVRWT_GROUP "overwritten" /* relative to file */
#define NODELETE_TMP /* don't delete data files */
#define DATATYPE H5T_NATIVE_UINT_LEAST8
/* #define DATATYPE H5T_NATIVE_UINT_FAST8 */
/* #define DATATYPE H5T_NATIVE_UINT8 */

static int s_gen_bkpname (const char* name, char* buf);
static int s_del_grp (hid_t lid, const char* group);
static int s_grp_exists (hid_t lid, const char* group);
static hid_t s_open_grp (hid_t lid, const char* group,
	hid_t bkp_lid, bool excl, bool discard);
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
 * TODO: move to cutil
 * Append a timestamp. buf needs to be able to hold PATH_MAX
 * characters (including terminating null byte).
 * Returns 0 on success, -1 on error.
 * On error contents of buf are undefined.
 */
static int s_gen_bkpname (const char* name, char* buf)
{
	int rc = snprintf (buf, PATH_MAX, "%s_%lu", name, time (NULL));
	if (rc >= PATH_MAX)
	{
		logmsg (0, LOG_ERR,
				"Filename too long, cannot append timestamp");
		return -1;
	}
	else if (rc < 0)
	{
		logmsg (errno, LOG_ERR,
				"Cannot write to stack buffer");
		return -1;
	}
	return 0;
}

/*
 * Delete group. Checks to make sure the link is gone.
 * Return 0 on success, -1 on error.
 */
static int
s_del_grp (hid_t lid, const char* group)
{
	herr_t err = H5Ldelete (lid, group, H5P_DEFAULT);

	/* Make sure the old link is gone. */
	if (err >= 0 && s_grp_exists (lid, group) != 0)
		err = -1;

	if (err < 0)
	{
		logmsg (0, LOG_ERR,
				"Cannot delete group %s", group);
		return -1;
	}

	logmsg (0, LOG_DEBUG,
		"Deleted group %s", group);
	return 0;
}

/*
 * Returns 0 if group doesn't exist, 1 if it exists, -1 on error.
 */
static int
s_grp_exists (hid_t lid, const char* group)
{
	htri_t gstat = H5Lexists (lid, group, H5P_DEFAULT);
	if (gstat < 0)
	{
		logmsg (0, LOG_ERR,
			"Could not check if group %s exists", group);
		return -1;
	}
	return (gstat == 0 ? 0 : 1);
}

/*
 * Open or create <group> relative to lid.
 * The flags excl and discard have the following meaning:
 *
 *  excl   discard   action if existing   action if not existing
 *    F       F             open                create
 *    T       F             open                 skip
 *    F       T           overwrite             create
 *    T       T             skip                create
 *
 * bkp_lid is used only if group exists and overwriting (excl = F,
 * discard = T).
 *
 * Returns positive group id on success.
 * Returns 0 if nothing done (skip).
 * Returns -1 on error.
 */
static hid_t
s_open_grp (hid_t lid, const char* group, hid_t bkp_lid,
	bool excl, bool discard)
{
	assert (group != NULL);
	assert (lid > 0);

	/* Check if group exists. */
	int rc = s_grp_exists (lid, group);
	if (rc == -1)
		return -1;
	bool exists = (rc > 0);

	if ( excl && ! (discard ^ exists) )
	{
		logmsg (0, LOG_INFO, "Group %s %s",
			group, exists ? "exists" : "doesn't exist");
		return 0;
	}

	if (exists && discard)
	{
		if (bkp_lid > 0)
		{ /* rename the group */
			char bkpgroup[PATH_MAX] = {0};
			if (s_gen_bkpname (group, bkpgroup) == -1)
				return -1;

			/* FIXME: make sure backup destination does not exist? */

			/* Copy, then delete to prevent data loss. */
			herr_t err = H5Lcopy (lid, group, bkp_lid, bkpgroup,
				H5P_DEFAULT, H5P_DEFAULT);
			if (err < 0)
			{
				logmsg (0, LOG_ERR,
					"Cannot move group %s to %s", group, bkpgroup);
				return -1;
			}

			logmsg (0, LOG_DEBUG,
				"Renamed group %s to %s", group, bkpgroup);
		}

		/* Delete the group. */
		if (s_del_grp (lid, group) == -1)
			return -1;

		exists = false;
	}

	hid_t gid = -1;
	logmsg (0, LOG_DEBUG,
		"%s group %s", exists ? "Opening" : "Creating", group);

	if (exists)
		gid = H5Gopen (lid, group, H5P_DEFAULT);
	else
		gid = H5Gcreate (lid, group, H5P_DEFAULT,
			H5P_DEFAULT, H5P_DEFAULT);

	if (gid < 0)
	{
		logmsg (0, LOG_ERR,
			"Could not %s group %s",
			(exists) ? "open" : "create", group);
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
 * Returns HDF5CONV_REQ_*
 */
static int
s_map_file (struct hdf5_dset_desc_t* ddesc, hid_t gid)
{
	// sleep (1);
	assert (ddesc != NULL);
	assert (ddesc->filename != NULL);
	assert (ddesc->buffer == NULL);
	
	if (ddesc->length == 0)
		return HDF5CONV_REQ_OK;

	/* Open the data file. */
	int fd = open (ddesc->filename, O_RDONLY);
	if (fd == -1)
	{
		logmsg (errno, LOG_ERR,
			"Could not open data file %s",
			ddesc->filename);
		return HDF5CONV_REQ_EINIT;
	}

	/* Get its size. */
	off_t fsize = lseek (fd, 0, SEEK_END);
	if (fsize == (off_t)-1)
	{
		logmsg (errno, LOG_ERR,
			"Could not seek to end of file %s",
			ddesc->filename);
		close (fd);
		return HDF5CONV_REQ_EINIT;
	}
	
	/* Check offset. */
	if (ddesc->offset < 0)
		ddesc->offset += fsize;
	if (fsize == 0 || ddesc->offset >= fsize || ddesc->offset < 0)
	{ /* file empty or abs(offset) too large */
		ddesc->length = 0;
		close (fd);
		return HDF5CONV_REQ_OK;
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
		return HDF5CONV_REQ_EINIT;
	}

	/* Store the address. Remember to add offset when reading and
	 * unmap offset+length (though unmapping is not necessary). */
	ddesc->buffer = data;

	return HDF5CONV_REQ_OK;
}

/*
 * Write data given in ddesc as a dataset inside group gid.
 * Returns HDF5CONV_REQ_*
 */
static int
s_create_dset (const struct hdf5_dset_desc_t* ddesc, hid_t gid)
{
	// sleep (1);
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
		return HDF5CONV_REQ_ECONV;
	}
	hid_t dset = H5Dcreate (gid, ddesc->dsetname, DATATYPE,
		dspace, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
	if (dset < 0)
	{
		logmsg (0, LOG_ERR,
			"Could not create dataset %s",
			ddesc->dsetname);
		H5Sclose (dspace);
		return HDF5CONV_REQ_ECONV;
	}

	/* Check if dataset is empty. */
	if (ddesc->length == 0)
	{
		H5Dclose (dset);
		H5Sclose (dspace);
		return HDF5CONV_REQ_OK;
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

	return (err < 0 ? HDF5CONV_REQ_ECONV : HDF5CONV_REQ_OK);
}

/*
 * Open/create the hdf5 file, open/create the groups.
 * Open all dataset files and mmap them (calls s_map_file).
 * On success, save the file and dataset group ids in creq_data_.
 * Returns HDF5CONV_REQ_*
 */
static int
s_hdf5_init (void* creq_data_)
{
	assert (creq_data_ != NULL);

	struct s_creq_data_t* creq_data =
		(struct s_creq_data_t*)creq_data_;
	struct hdf5_conv_req_t* creq = creq_data->creq;
	int rc = HDF5CONV_REQ_EINIT; /* default error */
	hid_t fid = -1; /* hdf5 file */
	hid_t root_gid = -1;   /* ROOT_GRP */
	hid_t ovrwt_gid = -1;  /* OVRWT_GROUP */
	hid_t client_gid = -1; /* creq->group */

	/* Check if hdf5 file exists. */
	int fok = access (creq->filename, F_OK);
	if (fok == 0 && creq->use_existing)
	{ /* open it for writing */
		logmsg (0, LOG_DEBUG,
			"Opening existing hdf5 file: %s", creq->filename);
		fid = H5Fopen (creq->filename, H5F_ACC_RDWR, H5P_DEFAULT);
	}
	else
	{ /* create it */
		if (fok == 0)
		{
			if ( ! creq->overwrite )
			{ /* abort */
				logmsg (0, LOG_INFO,
					"Not overwriting existing hdf5 file");
				rc = HDF5CONV_REQ_EABORT;
				goto err;
			}
			else if (creq->backup)
			{ /* rename it */
				char tmpfname[PATH_MAX] = {0};
				if (s_gen_bkpname (creq->filename, tmpfname) == -1)
					goto err;

				if (rename (creq->filename, tmpfname) == -1)
				{
					logmsg (errno, LOG_ERR,
						"Cannot rename hdf5 file %s", creq->filename);
					goto err;
				}
				logmsg (0, LOG_DEBUG,
					"Renamed hdf5 file %s to %s", creq->filename, tmpfname);
			}
			else
			{ /* unlink it */
				if (unlink (creq->filename) == -1)
				{
					logmsg (errno, LOG_ERR,
						"Cannot delete hdf5 file %s", creq->filename);
					goto err;
				}
				logmsg (0, LOG_DEBUG,
					"Deleted hdf5 file %s", creq->filename);
			}
		}

		logmsg (0, LOG_DEBUG,
			"Creating hdf5 file: %s", creq->filename);
		fid = H5Fcreate (creq->filename, H5F_ACC_TRUNC,
			H5P_DEFAULT, H5P_DEFAULT);
	}

	if (fid < 0)
	{
		logmsg (0, LOG_ERR,
			"Could not %s hdf5 file %s",
			(fok == 0 && creq->use_existing) ? "open" : "create",
			creq->filename);
		goto err;
	}

	/* Open the root group. */
	root_gid = s_open_grp (fid, ROOT_GROUP, -1, false, false);
	if (root_gid < 0)
		goto err;
	assert (root_gid > 0);

	if (creq->use_existing && creq->overwrite && creq->backup)
	{ /* open the overwrite group, in case client group exists */
		ovrwt_gid = s_open_grp (fid, OVRWT_GROUP, -1, false, false);
		if (ovrwt_gid < 0)
			goto err;
		assert (ovrwt_gid > 0);
	}

	/* (Re-)create the client requested subgroup. */
	client_gid = s_open_grp (root_gid, creq->group, ovrwt_gid,
		! creq->overwrite, true);
	if (client_gid <= 0)
	{
		rc = (client_gid == 0 ?
			HDF5CONV_REQ_EABORT : HDF5CONV_REQ_EINIT);
		goto err;
	}

	/* mmap files. */
	for (size_t d = 0; d < creq->num_dsets; d++)
	{
		struct hdf5_dset_desc_t* ddesc = &creq->dsets[d];
		if (ddesc->filename == NULL)
			continue;

		int rc = s_map_file (ddesc, client_gid);
		if (rc != HDF5CONV_REQ_OK)
			goto err;
	}

	/* Save the file and group id. */
	creq_data->file_id = fid;
	creq_data->group_id = client_gid;
	rc = HDF5CONV_REQ_OK;
err:
	if (root_gid > 0)
		H5Gclose (root_gid);
	if (ovrwt_gid > 0)
		H5Gclose (ovrwt_gid);
	if (rc != HDF5CONV_REQ_OK)
	{
		if (client_gid > 0)
			H5Gclose (client_gid);
		if (fid > 0)
			H5Fclose (fid);
	}
	return rc;
}

/*
 * Create all datasets (calls s_create_dset).
 * Returns HDF5CONV_REQ_*
 */
static int
s_hdf5_write (void* creq_data_)
{
	assert (creq_data_ != NULL);

	struct s_creq_data_t* creq_data =
		(struct s_creq_data_t*)creq_data_;

	/* Create the datasets. */
	int rc = 0;
	for (size_t d = 0; d < creq_data->creq->num_dsets; d++)
	{
		struct hdf5_dset_desc_t* ddesc =
			&creq_data->creq->dsets[d];
		int rc = s_create_dset (ddesc, creq_data->group_id);
		if (rc != HDF5CONV_REQ_OK)
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
		creq->num_dsets == 0)
	{
		logmsg (0, LOG_ERR, "Invalid request");
		return HDF5CONV_REQ_EINV;
	}
	for (size_t d = 0; d < creq->num_dsets; d++)
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
			return HDF5CONV_REQ_EINV;
		}
	}

	struct s_creq_data_t creq_data = {
		.creq = creq,
		.group_id = -1,
		.file_id = -1,
		};

	/* If operating in asynchronous mode, fork here before opening
	 * the files and signal parent before starting copy. */
	int status = HDF5CONV_REQ_OK;
	if (creq->async)
	{
		int rc = fork_and_run (s_hdf5_init, s_hdf5_write,
			&creq_data, INIT_TIMEOUT);
		if (rc == -1)
			status = HDF5CONV_REQ_EINIT;
	}
	else
	{
		status = s_hdf5_init (&creq_data);
		if (status == HDF5CONV_REQ_OK)
			status = s_hdf5_write (&creq_data);
	}

	/* Unmap data and unlink files. */
	for (size_t d = 0; d < creq->num_dsets; d++)
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
		if (status == HDF5CONV_REQ_OK)
		{
			int rc = unlink (ddesc->filename)
			if (rc == -1)
				status = HDF5CONV_REQ_EFIN;
		}
#endif
	}

	/* Unlink files. */
	return status;
}
