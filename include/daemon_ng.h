#ifndef __DAEMON_NG_H_INCLUDED__
#define __DAEMON_NG_H_INCLUDED__

#define DAEMON_OK_MSG "0"
#define DAEMON_ERR_MSG "1"

int daemonize_noexit (const char*);
int daemonize (const char*);

#endif
