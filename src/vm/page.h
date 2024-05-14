#ifndef VM_PAGE_H
#define VM_PAGE_H
#include <hash.h>
#include "threads/palloc.h"
#include "threads/thread.h"
// #define VM
#ifdef VM

#define STACK_LIMIT 0x800000

enum page_status
{
    IN_USE,
    SWAPPED_OUT,
    DEMAND_ZERO,
    DEMAND_PAGING
};

/* frame hash table entry */
struct frame_entry
{
    struct thread *t;
    void *kpage;
    void *upage;
    struct hash_elem elem;
};
struct lock frame_lock; /* frame的锁 */

/* supplemental page table entry */
struct spt_entry
{
    void *upage;
    bool writable;
    enum page_status status;
    uint32_t swap_index;
    struct hash_elem elem;
};

void frame_init(void);
void *get_frame(enum palloc_flags flag, void *upage);
void free_frame(void *kpage);

bool spt_add_page(struct thread *t, void *upage, enum page_status status);
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
