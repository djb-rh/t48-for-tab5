/* minipro's own command-line front end, compiled in unchanged with its main()
 * renamed so the firmware can call it with an argv. Everything it does on the
 * read/write/verify paths returns; exit() is only reached from --help and
 * argument errors, which the firmware does not send. */
#define main minipro_main
#include "../../third_party/minipro/src/main.c"
