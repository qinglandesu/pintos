#ifndef VM_PAGE_H
#define VM_PAGE_H
#include <hash.h>
#include "threads/palloc.h"
#include "threads/thread.h"
#define VM
#ifdef VM

/* frame hash table entry */
struct frame_entry
{
    struct thread *t;
    void *kernel_addr;
    void *user_vaddr;
    struct hash_elem elem;
};

/* supplemental page table entry */
struct spt_entry
{
    void *user_vaddr;
    enum
    {
        IN_USE,
        SWAPPED_OUT,
        DEMAND_ZERO
    } status;
    struct hash_elem elem;
};

void frame_init(void);
void *get_frame(enum palloc_flags);
void free_frame(void *);
bool spt_set_page(struct thread *, void *);
struct spt_entry *spt_lookup(struct hash *, void *);
bool activate_page(struct thread *, void *);

void swap_init(void);
void swap_free(uint32_t);
void swap_in(uint32_t, void *);
uint32_t swap_out(void *);

unsigned frame_hash_func(const struct hash_elem *, void *);
bool frame_less_func(const struct hash_elem *,
                     const struct hash_elem *, void *);
unsigned spte_hash_func(const struct hash_elem *, void *);
bool spte_less_func(const struct hash_elem *,
                    const struct hash_elem *, void *);
void spte_destroy_func(struct hash_elem *, void *);

#else
#define get_frame(X) palloc_get_page(X)
#define free_frame(X) palloc_free_page(X)

#endif

#endif /**< vm/page.h */
