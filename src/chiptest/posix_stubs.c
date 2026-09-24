/* POSIX calls minipro's main.c links against but only uses in paths the
 * firmware never takes: the pager behind the device list (popen, dup2,
 * signal) and the program name in --help (basename). */
#include <signal.h>
#include <stdio.h>

_sig_func_ptr signal(int sig, _sig_func_ptr fn) { (void)sig; (void)fn; return SIG_DFL; }
FILE *popen(const char *cmd, const char *mode) { (void)cmd; (void)mode; return NULL; }
int pclose(FILE *f) { (void)f; return -1; }
int dup2(int a, int b) { (void)a; (void)b; return -1; }
/* No <string.h> here: its GNU basename() has a different signature. */
char *basename(char *path) {
	char *last = path;
	for (char *p = path; *p; p++)
		if (*p == '/')
			last = p + 1;
	return last;
}
