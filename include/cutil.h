/*
 * Miscellaneous file system and process helpers.
 */

#ifndef __CUTIL_H__INCLUDED__
#define __CUTIL_H__INCLUDED__

#include <sys/types.h>
#include <stdbool.h>
#include <time.h>

#ifndef PATH_MAX
#  ifdef MAXPATHLEN
#    define PATH_MAX MAXPATHLEN
#  else
#    define PATH_MAX 4096
#  endif
#endif
#define NSEC_IN_SEC 1000000000

/*
 * Append a timestamp to name. Result is saved in buf.
 * buf needs to be able to hold PATH_MAX characters (including
 * terminating null byte).
 * Returns 0 on success, -1 on error.
 * On error contents of buf are undefined.
 */
int gen_bkpname (const char* name, char* buf);

/*
 * Start/stop a timer. toc returns nanosecond difference.
 */
void tic (struct timespec* ts);
long long toc (struct timespec* ts);

/*
 * Set the CPU affinity of the calling thread to cpu % <num_cpus - 1>.
 * Will try to detect the number of cpus and fallback to 4 otherwise.
 * Returns 0 on success, -1 on error (the cpu id was not in the
 * affinity set, or other ids were also there).
 */
int pth_set_cpuaff (int cpu);

/*
 * Drop privileges of the current process. It calls setuid and setgid
 * and if the calling process was privileged, makes sure it is not
 * able to regain privileges.
 * Returns 0 on success, -1 on error.
 */
int run_as (uid_t uid, gid_t gid);

/*
 * Prepends root to path and canonicalizes the path via realpath.
 * If root is not given (NULL or empty) and path is relative, root
 * defaults to the current directory.
 * If root is not given (NULL or empty) and path is absolute, root
 * defaults to /.
 * If root is given and is relative, the current directory is
 * prepended.
 *
 * If mustexist is true, path must exist and resolve to a path under
 * root.
 * Otherwise, directory part must resolve to a path under roor (or be
 * root). Any missing directories are created with the given mode.
 *
 * On success saves the result in finalpath, which must be able to
 * hold PATH_MAX characters, and returns a pointer to finalpath.
 * On error, returns NULL.
 *
 * If NULL is returned and errno is 0, it should be because the path
 * is not allowed (i.e. outside of root).
 */
char* canonicalize_path (const char* root, const char* path,
	char* finalpath, bool mustexist, mode_t mode);

/*
 * Recursively create missing directories for path.
 * If path ends with a slash or create_basename is true, the final
 * part is treated as a directory and is created.
 * Otherwise the basename is ignored and the part before the last
 * slash is the final directory created.
 * Returns 0 on success, -1 on error.
 */
int mkdirr (const char* path, mode_t mode, bool create_basename);

#endif
