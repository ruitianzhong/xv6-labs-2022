#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "sleeplock.h"
#include "fs.h"
#include "file.h"
#include "fcntl.h"

struct cpu cpus[NCPU];

struct proc proc[NPROC];

struct vma_struct vma[NVMA];

struct proc *initproc;

int nextpid = 1;
struct spinlock pid_lock;

extern void forkret(void);
static void freeproc(struct proc *p);
void free_mmap_page(uint64, uint64, struct inode *, int, int);
uint64 min(uint64, uint64);
uint64 max(uint64, uint64);

extern char trampoline[]; // trampoline.S

// helps ensure that wakeups of wait()ing
// parents are not lost. helps obey the
// memory model when using p->parent.
// must be acquired before any p->lock.
struct spinlock wait_lock;

// Allocate a page for each process's kernel stack.
// Map it high in memory, followed by an invalid
// guard page.
void
proc_mapstacks(pagetable_t kpgtbl)
{
  struct proc *p;
  
  for(p = proc; p < &proc[NPROC]; p++) {
    char *pa = kalloc();
    if(pa == 0)
      panic("kalloc");
    uint64 va = KSTACK((int) (p - proc));
    kvmmap(kpgtbl, va, (uint64)pa, PGSIZE, PTE_R | PTE_W);
  }
}

// initialize the proc table.
void
procinit(void)
{
  struct proc *p;
  
  initlock(&pid_lock, "nextpid");
  initlock(&wait_lock, "wait_lock");
  for(p = proc; p < &proc[NPROC]; p++) {
      initlock(&p->lock, "proc");
      p->state = UNUSED;
      p->kstack = KSTACK((int) (p - proc));
  }
}
void 
vmainit(void){
  struct vma_struct *vp;

  for (vp = vma; vp < &vma[NVMA]; vp++)
  {
      initlock(&vp->lock, "vma_struct");
      vp->file = 0;
      vp->used = 0;
  }
}

static struct vma_struct *
allocvma(void){
  struct vma_struct *vp;
  for (vp = vma; vp < &vma[NVMA]; vp++)
  {
      acquire(&vp->lock);
      if (vp->used == 0)
      {
      vp->file = 0;
      vp->flags = 0;
      vp->addr = MAPFAIL;
      vp->length = 0;
      vp->prot = 0;
      vp->used = 1;
      vp->offset = 0;
      release(&vp->lock);
      return vp;
      }
      release(&vp->lock);
  }
  return 0;
}

static void
freevma(struct vma_struct *vp)
{
  fileclose(vp->file); // important to avoid tricky bug
  vp->addr  = MAPFAIL;
  vp->file  = 0;
  vp->flags = 0;
  vp ->prot = 0;
  vp ->length = 0;
  vp->used = 1;
  vp->offset = 0;
}

// Must be called with interrupts disabled,
// to prevent race with process being moved
// to a different CPU.
int
cpuid()
{
  int id = r_tp();
  return id;
}

// Return this CPU's cpu struct.
// Interrupts must be disabled.
struct cpu*
mycpu(void)
{
  int id = cpuid();
  struct cpu *c = &cpus[id];
  return c;
}

// Return the current struct proc *, or zero if none.
struct proc*
myproc(void)
{
  push_off();
  struct cpu *c = mycpu();
  struct proc *p = c->proc;
  pop_off();
  return p;
}

int
allocpid()
{
  int pid;
  
  acquire(&pid_lock);
  pid = nextpid;
  nextpid = nextpid + 1;
  release(&pid_lock);

  return pid;
}

// Look in the process table for an UNUSED proc.
// If found, initialize state required to run in the kernel,
// and return with p->lock held.
// If there are no free procs, or a memory allocation fails, return 0.
static struct proc*
allocproc(void)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if(p->state == UNUSED) {
      goto found;
    } else {
      release(&p->lock);
    }
  }
  return 0;

found:
  p->pid = allocpid();
  p->state = USED;
  p->nvma = 0;

  // Allocate a trapframe page.
  if((p->trapframe = (struct trapframe *)kalloc()) == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // An empty user page table.
  p->pagetable = proc_pagetable(p);
  if(p->pagetable == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // Set up new context to start executing at forkret,
  // which returns to user space.
  memset(&p->context, 0, sizeof(p->context));
  p->context.ra = (uint64)forkret;
  p->context.sp = p->kstack + PGSIZE;

  return p;
}

// free a proc structure and the data hanging from it,
// including user pages.
// p->lock must be held.
static void
freeproc(struct proc *p)
{
  if(p->trapframe)
    kfree((void*)p->trapframe);
  p->trapframe = 0;
  if(p->pagetable)
    proc_freepagetable(p->pagetable, p->sz);
  p->pagetable = 0;
  p->sz = 0;
  p->pid = 0;
  p->parent = 0;
  p->name[0] = 0;
  p->chan = 0;
  p->killed = 0;
  p->xstate = 0;
  p->state = UNUSED;
}

// Create a user page table for a given process, with no user memory,
// but with trampoline and trapframe pages.
pagetable_t
proc_pagetable(struct proc *p)
{
  pagetable_t pagetable;

  // An empty page table.
  pagetable = uvmcreate();
  if(pagetable == 0)
    return 0;

  // map the trampoline code (for system call return)
  // at the highest user virtual address.
  // only the supervisor uses it, on the way
  // to/from user space, so not PTE_U.
  if(mappages(pagetable, TRAMPOLINE, PGSIZE,
              (uint64)trampoline, PTE_R | PTE_X) < 0){
    uvmfree(pagetable, 0);
    return 0;
  }

  // map the trapframe page just below the trampoline page, for
  // trampoline.S.
  if(mappages(pagetable, TRAPFRAME, PGSIZE,
              (uint64)(p->trapframe), PTE_R | PTE_W) < 0){
    uvmunmap(pagetable, TRAMPOLINE, 1, 0);
    uvmfree(pagetable, 0);
    return 0;
  }

  return pagetable;
}

// Free a process's page table, and free the
// physical memory it refers to.
void
proc_freepagetable(pagetable_t pagetable, uint64 sz)
{
  uvmunmap(pagetable, TRAMPOLINE, 1, 0);
  uvmunmap(pagetable, TRAPFRAME, 1, 0);
  uvmfree(pagetable, sz);
}

// a user program that calls exec("/init")
// assembled from ../user/initcode.S
// od -t xC ../user/initcode
uchar initcode[] = {
  0x17, 0x05, 0x00, 0x00, 0x13, 0x05, 0x45, 0x02,
  0x97, 0x05, 0x00, 0x00, 0x93, 0x85, 0x35, 0x02,
  0x93, 0x08, 0x70, 0x00, 0x73, 0x00, 0x00, 0x00,
  0x93, 0x08, 0x20, 0x00, 0x73, 0x00, 0x00, 0x00,
  0xef, 0xf0, 0x9f, 0xff, 0x2f, 0x69, 0x6e, 0x69,
  0x74, 0x00, 0x00, 0x24, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00
};

// Set up first user process.
void
userinit(void)
{
  struct proc *p;

  p = allocproc();
  initproc = p;
  
  // allocate one user page and copy initcode's instructions
  // and data into it.
  uvmfirst(p->pagetable, initcode, sizeof(initcode));
  p->sz = PGSIZE;

  // prepare for the very first "return" from kernel to user.
  p->trapframe->epc = 0;      // user program counter
  p->trapframe->sp = PGSIZE;  // user stack pointer

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  p->state = RUNNABLE;

  release(&p->lock);
}

// Grow or shrink user memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint64 sz;
  struct proc *p = myproc();

  sz = p->sz;
  if(n > 0){
    if((sz = uvmalloc(p->pagetable, sz, sz + n, PTE_W)) == 0) {
      return -1;
    }
  } else if(n < 0){
    sz = uvmdealloc(p->pagetable, sz, sz + n);
  }
  p->sz = sz;
  return 0;
}
void copy_vma(struct vma_struct *old_vp, struct vma_struct *new_vp)
{
  new_vp->addr = old_vp->addr;
  new_vp->file = filedup(old_vp->file);
  new_vp->length = old_vp->length;
  new_vp->flags = old_vp->flags;
  new_vp->prot = old_vp->prot;
  new_vp->offset = old_vp->offset;
}
// Create a new process, copying the parent.
// Sets up child kernel stack to return as if from fork() system call.
int
fork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *p = myproc();
  struct vma_struct *vp;

  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }
  for (i = 0; i < p->nvma; i++)
  {
    if ((vp = allocvma()) == 0)
    {
      for (int j = 0; j < i - 1; j++)
      {
        freevma(np->vma[j]);
      }
      freeproc(np);
      release(&np->lock);
      return -1;
    }
    np->vma[i] = vp;
    copy_vma(p->vma[i], np->vma[i]);
  }
  np->nvma = p->nvma;

  // Copy user memory from parent to child.
  if(uvmcopy(p->pagetable, np->pagetable, p->sz) < 0){
    freeproc(np);
    release(&np->lock);
    return -1;
  }
  np->sz = p->sz;

  // copy saved user registers.
  *(np->trapframe) = *(p->trapframe);

  // Cause fork to return 0 in the child.
  np->trapframe->a0 = 0;

  // increment reference counts on open file descriptors.
  for(i = 0; i < NOFILE; i++)
    if(p->ofile[i])
      np->ofile[i] = filedup(p->ofile[i]);
  np->cwd = idup(p->cwd);

  safestrcpy(np->name, p->name, sizeof(p->name));

  pid = np->pid;

  release(&np->lock);

  acquire(&wait_lock);
  np->parent = p;
  release(&wait_lock);

  acquire(&np->lock);
  np->state = RUNNABLE;
  release(&np->lock);

  return pid;
}

// Pass p's abandoned children to init.
// Caller must hold wait_lock.
void
reparent(struct proc *p)
{
  struct proc *pp;

  for(pp = proc; pp < &proc[NPROC]; pp++){
    if(pp->parent == p){
      pp->parent = initproc;
      wakeup(initproc);
    }
  }
}
static void
free_all_vma_page(struct vma_struct *vp)
{
  uint64 start = vp->addr, end = PGROUNDUP(vp->addr + vp->length), a;
  for (a = start; a < end; a += PGSIZE)
  {
    free_mmap_page(start, PGSIZE, vp->file->ip, vp->flags, vp->offset);
  }
}
// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait().
void
exit(int status)
{
  struct proc *p = myproc();

  if(p == initproc)
    panic("init exiting");

  // Close all open files.
  for(int fd = 0; fd < NOFILE; fd++){
    if(p->ofile[fd]){
      struct file *f = p->ofile[fd];
      fileclose(f);
      p->ofile[fd] = 0;
    }
  }
  int nvma = p->nvma;
  for (int i = 0; i < nvma; i++)
  {
    free_all_vma_page(p->vma[i]);
    freevma(p->vma[i]);
    p->vma[i] = 0;
    p->nvma--;
  }

  begin_op();
  iput(p->cwd);
  end_op();
  p->cwd = 0;

  acquire(&wait_lock);

  // Give any children to init.
  reparent(p);

  // Parent might be sleeping in wait().
  wakeup(p->parent);
  
  acquire(&p->lock);

  p->xstate = status;
  p->state = ZOMBIE;

  release(&wait_lock);

  // Jump into the scheduler, never to return.
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(uint64 addr)
{
  struct proc *pp;
  int havekids, pid;
  struct proc *p = myproc();

  acquire(&wait_lock);

  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(pp = proc; pp < &proc[NPROC]; pp++){
      if(pp->parent == p){
        // make sure the child isn't still in exit() or swtch().
        acquire(&pp->lock);

        havekids = 1;
        if(pp->state == ZOMBIE){
          // Found one.
          pid = pp->pid;
          if(addr != 0 && copyout(p->pagetable, addr, (char *)&pp->xstate,
                                  sizeof(pp->xstate)) < 0) {
            release(&pp->lock);
            release(&wait_lock);
            return -1;
          }
          freeproc(pp);
          release(&pp->lock);
          release(&wait_lock);
          return pid;
        }
        release(&pp->lock);
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || killed(p)){
      release(&wait_lock);
      return -1;
    }
    
    // Wait for a child to exit.
    sleep(p, &wait_lock);  //DOC: wait-sleep
  }
}

// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run.
//  - swtch to start running that process.
//  - eventually that process transfers control
//    via swtch back to the scheduler.
void
scheduler(void)
{
  struct proc *p;
  struct cpu *c = mycpu();
  
  c->proc = 0;
  for(;;){
    // Avoid deadlock by ensuring that devices can interrupt.
    intr_on();

    for(p = proc; p < &proc[NPROC]; p++) {
      acquire(&p->lock);
      if(p->state == RUNNABLE) {
        // Switch to chosen process.  It is the process's job
        // to release its lock and then reacquire it
        // before jumping back to us.
        p->state = RUNNING;
        c->proc = p;
        swtch(&c->context, &p->context);

        // Process is done running for now.
        // It should have changed its p->state before coming back.
        c->proc = 0;
      }
      release(&p->lock);
    }
  }
}

// Switch to scheduler.  Must hold only p->lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->noff, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void)
{
  int intena;
  struct proc *p = myproc();

  if(!holding(&p->lock))
    panic("sched p->lock");
  if(mycpu()->noff != 1)
    panic("sched locks");
  if(p->state == RUNNING)
    panic("sched running");
  if(intr_get())
    panic("sched interruptible");

  intena = mycpu()->intena;
  swtch(&p->context, &mycpu()->context);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  struct proc *p = myproc();
  acquire(&p->lock);
  p->state = RUNNABLE;
  sched();
  release(&p->lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch to forkret.
void
forkret(void)
{
  static int first = 1;

  // Still holding p->lock from scheduler.
  release(&myproc()->lock);

  if (first) {
    // File system initialization must be run in the context of a
    // regular process (e.g., because it calls sleep), and thus cannot
    // be run from main().
    first = 0;
    fsinit(ROOTDEV);
  }

  usertrapret();
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void
sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();
  
  // Must acquire p->lock in order to
  // change p->state and then call sched.
  // Once we hold p->lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup locks p->lock),
  // so it's okay to release lk.

  acquire(&p->lock);  //DOC: sleeplock1
  release(lk);

  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;

  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  release(&p->lock);
  acquire(lk);
}

// Wake up all processes sleeping on chan.
// Must be called without any p->lock.
void
wakeup(void *chan)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++) {
    if(p != myproc()){
      acquire(&p->lock);
      if(p->state == SLEEPING && p->chan == chan) {
        p->state = RUNNABLE;
      }
      release(&p->lock);
    }
  }
}

// Kill the process with the given pid.
// The victim won't exit until it tries to return
// to user space (see usertrap() in trap.c).
int
kill(int pid)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++){
    acquire(&p->lock);
    if(p->pid == pid){
      p->killed = 1;
      if(p->state == SLEEPING){
        // Wake process from sleep().
        p->state = RUNNABLE;
      }
      release(&p->lock);
      return 0;
    }
    release(&p->lock);
  }
  return -1;
}

void
setkilled(struct proc *p)
{
  acquire(&p->lock);
  p->killed = 1;
  release(&p->lock);
}

int
killed(struct proc *p)
{
  int k;
  
  acquire(&p->lock);
  k = p->killed;
  release(&p->lock);
  return k;
}

// Copy to either a user address, or kernel address,
// depending on usr_dst.
// Returns 0 on success, -1 on error.
int
either_copyout(int user_dst, uint64 dst, void *src, uint64 len)
{
  struct proc *p = myproc();
  if(user_dst){
    return copyout(p->pagetable, dst, src, len);
  } else {
    memmove((char *)dst, src, len);
    return 0;
  }
}

// Copy from either a user address, or kernel address,
// depending on usr_src.
// Returns 0 on success, -1 on error.
int
either_copyin(void *dst, int user_src, uint64 src, uint64 len)
{
  struct proc *p = myproc();
  if(user_src){
    return copyin(p->pagetable, dst, src, len);
  } else {
    memmove(dst, (char*)src, len);
    return 0;
  }
}

// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
  [UNUSED]    "unused",
  [USED]      "used",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
  };
  struct proc *p;
  char *state;

  printf("\n");
  for(p = proc; p < &proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    printf("%d %s %s", p->pid, state, p->name);
    printf("\n");
  }
}

static uint64
insert_vma(uint64 start, uint64 length, struct file *f, int prot, int flags, int offset)
{
  struct vma_struct **vma = myproc()->vma, *vp;
  int i;
  if (start + length <= start || start + length > TRAMPOLINE || offset + length < length)
  {
    return MAPFAIL;
  }
  struct inode *ip = f->ip;
  ilock(ip);
  if (offset + length > PGROUNDUP(ip->size))
  {
    iunlock(ip);
    return MAPFAIL;
  }
  length = min(length, (uint64)(ip->size - offset));
  iunlock(ip);
  int nvma = myproc()->nvma;
  if (nvma >= NVMA)
  {
    panic("insert_vma");
  }
  for (i = 0; i < nvma; i++)
  {
    if (start + length <= vma[i]->addr)
    {
      break;
    }
    start = PGROUNDUP(vma[i]->addr + vma[i]->length);
  }
  if (i == nvma || (vp = allocvma()) == 0)
  {
    return MAPFAIL;
  }
  for (int j = nvma; j > i; j--)
  {
    vma[j] = vma[j - 1];
  }
  vma[nvma] = vp;
  vp->addr = start;
  vp->length = length;
  vp->file = filedup(f);
  vp->flags = flags;
  vp->prot = prot;
  vp->offset = 0;
  myproc()->nvma++;
  return vp->addr;
}

void free_mmap_page(uint64 addr, uint64 length, struct inode *ip, int flags, int off)
{
  pte_t *pte;
  uint64 pa;
  if ((pte = walk(myproc()->pagetable, addr, 0)) == 0)
    return;
  pa = PTE2PA(*pte);
  if ((*pte & PTE_W) && (flags & MAP_SHARED))
  {
    ilock(ip);
    begin_op();
    writei(ip, 0, pa, off, length);
    iunlock(ip);
    end_op();
  }
  *pte = 0;
  kfree((void *)pa);
  return;
}

uint64
max(uint64 x, uint64 y)
{
  return x > y ? x : y;
}

uint64
min(uint64 x, uint64 y)
{
  return x > y ? y : x;
}
uint64
mmap(uint64 addr, uint64 length, int prot, int flags, struct file *f, int offset)
{
  if (length == 0 || f->type != FD_INODE)
    return MAPFAIL;
  if (!f->writable && (PROT_WRITE & prot))
    return MAPFAIL;
  if (!(flags & MAP_PRIVATE) && !(flags & MAP_SHARED))
    return MAPFAIL;
  struct proc *p = myproc();
  uint64 start = PGROUNDUP(p->sz) + PGSIZE * 5;
  return insert_vma(start, length, f, prot, flags, offset);
}
// All pages containing a part of the indicated  range  are  un‐
// mapped, and subsequent references to these pages will generate SIGSEGV.
// It is not an error if the indicated range does not contain  any  mapped
// pages.    --- man page for munmap
int munmap(uint64 addr, uint64 length)
{
  int nvma = myproc()->nvma;
  struct vma_struct **vma = myproc()->vma;
  int i;
  if (addr % PGSIZE != 0) // see man 2 munmap
    return -1;
  if (addr + length <= addr)
  {
    return -1;
  }
  for (i = 0; i < nvma; i++)
  {
    uint64 start = max(addr, vma[i]->addr),
           end = min(PGROUNDUP(addr + length), PGROUNDUP(vma[i]->addr + vma[i]->length));
    if(start > vma[i]->addr && end<PGROUNDUP(vma[i]->addr+vma[i]->length)){
      return -1;
    }
    uint64 p;
    for (p = start; p < end; p += PGSIZE)
    {
      // free page
      if (end - p == PGSIZE)
      {
        free_mmap_page(p, vma[i]->length - (p - start),
                       vma[i]->file->ip, vma[i]->flags, vma[i]->offset);
      }
      else
      {
        free_mmap_page(p, PGSIZE, vma[i]->file->ip,
                       vma[i]->flags, vma[i]->offset);
      }
    }
    if (start > vma[i]->addr)
    {
      vma[i]->length = start - vma[i]->addr;
    }
    else if (end < PGROUNDUP(vma[i]->addr + vma[i]->length))
    {
      vma[i]->offset += (end - start);
      vma[i]->length -= (end - start);
      vma[i]->addr = end;
    }
    if (start == vma[i]->addr && end == PGROUNDUP(vma[i]->addr + vma[i]->length))
    {
      freevma(vma[i]);
      for (int j = i; j < nvma - 1; j++)
      {
        vma[j] = vma[j + 1];
      }
      nvma--;
      i--;
    }
  }
  myproc()->nvma = nvma;
  return 0;
}