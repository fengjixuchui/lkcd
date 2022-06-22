#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include "../bpfdump/jit/types.h"
#include "../bpfdump/jit/bpf.h"

typedef struct bpf_prog *(*jit_compile)(struct bpf_prog *prog);
typedef void (*set_kdata)(unsigned long base, unsigned long enter, unsigned long ex);

jit_compile s_j = NULL;
set_kdata s_kdata = NULL;
void *s_jm = NULL;

int ujit_opened()
{
  return s_jm != NULL;
}

void ujit_close()
{
  if ( s_jm )
  {
    dlclose(s_jm);
    s_jm = NULL;
  }
}

int put_kdata(unsigned long base, unsigned long enter, unsigned long ex)
{
  if ( !s_kdata )
   return 0;
  s_kdata(base, enter, ex);
  return 1;
}

int ujit_open(const char *fname)
{
  if ( s_jm )
    ujit_close();
  s_jm = dlopen(fname, RTLD_LAZY);
  if ( !s_jm )
  {
    fprintf(stderr, "dlopen(%s) failed, err %d\n", fname, errno);
    return 0;
  }
  s_j = (jit_compile)dlsym(s_jm, "bpf_int_jit_compile");
  if ( !s_j )
  {
    fprintf(stderr, "cannot find bpf_int_jit_compile\n");
    return 0;
  }
  s_kdata = (set_kdata)dlsym(s_jm, "put_call_base");
  return 1;
}

int ujit(int idx, unsigned char *body, long len, unsigned int stack_depth)
{
  if ( s_j == NULL )
    return -1;
  // make new bpf_prog
  size_t asize = sizeof(bpf_prog) + 8 * len;
  bpf_prog *prog = (bpf_prog *)malloc(asize);
  struct bpf_prog_aux aux;
  memset(&aux, 0, sizeof(aux));
  // copy ebpf opcodes
  memcpy(prog->insnsi, body, len * 8);
  prog->len = len;
  prog->jited_len = 0;
  prog->aux = &aux;
  prog->pages = len * 8 / 0x1000;
  prog->aux->prog = prog;
  prog->aux->jit_data = NULL;
  prog->aux->func = NULL;
  prog->aux->func_cnt = 0;
  prog->aux->stack_depth = stack_depth;
  prog->bpf_func = NULL;
  prog->jit_requested = 1;
  printf("s_j %p\n", s_j); fflush(stdout);
  auto f = s_j(prog);
  if ( !f )
  {
    fprintf(stderr, "bpf_int_jit_compile failed\n");
    free(prog);
    return 0;
  }
  printf("jited_len %d bpf_func %p\n", f->jited_len, f->bpf_func);
  char fn[256];
  snprintf(fn, sizeof(fn), "%d.bin", idx);
  FILE *fp = fopen(fn, "wb");
  if ( fp == NULL )
  {
    printf("cannot open file %s\n", fn);
    free(prog);
    return 0;
  }
  fwrite((void *)f->bpf_func, f->jited_len, 1, fp);
  fclose(fp);
  free(prog);
  return 0;
}