#include "cutil.h"
#include "daemon_ng.h"
#include <stdio.h>

int main (int argc, char *argv[])
{
	set_verbose (true);
	if (argc != 6)
		return -1;

	int rc = mkdirr (argv[1], 0700, false);
	if (rc == 0)
		rc = mkdirr (argv[2], 0700, true);
	if (rc != 0)
	{
		perror ("mkdir");
		return -1;
	}

	char buf[PATH_MAX] = {0};
	char* rs = canonicalize_path (NULL, argv[3], buf, false, 0777);
	if (rs != NULL)
		rs = canonicalize_path (argv[4], argv[5], buf, false, 0777);
	if (rs == NULL)
		perror ("");
	return (rs == NULL ? -1 : 0);
}
