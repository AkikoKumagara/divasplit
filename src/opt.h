#ifndef DIVASPLIT_OPT_H
#define DIVASPLIT_OPT_H

#include <psp2/types.h>
#include <stdint.h>

// engine optimizations (opt.c); *on switches them at run time
int opt_init(SceUID modid, uint32_t matmul_off, volatile int *on);
void opt_shutdown(void);

#endif
