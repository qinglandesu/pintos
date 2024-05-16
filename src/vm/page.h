#ifndef VM_PAGE_H
#define VM_PAGE_H
#include <hash.h>
#include "threads/palloc.h"
#include "threads/thread.h"
//#define VM
#ifdef VM

#define STACK_LIMIT 0x800000
typedef int mmap_id;

enum page_status
{
    IN_USE,
    SWAPPED_OUT,
    DEMAND_ZERO,
    LAZY_LOAD
};

/* frame hash table entry */
struct frame_entry
{
    struct thread *t;
    void *kpage;
    void *upage;
    struct hash_elem elem;
};
/* 保护frame的锁 */
struct lock frame_lock; 

/* supplemental page table entry */
struct spt_entry
{
    void *upage;
    void *kpage;
    bool writable;
    enum page_status status;
    struct file *file;
    uint32_t offset;
    uint32_t read_bytes;
    uint32_t zero_bytes;
    uint32_t swap_index;
    struct hash_elem elem;
};

/* mmap list entry */
struct mmap_entry
{
    mmap_id id;
    void *upage;
    struct file *file;
    size_t f_size;
    struct list_elem elem;
};

void frame_init(void);
void *get_frame(enum palloc_flags flag, void *upage);
void free_frame(void *kpage);
// void frame_unpin(void *kpage);
void free_frame_on_exit(void);
void frame_table_remove(void *kpage);

bool spt_add_page(struct thread *t, void *upage, enum page_status status, void *kpage);
struct spt_entry *lookup_in_tspt(struct thread *t, void *upage);
bool activate_page(struct thread *t, void *upage);

void swap_init(void);
void swap_free(uint32_t);
void swap_in(uint32_t, void *);
uint32_t swap_out(void *);
void page_evict(void);

void munmap_id(mmap_id id);
void munmap_on_exit(void);
struct mmap_entry *mid_to_me(mmap_id id);

unsigned frame_hash_func(const struct hash_elem *, void *);
bool frame_less_func(const struct hash_elem *,
                     const struct hash_elem *, void *);
unsigned spte_hash_func(const struct hash_elem *, void *);
bool spte_less_func(const struct hash_elem *,
                    const struct hash_elem *, void *);
void spte_destroy_func(struct hash_elem *, void *);


#else
#define get_frame(X, Y) palloc_get_page(X)
#define free_frame(X) palloc_free_page(X)

#endif

#endif /**< vm/page.h */
