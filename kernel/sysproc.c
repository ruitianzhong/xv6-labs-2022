#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"

uint64
sys_exit(void)
{
  int n;
  argint(0, &n);
  exit(n);
  return 0;  // not reached
}

uint64
sys_getpid(void)
{
  return myproc()->pid;
}

uint64
sys_fork(void)
{
  return fork();
}

uint64
sys_wait(void)
{
  uint64 p;
  argaddr(0, &p);
  return wait(p);
}

uint64
sys_sbrk(void)
{
  uint64 addr;
  int n;

  argint(0, &n);
  addr = myproc()->sz;
  if(growproc(n) < 0)
    return -1;
  return addr;
}

uint64
sys_sleep(void)
{
  backtrace();
  int n;
  uint ticks0;

  argint(0, &n);
  if(n < 0)
    n = 0;
  acquire(&tickslock);
  ticks0 = ticks;
  while(ticks - ticks0 < n){
    if(killed(myproc())){
      release(&tickslock);
      return -1;
    }
    sleep(&ticks, &tickslock);
  }
  release(&tickslock);
  return 0;
}

uint64
sys_kill(void)
{
  int pid;

  argint(0, &pid);
  return kill(pid);
}

// return how many clock tick interrupts have occurred
// since start.
uint64
sys_uptime(void)
{
  uint xticks;

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);
  return xticks;
}

uint64
sys_sigreturn()
{
  struct proc *p = myproc();
  intr_off();
  // if (p->alarm == 0)
  // {
  //   intr_on();
  //   return -1;
  // }
  p->alarm = 0;
  restore_alarmcontext(p);
  intr_on();
  return p->trapframe->a0;
}

uint64
sys_sigalarm()
{
  void (*handler)(void);
  int interval;
  argint(0, &interval);
  argaddr(1, (uint64 *)&handler);
  struct proc *p = myproc();
  intr_off();

  if (interval == 0)
  {
    p->interval = 0;
    p->elapse = 0;
    p->handler = 0;
    p->alarm = 0;
    intr_on();
    return 0;
  }
  if (p->alarm == 1)
  {
    intr_on();
    return -1;
  }
  pte_t *pte = walk(p->pagetable, (uint64)handler, 0);
  if (*pte & PTE_V && *pte & PTE_X && *pte & PTE_U && interval > 0)
  {
    p->interval = interval;
    p->elapse = interval;
    p->handler = handler;
    intr_on();
    return 0;
  }
  else
  {
    intr_on();
    return -1;
  }
}
