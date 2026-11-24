#include <unistd.h>
#include <stdio.h>
#include <mint/osbind.h>

/* libcmini keeps this much stack for us while MIWA.PRG runs (its default
 * cost the game's sprite arena ~18KB, and the credits their 115KB sprite) */
long _stksize = 2048;

void reset(void) {
  (* (void (**)(void)) 0x4)();
}

int main(int argc, char *argv[]) {
  int ret = chdir("\\MIWA");
  char cmdline[] = "";
  /* NULL: inherit our environment; "" was not double-NUL terminated */
  Pexec(0, "MIWA.PRG", cmdline, NULL);
  Supexec(reset);
}
