/*
 * Miscellaneous file system and process helpers.
 */

#ifndef __CUTIL_H__INCLUDED__
#define __CUTIL_H__INCLUDED__

#include <sys/types.h>
#include <stdbool.h>

#ifndef PATH_MAX
#  ifdef MAXPATHLEN
#    define PATH_MAX MAXPATHLEN
#  else
#    define PATH_MAX 4096
#  endif
#endif

/*
 * Set the CPU affinity of the calling thread to cpu % <num_cpus - 1>.
 * Will try to detect the number of cpus and fallback to 4 otherwise.
 * Returns 0 on success, -1 on error (the cpu id was not in the
 * affinity set, or other ids were also there).
 */
int pth_set_cpuaff (int cpu);

/*
 * Prepends root to path and canonicalizes the path via realpath.
 * If root is NULL it defaults to '/'. A leading slash is prepended if
 * missing.
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

#endif
