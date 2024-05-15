#ifndef VM_PAGE_H
#define VM_PAGE_H
#include <hash.h>
#include "threads/palloc.h"
#include "threads/thread.h"
//#define VM
#ifdef VM

#define STACK_LIMIT 0x800000

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
    bool pinned; /* true时不能被evict */
                 /* 为什么pin：process在load时是在
                    kernel thread中进行的，此时地址
                    访问是直接用的kernel_address，
                    如果正在用到的页面被驱逐，swap_in
                    的时候kernel_address会变 */
};
struct lock frame_lock; /* frame的锁 */

/* supplemental page table entry */
struct spt_entry
{
    void *upage;
    void *kpage;
    bool writable;
    struct file *file;
    uint32_t offset;
    uint32_t read_bytes;
    uint32_t zero_bytes;
    enum page_status status;
    uint32_t swap_index;
    struct hash_elem elem;
};

void frame_init(void);
void *get_frame(enum palloc_flags flag, void *upage);
void free_frame(void *kpage);
void frame_unpin(void *kpage);
void free_frame_on_exit(void);
void frame_table_remove(void *kpage);

bool spt_add_page(struct thread *t, void *upage, enum page_status status, void *kpage);
struct spt_entry *spt_lookup(struct thread *t, void *upage);
bool activate_page(struct thread *t, void *upage);

void swap_init(void);
void swap_free(uint32_t);
void swap_in(uint32_t, void *);
uint32_t swap_out(void *);
void page_daemon(void);

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
