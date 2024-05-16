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

static struct lock swap_lock;      /*保护swap_block的锁 */
static struct block *swap_block;   /* swap的block*/
static struct bitmap *swap_bitmap; /* swap slot占用状态的bitmap*/
static size_t swap_maxnum;         /* swap的最大页数量 */

void frame_init()
{
    lock_init(&frame_lock);
    hash_init(&frame_hash_table, frame_hash_func, frame_less_func, NULL);
}

/* 分配一个物理帧kpage并将其关联到一个frame table(必须持有frame_lock) */
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
        return NULL;
    }
    fe->t = thread_current();
    fe->kpage = kpage;
    fe->upage = upage;
    hash_insert(&frame_hash_table, &fe->elem);

    return kpage;
}

/* 释放一个物理帧kpage并从frame table中删除对应的条目(必须持有frame_lock) */
void free_frame(void *kpage)
{
    ASSERT(lock_held_by_current_thread(&frame_lock));
    ASSERT((uint32_t)kpage % PGSIZE == 0);
    palloc_free_page(kpage);
    frame_table_remove(kpage);
}

/* 从frame table删除对应的条目,并free kpage对应的frame_entry(必须持有frame_lock) */
void frame_table_remove(void *kpage)
{
    ASSERT(lock_held_by_current_thread(&frame_lock));
    ASSERT((uint32_t)kpage % PGSIZE == 0);

    struct frame_entry temp;
    temp.kpage = kpage;
    struct hash_elem *h = hash_find(&frame_hash_table, &temp.elem);
    struct frame_entry *fe = hash_entry(h, struct frame_entry, elem);
    hash_delete(&frame_hash_table, &fe->elem);
    free(fe);
}

/* 在进程退出时调用，清除补充页表spt中的帧条目(必须持有frame_lock) */
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
            frame_table_remove(spte->kpage);
            break;
        case SWAPPED_OUT:
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

/* 向spt添加条目 */
bool spt_add_page(struct thread *t, void *upage,
                  enum page_status status, void *kpage)
{
    ASSERT((uint32_t)upage % PGSIZE == 0);
    struct spt_entry *spte;
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
    spte->read_bytes = 0;
    spte->zero_bytes = PGSIZE;
    if (hash_insert(t->spt, &spte->elem) == NULL)
    {
        // hash_insert没有重复条目时return NULL，此时成功插入
        return true;
    }
    else // spt已经有相同条目
    {
        free(spte);
        PANIC("same entry in spt\n");
        return false;
    }
}

/* 在thread t->spt中寻找页，如果失败则返回NULL */
struct spt_entry *lookup_in_tspt(struct thread *t, void *upage)
{
    struct hash *spt = t->spt;
    struct spt_entry temp;
    temp.upage = upage;
    struct hash_elem *e = hash_find(spt, &temp.elem);
    return e != NULL ? hash_entry(e, struct spt_entry, elem) : NULL;
}

/* 依据thread t->spt，激活一个页，并向页表添加条目(必须持有frame_lock)*/
bool activate_page(struct thread *t, void *upage)
{
    ASSERT(lock_held_by_current_thread(&frame_lock));
    uint32_t *pagedir = t->pagedir;
    struct spt_entry *spte = lookup_in_tspt(t, upage);
    if (spte == NULL) // spt中没有该条目
        return false;
    void *kpage = get_frame(PAL_USER, upage);
    if (!(kpage)) // 需要腾出空间
    {
        page_evict();
        kpage = get_frame(PAL_USER, upage);
    }
    ASSERT(kpage);

    switch (spte->status)
    {
    case IN_USE:
        PANIC("activating an active page\n");
        break;
    case DEMAND_ZERO:
        memset(kpage, 0, PGSIZE);
        if (!pagedir_get_page(t->pagedir, upage))
            pagedir_set_page(pagedir, upage, kpage, spte->writable);
        break;
    case SWAPPED_OUT:
        swap_in(spte->swap_index, kpage);
        if (!pagedir_get_page(t->pagedir, upage))
            pagedir_set_page(pagedir, upage, kpage, spte->writable);
        pagedir_set_dirty(t->pagedir, upage, true);
        break;
    case LAZY_LOAD:
        if (spte->read_bytes > 0)
        {
            file_seek(spte->file, spte->offset);
            uint32_t read_bytes = 0;
            while (read_bytes < spte->read_bytes)
            {
                read_bytes += file_read(spte->file,
                                        (uint8_t *)kpage + read_bytes,
                                        spte->read_bytes - read_bytes);
            }
            memset((int8_t *)kpage + read_bytes, 0, spte->zero_bytes);
        }
        else // read_bytes是零可能是DEMAND_ZERO之后被驱逐的干净页
            memset(kpage, 0, spte->zero_bytes);
        if (!pagedir_get_page(t->pagedir, upage))
            pagedir_set_page(pagedir, upage, kpage, spte->writable);
        break;
    default:
        PANIC("unknown status of spte\n");
        break;
    }

    spte->status = IN_USE;
    spte->kpage = kpage;
    spte->swap_index = INT32_MAX;

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

/* 把kpage放到swap_block里 */
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

/* evict一个页，处理spt、fht、pagedir(必须持有frame_lock) */
void page_evict()
{
    ASSERT(lock_held_by_current_thread(&frame_lock));
    size_t n = hash_size(&frame_hash_table);
    static unsigned prng = 1;
    struct frame_entry *fe;

    /*选择一个随机的frame*/
    prng = prng * 1664525u + 1013904223u;
    size_t pointer = prng % n;
    struct hash_iterator it;
    hash_first(&it, &frame_hash_table);
    for (size_t i = 0; i <= pointer; ++i)
        hash_next(&it);
    fe = hash_entry(hash_cur(&it), struct frame_entry, elem);

    /*先处理pagedir，这样将要被evict的页所属的线程再访问这个页的时候会fault，
    之后进入activate_page等待frame_lock，实现了只要一个页被选中要驱逐，对其的访问都会被阻塞*/
    pagedir_clear_page(fe->t->pagedir, fe->upage);
    bool dirty = pagedir_is_dirty(fe->t->pagedir, fe->upage);

    // 处理spt
    if (dirty)
    {
        uint32_t index = swap_out(fe->kpage);
        struct spt_entry *spte = lookup_in_tspt(fe->t, fe->upage);
        ASSERT(spte);
        spte->kpage = NULL;
        spte->status = SWAPPED_OUT;
        spte->swap_index = index;
    }
    else
    {
        struct spt_entry *spte = lookup_in_tspt(fe->t, fe->upage);
        ASSERT(spte);
        spte->kpage = NULL;
        spte->status = LAZY_LOAD;
    }
    free_frame(fe->kpage);
}

/* 根据mmap_id取消映射内存映射区域,写回文件,并从mmap_list中移除mmap_entry */
void munmap_id(mmap_id id)
{
    ASSERT(lock_held_by_current_thread(&frame_lock));
    struct thread *t = thread_current();
    struct mmap_entry *me = mid_to_me(id);
    ASSERT(me != NULL);
    ASSERT(me->file != NULL)

    uint32_t offset;
    uint32_t file_size = me->f_size;
    for (offset = 0; offset < file_size; offset += PGSIZE)
    {
        void *upage = (int8_t *)me->upage + offset;
        struct spt_entry *spte = lookup_in_tspt(t, upage);
        ASSERT(spte != NULL)
        uint32_t bytes_write;
        switch (spte->status)
        {
        case IN_USE:
            ASSERT(spte->kpage != NULL);             // 确认在内存里
            if (pagedir_is_dirty(t->pagedir, upage)) // 不dirty不用写回
            {
                lock_acquire(&filesys_lock);
                file_seek(me->file, offset);
                bytes_write = 0;
                while (bytes_write < spte->read_bytes)
                {
                    bytes_write += file_write(me->file,
                                              (int8_t *)upage + bytes_write,
                                              spte->read_bytes - bytes_write);
                }
                lock_release(&filesys_lock);
            }
            pagedir_clear_page(t->pagedir, upage);
            free_frame(spte->kpage);
            break;
        case SWAPPED_OUT: // swap出去的一定dirty
            activate_page(t, spte->upage);
            ASSERT(spte->status == IN_USE);
            ASSERT(spte->kpage != NULL);
            lock_acquire(&filesys_lock);
            file_seek(me->file, offset);
            bytes_write = 0;
            while (bytes_write < spte->read_bytes)
            {
                bytes_write += file_write(me->file,
                                          (int8_t *)upage + bytes_write,
                                          spte->read_bytes - bytes_write);
            }
            lock_release(&filesys_lock);
            pagedir_clear_page(t->pagedir, upage);
            free_frame(spte->kpage);
            break;
        case DEMAND_ZERO:
        case LAZY_LOAD:
            break; // 什么也不用管
        default:
            PANIC("unknown status of spte");
        }
        // 处理spt
        hash_delete(t->spt, &spte->elem);
    }
    lock_acquire(&filesys_lock);
    file_close(me->file);
    lock_release(&filesys_lock);
    list_remove(&me->elem);
    free(me);
}

/* 进程退出时取消映射所有内存映射区域 */
void munmap_on_exit()
{
    ASSERT(lock_held_by_current_thread(&frame_lock));
    struct thread *t = thread_current();
    mmap_id id;
    while (!list_empty(&t->mmap_list))
    {
        id = list_entry(list_front(&t->mmap_list),
                        struct mmap_entry, elem)
                 ->id;
        munmap_id(id);
    }
}

/* 寻找当前thread的mmap_id对应的mmap_entry，没找到返回NULL */
struct mmap_entry *mid_to_me(mmap_id id)
{
    struct thread *t = thread_current();
    struct list_elem *e = list_begin(&t->mmap_list);
    while (e != list_end(&t->child_list))
    {
        struct mmap_entry *me = list_entry(e, struct mmap_entry, elem);
        if (me->id == id)
            return me;
        e = list_next(e);
    }
    return NULL;
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