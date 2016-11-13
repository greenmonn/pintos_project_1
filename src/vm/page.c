/* page.c */

#include "page.h"
#include "swap.h"
#include "frame.h"
#include "threads/thread.h"
#include "threads/malloc.h"
#include "threads/pte.h"
#include "userprog/syscall.h"
#include "threads/palloc.h"
#include "threads/vaddr.h"
#include <stdio.h>
#include <string.h>

/* Create a new "page", which saves information for later installation of physical memory frame. */
struct page *
make_page(void *uaddr, enum page_location place)
{
    struct page *pg = malloc(sizeof (struct page)); //ASSERT: kernel pool?
    if (pg != NULL) {
        pg->uaddr = uaddr;
        pg->location = place;
        pg->is_code_seg = false;
    }
    return pg;
}

/* 1. page type : FILE */

void
page_set_file(struct hash *pages, struct page *page_, struct file *file_,int32_t ofs_, bool writable_, int page_read_bytes_)
{
    page_insert(pages, page_);
    page_->file = file_;
    page_->ofs = ofs_;
    page_->writable = writable_;
    page_->page_read_bytes = page_read_bytes_;
    page_->is_code_seg = false;
}
// Later we'll get frame and read the file then install to the physical memory.


/* 2. page type : SWAP */

//TODO


/* Create a new supplemental "page table", which is maintained by each process */

struct hash *
suppl_pages_create (void)
{
    struct hash *pages = malloc(sizeof (struct hash));
    hash_init(pages, page_hash, page_less, NULL);
    return pages;
}

static void page_free_func (struct hash_elem *e, void *aux UNUSED)
{
    struct page *pg = hash_entry(e, struct page, elem);
    if (pg->location == FRAME)
    {
        frame_free(pagedir_get_page(thread_current()->pagedir, pg->uaddr));
        pagedir_clear_page(thread_current()->pagedir, pg->uaddr);
    }
    free(pg);
}

void
suppl_pages_destroy (struct hash * pages) 
{
    hash_destroy(pages, page_free_func);
}

/* Functions for Hashing. */

unsigned 
page_hash(const struct hash_elem *p_, void *aux UNUSED)
{
    const struct page *p = hash_entry(p_, struct page, elem);
    return hash_bytes(&p->uaddr, sizeof p->uaddr);
}

bool
page_less(const struct hash_elem *a_, const struct hash_elem *b_, void *aux UNUSED)
{
    const struct page *a = hash_entry (a_, struct page, elem);
    const struct page *b = hash_entry (b_, struct page, elem);

    return a->uaddr < b->uaddr;
}

void
page_insert(struct hash *pages, struct page *page)
{
    hash_replace(pages, &page->elem);
    //not allow duplication!
}

/* Returns the page containing the given virtual address,
 * or a null pointer if no such page exists */
struct page *
page_lookup(struct hash *pages, const void *addr)
{
    struct page p;
    struct hash_elem *e;

    p.uaddr = addr;
    e = hash_find(pages, &p.elem);
    return e != NULL ? hash_entry (e, struct page, elem) : NULL;
}

bool
install_page (void *upage, void *kpage, bool writable)
{
    struct thread *t = thread_current();

    //1. Save upage to the frame
    struct frame *fr = frame_find(kpage);
    ASSERT(fr != NULL);
    if (fr != NULL) { 
        fr->upage = upage;
    }
    //2. Save writable value to the page
    struct page *pg = page_lookup(t->suppl_pages, upage);
    if (pg != NULL) {
        pg->writable = writable;
    }


    return (pagedir_get_page (t->pagedir, upage) == NULL
            && pagedir_set_page (t->pagedir, upage, kpage, writable));
}

int
install_suppl_page(struct hash *pages, struct page *pg, void *upage) 
{
    uint8_t *kpage;
    size_t page_read_bytes;
    size_t page_zero_bytes;
    if (pg != NULL) {
        switch(pg->location) {
            case ZERO:
                kpage = frame_alloc(true);
                if (kpage == NULL) {
                    return 0;
                }
                //memset (kpage, 0, PGSIZE);
                if (!install_page (upage, kpage, pg->writable))
                {
                    frame_free(kpage);
                    printf("install page fail\n");
                    return 0;
                }
                pg->location = FRAME;
                return 1;
                break; //Never reached
            case SWAP:
				kpage = frame_alloc(false);
				if (kpage == NULL) return 0;
				swap_in(pg->swap_index, kpage);
				pg->location = FRAME;
				pg->swap_index = -1;
				if (!install_page(upage, kpage, pg->writable))
				{
					frame_free(kpage);
					return 0;
				}
				return 1;
                break;
            case FRAME:
                break;
            case FILE: //Lazy Loading!
                filesys_lock_acquire();
                kpage = frame_alloc(false);
                if (kpage == NULL) {
                    printf("frame_alloc fail : should PANIC\n");
                    return 0;
                }
                page_read_bytes = pg->page_read_bytes;
                page_zero_bytes = PGSIZE - page_read_bytes;


                file_seek(pg->file, pg->ofs);
                if (file_read (pg->file, kpage, page_read_bytes) != (int) page_read_bytes) {
                    frame_free(kpage);
                    filesys_lock_release();
                    printf("file_read fail\n");
                    return 0;
                }
                memset (kpage + page_read_bytes, 0, page_zero_bytes);
                filesys_lock_release();
                
                if (!install_page (upage, kpage, pg->writable))
                {
                    frame_free(kpage);
                    printf("install page fail\n");
                    return 0;
                }
				pg->location = FRAME;

                return 1;

                break;
            default:
                break;
        }
    }
    else {
        //printf("Faulted page %x has no SUPP PAGE - EXIT(-1)\n", upage);
        return 0;
    }
}



