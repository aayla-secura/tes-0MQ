#ifdef linux
/* _GNU_SOURCE needed for pthread_[sg]etaffinity_np and strchrnul */
#  define _GNU_SOURCE
#endif

#include "cutil.h"
#include "daemon_ng.h"

#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <syslog.h>
#include <assert.h>
#include <errno.h>

#define DBG_VERBOSE 1

int
gen_bkpname (const char* name, char* buf)
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

int
mkdirr (const char* path, mode_t mode, bool create_basename)
{
	if (path == NULL || strlen (path) == 0)
		return -1;
	
	logmsg (0, LOG_DEBUG + DBG_VERBOSE,
		"Recursively create '%s'", path);

	/* Start from the root and create directories as needed. */
	char buf[PATH_MAX] = {0};
	const char* cur_seg = path;
	const char* next_seg = NULL;
	size_t len = 0;
	/* Handle both cases of trailing and non-trailing slash. Break when
	 * either the end of the string of the slash preceding the end is
	 * reached. */
	while ( ! (cur_seg[0] == '\0' ||
		( cur_seg[0] == '/' && cur_seg[1] == '\0' )) )
	{
		next_seg = strchrnul (cur_seg + 1, '/');
		if (*next_seg == '\0' && ! create_basename)
			break;
		/* Copy from leading slash of cur_seg to leading slash of next_seg
		 * excluding. */
		size_t thislen = next_seg - cur_seg;
		if (len + thislen >= PATH_MAX)
		{
			logmsg (0, LOG_ERR, "Filename too long");
			errno = ENAMETOOLONG;
			return -1;
		}
		strncpy (buf + len, cur_seg, thislen);
		len += thislen;
		assert (len == strlen (buf));
		assert (len < PATH_MAX);

		/* If link is a dangling link, it will result in EEXIST.
		 * On the next invocation it will be ENOENT. */
		logmsg (0, LOG_DEBUG + DBG_VERBOSE,
			"Checking directory '%s'", buf);
		int rc = mkdir (buf, mode);
		if (rc == 0)
			logmsg (0, LOG_DEBUG, "Created directory '%s'", buf);
		else if (errno != EEXIST && errno != EISDIR)
			return -1; /* don't handle other errors */

		cur_seg = next_seg;
	}
	if (create_basename)
		assert (strlen (cur_seg) == 0 ||
			(strlen (cur_seg) == 1 && cur_seg[0] == '/'));
	return 0;
}

char*
canonicalize_path (const char* root, const char* path,
	char* finalpath, bool mustexist, mode_t mode)
{
	assert (path != NULL);
	assert (finalpath != NULL);

	/*
	 * Make sure realroot starts and ends with a slash.
	 * It must end with a slash for memcmp to determine if inside
	 * realroot.
	 */
	logmsg (0, LOG_DEBUG + DBG_VERBOSE,
		"Canonicalize path '%s' under '%s'",
		path, (root == NULL ? "" : root));
	char buf[PATH_MAX] = {0};
	bool root_given = (root != NULL && strlen (root) > 0);
	if ( (root_given && root[0] != '/') ||
		(path[0] != '/' && ! root_given) )
	{ /* prepend cwd */
		char* rs = getcwd (buf, PATH_MAX);
		if (rs == NULL)
		{
			logmsg (errno, LOG_ERR,
				"Could not get current working directory");
			return NULL;
		}
		assert (buf[0] == '/');
		logmsg (0, LOG_DEBUG + DBG_VERBOSE,
			"Prepending current working directory '%s'", buf);
	}

	/* add given root */
	snprintf (buf + strlen (buf), PATH_MAX - strlen (buf), "/%s",
		root_given ? root : ""); /* root may be NULL */

	/* Canonicalize the root, since we need to know the realpath for
	 * later comparison (to determine if outside of root). */
	if ( ! mustexist && mkdirr (buf, mode, true) == -1)
		return NULL;
	char realroot[PATH_MAX] = {0};
	char* rs = realpath (buf, realroot);
	if (rs == NULL)
	{
		if ( ! mustexist || errno != ENOENT)
			logmsg (errno, LOG_ERR,
				"Could not resolve root");
		return NULL;
	}
	assert (rs == realroot);
	size_t rlen = strlen (realroot);
	if (realroot[rlen - 1] != '/')
	{
		if (rlen == PATH_MAX)
		{
			logmsg (0, LOG_ERR, "Root path too long");
			errno = ENAMETOOLONG;
			return NULL;
		}
		strcpy (realroot + rlen, "/");
		rlen++;
	}
	assert (rlen == strlen (realroot) && rlen > 0 &&
		realroot[0] == '/' && realroot[rlen - 1] == '/');

	/* Add the given path, rlen must remain the length of the root
	 * (including trailing slash) for later comparison. */
	int rc = snprintf (buf, PATH_MAX, "%s%s", realroot, path);
	if (rc >= PATH_MAX)
	{
		logmsg (0, LOG_INFO, "Filename too long");
		errno = ENAMETOOLONG;
		return NULL;
	}
	logmsg (0, LOG_DEBUG + DBG_VERBOSE,
		"Canonicalizing path '%s'", buf);

	/* Check if the file exists first. */
	rs = realpath (buf, finalpath);
	if (rs)
	{
		assert (rs == finalpath);
		if ( memcmp (finalpath, realroot, rlen) != 0 )
		{
			logmsg (0, LOG_DEBUG, "Resolved to '%s', outside of root",
				finalpath);
			return NULL;
		}

		logmsg (0, LOG_DEBUG + DBG_VERBOSE,
			"Final path resolved to '%s'", finalpath);
		return finalpath;
	}

	if (mustexist)
	{
		logmsg (0, LOG_DEBUG, "File doesn't exist");
		return NULL;
	}
	
	/*
	 * We proceed only if some of the directories are missing, i.e.
	 * errno is ENOENT.
	 * errno is ENOTDIR only when a component of the parent path
	 * exists but is not a directory.
	 */
	if (errno != ENOENT)
		return NULL;

	/* Create missing directories (minus basename). */
	char dirbuf[PATH_MAX] = {0};
	char* basename = strrchr (buf, '/');
	basename++;
	snprintf (dirbuf, basename - buf, "%s", buf);
	if (mkdirr (dirbuf, mode, true) == -1)
		return NULL;

	/* Canonicalize the directory path. The file doesn't exist (checked
	 * above), so would error here. */
	rs = realpath (dirbuf, finalpath);
	if (rs == NULL)
		return NULL;
	
	size_t len = strlen (finalpath);
	if (finalpath[len - 1] != '/')
		len += snprintf (finalpath + len, PATH_MAX - len, "/");
	if ( memcmp (finalpath, realroot, rlen) != 0 )
	{
		logmsg (0, LOG_DEBUG,
			"Directory part resolved to %s, outside of root",
			finalpath);
		return NULL;
	}
	assert (len == strlen (finalpath) && finalpath[len - 1] == '/');
	
	/* Add back the basename. */
	len += snprintf (finalpath + len, PATH_MAX - len, "%s", basename);
	if (len >= PATH_MAX)
	{
		logmsg (0, LOG_INFO, "Filename too long");
		errno = ENAMETOOLONG;
		return NULL;
	}

	logmsg (0, LOG_DEBUG + DBG_VERBOSE,
		"Final path resolved to '%s'", finalpath);
	return finalpath;
}
