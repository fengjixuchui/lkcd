#ifndef _MSC_VER
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/sysinfo.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#endif

#include <iostream>
#include <elfio/elfio_dump.hpp>
#include "ksyms.h"
#include "getopt.h"
#include "x64_disasm.h"
#include "arm64_disasm.h"
#include "arm64relocs.h"
#ifndef _MSC_VER
#include "../shared.h"
#include "kmods.h"
#include "lk.h"
#endif

int g_opt_v = 0;

using namespace ELFIO;

struct x64_thunk
{
  const char *name;
  ud_type reg;
};

void usage(const char *prog)
{
  printf("%s usage: [options] image [symbols]\n", prog);
  printf("Options:\n");
  printf("-b - check .bss section\n");
  printf("-c - check memory. Achtung - you must first load lkcd driver\n");
  printf("-d - use disasm\n");
  printf("-F - dump super-blocks\n");
  printf("-f - dump ftraces\n");  
  printf("-k - dump kprobes\n");
  printf("-r - check .rodata section\n");
  printf("-s - check fs_ops for sysfs files\n");
  printf("-u - dump usb_monitor\n");
  printf("-v - verbose mode\n");
  exit(6);
}

static const x64_thunk s_x64_thunks[] = {
  { "__x86_indirect_thunk_rax", UD_R_RAX },
  { "__x86_indirect_thunk_rbx", UD_R_RBX },
  { "__x86_indirect_thunk_rcx", UD_R_RCX },
  { "__x86_indirect_thunk_rdx", UD_R_RDX },
  { "__x86_indirect_thunk_rsi", UD_R_RSI },
  { "__x86_indirect_thunk_rdi", UD_R_RDI },
  { "__x86_indirect_thunk_rbp", UD_R_RBP },
  { "__x86_indirect_thunk_r8",  UD_R_R8 },
  { "__x86_indirect_thunk_r9",  UD_R_R9 },
  { "__x86_indirect_thunk_r10", UD_R_R10 },
  { "__x86_indirect_thunk_r11", UD_R_R11 },
  { "__x86_indirect_thunk_r12", UD_R_R12 },
  { "__x86_indirect_thunk_r13", UD_R_R13 },
  { "__x86_indirect_thunk_r14", UD_R_R14 },
  { "__x86_indirect_thunk_r15", UD_R_R15 },
};

section* find_section(const elfio& reader, a64 addr)
{
  Elf_Half n = reader.sections.size();
  for ( Elf_Half i = 0; i < n; ++i ) 
  {
    section* sec = reader.sections[i];
    auto start = sec->get_address();
    if ( (addr >= start) &&
         addr < (start + sec->get_size())
       )
      return sec;
  }
  return NULL;
}

const char *find_addr(const elfio& reader, a64 addr)
{
  section *s = find_section(reader, addr);
  if ( NULL == s )
    return NULL;
  if ( s->get_type() & SHT_NOBITS )
    return NULL;
  return s->get_data() + (addr - s->get_address());
}

void dump_arm64_fraces(const elfio& reader, a64 start, a64 end)
{
  Elf_Half n = reader.sections.size();
  if ( !n )
    return;
  for ( Elf_Half i = 0; i < n; ++i ) 
  {
    section* sec = reader.sections[i];
    if ( sec->get_type() == SHT_RELA )
    {
      const_relocation_section_accessor rsa(reader, sec);
      Elf_Xword relno = rsa.get_entries_num();
      for ( int i = 0; i < relno; i++ )
      {
         Elf64_Addr offset;
         Elf_Word   symbol;
         Elf_Word   type;
         Elf_Sxword addend;
         rsa.get_entry(i, offset, symbol, type, addend);
         if ( offset < start || offset > end )
           continue;
         if ( type != R_AARCH64_RELATIVE )
           continue;
         const char *name = lower_name_by_addr(addend);
         if ( name != NULL )
           printf("%p # %s\n", (void *)addend, name);
         else
           printf("%p\n", (void *)addend);
      }
    }
  }
}

size_t filter_arm64_relocs(const elfio& reader, a64 start, a64 end, a64 fstart, a64 fend, std::map<a64, a64> &filled)
{
  size_t res = 0;
  Elf_Half n = reader.sections.size();
  if ( !n )
    return 0;
  for ( Elf_Half i = 0; i < n; ++i ) 
  {
    section* sec = reader.sections[i];
    if ( sec->get_type() == SHT_RELA )
    {
      const_relocation_section_accessor rsa(reader, sec);
      Elf_Xword relno = rsa.get_entries_num();
      for ( int i = 0; i < relno; i++ )
      {
         Elf64_Addr offset;
         Elf_Word   symbol;
         Elf_Word   type;
         Elf_Sxword addend;
         rsa.get_entry(i, offset, symbol, type, addend);
         if ( offset < start || offset > end )
           continue;
         if ( type != R_AARCH64_RELATIVE )
           continue;
         if ( addend >= fstart && addend < fend )
         {
           filled[offset] = addend;
           res++;
         }
      }
    }
  }
  return res;
}

void dump_patched(a64 curr_addr, char *ptr, char *arg, sa64 delta)
{
   size_t off = 0;
   const char *name = lower_name_by_addr_with_off(curr_addr, &off);
   if ( name != NULL )
   {
     const char *pto = name_by_addr((a64)(arg - delta));
     if ( pto != NULL )
     {
        if ( off )
          printf("mem at %p (%s+%lX) patched to %p (%s)\n", ptr, name, off, arg, pto);
        else
          printf("mem at %p (%s) patched to %p (%s)\n", ptr, name, arg, pto);
      } else {
        if ( off )
          printf("mem at %p (%s+%lX) patched to %p\n", ptr, name, off, arg);
        else
          printf("mem at %p (%s) patched to %p\n", ptr, name, arg);
      }
  } else
     printf("mem at %p patched to %p\n", ptr, arg);
}

void dump_and_check(int fd, int opt_c, sa64 delta, int has_syms, std::map<a64, a64> &filled)
{
  for ( auto &c: filled )
  {
    auto curr_addr = c.first;
    auto addr = c.second;
    if ( g_opt_v )
    {
      size_t off = 0;
      const char *name = lower_name_by_addr_with_off(curr_addr, &off);
      if ( name != NULL )
      {
         const char *pto = name_by_addr(addr);
         if ( pto != NULL )
         {
           if ( off )
             printf("# %s+%lX -> %s\n", name, off, pto);
           else
             printf("# %s -> %s\n", name, pto);
           } else {
             if ( off )
               printf("# %s+%lX\n", name, off);
             else
               printf("# %s\n", name);
           }
         }
         printf("%p\n", (void *)curr_addr);
      }
#ifndef _MSC_VER
      if ( opt_c )
      {
         char *ptr = (char *)curr_addr + delta;
         char *arg = ptr;
         int err = ioctl(fd, IOCTL_READ_PTR, (int *)&arg);
         if ( err )
         {
           printf("read at %p failed, error %d (%s)\n", ptr, errno, strerror(errno));
           continue;
         }
         char *real = (char *)addr + delta;
         if ( real != arg )
         {
           if ( is_inside_kernel((unsigned long)arg) )
           {
              if ( !has_syms )
                printf("mem at %p: %p (must be %p)\n", ptr, arg, real);
              else 
              {
                size_t off = 0;
                const char *name = lower_name_by_addr_with_off(curr_addr, &off);
                if ( name != NULL )
                {
                  const char *pto = name_by_addr((a64)(arg - delta));
                  if ( pto != NULL )
                  {
                     if ( off )
                       printf("mem at %p (%s+%lX) patched to %p (%s)\n", ptr, name, off, arg, pto);
                     else
                       printf("mem at %p (%s) patched to %p (%s)\n", ptr, name, arg, pto);
                   } else {
                     if ( off )
                       printf("mem at %p (%s+%lX) patched to %p\n", ptr, name, off, arg);
                     else
                       printf("mem at %p (%s) patched to %p\n", ptr, name, arg);
                   }
                } else
                   printf("mem at %p: %p (must be %p)\n", ptr, arg, real);
              }
           } else 
           { // address not in kernel
              const char *mname = find_kmod((unsigned long)arg);
              if ( mname )
                printf("mem at %p: %p (must be %p) - patched by %s\n", ptr, arg, real, mname);
              else
                printf("mem at %p: %p (must be %p) - patched by UNKNOWN\n", ptr, arg, real);
            }
         }
      } /* opt_c */
#endif /* !_MSC_VER */
  }
}

#ifndef _MSC_VER
void dump_kptr(unsigned long l, const char *name, sa64 delta)
{
  if (is_inside_kernel(l))
  {
    const char *sname = name_by_addr(l - delta);
    if (sname != NULL)
      printf(" %s: %p - kernel!%s\n", name, (void *)l, sname);
    else
      printf(" %s: %p - kernel\n", name, (void *)l);
  }
  else {
    const char *mname = find_kmod(l);
    if (mname)
      printf(" %s: %p - %s\n", name, (void *)l, mname);
    else
      printf(" %s: %p - UNKNOWN\n", name, (void *)l);
  }
}

static size_t calc_uprobes_size(size_t n)
{
  return n * sizeof(one_uprobe) + sizeof(unsigned long);
}

static size_t calc_uprobes_clnt_size(size_t n)
{
  return n * sizeof(one_uprobe_consumer) + sizeof(unsigned long);
}

void dump_uprobes(int fd, sa64 delta)
{
  unsigned long a1 = get_addr("uprobes_tree");
  if ( !a1 )
  {
    printf("cannot find uprobes_tree\n");
    return;
  }
  unsigned long a2 = get_addr("uprobes_treelock");
  if ( !a2 )
  {
    printf("cannot find uprobes_treelock\n");
    return;
  }
  unsigned long params[2] = { a1 + delta, a2 + delta };
  int err = ioctl(fd, IOCTL_CNT_UPROBES, (int *)&params);
  if ( err )
  {
    printf("IOCTL_CNT_UPROBES failed, error %d (%s)\n", errno, strerror(errno));
    return;
  }
  printf("uprobes: %ld\n", params[0]);
  if ( !params[0] )
    return;
  size_t size = calc_uprobes_size(params[0]);
  char *buf = (char *)malloc(size);
  if ( !buf )
  {
    printf("cannot alloc buffer for uprobes, len %lX\n", size);
    return;
  }
  unsigned long *palias = (unsigned long *)buf;
  palias[0] = a1 + delta;
  palias[1] = a2 + delta;
  palias[2] = params[0];
  err = ioctl(fd, IOCTL_UPROBES, (int *)palias);
  if ( err )
  {
    printf("IOCTL_UPROBES failed, error %d (%s)\n", errno, strerror(errno));
  } else {
    one_uprobe *up = (one_uprobe *)(buf + sizeof(unsigned long));
    for ( auto cnt = 0; cnt < *palias; cnt++ )
    {
      printf("[%d] addr %p inode %p ino %ld clnts %ld offset %lX flags %lX %s\n", 
        cnt, up[cnt].addr, up[cnt].inode, up[cnt].i_no, up[cnt].cons_cnt, up[cnt].offset, up[cnt].flags, up[cnt].name);
      if ( !up[cnt].cons_cnt )
        continue;
      size_t client_size = calc_uprobes_clnt_size(up[cnt].cons_cnt);
      char *cbuf = (char *)malloc(client_size);
      if ( !cbuf )
      {
        printf("cannot alloc buffer for uprobe %p consumers, len %lX\n", up[cnt].addr, client_size);
        continue;
      }
      // form params for IOCTL_CNT_UPROBES
      unsigned long *calias = (unsigned long *)cbuf;
      calias[0] = a1 + delta;
      calias[1] = a2 + delta;
      calias[2] = (unsigned long)up[cnt].addr;
      calias[3] = up[cnt].cons_cnt;
      err = ioctl(fd, IOCTL_UPROBES_CONS, (int *)calias);
      if ( err )
      {
        printf("IOCTL_UPROBES_CONS for %p failed, error %d (%s)\n", up[cnt].addr, errno, strerror(errno));
        free(cbuf);
        continue;
      }
      // dump consumers
      one_uprobe_consumer *uc = (one_uprobe_consumer *)(cbuf + sizeof(unsigned long));
      for ( auto cnt2 = 0; cnt2 < *calias; cnt2++ )
      {
        printf(" consumer[%d] at %p\n", cnt2, uc[cnt2].addr);
        if ( uc[cnt2].handler )
          dump_kptr((unsigned long)uc[cnt2].handler, "  handler", delta);
        if ( uc[cnt2].ret_handler )
          dump_kptr((unsigned long)uc[cnt2].ret_handler, "  ret_handler", delta);
        if ( uc[cnt2].filter )
          dump_kptr((unsigned long)uc[cnt2].filter, "  filter", delta);
      }
      free(cbuf);
    }
  }
  free(buf);
}

static size_t calc_super_size(size_t n)
{
   return n * sizeof(one_super_block) + sizeof(unsigned long);
}

void dump_super_blocks(int fd, sa64 delta)
{
  unsigned long cnt = 0;
  int err = ioctl(fd, IOCTL_GET_SUPERBLOCKS, (int *)&cnt);
  if ( err )
  {
    printf("IOCTL_GET_SUPERBLOCKS count failed, error %d (%s)\n", errno, strerror(errno));
    return;
  }
  printf("super-blocks: %ld\n", cnt);
  if ( !cnt )
    return;
  size_t size = calc_super_size(cnt);
  unsigned long *buf = (unsigned long *)malloc(size);
  if ( !buf )
    return;
  buf[0] = cnt;
  err = ioctl(fd, IOCTL_GET_SUPERBLOCKS, (int *)buf);
  if ( err )
  {
    printf("IOCTL_GET_SUPERBLOCKS failed, error %d (%s)\n", errno, strerror(errno));
    free(buf);
    return;
  }
  size = buf[0];
//  printf("size %ld\n", size);
  struct one_super_block *sb = (struct one_super_block *)(buf + 1);
  for ( size_t idx = 0; idx < size; idx++ )
  {
    printf("superblock[%ld] at %p dev %ld %s\n", idx, sb[idx].addr, sb[idx].dev, sb[idx].s_id);
    if ( sb[idx].s_type )
      dump_kptr((unsigned long)sb[idx].s_type, "s_type", delta);
    if ( sb[idx].s_op )
      dump_kptr((unsigned long)sb[idx].s_op, "s_op", delta);
    if ( sb[idx].dq_op )
      dump_kptr((unsigned long)sb[idx].dq_op, "dq_op", delta);
    if ( sb[idx].s_qcop )
      dump_kptr((unsigned long)sb[idx].s_qcop, "s_qcop", delta);
    if ( sb[idx].s_export_op )
      dump_kptr((unsigned long)sb[idx].s_export_op, "s_export_op", delta);
    if ( sb[idx].s_fsnotify_mask || sb[idx].s_fsnotify_marks )
      printf(" s_fsnotify_mask: %lX s_fsnotify_marks %p\n", sb[idx].s_fsnotify_mask, sb[idx].s_fsnotify_marks);
  }
  free(buf);
}

static size_t calc_kprobes_size(size_t n)
{
  return n * sizeof(one_kprobe) + sizeof(unsigned long);
}

void dump_kprobes(int fd, sa64 delta)
{
  unsigned long a1 = get_addr("kprobe_table");
  if ( !a1 )
  {
    printf("cannot find kprobe_table\n");
    return;
  }
  unsigned long a2 = get_addr("kprobe_mutex");
  if ( !a2 )
  {
    printf("cannot find kprobe_mutex\n");
    return;
  }
  size_t curr_n = 3;
  size_t ksize = calc_kprobes_size(curr_n);
  unsigned long *buf = (unsigned long *)malloc(ksize);
  if ( !buf )
    return;
  for ( int i = 0; i < 64; i++ )
  {
    unsigned long params[3] = { a1 + delta, a2 + delta, (unsigned long)i };
    int err = ioctl(fd, IOCTL_CNT_KPROBE_BUCKET, (int *)&params);
    if ( err )
    {
      printf("IOCTL_CNT_KPROBE_BUCKET(%d) failed, error %d (%s)\n", i, errno, strerror(errno));
      continue;
    }
    if ( !params[0] )
      continue;
    printf("kprobes[%d]: %ld\n", i, params[0]);
    // ok, we have some kprobes, read them all
    if ( params[0] > curr_n )
    {
      unsigned long *tmp;
      ksize = calc_kprobes_size(params[0]);
      tmp = (unsigned long *)malloc(ksize);
      if ( tmp == NULL )
        break;
      curr_n = params[0];
      free(buf);
      buf = tmp;
    }
    // fill params
    buf[0] = a1 + delta;
    buf[1] = a2 + delta;
    buf[2] = (unsigned long)i;
    buf[3] = params[0];
    err = ioctl(fd, IOCTL_GET_KPROBE_BUCKET, (int *)buf);
    if ( err )
    {
      printf("IOCTL_GET_KPROBE_BUCKET(%d) failed, error %d (%s)\n", i, errno, strerror(errno));
      continue;
    }
    // dump
    ksize = buf[0];
    struct one_kprobe *kp = (struct one_kprobe *)(buf + 1);
    for ( size_t idx = 0; idx < ksize; idx++ )
    {
      printf(" kprobe at %p flags %X\n", kp[idx].kaddr, kp[idx].flags);
      dump_kptr((unsigned long)kp[idx].addr, " addr", delta);
      if ( kp[idx].pre_handler )
        dump_kptr((unsigned long)kp[idx].pre_handler, " pre_handler", delta);
      if ( kp[idx].post_handler )
        dump_kptr((unsigned long)kp[idx].post_handler, " post_handler", delta);
    }
  }
  if ( buf != NULL )
    free(buf);
}

void install_urn(int fd, int action)
{
  unsigned long param = action;
  int err = ioctl(fd, IOCTL_TEST_URN, (int *)&param);
  if ( err )
    printf("install_urn(%d) failed, error %d (%s)\n", action, errno, strerror(errno));
}

static size_t calc_urntfy_size(size_t n)
{
  return (n + 1) * sizeof(unsigned long);
}

void dump_return_notifier_list(int fd, unsigned long this_off, unsigned long off, sa64 delta)
{
  int cpu_num = get_nprocs();
  size_t curr_n = 3;
  size_t size = calc_urntfy_size(curr_n);
  unsigned long *ntfy = (unsigned long *)malloc(size);
  if ( ntfy == NULL )
    return;
  for ( int i = 0; i < cpu_num; i++ )
  {
    unsigned long buf[3] = { (unsigned long)i, this_off, off };
    int err = ioctl(fd, IOCTL_CNT_RNL_PER_CPU, (int *)buf);
    if ( err )
    {
      printf("dump_return_notifier_list count for cpu_id %d failed, error %d (%s)\n", i, errno, strerror(errno));
      break;
    }
    if ( buf[0] )
      printf("cpu[%d]: head %p %ld\n", i, (void *)buf[0], buf[1]);
    else
      printf("cpu[%d]: %ld\n", i, buf[1]);
    if ( !buf[1] )
      continue; // no ntfy on this cpu
    // read ntfy
    if ( buf[1] > curr_n )
    {
      unsigned long *tmp;
      size = calc_urntfy_size(buf[1]);
      tmp = (unsigned long *)malloc(size);
      if ( tmp == NULL )
        break;
      curr_n = buf[1];
      free(ntfy);
      ntfy = tmp;
    }
    // fill params
    ntfy[0] = (unsigned long)i;
    ntfy[1] = this_off;
    ntfy[2] = off;
    ntfy[3] = buf[1];
    err = ioctl(fd, IOCTL_RNL_PER_CPU, (int *)ntfy);
    if ( err )
    {
      printf("dump_return_notifier_list for cpu_id %d cnt %ld failed, error %d (%s)\n", i, buf[1], errno, strerror(errno));
      break;
    }
    // dump
    size = ntfy[0];
    for ( size_t j = 0; j < size; j++ )
    {
      dump_kptr(ntfy[1 + j], "ntfy", delta);
    }
  }
  if ( ntfy != NULL )
    free(ntfy);
}

// generic_efivars is struct efivars - 2nd ptr is efivar_operations which has 5 function pointers
// see https://elixir.bootlin.com/linux/v5.14-rc7/source/include/linux/efi.h#L948
void dump_efivars(int fd, a64 saddr, sa64 delta)
{
   char *ptr = (char *)saddr + delta + 2 * sizeof(void *);
   char *arg = ptr;
   int err = ioctl(fd, IOCTL_READ_PTR, (int *)&arg);
   if ( err )
   {
      printf("dump_efivars: read at %p failed, error %d (%s)\n", ptr, errno, strerror(errno));
      return;
   }
   if ( !arg )
     return;
   if ( is_inside_kernel((unsigned long)arg) )
      printf("efivar_operations at %p: %p - kernel\n", ptr, arg);
   else {
     const char *mname = find_kmod((unsigned long)arg);
     if ( mname )
       printf("efivar_operations at %p: %p - %s\n", ptr, arg, mname);
     else
       printf("efivar_operations at %p: %p UNKNOWN\n", ptr, arg);
   }
   // dump all five fields
   ptr = arg;
   err = ioctl(fd, IOCTL_READ_PTR, (int *)&arg);
   if ( err )
    printf("cannot read get_variable at %p, err %d\n", ptr, err);
   else if ( arg )
     dump_kptr((unsigned long)arg, "get_variable", delta);

   ptr += sizeof(void *);
   arg = ptr;
   err = ioctl(fd, IOCTL_READ_PTR, (int *)&arg);
   if ( err )
    printf("cannot read get_variable_next at %p, err %d\n", ptr, err);
   else if ( arg )
     dump_kptr((unsigned long)arg, "get_variable_next", delta);

   ptr += sizeof(void *);
   arg = ptr;
   err = ioctl(fd, IOCTL_READ_PTR, (int *)&arg);
   if ( err )
    printf("cannot read set_variable at %p, err %d\n", ptr, err);
   else if ( arg )
     dump_kptr((unsigned long)arg, "set_variable", delta);

   ptr += sizeof(void *);
   arg = ptr;
   err = ioctl(fd, IOCTL_READ_PTR, (int *)&arg);
   if ( err )
    printf("cannot read set_variable_nonblocking at %p, err %d\n", ptr, err);
   else if ( arg )
     dump_kptr((unsigned long)arg, "set_variable_nonblocking", delta);

   ptr += sizeof(void *);
   arg = ptr;
   err = ioctl(fd, IOCTL_READ_PTR, (int *)&arg);
   if ( err )
    printf("cannot read query_variable_store at %p, err %d\n", ptr, err);
   else if ( arg )
     dump_kptr((unsigned long)arg, "query_variable_store", delta);
}

void dump_usb_mon(int fd, a64 saddr, sa64 delta)
{
   char *ptr = (char *)saddr + delta;
   char *arg = ptr;
   int err = ioctl(fd, IOCTL_READ_PTR, (int *)&arg);
   if ( err )
   {
      printf("dump_usb_mon: read at %p failed, error %d (%s)\n", ptr, errno, strerror(errno));
      return;
   }
   if ( arg )
   {
     if ( is_inside_kernel((unsigned long)arg) )
       printf("mon_ops at %p: %p - kernel\n", (char *)saddr + delta, arg);
     else {
       const char *mname = find_kmod((unsigned long)arg);
       if ( mname )
         printf("mon_ops at %p: %p - %s\n", (char *)saddr + delta, arg, mname);
       else
         printf("mon_ops at %p: %p UNKNOWN\n", (char *)saddr + delta, arg);
     }
   } else 
     printf("mon_ops at %p: %p\n", (char *)saddr + delta, arg);
   if ( !arg )
     return;
   // see https://elixir.bootlin.com/linux/v5.14-rc7/source/include/linux/usb/hcd.h#L702
   // we need read 3 pointers at ptr
   char *stored_ptr = arg;
   arg = ptr;
   err = ioctl(fd, IOCTL_READ_PTR, (int *)&arg);
   if ( err )
    printf("cannot read urb_submit at %p, err %d\n", stored_ptr, err);
   else if ( arg )
     dump_kptr((unsigned long)arg, "urb_submit", delta);

   ptr = stored_ptr + sizeof(void *);
   arg = ptr;
   err = ioctl(fd, IOCTL_READ_PTR, (int *)&arg);
   if ( err )
     printf("cannot read urb_submit_error at %p, err %d\n", stored_ptr + sizeof(void *), err);
   else if ( arg )
     dump_kptr((unsigned long)arg, "urb_submit_error", delta);
 
   ptr = stored_ptr + 2 * sizeof(void *);
   arg = ptr;
   err = ioctl(fd, IOCTL_READ_PTR, (int *)&arg);
   if ( err )
     printf("cannot read urb_submit_error at %p, err %d\n", stored_ptr + 2 * sizeof(void *), err);
   else if ( arg )
     dump_kptr((unsigned long)arg, "urb_complete", delta);
}

static size_t calc_tp_size(size_t n)
{
  return (n + 1) * sizeof(unsigned long);
}

void check_tracepoints(int fd, sa64 delta, addr_sym *tsyms, size_t tcount)
{
  // alloc enough memory for tracepoint info
  size_t i, j, curr_n = 3;
  size_t size = calc_tp_size(curr_n);
  unsigned long *ntfy = (unsigned long *)malloc(size);
  if ( ntfy == NULL )
    return;
  for ( i = 0; i < tcount; i++ )
  {
    a64 addr = (a64)((char *)tsyms[i].addr + delta);
    ntfy[0] = addr;
    int err = ioctl(fd, IOCTL_TRACEPOINT_INFO, (int *)ntfy);
    if ( err )
    {
      printf("error %d while read tracepoint info for %s at %p\n", err, tsyms[i].name, (void *)addr);
      continue;
    }
    printf(" %s at %p: enabled %d cnt %d\n", tsyms[i].name, (void *)addr, (int)ntfy[0], (int)ntfy[3]);
    // 1 - regfunc
    if ( ntfy[1] )
    {
      if ( is_inside_kernel(ntfy[1]) )
      {
        const char *fname = name_by_addr(ntfy[1] - delta);
        if ( fname != NULL )
          printf("  regfunc %p - kernel!%s\n", (void *)ntfy[1], fname);
        else
          printf("  regfunc %p - kernel\n", (void *)ntfy[1]);
      } else {
        const char *mname = find_kmod(ntfy[1]);
        if ( mname )
          printf("  regfunc %p - %s\n", (void *)ntfy[1], mname);
        else
          printf("  regfunc %p UNKNOWN\n", (void *)ntfy[1]);
      }
    }
    // 2 - unregfunc
    if ( ntfy[2] )
    {
      if ( is_inside_kernel(ntfy[2]) )
      {
        const char *fname = name_by_addr(ntfy[2] - delta);
        if ( fname != NULL )
          printf("  unregfunc %p - kernel!%s\n", (void *)ntfy[2], fname);
        else
          printf("  unregfunc %p - kernel\n", (void *)ntfy[2]);
      } else {
        const char *mname = find_kmod(ntfy[2]);
        if ( mname )
          printf("  unregfunc %p - %s\n", (void *)ntfy[2], mname);
        else
          printf("  unregfunc %p UNKNOWN\n", (void *)ntfy[2]);
      }
    }
    if ( !ntfy[3] )
      continue;
    auto curr_cnt = ntfy[3];
    if ( curr_cnt > curr_n )
    {
      unsigned long *tmp;
      size = calc_tp_size(addr);
      tmp = (unsigned long *)malloc(size);
      if ( tmp == NULL )
        break;
      curr_n = curr_cnt;
      free(ntfy);
      ntfy = tmp;
    }
    // dump funcs
    ntfy[0] = addr;
    ntfy[1] = curr_cnt;
    err = ioctl(fd, IOCTL_TRACEPOINT_FUNCS, (int *)ntfy);
    if ( err )
    {
      printf("error %d while read tracepoint funcs for %s at %p\n", err, tsyms[i].name, (void *)addr);
      continue;
    }
    size = ntfy[0];
    for ( j = 0; j < size; j++ )
    {
      if ( is_inside_kernel(ntfy[1 + j]) )
        printf("  [%ld] %p - kernel\n", j, (void *)ntfy[1 + j]);
      else {
        const char *mname = find_kmod(ntfy[1 + j]);
        if ( mname )
          printf("  [%ld] %p - %s\n", j, (void *)ntfy[1 + j], mname);
        else
          printf("  [%ld] %p UNKNOWN\n", j, (void *)ntfy[1 + j]);
      }
    }
  }
  free(ntfy);
}
#endif /* !_MSC_VER */

int is_nop(unsigned char *body)
{
  // nop dword ptr [rax+rax+00h] - 0F 1F 44 00 00
  if ( body[0] == 0xF  &&
       body[1] == 0x1F &&
       body[2] == 0x44 &&
       body[3] == 0    &&
       body[4] == 0
     )
   return 1;
  // just 90
  if ( body[0] == 0x90 &&
       body[1] == 0x90 &&
       body[2] == 0x90 &&
       body[3] == 0x90 &&
       body[4] == 0x90
     )
   return 1;
  return 0;
}

int main(int argc, char **argv)
{
   // read options
   int opt_f = 0,
       opt_F = 0,
       opt_d = 0,
       opt_c = 0,
       opt_k = 0,
       opt_r = 0,
       opt_s = 0,
       opt_t = 0,
       opt_b = 0,
       opt_u = 0;
   int c;
   int fd = 0;
   while (1)
   {
     c = getopt(argc, argv, "bcdFfkrstuv");
     if (c == -1)
	break;

     switch (c)
     {
        case 'b':
          opt_b = 1;
         break;
 	case 'F':
 	  opt_F = 1;
         break;
 	case 'f':
 	  opt_f = 1;
         break;
        case 'v':
          g_opt_v = 1;
         break;
        case 'd':
          opt_d = 1;
         break;
        case 'c':
          opt_c = 1;
         break;
        case 'k':
          opt_k = 1;
          opt_c = 1;
         break;
        case 'r':
          opt_r = 1;
         break;
        case 's':
          opt_s = 1;
          opt_c = 1;
         break;
        case 'u':
          opt_u = 1;
          opt_c = 1;
         break;
        case 't':
          opt_t = 1;
         break;
        default:
         usage(argv[0]);
     }
   }
   if (optind == argc)
     usage(argv[0]);

   elfio reader;
   int has_syms = 0;
   if ( !reader.load( argv[optind] ) ) 
   {
      printf( "File %s is not found or it is not an ELF file\n", argv[optind] );
      return 1;
   }
   optind++;
   Elf_Half n = reader.sections.size();
   for ( Elf_Half i = 0; i < n; ++i ) { // For all sections
     section* sec = reader.sections[i];
     if ( SHT_SYMTAB == sec->get_type() ||
          SHT_DYNSYM == sec->get_type() ) 
     {
       symbol_section_accessor symbols( reader, sec );
       if ( !read_syms(reader, symbols) )
         has_syms++;
     }
   }
   // try to find symbols
   if ( !has_syms && optind != argc )
   {
     int err = read_ksyms(argv[optind]);
     if ( err )
     {
       printf("cannot read %s, error %d\n", argv[optind], err);
       return err;
     }
     has_syms = 1;
     optind++;
   }
   sa64 delta = 0;
#ifndef _MSC_VER
   // open driver
   if ( opt_c ) 
   {
     fd = open("/dev/lkcd", 0);
     if ( -1 == fd )
     {
       printf("cannot open device, error %d\n", errno);
       opt_c = 0;
       goto end;
     }
     // find delta between symbols from system.map and loaded kernel
     auto symbol_a = get_addr("group_balance_cpu");
     if ( !symbol_a )
     {
       close(fd);
       fd = 0;
       opt_c = 0;
       goto end;
     } else {
       if ( read_kernel_area(fd) )
       {
         close(fd);
         fd = 0;
         opt_c = 0;
         goto end;
       }
       int err = init_kmods();
       if ( err )
       {
         printf("init_kmods failed, error %d\n", err);
         goto end;
       }
       printf("group_balance_cpu from symbols: %p\n", (void *)symbol_a);
       union ksym_params kparm;
       strcpy(kparm.name, "group_balance_cpu");
       err = ioctl(fd, IOCTL_RKSYM, (int *)&kparm);
       if ( err )
       {
         printf("IOCTL_RKSYM test failed, error %d\n", err);
         close(fd);
         fd = 0;
         opt_c = 0;
       } else {
         printf("group_balance_cpu: %p\n", (void *)kparm.addr);
         delta = (char *)kparm.addr - (char *)symbol_a;
         printf("delta: %lX\n", delta);
       }
     }
     // dump kprobes
     if ( opt_k && opt_c )
     {
       dump_kprobes(fd, delta);
       dump_uprobes(fd, delta);
     }
     // dump super-blocks
     if ( opt_F && opt_c )
     {
       dump_super_blocks(fd, delta);
     }
     // check sysfs f_ops
     if ( opt_c && opt_s )
     {
       if ( optind == argc )
       {
         printf("where is files?\n");
         exit(6);
       }
       union kernfs_params kparm;
       for ( int idx = optind; idx < argc; idx++ )
       {
         strncpy(kparm.name, argv[idx], sizeof(kparm.name) - 1);
         kparm.name[sizeof(kparm.name) - 1] = 0;
         int err = ioctl(fd, IOCTL_KERNFS_NODE, (int *)&kparm);
         if ( err )
         {
           printf("IOCTL_KERNFS_NODE(%s) failed, error %d\n", argv[idx], err);
           continue;
         }
         printf("res %s: %p\n", argv[idx], (void *)kparm.res.addr);
         if ( kparm.res.addr )
         {
           // dump flags
           printf(" flags: %lX", kparm.res.flags);
           if ( kparm.res.flags & 1 )
             printf(" DIR");
           if ( kparm.res.flags & 2 )
             printf(" FILE");
           if ( kparm.res.flags & 4 )
             printf(" LINK");
           printf("\n");

           printf(" priv: %p\n", (void *)kparm.res.priv);
           if ( kparm.res.kobject )
             printf("kobject: %p\n", (void *)kparm.res.kobject);
           if ( kparm.res.ktype )
             dump_kptr(kparm.res.ktype, "ktype", delta);
           if ( kparm.res.sysfs_ops )
             dump_kptr(kparm.res.sysfs_ops, "sysfs_ops", delta);
           if ( kparm.res.show )
             dump_kptr(kparm.res.sysfs_ops, "sysfs_ops.show", delta);
           if ( kparm.res.store )
             dump_kptr(kparm.res.sysfs_ops, "sysfs_ops.store", delta);
         } else {
           printf(" inode: %p\n", (void *)kparm.res.flags);
           if ( kparm.res.s_op )
             dump_kptr(kparm.res.s_op, "s_op", delta);
           if ( kparm.res.priv )
             dump_kptr(kparm.res.priv, "inode->i_fop", delta);
         }
       }
     }
   }
end:
#endif /* _MSC_VER */
   // find .text section
   Elf64_Addr text_start = 0;
   Elf_Xword text_size = 0;
   section *text_section = NULL;
   for ( Elf_Half i = 0; i < n; ++i ) { // For all sections
     section* sec = reader.sections[i];
     if ( sec->get_name() == ".text" )
     {
       text_start = sec->get_address();
       text_size  = sec->get_size();
       text_section = sec;
       break;
     }
   }
   if ( has_syms )
   {
     // make some tests
     auto a1 = get_addr("__start_mcount_loc");
     printf("__start_mcount_loc: %p\n", (void *)a1);
     auto a2 = get_addr("__stop_mcount_loc");
     printf("__stop_mcount_loc: %p\n", (void *)a2);
     // if we had -f option
     if ( opt_f && a1 && a2 )
     {
       // under arm64 we need process relocs
       if ( reader.get_machine() == 183 )
         dump_arm64_fraces(reader, a1, a2);
       else {
         const a64 *data = (const a64 *)find_addr(reader, a1);
         if ( data != NULL )
         {
           for ( a64 i = a1; i < a2; i += sizeof(a64) )
           {
             a64 addr = *data;
             const char *name = lower_name_by_addr(addr);
             if ( name != NULL )
               printf("%p # %s\n", (void *)addr, name);
             else
               printf("%p\n", (void *)addr);
             data++;
#ifndef _MSC_VER
             if ( opt_c )
             {
               // filter out maybe discarded sections like .init.text
               if ( text_section != NULL &&
                    ( (addr < text_start) || (addr > (text_start + text_size)) )
                  )
                 continue;
               char *ptr = (char *)addr + delta;
               char *arg = ptr;
               int err = ioctl(fd, IOCTL_READ_PTR, (int *)&arg);
               if ( err )
                 printf("read ftrace at %p failed, error %d (%s)\n", ptr, errno, strerror(errno));
               else if ( !is_nop((unsigned char *)&arg) )
                 HexDump((unsigned char *)&arg, sizeof(arg));
             }
#endif /* !_MSC_VER */
           }
         }
       }
     }
   }

   if ( !text_start )
   {
     printf("cannot find .text\n");
     return 1;
   }
   for ( Elf_Half i = 0; i < n; ++i ) 
   {
     section* sec = reader.sections[i];
     if ( opt_r && sec->get_name() == ".rodata" )
     {
       std::map<a64, a64> filled;
       auto off = sec->get_offset();
       printf(".rodata section offset %lX\n", off);
       size_t count = 0;
       a64 curr_addr;
       // under arm64 we need count relocs in .data section       
       if ( reader.get_machine() == 183 )
       {
         a64 dstart = (a64)sec->get_address();
         count = filter_arm64_relocs(reader, dstart, dstart + sec->get_size(), (a64)text_start, (a64)(text_start + text_size), filled);
       } else {
         a64 *curr = (a64 *)sec->get_data();
         a64 *end  = (a64 *)((char *)curr + sec->get_size());
         curr_addr = sec->get_address();
         const endianess_convertor &conv = reader.get_convertor();
         for ( ; curr < end; curr++, curr_addr += sizeof(a64) )
         {
           auto addr = conv(*curr);
           if ( addr >= (a64)text_start &&
                addr < (a64)(text_start + text_size)
              )
           {
             count++;
             filled[curr_addr] = addr;
           }
         }
       }
       printf("found in .rodata %ld\n", count);
       // dump or check collected addresses
       if ( g_opt_v || opt_c )
         dump_and_check(fd, opt_c, delta, has_syms, filled);
       continue;
     }
     if ( sec->get_name() == ".data" )
     {
       std::map<a64, a64> filled;
       auto off = sec->get_offset();
       printf(".data section offset %lX\n", off);
       size_t count = 0;
       a64 curr_addr;
       if ( opt_u && has_syms )
       {
         a64 addr = get_addr("mon_ops");
         if ( !addr )
           printf("cannot find mon_ops\n");
#ifndef _MSC_VER
         else
           dump_usb_mon(fd, addr, delta);
#endif /* !_MSC_VER */
         addr = get_addr("generic_efivars");
         if ( !addr )
           printf("cannot find generic_efivars\n");
#ifndef _MSC_VER
         else
           dump_efivars(fd, addr, delta);
#endif /* !_MSC_VER */
       }
       if ( opt_t && has_syms )
       {
         size_t tcount = 0;
         a64 dstart = (a64)sec->get_address();
         struct addr_sym *tsyms = start_with("__tracepoint_", dstart, dstart + sec->get_size(), &tcount);
         if ( tsyms != NULL )
         {
           printf("found %ld tracepoints\n", tcount);
#ifdef _MSC_VER
           if ( g_opt_v )
           {
             for ( size_t i = 0; i < tcount; i++ )
               printf(" %p: %s\n", (void *)(tsyms[i].addr), tsyms[i].name);
           }
#else
           if ( opt_c )
             check_tracepoints(fd, delta, tsyms, tcount);
#endif /* _MSC_VER */
           free(tsyms);
         }
       }
       // under arm64 we need count relocs in .data section       
       if ( reader.get_machine() == 183 )
       {
         a64 dstart = (a64)sec->get_address();
         count = filter_arm64_relocs(reader, dstart, dstart + sec->get_size(), (a64)text_start, (a64)(text_start + text_size), filled);
       } else {
         a64 *curr = (a64 *)sec->get_data();
         a64 *end  = (a64 *)((char *)curr + sec->get_size());
         curr_addr = sec->get_address();
         const endianess_convertor &conv = reader.get_convertor();
         for ( ; curr < end; curr++, curr_addr += sizeof(a64) )
         {
           auto addr = conv(*curr);
           if ( addr >= (a64)text_start &&
                addr < (a64)(text_start + text_size)
              )
           {
             count++;
             filled[curr_addr] = addr;
           }
         }
       }
       printf("found %ld\n", count);
       // dump or check collected addresses
       if ( g_opt_v || opt_c )
         dump_and_check(fd, opt_c, delta, has_syms, filled);
       if ( opt_d )
       {
          dis_base *bd = NULL;
          if ( reader.get_machine() == 183 )
          {
            arm64_disasm *ad = new arm64_disasm(text_start, text_size, text_section->get_data(), sec->get_address(), sec->get_size());
            a64 addr = get_addr("__stack_chk_fail");
            if ( addr )
              ad->add_noreturn(addr);
            bd = ad;
          } else if ( reader.get_machine() == EM_X86_64 )
          {
            x64_disasm *x64 = new x64_disasm(text_start, text_size, text_section->get_data(), sec->get_address(), sec->get_size());
            // fill indirect thunks
            for ( auto &c: s_x64_thunks )
            {
              a64 thunk_addr = get_addr(c.name);
              if ( !thunk_addr )
                printf("cannot find %s\n", c.name);
              else
                x64->set_indirect_thunk(thunk_addr, c.reg);
             }
             a64 ntfy_addr = get_addr("fire_user_return_notifiers");
             if ( !ntfy_addr )
               printf("cannot find fire_user_return_notifiers\n");
             else {
               if ( x64->find_return_notifier_list(ntfy_addr) )
               {
                 unsigned long this_cpu_off = 0,
                               return_notifier_list = 0;
                 if ( x64->get_return_notifier_list(this_cpu_off, return_notifier_list) )
                 {
                   printf("this_cpu_off: %lX, return_notifier_list: %lX\n", this_cpu_off, return_notifier_list);
#ifndef _MSC_VER
                   if ( opt_c )
                   {
                     install_urn(fd, 1);
                     dump_return_notifier_list(fd, this_cpu_off, return_notifier_list, delta);
                     install_urn(fd, 0);
                   }
#endif
                 }
               } else
                 printf("cannot extract return_notifier_list\n");
             }
             bd = x64;
          } else {
            printf("no disasm for machine %d\n", reader.get_machine());
            break;
          }
          // find bss if we need
          if ( opt_b )
          {
            for ( Elf_Half j = 0; j < n; ++j )
            {
              section* s = reader.sections[j];
              if ( (s->get_type() & SHT_NOBITS) && 
                   (s->get_name() == ".bss" )
                 )
              {
                a64 bss_addr = s->get_address();
                if ( g_opt_v )
                  printf(".bss address %p size %lX\n", (void *)bss_addr, s->get_size());
                bd->set_bss(bss_addr, s->get_size());
                break;
              }
            }
          }
          std::set<a64> out_res;
          size_t tcount = 0;
          struct addr_sym *tsyms = get_in_range(text_start, text_start + text_size, &tcount);
          if (tsyms != NULL)
          {
#ifdef _DEBUG
            a64 taddr = get_addr("netdev_store.isra.14");
            if ( taddr )
              bd->process(taddr, filled, out_res);
#endif /* _DEBUG */
            for (size_t i = 0; i < tcount; i++)
            {
#ifdef _DEBUG
              printf("%s:\n", tsyms[i].name);
#endif /* _DEBUG */
              bd->process(tsyms[i].addr, filled, out_res);
            }
            free(tsyms);
          }
          else
          {
            // now disasm some funcs - security_load_policy
            a64 faddr = get_addr("rcu_sched_clock_irq");
            if (faddr)
            {
              bd->process(faddr, filled, out_res);
            }
          }
          delete bd;
          printf("found with disasm: %ld\n", out_res.size());
          if ( g_opt_v )
          {
            for ( auto c: out_res )
            {
              size_t off = 0;
              const char *name = lower_name_by_addr_with_off(c, &off);
              if ( name != NULL )
              {
                if ( off )
                  printf("# %s+%lX\n", name, off);
                else
                  printf("# %s\n", name);
              }
              printf("%p\n", (void *)c);
            }
          }
#ifndef _MSC_VER
          if ( opt_c )
          {
            for ( auto c: out_res )
            {
              char *ptr = (char *)c + delta;
              char *arg = ptr;
              int err = ioctl(fd, IOCTL_READ_PTR, (int *)&arg);
              if ( err )
                printf("read at %p failed, error %d (%s)\n", ptr, errno, strerror(errno));
              else if ( arg != NULL )
              {
                 if ( is_inside_kernel((unsigned long)arg) )
                 {
                    if ( !has_syms )
                      printf("mem at %p: %p\n", ptr, arg);
                    else
                      dump_patched(c, ptr, arg, delta);
                 } else {
                    const char *mname = find_kmod((unsigned long)arg);
                    if ( mname )
                      printf("mem at %p: %p - patched by %s\n", ptr, arg, mname);
                    else
                      printf("mem at %p: %p - patched by UNKNOWN\n", ptr, arg);
                 }
              }
            }
          } // opt_c
#endif /* !_MSC_VER */
       } // opt_d
       break;
     }
   }
#ifndef _MSC_VER
   if ( fd )
     close(fd);
#endif /* _MSC_VER */
}
