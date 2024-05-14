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

static struct lock swap_lock;      /*swap的锁 */
static struct block *swap_block;   /* swap的block*/
static struct bitmap *swap_bitmap; /* swap的bitmap*/
static size_t swap_maxnum;

void frame_init()
{
    lock_init(&frame_lock);
    hash_init(&frame_hash_table, frame_hash_func, frame_less_func, NULL);
}

void *get_frame(enum palloc_flags flag, void *upage)
{
    ASSERT(lock_held_by_current_thread(&frame_lock));
    ASSERT((uint32_t)upage % PGSIZE == 0);

    void *kpage = palloc_get_page(PAL_USER | flag);
    if (kpage == NULL)
    {
        PANIC("palloc_get_page failed\n");
        return NULL;
    }

    struct frame_entry *fe;
    fe = (struct frame_entry *)malloc(sizeof(struct frame_entry));
    if (fe == NULL)
    {
        palloc_free_page(kpage);
        PANIC("malloc failed in get_frame()\n");
        return NULL;
    }

    fe->t = thread_current();
    fe->kpage = kpage;
    fe->upage = upage; // upage应作修改

    hash_insert(&frame_hash_table, &fe->elem);

    return kpage;
}

void free_frame(void *kpage)
{
    ASSERT((uint32_t)kpage % PGSIZE == 0);
    lock_acquire(&frame_lock);
    palloc_free_page(kpage);

    struct frame_entry temp;
    temp.kpage = kpage;
    struct hash_elem *h = hash_find(&frame_hash_table, &temp.elem);
    ASSERT(h);
    struct frame_entry *fe = hash_entry(h, struct frame_entry, elem);
    hash_delete(&frame_hash_table, &fe->elem);
    lock_release(&frame_lock);
    free(fe);
}

/* 向spt添加条目 */
bool spt_add_page(struct thread *t, void *upage, enum page_status status)
{
    ASSERT((uint32_t)upage % PGSIZE == 0);
    struct spt_entry *spte;
    // spt通过hash_destroy()来free spte
    spte = (struct spt_entry *)malloc(sizeof(struct spt_entry));

    spte->upage = upage;
    spte->status = status;
    spte->writable = true;
    spte->swap_index = INT32_MAX;
    if (hash_insert(t->spt, &spte->elem) == NULL)
    {
        // hash_insert没有重复条目时return NULL，此时成功插入
        return true;
    }
    else // spt已经有相同条目
    {
        free(spte);
        PANIC("already has same entry in spt\n");
        return false;
    }
}

/* 在thread t->spt中寻找页，如果失败则返回NULL */
struct spt_entry *spt_lookup(struct thread *t, void *upage)
{
    struct hash *spt = t->spt;
    struct spt_entry temp;
    temp.upage = upage;
    struct hash_elem *e = hash_find(spt, &temp.elem);
    return e != NULL ? hash_entry(e, struct spt_entry, elem) : NULL;
}

/* 依据thread t->spt, 激活一个not exist的页, 向页表添加条目*/
bool activate_page(struct thread *t, void *upage)
{
    lock_acquire(&frame_lock);
    uint32_t *pagedir = t->pagedir;
    struct spt_entry *spte;
    spte = spt_lookup(t, upage);
    if (spte == NULL) // spt中没有该条目
    {
        PANIC("activating a page not in spt");
        return false;
    }
    void *frame;
    if (!(frame = get_frame(PAL_USER, upage))) // 需要腾出空间
    {
        page_daemon();
    }
    if (!frame)
    {
        PANIC("can't get frame\n");
    }

    switch (spte->status)
    {
    case IN_USE:
        PANIC("activating an active page\n");
        break;
    case DEMAND_ZERO:
        memset(frame, 0, PGSIZE);
        break;
    case SWAPPED_OUT:
        swap_in(spte->swap_index, frame);
        break;
    default:
        PANIC("spte not being properly initialized\n");
        break;
    }
    if (pagedir_get_page(t->pagedir, upage))
    {
        PANIC("upage %p in pagedir has been occupied\n", upage);
    }
    if (!pagedir_set_page(pagedir, upage, frame, true))
    {
        free_frame(frame);
        PANIC("pagedir_set_page failed");
        return false;
    }
    spte->status = IN_USE;
    spte->swap_index = INT32_MAX;
    pagedir_set_dirty(pagedir, frame, false);
    lock_release(&frame_lock);

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
    bitmap_set_all(swap_bitmap, false);
}

/* 把kpage放到swap block里 */
uint32_t swap_out(void *kpage)
{
    lock_acquire(&swap_lock);
    uint32_t index = bitmap_scan(swap_bitmap, 0, 1, false);
    ASSERT(index != BITMAP_ERROR);
    for (uint32_t i = 0; i < BLOCK_PER_PAGE; i++)
    {
        block_write(swap_block,
                    index * BLOCK_PER_PAGE + i,
                    (int8_t *)kpage + i * BLOCK_SECTOR_SIZE);
    }
    bitmap_set(swap_bitmap, index, true);
    lock_release(&swap_lock);
    return index;
}

/* 从swap_block拿到kpage */
void swap_in(uint32_t index, void *kpage)
{
    ASSERT(index < swap_maxnum);
    lock_acquire(&swap_lock);
    if (!bitmap_test(swap_bitmap, index))
    {
        PANIC("swap in from an empty block\n");
    }
    for (uint32_t i = 0; i < BLOCK_PER_PAGE; ++i)
    {
        block_read(swap_block,
                   index * BLOCK_PER_PAGE + i,
                   (int8_t *)kpage + i * BLOCK_SECTOR_SIZE);
    }
    bitmap_set(swap_bitmap, index, false);
    lock_release(&swap_lock);
}

void swap_free(uint32_t index)
{
    ASSERT(index < swap_maxnum);
    lock_acquire(&swap_lock);
    if (!bitmap_test(swap_bitmap, index))
    {
        PANIC("freeing an empty block\n");
    }
    bitmap_set(swap_bitmap, index, false);
    lock_release(&swap_lock);
}

/* swap_out一个页，并处理spt、fht、pagedir(必须持有frame_lock) */
void page_daemon()
{
    ASSERT(lock_held_by_current_thread(&frame_lock));
    size_t n = hash_size(&frame_hash_table);
    static unsigned prng = 1;
    prng = prng * 1664525u + 1013904223u;
    size_t pointer = prng % n;

    struct hash_iterator it;
    hash_first(&it, &frame_hash_table);
    size_t i;
    for (i = 0; i <= pointer; ++i)
        hash_next(&it);

    struct frame_entry *fe;
    fe = hash_entry(hash_cur(&it), struct frame_entry, elem);
    /*先处理pagedir，这样将要被evict的页所属的线程再访问这个页的时候会fault，
    之后进入activate_page等待frame_lock，实现了只要一个页被选中要驱逐，对其的访问都会被阻塞*/
    pagedir_clear_page(fe->t->pagedir, fe->upage);
    uint32_t index = swap_out(fe->kpage);

    // 处理spt
    struct spt_entry temp;
    temp.upage = fe->upage;
    struct hash_elem *e = hash_find(fe->t->spt, &temp.elem);
    struct spt_entry *spte = hash_entry(e, struct spt_entry, elem);
    spte->status = SWAPPED_OUT;
    spte->swap_index = index;

    // 处理fht
    if (!hash_delete(&frame_hash_table, &fe->elem))
    {
        PANIC("can't find fe\n");
    }
    free(fe);
}

unsigned frame_hash_func(const struct hash_elem *elem, void *aux UNUSED)
{
    struct frame_entry *fe = hash_entry(elem, struct frame_entry, elem);
    return hash_bytes(&fe->kpage, sizeof(void *));
}

bool frame_less_func(const struct hash_elem *left,
                     const struct hash_elem *right, void *aux UNUSED)
{
    struct frame_entry *l_fe = hash_entry(left, struct frame_entry, elem);
    struct frame_entry *r_fe = hash_entry(right, struct frame_entry, elem);
    return (uint32_t)(l_fe->kpage) < (uint32_t)(r_fe->kpage);
}

unsigned spte_hash_func(const struct hash_elem *elem, void *aux UNUSED)
{
    struct spt_entry *spte = hash_entry(elem, struct spt_entry, elem);
    return hash_bytes(&spte->upage, sizeof(void *));
}

bool spte_less_func(const struct hash_elem *left,
                    const struct hash_elem *right, void *aux UNUSED)
{
    struct spt_entry *l_spte = hash_entry(left, struct spt_entry, elem);
    struct spt_entry *r_spte = hash_entry(right, struct spt_entry, elem);
    return (uint32_t)(l_spte->upage) < (uint32_t)(r_spte->upage);
}

void spte_destroy_func(struct hash_elem *elem, void *aux UNUSED)
{
    struct spt_entry *spte = hash_entry(elem, struct spt_entry, elem);
    free(spte);
}