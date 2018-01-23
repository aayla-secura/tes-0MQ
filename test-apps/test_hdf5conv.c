#include "hdf5conv.h"
#include "common.h" /* is_verbose, is_daemon */

#define BASEFNAME "/media/data/testcap"
#define H5FNAME "/media/data/test.hdf5"
// #define MEASUREMENT "measurement"
#define MEASUREMENT ""
#define OVRWRT 1
#define ASYNC 1

int main (void)
{
	is_daemon = 1;
	is_verbose = 1;

	uint8_t num_dsets = 2;
	struct hdf5_dset_desc_t dsets[] = {
		{ /* tick stream */
			.filename = BASEFNAME ".tdat",
			.dname = "tick",
			.offset = 2,
			.length = 6,
		},
		{ /* event stream */
			.filename = BASEFNAME ".edat",
			.dname = "event",
		},
	};
	assert (num_dsets == sizeof (dsets) / sizeof (struct hdf5_dset_desc_t));

	struct hdf5_conv_req_t creq = {
		.filename = H5FNAME,
		.group = MEASUREMENT,
		.datasets = dsets,
		.num_dsets = num_dsets,
		.ovrwt = OVRWRT,
		.async = ASYNC,
	};

	int rc = 0;
	if (is_daemon)
		rc = daemonize (NULL);
	if (rc == 0)
		rc = hdf5_conv (&creq);

	return rc;
}
