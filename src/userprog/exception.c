#include "userprog/exception.h"
#include <inttypes.h>
#include <stdio.h>
#include "userprog/gdt.h"
#include "userprog/syscall.h"
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/palloc.h"
#include "threads/vaddr.h"
#include "vm/page.h"


/* Number of page faults processed. */
static long long page_fault_cnt;

static void kill (struct intr_frame *);
static void page_fault (struct intr_frame *);

int non_IO_cnt;
int IO_cnt;
struct semaphore IO_mutex;
struct semaphore nonIO_mutex;
struct semaphore IO_sema;

/* Registers handlers for interrupts that can be caused by user
   programs.

   In a real Unix-like OS, most of these interrupts would be
   passed along to the user process in the form of signals, as
   described in [SV-386] 3-24 and 3-25, but we don't implement
   signals.  Instead, we'll make them simply kill the user
   process.

   Page faults are an exception.  Here they are treated the same
   way as other exceptions, but this will need to change to
   implement virtual memory.

   Refer to [IA32-v3a] section 5.15 "Exception and Interrupt
   Reference" for a description of each of these exceptions. */
void
exception_init (void) 
{
  /* These exceptions can be raised explicitly by a user program,
     e.g. via the INT, INT3, INTO, and BOUND instructions.  Thus,
     we set DPL==3, meaning that user programs are allowed to
     invoke them via these instructions. */
    sema_init(&IO_sema, 0);
  intr_register_int (3, 3, INTR_ON, kill, "#BP Breakpoint Exception");
  intr_register_int (4, 3, INTR_ON, kill, "#OF Overflow Exception");
  intr_register_int (5, 3, INTR_ON, kill,
                     "#BR BOUND Range Exceeded Exception");

  /* These exceptions have DPL==0, preventing user processes from
     invoking them via the INT instruction.  They can still be
     caused indirectly, e.g. #DE can be caused by dividing by
     0.  */
  intr_register_int (0, 0, INTR_ON, kill, "#DE Divide Error");
  intr_register_int (1, 0, INTR_ON, kill, "#DB Debug Exception");
  intr_register_int (6, 0, INTR_ON, kill, "#UD Invalid Opcode Exception");
  intr_register_int (7, 0, INTR_ON, kill,
                     "#NM Device Not Available Exception");
  intr_register_int (11, 0, INTR_ON, kill, "#NP Segment Not Present");
  intr_register_int (12, 0, INTR_ON, kill, "#SS Stack Fault Exception");
  intr_register_int (13, 0, INTR_ON, kill, "#GP General Protection Exception");
  intr_register_int (16, 0, INTR_ON, kill, "#MF x87 FPU Floating-Point Error");
  intr_register_int (19, 0, INTR_ON, kill,
                     "#XF SIMD Floating-Point Exception");

  /* Most exceptions can be handled with interrupts turned on.
     We need to disable interrupts for page faults because the
     fault address is stored in CR2 and needs to be preserved. */
  intr_register_int (14, 0, INTR_OFF, page_fault, "#PF Page-Fault Exception");

  non_IO_cnt = 0;
  IO_cnt = 0;
  sema_init(&IO_sema, 0);
  sema_init(&IO_mutex, 1);
  sema_init(&nonIO_mutex, 1);

}

void
IO_sema_down(void)
{
    sema_down(&IO_sema);
}

void 
IO_sema_up(void)
{
    sema_up(&IO_sema);
}

/* Prints exception statistics. */
void
exception_print_stats (void) 
{
  printf ("Exception: %lld page faults\n", page_fault_cnt);
}

/* Handler for an exception (probably) caused by a user process. */
static void
kill (struct intr_frame *f) 
{
  /* This interrupt is one (probably) caused by a user process.
     For example, the process might have tried to access unmapped
     virtual memory (a page fault).  For now, we simply kill the
     user process.  Later, we'll want to handle page faults in
     the kernel.  Real Unix-like operating systems pass most
     exceptions back to the process via signals, but we don't
     implement them. */
    //printf("in kill function\n"); 
  /* The interrupt frame's code segment value tells us where the
     exception originated. */
  switch (f->cs)
    {
    case SEL_UCSEG:
      /* User's code segment, so it's a user exception, as we
         expected.  Kill the user process.  */
      printf ("%s: dying due to interrupt %#04x (%s).\n",
              thread_name (), f->vec_no, intr_name (f->vec_no));
      intr_dump_frame (f);
      exit (-1); 

    case SEL_KCSEG:
      /* Kernel's code segment, which indicates a kernel bug.
         Kernel code shouldn't throw exceptions.  (Page faults
         may cause kernel exceptions--but they shouldn't arrive
         here.)  Panic the kernel to make the point.  */
      intr_dump_frame (f);
      PANIC ("Kernel bug - unexpected interrupt in kernel"); 

    default:
      /* Some other code segment?  Shouldn't happen.  Panic the
         kernel. */
      printf ("Interrupt %#04x (%s) in unknown segment %04x\n",
             f->vec_no, intr_name (f->vec_no), f->cs);
      exit(-1);
	  //thread_exit ();
    }
}

/* Page fault handler.  This is a skeleton that must be filled in
   to implement virtual memory.  Some solutions to project 2 may
   also require modifying this code.

   At entry, the address that faulted is in CR2 (Control Register
   2) and information about the fault, formatted as described in
   the PF_* macros in exception.h, is in F's error_code member.  The
   example code here shows how to parse that information.  You
   can find more information about both of these in the
   description of "Interrupt 14--Page Fault Exception (#PF)" in
   [IA32-v3a] section 5.15 "Exception and Interrupt Reference". */
/*
bool
install_page (void *upage, void *kpage, bool writable)
{
    struct thread *t = thread_current ();

    return (pagedir_get_page (t->pagedir, upage) == NULL
            && pagedir_set_page (t->pagedir, upage, kpage, writable));
}
*/

bool
is_stack_access(void *fault_addr, struct intr_frame *f)
{
    if (!is_user_vaddr(fault_addr))
        return false; //it should not happen.

    if (f->esp - 32 <= fault_addr) 
        return true;
    return false;
}

static void
page_fault (struct intr_frame *f) 
{
  bool not_present;  /* True: not-present page, false: writing r/o page. */
  bool write;        /* True: access was write, false: access was read. */
  bool user;         /* True: access by user, false: access by kernel. */
  void *fault_addr;  /* Fault address. */
  int success = 2;

  /* Obtain faulting address, the virtual address that was
     accessed to cause the fault.  It may point to code or to
     data.  It is not necessarily the address of the instruction
     that caused the fault (that's f->eip).
     See [IA32-v2a] "MOV--Move to/from Control Registers" and
     [IA32-v3a] 5.15 "Interrupt 14--Page Fault Exception
     (#PF)". */
  asm ("movl %%cr2, %0" : "=r" (fault_addr));

  /* Turn interrupts back on (they were only off so that we could
     be assured of reading CR2 before it changed). */
  intr_enable ();

  //printf("Entered page_fault(%x)\n", fault_addr);
  /* Count page faults. */
  page_fault_cnt++;

  /* Determine cause. */
  not_present = (f->error_code & PF_P) == 0;
  write = (f->error_code & PF_W) != 0;
  user = (f->error_code & PF_U) != 0;

  /* To implement virtual memory, delete the rest of the function
     body, and replace it with code that brings in the page to
     which fault_addr refers. */

  if (is_user_vaddr(fault_addr)) {   //Page fault of user virtual address
      //TODO 1 : find given faulted address in  supplemental page table of current thread
      bool use_IO = true;
      struct hash *supp = thread_current ()->suppl_pages;
      uint8_t *kpage;
      uint8_t *upage = pg_round_down(fault_addr);
      struct page *pg = page_lookup(supp, upage);
      //printf("suppl. page addr %x\n", pg);
      //success = install_suppl_page(supp, pg, upage);
      //printf("File install success\n");
      //printf("success : %d\n", success);
      size_t page_read_bytes;
      size_t page_zero_bytes;
      //int readbytes;


      //TODO : SYNCHRONIZATION - page fault need I/O or not
      //should handle I/O-required-pagefault first
      bool is_stack = is_stack_access(fault_addr, f);
      int pg_location = (pg ==  NULL ? -1 : pg->location);

/*

      sema_down(&IO_mutex);
      if (is_stack || pg_location == ZERO) {
          use_IO = false;
          non_IO_cnt++;
      } 
      else {
          IO_cnt++;
          if (non_IO_cnt != 0) {
              sema_down(&IO_sema);
          }
      }
      sema_up(&IO_mutex);
      */
          


      if (pg == NULL && is_stack) {
          //Stack access : fault address >= esp - 32
          //TODO : set up additional stack
          //kpage = frame_alloc(true);
          //printf("%x Stack growth : %x kpage alloc?\n", fault_addr, kpage);
          //printf("esp is %x\n", f->esp);
         // if (kpage != NULL)
         // {
              uint8_t *stack_end = pg_round_down(f->esp);
              if (pg_round_down(fault_addr) < stack_end)
                  stack_end = pg_round_down(fault_addr);
              int i = 0;
              success = 1;
              while(success) {
                  kpage = frame_alloc(true);
                  if (kpage != NULL) {
                     success = install_page(stack_end + PGSIZE*i, kpage, true);
                     i++;
                  } 
                  else { 
                      success = -1;
                      break;
                  }
              }
              if (success != -1) {
                  frame_free(kpage);
                  success = 1;
              } 
              else if (success == -1) { //Frame allocation fail
                  success = 0;
              }
         // }
          //although success == 0, page fault would not terminate program
          //success = 1;
      }
      
      else {
        success = install_suppl_page(supp, pg, upage);
        //printf("success : %d\n");
      }
      /*
      if (pg != NULL) {
          //Exist in the supplemental table -> install now
          switch(pg->location) {
              case SWAP:
                  break;
              case FRAME:
                  break;
              case FILE:    //Lazy Loading!
                  //printf("1");

                  filesys_lock_acquire();
                  kpage = frame_alloc(false);
                  if (kpage == NULL)
                      exit(-1);
                  //printf("2");
                  page_read_bytes = pg->page_read_bytes;
                  page_zero_bytes = PGSIZE - page_read_bytes;

                  file_seek(pg->file, pg->ofs);
                  //printf("3");
                  if (file_read (pg->file, kpage, page_read_bytes) != (int) page_read_bytes) {
                      //printf("4");
                      frame_free(kpage);
                      filesys_lock_release();
                      //printf("5");
                      exit(-1);
                  }
                 // printf("6");
                  memset (kpage + page_read_bytes, 0, page_zero_bytes);
                  filesys_lock_release();
                  //printf("7");

                  if(!install_page (upage, kpage, pg->writable))
                  {
                     // printf("8");
                      frame_free(kpage);
                      exit(-1);
                  }
                  //printf("9");
                  success = true;
                  
                  break;

              default:
                  break;
          }
      } //There exist page in suppl. page table
      else {
      exit(-1);
      }
      */
        /*
      sema_down(&IO_mutex);
      if (!use_IO) {
          non_IO_cnt--;
          if (non_IO_cnt == 0) {
              int i;
              for (i = 0; i < IO_cnt; i++) {
                  sema_up(&IO_sema);
              }
          }
      } else {
          IO_cnt--;
      }
      sema_up(&IO_mutex);
      */

      if (success != 1) {
          exit(-1);
      }

  }

    if (success == 2) {  //success value unchanged
      printf ("Page fault at %p: %s error %s page in %s context.\n",
              fault_addr,
              not_present ? "not present" : "rights violation",
              write ? "writing" : "reading",
              user ? "user" : "kernel");

      kill (f);

  }

}



