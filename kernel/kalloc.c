// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  struct run *freelist;
} kmem[NCPU];

void
kinit()
{
  int i;
  char buf[6];
  for (i = 0; i < NCPU; i++)
  {
    snprintf(buf, sizeof(buf), "kmem%d", i);
    initlock(&kmem[i].lock, buf);
  }
  freerange(end, (void *)PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
    kfree(p);
}

// Free the page of physical memory pointed at by pa,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  struct run *r;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run *)pa;
  push_off();
  int id = cpuid();
  acquire(&kmem[id].lock);
  r->next = kmem[id].freelist;
  kmem[id].freelist = r;
  release(&kmem[id].lock);
  pop_off();
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r, temp, *rp;
  int i, j;
  push_off();
  int id = cpuid();
  acquire(&kmem[id].lock);
  r = kmem[id].freelist;
  if(r)
    kmem[id].freelist = r->next;
  release(&kmem[id].lock);
  if (r)
  {
    pop_off();
    memset((char *)r, 5, PGSIZE); // fill with junk
    return (void *)r;
  }
  for (i = 0; i < NCPU; i++)
  {
    acquire(&kmem[i].lock);
    temp.next = 0;
    for (j = 0; j < NFETCH; j++){
      rp = kmem[i].freelist;
      if(rp){
        kmem[i].freelist = rp->next;
        rp->next = temp.next;
        temp.next = rp;
      }else {
        break;
      }
    }
    release(&kmem[i].lock);

    if ((rp = temp.next) == 0)
    {
      continue;
    }

    acquire(&kmem[id].lock);
    while (rp->next)
    {
      rp = rp->next;
    }
    rp->next = kmem[id].freelist;
    kmem[id].freelist = temp.next;
    r = kmem[id].freelist;
    kmem[id].freelist = r->next;
    release(&kmem[id].lock);
    pop_off();
    if (r)
    {
      memset((char *)r, 5, PGSIZE);
      return (void *)r;
    }
    panic("kalloc : unexpected behavior");
  }
  pop_off();
  return (void *)r;
}
