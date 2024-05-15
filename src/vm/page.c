#include "vm/page.h"
#include <stdio.h>
#include <string.h>
#include <hash.h>
#include <bitmap.h>
#include "devices/block.h"
#include "threads/thread.h"
#include "threads/palloc.h"
#include "threads/malloc.h"
#include "threads/vaddr.h"
#include "userprog/pagedir.h"
#include "filesys/file.h"

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
        return NULL;

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
    fe->upage = upage;
    fe->pinned = true;
    hash_insert(&frame_hash_table, &fe->elem);

    return kpage;
}

void free_frame(void *kpage)
{
    ASSERT(lock_held_by_current_thread(&frame_lock));
    ASSERT((uint32_t)kpage % PGSIZE == 0);
    palloc_free_page(kpage);
    frame_table_remove(kpage);
}

/* 仅从fht删掉并free kpage对应的fhte */
void frame_table_remove(void *kpage)
{
    ASSERT(lock_held_by_current_thread(&frame_lock));
    ASSERT((uint32_t)kpage % PGSIZE == 0);

    struct frame_entry temp;
    temp.kpage = kpage;
    struct hash_elem *h = hash_find(&frame_hash_table, &temp.elem);
    struct frame_entry *fhte = hash_entry(h, struct frame_entry, elem);
    hash_delete(&frame_hash_table, &fhte->elem);
    free(fhte);
}

/* 在process将要退出时调用，根据spt清除fhte、并根据spt状态对具体物理页进行操作 */
void free_frame_on_exit()
{
    ASSERT(lock_held_by_current_thread(&frame_lock));
    struct thread *t = thread_current();
    struct hash *spt = t->spt;
    struct spt_entry *spte;
    struct hash_iterator it;
    hash_first(&it, spt);
    while (hash_next(&it))
    {
        spte = hash_entry(hash_cur(&it), struct spt_entry, elem);
        switch (spte->status)
        {
        case IN_USE:
            ASSERT(spte->kpage != NULL);
            // process_exit会根据pagdir来palloc_free_page
            frame_table_remove(spte->kpage);
            break;
        case SWAPPED_OUT:
            // swap_out的时候已经删了fhte
            ASSERT(spte->kpage == NULL);
            swap_free(spte->swap_index);
            break;
        case DEMAND_ZERO:
            ASSERT(spte->kpage == NULL);
            break;
        case LAZY_LOAD:
            ASSERT(spte->kpage == NULL);
            break;
        default:
            PANIC("unknown status of spte\n");
        }
    }
}

/* 必须持有frame_lock */
void frame_unpin(void *kpage)
{
    ASSERT(lock_held_by_current_thread(&frame_lock));

    struct frame_entry temp;
    temp.kpage = kpage;
    struct hash_elem *h = hash_find(&frame_hash_table, &(temp.elem));
    if (h == NULL)
    {
        PANIC("The frame to be unpinned does not exist");
    }

    struct frame_entry *f;
    f = hash_entry(h, struct frame_entry, elem);
    f->pinned = false; // unpin.
}

/* 向spt添加条目 */
bool spt_add_page(struct thread *t, void *upage, enum page_status status, void *kpage)
{
    ASSERT((uint32_t)upage % PGSIZE == 0);
    struct spt_entry *spte;
    // spt通过hash_destroy()来free spte
    spte = (struct spt_entry *)malloc(sizeof(struct spt_entry));
    if (status != IN_USE)
    {
        ASSERT(kpage == NULL);
        spte->kpage = NULL;
    }
    else
    {
        spte->kpage = kpage;
    }
    spte->upage = upage;
    spte->status = status;
    spte->writable = true;
    spte->swap_index = INT32_MAX;
    spte->file = NULL;
    spte->offset = INT32_MAX;
    spte->read_bytes = INT32_MAX;
    spte->zero_bytes = INT32_MAX;
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
    void *kpage;
    if (!(kpage = get_frame(PAL_USER, upage))) // 需要腾出空间
    {
        page_daemon();
        kpage = get_frame(PAL_USER, upage);
    }
    if (!kpage)
    {
        PANIC("can't get frame\n");
    }

    bool filesys_lock_held = true;
    switch (spte->status)
    {
    case IN_USE:
        PANIC("activating an active page\n");
        break;
    case DEMAND_ZERO:
        memset(kpage, 0, PGSIZE);
        break;
    case SWAPPED_OUT:
        swap_in(spte->swap_index, kpage);
        break;
    case LAZY_LOAD:
        if (spte->read_bytes > 0)
        {
            // filesys_lock_held = lock_held_by_current_thread(&filesys_lock);
            // ASSERT(!filesys_lock_held);
            // 有可能是在read过程中触发page_fault到这里的
            if (!filesys_lock_held) // 暂时在这里完全不获取这个锁
            {
                lock_acquire(&filesys_lock);
            }
            file_seek(spte->file, spte->offset);
            uint32_t read_bytes = 0;
            while (read_bytes < spte->read_bytes)
            {
                read_bytes += file_read(spte->file, (uint8_t *)kpage + read_bytes, spte->read_bytes - read_bytes);
            }
            if (!filesys_lock_held)
            {
                lock_release(&filesys_lock);
            }
            memset((int8_t *)kpage + read_bytes, 0, spte->zero_bytes);
        }
        else
        {
            memset(kpage, 0, spte->zero_bytes);
        }
        break;
    default:
        PANIC("spte not being properly initialized\n");
        break;
    }
    if (pagedir_get_page(t->pagedir, upage))
    {
        PANIC("upage %p in pagedir has been occupied\n", upage);
    }
    if (!pagedir_set_page(pagedir, upage, kpage, spte->writable))
    {
        PANIC("pagedir_set_page failed");
    }
    spte->status = IN_USE;
    spte->kpage = kpage;
    spte->swap_index = INT32_MAX;
    pagedir_set_dirty(pagedir, kpage, false);
    frame_unpin(kpage);
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
    struct frame_entry *fe;
    while (true)
    {
        prng = prng * 1664525u + 1013904223u;
        size_t pointer = prng % n;

        struct hash_iterator it;
        hash_first(&it, &frame_hash_table);
        size_t i;
        for (i = 0; i <= pointer; ++i)
            hash_next(&it);

        fe = hash_entry(hash_cur(&it), struct frame_entry, elem);
        if (fe->pinned) // is pinned
        {
            printf("pinned, continue\n");
            continue;
        }
        else // unpinned. evict it!
            break;
    }

    /*先处理pagedir，这样将要被evict的页所属的线程再访问这个页的时候会fault，
    之后进入activate_page等待frame_lock，实现了只要一个页被选中要驱逐，对其的访问都会被阻塞*/
    pagedir_clear_page(fe->t->pagedir, fe->upage);
    uint32_t index = swap_out(fe->kpage);

    // 处理spt
    struct spt_entry *spte = spt_lookup(fe->t, fe->upage);
    ASSERT(spte);
    spte->status = SWAPPED_OUT;
    spte->kpage = NULL;
    spte->swap_index = index;

    free_frame(fe->kpage);
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