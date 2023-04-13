// Buffer cache.
//
// The buffer cache is a linked list of buf structures holding
// cached copies of disk block contents.  Caching disk blocks
// in memory reduces the number of disk reads and also provides
// a synchronization point for disk blocks used by multiple processes.
//
// Interface:
// * To get a buffer for a particular disk block, call bread.
// * After changing buffer data, call bwrite to write it to disk.
// * When done with the buffer, call brelse.
// * Do not use the buffer after calling brelse.
// * Only one process at a time can use a buffer,
//     so do not keep them longer than necessary.


#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "buf.h"

struct {
  struct spinlock lock;
  struct buf buf[NBUF];

  // Linked list of all buffers, through prev/next.
  // Sorted by how recently the buffer was used.
  // head.next is most recent, head.prev is least.
} bcache;

struct {
  struct buf * buf;
  struct spinlock lock;
} bentry[NBUCKET];

void
binit(void)
{
  struct buf *b;
  int i;
  initlock(&bcache.lock, "bcache");
  for (i = 0; i < NBUCKET; i++)
  {
    initlock(&bentry[i].lock, "bcache");
    bentry->buf = 0;
  }

  // Create linked list of buffers
  for (b = bcache.buf; b < bcache.buf + NBUF; b++){
    initsleeplock(&b->lock, "buffer");
    b->next = bentry[b->blockno % NBUCKET].buf;
    bentry[b->blockno % NBUCKET].buf = b;
  }
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b, *pre;
  int slot = blockno % NBUCKET;
  int i, pos;
  acquire(&bentry[slot].lock);
  b = bentry[slot].buf;
  while (b != 0)
  {
    if (b->dev == dev && b->blockno == blockno)
    {
      // find if block already in buf
      // can not find another empty block
      // if so one block might have multiple copies in buf
      b->refcnt++;
      release(&bentry[slot].lock);
      acquiresleep(&b->lock);
      return b;
    }
    b =b->next;
  }
  release(&bentry[slot].lock);

  acquire(&bcache.lock);
  for (i = 0; i < NBUF; i++)
  {
    pos = bcache.buf[i].blockno % NBUCKET;
    acquire(&bentry[pos].lock);
    if (bcache.buf[i].refcnt == 0)
    {
      b = bentry[pos].buf;
      pre = 0;
      while (b != 0)
      {
        if (b == &bcache.buf[i])
          break;
        pre = b;
        b = b->next;
      }
      if (b && pre)
      {
        pre->next = b->next;
      }
      else if (b)
      {
        bentry[pos].buf = b->next;
      }
      else
      {
        panic("unexpected behavior in bget()");
      }
      release(&bentry[pos].lock);
      acquire(&bentry[slot].lock);
      struct buf *p = bentry[slot].buf;
      while (p)
      {
        if (p->blockno == blockno && p->dev == dev)
        {
          p->refcnt++;
          break;
        }
        p = p->next;
      }
      b->next = bentry[slot].buf;
      bentry[slot].buf = b;
      if (p)
      {
        release(&bentry[slot].lock);
        release(&bcache.lock);
        acquiresleep(&p->lock);
        return p;
      }
      b->blockno = blockno;
      b->dev = dev;
      b->valid = 0;
      b->refcnt = 1;
      release(&bentry[slot].lock);
      release(&bcache.lock);

      acquiresleep(&b->lock);
      return b;
    }
    release(&bentry[pos].lock);
  }
  panic("bget: no buffer");
}

// Return a locked buf with the contents of the indicated block.
struct buf*
bread(uint dev, uint blockno)
{
  struct buf *b;

  b = bget(dev, blockno);
  if(!b->valid) {
    virtio_disk_rw(b, 0);
    b->valid = 1;
  }
  return b;
}

// Write b's contents to disk.  Must be locked.
void
bwrite(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("bwrite");
  virtio_disk_rw(b, 1);
}

// Release a locked buffer.
// Move to the head of the most-recently-used list.
void
brelse(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("brelse");

  releasesleep(&b->lock);

  int slot = b->blockno % NBUCKET;
  acquire(&bentry[slot].lock);
  b->refcnt--;
  release(&bentry[slot].lock);
}

void
bpin(struct buf *b) {
  int slot = b->blockno % NBUCKET;
  acquire(&bentry[slot].lock);
  b->refcnt++;
  release(&bentry[slot].lock);
}

void
bunpin(struct buf *b) {
  int slot = b->blockno % NBUCKET;
  acquire(&bentry[slot].lock);
  b->refcnt--;
  release(&bentry[slot].lock);
}


