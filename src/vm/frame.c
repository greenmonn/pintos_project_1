#include "frame.h"
#include "swap.h"
#include "page.h"
#include "threads/vaddr.h"
#include "threads/pte.h"

struct list frame_table;
struct lock frame_lock;
struct lock frame_table_lock;
int eviction_cnt;
struct list_elem *fr_iter;

struct frame *
make_frame(void *addr, struct thread *owner)
{
    struct frame *fr = malloc(sizeof(struct frame));
    if (fr != NULL) {
        fr->addr = addr;
        fr->pte = NULL;
        fr->upage = NULL;
        fr->owner = owner;
        fr->pin = true;
    }
    return fr;
}

void
set_frame(struct frame *fr, uint32_t *pte)
{
    fr->pte = pte;
    //fr->pin = false;
}

void
frame_table_init(void)
{
    list_init(&frame_table);
    lock_init(&frame_lock);
    lock_init(&frame_table_lock);
    eviction_cnt = 0;
    fr_iter = list_begin(&frame_table);
}

struct frame *
frame_find(void *kaddr)
{
    struct list_elem *e;
    void *addr = vtop(kaddr);
    //lock_acquire(&frame_table_lock);
    for (e = list_begin(&frame_table); e != list_end(&frame_table); e = list_next(e))
    {
        struct frame *fr = list_entry(e, struct frame, elem);
        if (fr->addr == addr) {
            //printf("find addr : %x\n", fr->addr);
            //lock_release(&frame_table_lock);

            return fr;
        }
        
    }
    //lock_release(&frame_table_lock);
    return NULL;
}
struct frame *
frame_alloc(bool zero)
{	
    lock_acquire(&frame_lock);
    void *kaddr = palloc_get_page(PAL_USER | (zero ? PAL_ZERO : 0));
    if (!kaddr) {   //Evict a frame and make it as MINE!
        //printf("frame_alloc() : palloc failed\n");
		struct frame *evicted_fr = frame_evict();
        void *evicted_addr = ptov(evicted_fr->addr);
        //printf("CHOSEN VICTOM : on %x mapped to UADDR %x\n", evicted_addr, evicted_fr->upage);

        //printf("evicted frame's OWNER THREAD is %x, %s\n", (evicted_fr->owner), evicted_fr->owner->name);
      
		struct page * evicted_page = page_lookup((evicted_fr->owner)->suppl_pages, evicted_fr->upage);

        //printf("We have a supplemental page : %x\n", evicted_page); 


       pagedir_clear_page((evicted_fr->owner)->pagedir, evicted_fr->upage);
		size_t swap_index = swap_out(evicted_addr);
        if(evicted_page != NULL) {

            evicted_page->location = SWAP;
            evicted_page->swap_index = swap_index;
            evicted_page->writable =(*(evicted_fr->pte) & PTE_W) == 0 ? false : true;
        } 
        else {
            //printf("NO SUPP PAGE : Make new one!\n");
            struct page *new_swap_page = make_page(evicted_fr->upage, SWAP);
            new_swap_page->writable = (*(evicted_fr->pte) & PTE_W) == 0 ? false : true;
            new_swap_page->swap_index = swap_index;
            page_insert(thread_current()->suppl_pages, new_swap_page);
        }

        lock_acquire(&frame_table_lock);
        list_remove(&evicted_fr->elem);
        lock_release(&frame_table_lock);

        free(evicted_fr);

        palloc_free_page(evicted_addr);
        //REtry palloc!
        kaddr = palloc_get_page(PAL_USER | (zero ? PAL_ZERO : 0));
        ASSERT(evicted_addr == kaddr);
    }
    struct frame *new_fr = make_frame(vtop(kaddr), thread_current());
    lock_acquire(&frame_table_lock);
    list_push_back(&frame_table, &new_fr->elem);
    lock_release(&frame_table_lock);

    lock_release(&frame_lock);
    return new_fr;
}

void 
frame_free(void *kaddr) //delete frame from list + free the frame!
{
	lock_acquire(&frame_lock);
    //palloc_free_page(kaddr);

    struct frame *fr_to_free = frame_find(kaddr);
    lock_acquire(&frame_table_lock);
    list_remove(&fr_to_free->elem);
    lock_release(&frame_table_lock);
    free(fr_to_free);
    palloc_free_page(kaddr);
	lock_release(&frame_lock);
}

struct frame *
frame_evict() 
{	
	//printf("frame_evict()\n");

	void * kaddr;
    struct frame *fr = NULL;

   // lock_acquire(&frame_table_lock);

    struct list_elem *e = fr_iter;
	while (true) {

        if (e == list_end(&frame_table)) {
            e = list_begin(&frame_table);
        }

		fr = list_entry(e, struct frame, elem);
		if (fr->pte != NULL && (*(fr->pte) & PTE_D) == 0) {
            //should never evict (read-only)
			//*(fr->pte) &= ~PTE_D;
			e = list_next(e);
		} else if (fr->pte != NULL && (*(fr->pte) & PTE_A) != 0) {
			*(fr->pte) &= ~PTE_A;
            //*(fr->pte) &= ~PTE_D;
			e = list_next(e);
		} else {
            if (fr->pte != NULL && fr->pin == false) {
                fr->pin = true;
			    //kaddr = ptov(fr->addr);
                e = list_next(e);
                //printf("eviction : %x %d\n", fr->upage, eviction_cnt++);
			    break;
            }
            else {
                e = list_next(e); 
            }
        }
    }
    fr_iter = e;
   // lock_release(&frame_table_lock);
	return fr; 
}

//Keep track of user pages.. later we'll use frame table to set a policy to evict frames and install new frame though pool is full!
