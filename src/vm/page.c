#include "vm/page.h"
#include <stdio.h>
#include <string.h>
#include <hash.h>
#include <bitmap.h>
#include "threads/thread.h"
#include "threads/palloc.h"
#include "threads/malloc.h"
#include "threads/vaddr.h"
#include "userprog/pagedir.h"
#include "devices/block.h"

#define BLOCK_PER_PAGE (PGSIZE / BLOCK_SECTOR_SIZE)

static struct hash frame_hash_table;
/* 保护frame的锁 */
struct lock frame_lock;
/* swap的bitmap和block的锁 */
static struct lock swap_lock;
static struct block *swap_block;
static struct bitmap *swap_bitmap;
static size_t swap_maxnum;

void frame_init()
{
    lock_init(&frame_lock);
    hash_init(&frame_hash_table, frame_hash_func, frame_less_func, NULL);
}

void *get_frame(enum palloc_flags flag)
{
    void *p = palloc_get_page(PAL_USER | flag);
    if (p == NULL) // allocate failed
    {
        printf("palloc_get_page failed\n");
        return NULL;
    }

    lock_acquire(&frame_lock);
    struct frame_entry *fhte;
    fhte = (struct frame_entry *)malloc(sizeof(struct frame_entry));
    if (fhte == NULL)
    {
        palloc_free_page(p);
        PANIC("malloc failed in get_frame()\n");
        return NULL;
    }

    fhte->t = thread_current();
    fhte->kernel_addr = p;
    fhte->user_vaddr = p; // user_vaddr应作修改

    hash_insert(&frame_hash_table, &fhte->elem);
    lock_release(&frame_lock);

    return p;
}

void free_frame(void *p)
{
    ASSERT((uint32_t)p % PGSIZE == 0);
    palloc_free_page(p);

    lock_acquire(&frame_lock);
    struct frame_entry temp;
    temp.kernel_addr = p; // 应作修改
    struct hash_elem *h = hash_find(&frame_hash_table, &temp.elem);
    ASSERT(h);
    struct frame_entry *fhte = hash_entry(h, struct frame_entry, elem);
    hash_delete(&frame_hash_table, &fhte->elem);
    lock_release(&frame_lock);
    ASSERT(p == fhte->user_vaddr);
    free(fhte);
}

bool spt_set_page(struct thread *t, void *upage)
{
    ASSERT((uint32_t)upage % PGSIZE == 0);
    struct spt_entry *spte;
    // spt通过hash_destroy()来free spte
    spte = (struct spt_entry *)malloc(sizeof(struct spt_entry));

    spte->user_vaddr = upage;
    spte->status = IN_USE;
    // hash_insert没有重复条目时return NULL，成功插入
    if (hash_insert(t->spt, &spte->elem) == NULL)
    {
        return true;
    }
    else // spt已经有相同条目
    {
        free(spte);
        PANIC("already has same entry in spt\n");
        return false;
    }
}

struct spt_entry *spt_lookup(struct hash *spt, void *upage)
{
    struct spt_entry temp;
    struct hash_elem *e;
    temp.user_vaddr = upage;
    e = hash_find(spt, &temp.elem);
    return e != NULL ? hash_entry(e, struct spt_entry, elem) : NULL;
}

bool activate_page(struct thread *t, void *upage)
{
    struct hash *spt = t->spt;
    uint32_t *pagedir = t->pagedir;
    struct spt_entry *spte;
    spte = spt_lookup(spt, upage);
    if (spte == NULL) // spt中没有该条目
        return false;
    void *frame = get_frame(PAL_USER);
    if (frame == NULL)
        return false;
    switch (spte->status)
    {
    case IN_USE:
        PANIC("activating an active page\n");
        break;
    case DEMAND_ZERO:
        memset(frame, 0, PGSIZE);
        break;
    case SWAPPED_OUT:
        printf("swapped out\n");
        break;
    default:
        PANIC("spte not being properly initialized\n");
        break;
    }
    if (!pagedir_set_page(pagedir, upage, frame, true))
    {
        PANIC("pagedir_set_page failed");
        free_frame(frame);
        return false;
    }

    spte->status = IN_USE;
    pagedir_set_dirty(pagedir, frame, false);

    return true;
}

/* 获取swap_block，创建bitmap */
void swap_init()
{
    lock_init(&swap_lock);
    swap_block = block_get_role(BLOCK_SWAP);
    ASSERT(swap_block != NULL);

    swap_maxnum = block_size(swap_block) / BLOCK_PER_PAGE;
    swap_bitmap = bitmap_create(swap_maxnum);
    ASSERT(swap_bitmap != NULL);
    bitmap_set_all(swap_bitmap, true);
}

uint32_t swap_out(void *kpage)
{
    lock_acquire(&swap_lock);
    uint32_t index = bitmap_scan(swap_bitmap, 0, 1, true);
    ASSERT(index != BITMAP_ERROR);
    for (uint32_t i = 0; i < BLOCK_PER_PAGE; i++)
    {
        block_write(swap_block,
                    index * BLOCK_PER_PAGE + i,
                    (int8_t *)kpage + i * BLOCK_SECTOR_SIZE);
    }
    bitmap_set(swap_bitmap, index, false);
    lock_release(&swap_lock);
    return index;
}

void swap_in(uint32_t index, void *kpage)
{
    ASSERT(index < swap_maxnum);
    lock_acquire(&swap_lock);
    if (bitmap_test(swap_bitmap, index))
    {
        PANIC("swap in from an empty block\n");
    }
    for (uint32_t i = 0; i < BLOCK_PER_PAGE; ++i)
    {
        block_read(swap_block,
                   index * BLOCK_PER_PAGE + i,
                   (int8_t *)kpage + i * BLOCK_SECTOR_SIZE);
    }
    bitmap_set(swap_bitmap, index, true);
    lock_release(&swap_lock);
}

void swap_free(uint32_t index)
{
    ASSERT(index < swap_maxnum);
    lock_acquire(&swap_lock);
    if (bitmap_test(swap_bitmap, index))
    {
        PANIC("freeing a empty block\n");
    }
    bitmap_set(swap_bitmap, index, true);
    lock_release(&swap_lock);
}

unsigned frame_hash_func(const struct hash_elem *elem, void *aux UNUSED)
{
    struct frame_entry *fe = hash_entry(elem, struct frame_entry, elem);
    return hash_bytes(&fe->kernel_addr, sizeof(void *));
}

bool frame_less_func(const struct hash_elem *left,
                     const struct hash_elem *right, void *aux UNUSED)
{
    struct frame_entry *l_fe = hash_entry(left, struct frame_entry, elem);
    struct frame_entry *r_fe = hash_entry(right, struct frame_entry, elem);
    return (uint32_t)(l_fe->kernel_addr) < (uint32_t)(r_fe->kernel_addr);
}

unsigned spte_hash_func(const struct hash_elem *elem, void *aux UNUSED)
{
    struct spt_entry *spte = hash_entry(elem, struct spt_entry, elem);
    return hash_bytes(&spte->user_vaddr, sizeof(void *));
}

bool spte_less_func(const struct hash_elem *left,
                    const struct hash_elem *right, void *aux UNUSED)
{
    struct spt_entry *l_spte = hash_entry(left, struct spt_entry, elem);
    struct spt_entry *r_spte = hash_entry(right, struct spt_entry, elem);
    return (uint32_t)(l_spte->user_vaddr) < (uint32_t)(r_spte->user_vaddr);
}

void spte_destroy_func(struct hash_elem *elem, void *aux UNUSED)
{
    struct spt_entry *spte = hash_entry(elem, struct spt_entry, elem);
    free(spte);
}