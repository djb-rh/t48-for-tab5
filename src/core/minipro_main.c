/* minipro's own command-line front end, compiled in unchanged with its main()
 * renamed so the firmware can call it with an argv.
 *
 * Its --help, -d (chip info), -l and argument-error paths end in exit(),
 * which on the Tab5 aborts and restarts the board; "Chip info" took that path.
 * exit() is caught here instead: it jumps back out of minipro_main() with the
 * exit code, as if main() had returned it. (Anything minipro had open on that
 * path is left as it was; those paths open little.) */
#include <setjmp.h>
#include <stdlib.h>

static jmp_buf minipro_exit_jmp;
static int minipro_exit_code;

__attribute__((noreturn)) void minipro_exit(int code) {
	minipro_exit_code = code;
	longjmp(minipro_exit_jmp, 1);
}

#define exit minipro_exit
#define main minipro_main_body
#include "../../third_party/minipro/src/main.c"
#undef main
#undef exit

int minipro_main(int argc, char **argv) {
	if (setjmp(minipro_exit_jmp)) return minipro_exit_code;
	return minipro_main_body(argc, argv);
}
