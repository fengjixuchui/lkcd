#pragma once
#include <map>
#include <set>
#include <vector>
#include "types.h"

struct lsm_hook
{
  std::string name;
  a64 addr;
  a64 list;

  lsm_hook(const char *f)
   : name(f)
  {
    addr = 0;
    list = 0;
  }
};

class dis_base
{
  public:
    dis_base(a64 text_base, size_t text_size, const char *text, a64 data_base, size_t data_size)
     : m_text_base(text_base),
       m_text_size(text_size),
       m_data_base(data_base),
       m_data_size(data_size),
       m_text(text)
    {
      m_bss_base = 0;
      m_bss_size = 0;
      m_this_cpu_off = 0;
      m_return_notifier_list = 0;
      m_security_hook_heads = 0;
    }
    void set_bss(a64 addr, size_t size)
    {
      m_bss_base = addr;
      m_bss_size = size;
    }
    void set_shook(a64 val)
    {
      m_security_hook_heads = val;
    }
    virtual ~dis_base() = default;
    // getters
    inline int get_return_notifier_list(unsigned long &this_cpu_off, unsigned long &return_notifier_list)
    {
      this_cpu_off = m_this_cpu_off;
      return_notifier_list = m_return_notifier_list;
      return (m_return_notifier_list != 0) && (m_this_cpu_off != 0);
    }
    // interface of disasm
    virtual int find_return_notifier_list(a64 addr)
    {
      return 0;
    }
    int process_sl(std::vector<lsm_hook> &arr)
    {
      int res = 0;
      for ( auto &c: arr )
      {
        if ( !c.addr )
          continue;
        res += process_sl(c);
      }
      return res;
    }
    virtual int process(a64 addr, std::map<a64, a64> &, std::set<a64> &out_res) = 0;
    virtual int process_sl(lsm_hook &) = 0;
  protected:
    inline int in_text(const char *psp)
    {
      return (psp >= m_text) && (psp < m_text + m_text_size);
    }
    inline int in_data(a64 addr)
    {
      if ( addr >= m_bss_base && addr < (m_bss_base + m_bss_size) )
        return 1;
      return ( addr >= m_data_base && addr < (m_data_base + m_data_size) );
    }

    a64 m_text_base;
    size_t m_text_size;
    a64 m_data_base;
    size_t m_data_size;
    const char *m_text;
    a64 m_bss_base;
    size_t m_bss_size;
    // some per_cpu var offsets
    unsigned long m_this_cpu_off;
    unsigned long m_return_notifier_list; // from fire_user_return_notifiers
    // security_hook_heads
    a64 m_security_hook_heads;
};