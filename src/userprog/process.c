#include "userprog/process.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "userprog/gdt.h"
#include "userprog/pagedir.h"
#include "userprog/tss.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "threads/synch.h"
#include "userprog/syscall.h"
#include "vm/page.h"

static thread_func start_process NO_RETURN;
static bool load (const char *cmdline,  void (**eip) (void), void **esp);

//Access to the parent's child list, and get the child struct!
struct child_elem *find_child(int pid) {
    //printf("find_child %d\n", pid);
    struct list_elem *e;
    struct child_elem *child;
    if (thread_current()->parent == NULL) //if parent has already exit
        return NULL;
    //printf("parent exist\n");
    struct list *child_list = &thread_current()->parent->child_list;
    //printf("child list exist\n");
    for (e = list_begin(child_list); e != list_end(child_list); e = list_next(e)) {
        child = list_entry(e, struct child_elem, elem);
        //printf("finding %d\n", child->pid);
        if (child->pid == pid) {
            //printf("finding %d\n", child->pid);
            return child;
        }
    }
    return NULL;
}

struct child_elem *find_my_child(int pid) {
    struct list_elem *e;
    struct child_elem *child;
    struct list *child_list = &thread_current()->child_list;
    for (e = list_begin(child_list); e != list_end(child_list); e= list_next(e)) {
        child = list_entry(e, struct child_elem, elem);
        if (child->pid == pid) {
            return child;
        }
    }
    return NULL;
}

int check_child_load () {
	struct list *child_list = &thread_current()->child_list;
	struct list_elem *e;
	for (e = list_begin(child_list); e != list_end(child_list); e = list_next(e)) {
		struct child_elem *proc = list_entry(e,struct child_elem, elem);
		if (proc->load == 0) {
			return 0;
		} else if (proc->load == 2) {
			return 2;
		}
	}
	return 1;
} 

/* Starts a new thread running a user program loaded from
   FILENAME.  The new thread may be scheduled (and may even exit)
   before process_execute() returns.  Returns the new process's
   thread id, or TID_ERROR if the thread cannot be created. */
tid_t
process_execute (const char *file_name) 
{
  char *fn_copy;
  tid_t tid;

  /* Make a copy of FILE_NAME.
     Otherwise there's a race between the caller and load(). */
  fn_copy = palloc_get_page (0);
  if (fn_copy == NULL) {
      palloc_free_page(fn_copy);
      return TID_ERROR;
  }
  strlcpy (fn_copy, file_name, PGSIZE);
  char *save_ptr;
  char* fn_copy2 = frame_alloc(false);
  if (!fn_copy2) {
      palloc_free_page(fn_copy2);
      return TID_ERROR;
  }

  strlcpy (fn_copy2, file_name, PGSIZE);
  strtok_r(fn_copy2, " ", &save_ptr);

  /* Create a new thread to execute FILE_NAME. */
  tid = thread_create (fn_copy2, PRI_DEFAULT, start_process, fn_copy);
  //printf("thread create complete\n"); 
  palloc_free_page(fn_copy2);

  int check;
  if (tid == TID_ERROR) {
      palloc_free_page (fn_copy);
      return tid;
  }
  //intr_disable();
  //thread_block();
  //intr_enable();
  //printf("check child already load\n");
  sema_up(&thread_current()->sema2);  //1
  sema_down(&thread_current()->sema);     //1
  //sema_up(&thread_current()->sema2);
  //printf("parent woke up by child\n");


  if (thread_current()->is_child_load == 2) {
      process_wait(tid);	
      tid = -1;    
  } 
  return tid;
}
/* A thread function that loads a user process and makes it start
   running. */
static void
start_process (void *f_name)
{
  char *file_name = f_name;
  struct intr_frame if_;
  bool success;

  /* Initialize interrupt frame and load executable. */
  memset (&if_, 0, sizeof if_);
  if_.gs = if_.fs = if_.es = if_.ds = if_.ss = SEL_UDSEG;
  if_.cs = SEL_UCSEG;
  if_.eflags = FLAG_IF | FLAG_MBS;
  

  //printf("before load\n");
  filesys_lock_acquire();
  success = load (f_name, &if_.eip, &if_.esp);
  filesys_lock_release();
  //printf("after load\n");
  sema_down(&thread_current()->parent->sema2);
  palloc_free_page(f_name);     //2
  if (success) {
      struct child_elem *child = find_child(thread_current()->tid);
      if (child != NULL) {
          child->load = 1;
      }

      if(thread_current()->parent != NULL) {
        thread_current()->parent->is_child_load = 1;
        sema_up(&thread_current()->parent->sema);   //3
        //printf("success - waked up parent\n");
        //sema_down(&thread_current()->parent->sema2);
        //printf("unblocked?\n");
      }
      
      //if(thread_current()->parent != NULL && thread_current()->parent->status == THREAD_BLOCKED)
         // thread_unblock(thread_current()->parent);
  }
  
  //hex_dump(PHYS_BASE-48, (void*)buf, 48, true); 
  /* If load failed, quit. */
  if (!success) {
      struct child_elem *child = find_child(thread_current()->tid);
      thread_current()->proc_status = -1;
      //thread_current()->parent->is_child_load = 2;
      
      //if(thread_current()->parent != NULL && thread_current()->parent->status == THREAD_BLOCKED)
      //thread_unblock(thread_current()->parent);
      if(thread_current()->parent != NULL) {
          thread_current()->parent->is_child_load = 2;
          if (child != NULL) {
              child->load = 2;
              child->status = -1;
          }

          sema_up(&thread_current()->parent->sema);
        //sema_down(&thread_current()->parent->sema2);
      }

      	  //printf("if not success:\n");
      thread_exit ();
      //exit(-1);
  }

  /* Start the user process by simulating a return from an
     interrupt, implemented by intr_exit (in
     threads/intr-stubs.S).  Because intr_exit takes all of its
     arguments on the stack in the form of a `struct intr_frame',
     we just point the stack pointer (%esp) to our stack frame
     and jump to it. */
  asm volatile ("movl %0, %%esp; jmp intr_exit" : : "g" (&if_) : "memory");
  NOT_REACHED ();
}

/* Waits for thread TID to die and returns its exit status.  If
   it was terminated by the kernel (i.e. killed due to an
   exception), returns -1.  If TID is invalid or if it was not a
   child of the calling process, or if process_wait() has already
   been successfully called for the given TID, returns -1
   immediately, without waiting.

   This function will be implemented in problem 2-2.  For now, it
   does nothing. */
int
process_wait (tid_t child_tid UNUSED) 

{ 
	int status = -1;
    struct list_elem *e;
    struct child_elem *waiting_child;
    for (e = list_begin(&thread_current()->child_list) ; e != list_end(&thread_current()->child_list); e = list_next(e)) {
        struct child_elem *proc = list_entry(e, struct child_elem, elem);
        if (proc->pid == child_tid) {
            waiting_child = proc;
            break;
        }
    }

    if ( e == list_end(&thread_current()->child_list)) {
		return status;
    }   // FAIL case 1 : pid does not refer to a direct child of the calling process.

    if (waiting_child->waited) {
        return status;
    }   // FAIL case 2 : the process that calls wait has already called wait on pid.
    
    waiting_child->waited = true;
/*
    while(!waiting_child->exit) {
        barrier();
    }
    */
    if (waiting_child->exit != 1) {
        struct child_elem *child = find_my_child(child_tid);
        sema_down(&child->TCB->sema_wait);
    }
    status = waiting_child->status;

    /* FREE RESOURCES

    struct file_elem *fe;
    // FILE_LIST
    while(!list_empty(&waiting_child->file_list)) {
        e = list_pop_front(&waiting_child->file_list);
        fe = list_entry(e, struct file_elem, elem);
        free(fe);
    }*/

    list_remove(&waiting_child->elem);

    free(waiting_child);
    return status;

   /* while (1) {
    }
    ;*/
  return -1;
}

/* Free the current process's resources. */
void
process_exit (void)
{
  struct thread *curr = thread_current ();
  uint32_t *pd;

  /* Destroy the current process's page directory and switch back
     to the kernel-only page directory. */
  pd = curr->pagedir;
  if (pd != NULL) 
    {
      /* Correct ordering here is crucial.  We must set
         cur->pagedir to NULL before switching page directories,
         so that a timer interrupt can't switch back to the
         process page directory.  We must activate the base page
         directory before destroying the process's page
         directory, or our active page directory will be one
         that's been freed (and cleared). */
      curr->pagedir = NULL;
      pagedir_activate (NULL);
      pagedir_destroy (pd);
    }

  /* FREE FILE DESCRIPTORS */
  struct list_elem *e;
  struct file_elem *fe;
  while(!list_empty(&thread_current()->file_list)) {
      e = list_begin(&thread_current()->file_list);
      fe = list_entry(e, struct file_elem, elem);
      //close -> it freed!
	  close(fe->fd);
  }
  

  struct child_elem *ce;
  /* Free child list - make children's parent to NULL */
  while(!list_empty(&thread_current()->child_list)) {
      e = list_pop_front(&thread_current()->child_list);
      ce = list_entry(e, struct child_elem, elem);
      ce->TCB->parent = NULL;
      free(ce);
  }

  //struct child_elem *child = find_child(thread_current()->tid);
  printf("%s: exit(%d)\n", thread_current()->name, thread_current()->proc_status);
 
  //thread_unblock(thread_current()->parent);
  /*if (thread_current()->parent != NULL && thread_current()->parent->status == THREAD_BLOCKED)
    thread_unblock(thread_current()->parent);*/
  if (!list_empty(&thread_current()->sema_wait.waiters)) {
     sema_up(&thread_current()->sema_wait);
  }
}

/* Sets up the CPU for running user code in the current
   thread.
   This function is called on every context switch. */
void
process_activate (void)
{
  struct thread *t = thread_current ();

  /* Activate thread's page tables. */
  pagedir_activate (t->pagedir);

  /* Set thread's kernel stack for use in processing
     interrupts. */
  tss_update ();
}

/* We load ELF binaries.  The following definitions are taken
   from the ELF specification, [ELF1], more-or-less verbatim.  */

/* ELF types.  See [ELF1] 1-2. */
typedef uint32_t Elf32_Word, Elf32_Addr, Elf32_Off;
typedef uint16_t Elf32_Half;

/* For use with ELF types in printf(). */
#define PE32Wx PRIx32   /* Print Elf32_Word in hexadecimal. */
#define PE32Ax PRIx32   /* Print Elf32_Addr in hexadecimal. */
#define PE32Ox PRIx32   /* Print Elf32_Off in hexadecimal. */
#define PE32Hx PRIx16   /* Print Elf32_Half in hexadecimal. */

/* Executable header.  See [ELF1] 1-4 to 1-8.
   This appears at the very beginning of an ELF binary. */
struct Elf32_Ehdr
  {
    unsigned char e_ident[16];
    Elf32_Half    e_type;
    Elf32_Half    e_machine;
    Elf32_Word    e_version;
    Elf32_Addr    e_entry;
    Elf32_Off     e_phoff;
    Elf32_Off     e_shoff;
    Elf32_Word    e_flags;
    Elf32_Half    e_ehsize;
    Elf32_Half    e_phentsize;
    Elf32_Half    e_phnum;
    Elf32_Half    e_shentsize;
    Elf32_Half    e_shnum;
    Elf32_Half    e_shstrndx;
  };

/* Program header.  See [ELF1] 2-2 to 2-4.
   There are e_phnum of these, starting at file offset e_phoff
   (see [ELF1] 1-6). */
struct Elf32_Phdr
  {
    Elf32_Word p_type;
    Elf32_Off  p_offset;
    Elf32_Addr p_vaddr;
    Elf32_Addr p_paddr;
    Elf32_Word p_filesz;
    Elf32_Word p_memsz;
    Elf32_Word p_flags;
    Elf32_Word p_align;
  };

/* Values for p_type.  See [ELF1] 2-3. */
#define PT_NULL    0            /* Ignore. */
#define PT_LOAD    1            /* Loadable segment. */
#define PT_DYNAMIC 2            /* Dynamic linking info. */
#define PT_INTERP  3            /* Name of dynamic loader. */
#define PT_NOTE    4            /* Auxiliary info. */
#define PT_SHLIB   5            /* Reserved. */
#define PT_PHDR    6            /* Program header table. */
#define PT_STACK   0x6474e551   /* Stack segment. */

/* Flags for p_flags.  See [ELF3] 2-3 and 2-4. */
#define PF_X 1          /* Executable. */
#define PF_W 2          /* Writable. */
#define PF_R 4          /* Readable. */

static bool setup_stack (void **esp, char *f_name);
static bool validate_segment (const struct Elf32_Phdr *, struct file *);
static bool load_segment (struct file *file, off_t ofs, uint8_t *upage,
                          uint32_t read_bytes, uint32_t zero_bytes,
                          bool writable);
int calc_argc (char* string);

/* Loads an ELF executable from FILE_NAME into the current thread.
   Stores the executable's entry point into *EIP
   and its initial stack pointer into *ESP.
   Returns true if successful, false otherwise. */

int calc_argc(char* string) {
    char* save_ptr, *token;
    int count = 0;
    int slength = strlen(string)+1;
    char* tmpstring = (char*)malloc(slength*sizeof(char));
    if (!tmpstring) {
		free(tmpstring);
		return -1;
	}
	strlcpy(tmpstring, string, slength);
    for (token = strtok_r(tmpstring, " ", &save_ptr); token != NULL ; 
            token = strtok_r(NULL, " ", &save_ptr)) {
        count++;
    }
    free(tmpstring);
    return count;
}

    

bool
load (const char *fn_copy,  void (**eip) (void), void **esp) 
{
  struct thread *t = thread_current ();
  struct Elf32_Ehdr ehdr;
  struct file *file = NULL;
  off_t file_ofs;
  bool success = false;
  int i;
  //char* save_ptr;
  //char* prog_name = (char*)malloc(strlen(fn_copy));
  //strlcpy(prog_name, fn_copy, strlen(fn_copy)+1);
  //strtok_r(prog_name, " ", &save_ptr);
  //strtok_r(NULL, " ", &save_ptr);
  
  char* save_ptr;
  char* prog_name = frame_alloc(false);
  if (!prog_name) {
  	goto done;
  }
  strlcpy(prog_name,fn_copy, strlen(fn_copy)+1);
  strtok_r(prog_name," ",&save_ptr);
  strtok_r(NULL," ", &save_ptr);
  /* Allocate and activate page directory. */
  t->pagedir = pagedir_create ();

  /* Implement VM : construct a supplemental page table */
  t->suppl_pages = suppl_pages_create ();
  
  if (t->pagedir == NULL || t->suppl_pages == NULL) 
    goto done;

  process_activate ();

  /* Open executable file. */
  file = filesys_open (prog_name);
  if (file == NULL) 
    {
      printf ("load: %s: open failed\n", prog_name);
      goto done; 
    }

  /* Read and verify executable header. */
  if (file_read (file, &ehdr, sizeof ehdr) != sizeof ehdr
      || memcmp (ehdr.e_ident, "\177ELF\1\1\1", 7)
      || ehdr.e_type != 2
      || ehdr.e_machine != 3
      || ehdr.e_version != 1
      || ehdr.e_phentsize != sizeof (struct Elf32_Phdr)
      || ehdr.e_phnum > 1024) 
    {
      printf ("load: %s: error loading executable\n", prog_name);
      goto done; 
    }

  /* Read program headers. */
  file_ofs = ehdr.e_phoff;
  for (i = 0; i < ehdr.e_phnum; i++) 
    {
      struct Elf32_Phdr phdr;

      if (file_ofs < 0 || file_ofs > file_length (file))
        goto done;
      file_seek (file, file_ofs);

      if (file_read (file, &phdr, sizeof phdr) != sizeof phdr)
        goto done;
      file_ofs += sizeof phdr;
      switch (phdr.p_type) 
        {
        case PT_NULL:
        case PT_NOTE:
        case PT_PHDR:
        case PT_STACK:
        default:
          /* Ignore this segment. */
          break;
        case PT_DYNAMIC:
        case PT_INTERP:
        case PT_SHLIB:
          goto done;
        case PT_LOAD:
          if (validate_segment (&phdr, file)) 
            {
              bool writable = (phdr.p_flags & PF_W) != 0;
              uint32_t file_page = phdr.p_offset & ~PGMASK;
              uint32_t mem_page = phdr.p_vaddr & ~PGMASK;
              uint32_t page_offset = phdr.p_vaddr & PGMASK;
              uint32_t read_bytes, zero_bytes;
              if (phdr.p_filesz > 0)
                {
                  /* Normal segment.
                     Read initial part from disk and zero the rest. */
                  read_bytes = page_offset + phdr.p_filesz;
                  zero_bytes = (ROUND_UP (page_offset + phdr.p_memsz, PGSIZE)
                                - read_bytes);
                }
              else 
                {
                  /* Entirely zero.
                     Don't read anything from disk. */
                  read_bytes = 0;
                  zero_bytes = ROUND_UP (page_offset + phdr.p_memsz, PGSIZE);
                }
              if (!load_segment (file, file_page, (void *) mem_page,
                                 read_bytes, zero_bytes, writable))
                goto done;
            }
          else
            goto done;
          break;
        }
    }

  /* Set up stack. */
  if (!setup_stack (esp, fn_copy))
    goto done;
  
  
  //hex_dump(0xbfffffc0, (void*)PHYS_BASE-64, 64, true);

  
  /* Start address */
  *eip = (void (*) (void)) ehdr.e_entry;

  success = true;

 done:
  /* We arrive here whether the load is successful or not. */
  
  palloc_free_page(prog_name);
  //file_counter--;
  //file_close (file);
  return success;
}

/* load() helpers. */

static bool install_page (void *upage, void *kpage, bool writable);

/* Checks whether PHDR describes a valid, loadable segment in
   FILE and returns true if so, false otherwise. */
static bool
validate_segment (const struct Elf32_Phdr *phdr, struct file *file) 
{
  /* p_offset and p_vaddr must have the same page offset. */
  if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK)) 
    return false; 

  /* p_offset must point within FILE. */
  if (phdr->p_offset > (Elf32_Off) file_length (file)) 
    return false;

  /* p_memsz must be at least as big as p_filesz. */
  if (phdr->p_memsz < phdr->p_filesz) 
    return false; 

  /* The segment must not be empty. */
  if (phdr->p_memsz == 0)
    return false;
  
  /* The virtual memory region must both start and end within the
     user address space range. */
  if (!is_user_vaddr ((void *) phdr->p_vaddr))
    return false;
  if (!is_user_vaddr ((void *) (phdr->p_vaddr + phdr->p_memsz)))
    return false;

  /* The region cannot "wrap around" across the kernel virtual
     address space. */
  if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
    return false;

  /* Disallow mapping page 0.
     Not only is it a bad idea to map page 0, but if we allowed
     it then user code that passed a null pointer to system calls
     could quite likely panic the kernel by way of null pointer
     assertions in memcpy(), etc. */
  if (phdr->p_vaddr < PGSIZE)
    return false;

  /* It's okay. */
  return true;
}

/* Loads a segment starting at offset OFS in FILE at address
   UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
   memory are initialized, as follows:

        - READ_BYTES bytes at UPAGE must be read from FILE
          starting at offset OFS.

        - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.

   The pages initialized by this function must be writable by the
   user process if WRITABLE is true, read-only otherwise.

   Return true if successful, false if a memory allocation error
   or disk read error occurs. */
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
              uint32_t read_bytes, uint32_t zero_bytes, bool writable) 
{
  ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
  ASSERT (pg_ofs (upage) == 0);
  ASSERT (ofs % PGSIZE == 0);

  off_t ofs_now = ofs;

  //file_seek (file, ofs);
  while (read_bytes > 0 || zero_bytes > 0) 
    {
      /* Do calculate how to fill this page.
         We will read PAGE_READ_BYTES bytes from FILE
         and zero the final PAGE_ZERO_BYTES bytes. */
      size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
      size_t page_zero_bytes = PGSIZE - page_read_bytes;

      //Allocate to supplemental page table
      if (page_read_bytes == 0) {
          struct page *new_page = make_page(upage, ZERO);
          page_insert(thread_current()->suppl_pages, new_page);
          new_page->writable = writable;
      }
      else {
          struct page *new_page = make_page(upage, FILE);
          page_set_file(thread_current ()->suppl_pages, new_page, file, ofs_now, writable, page_read_bytes);
      }
      //printf("set supplemental page - %x %x %x\n", upage, thread_current()->suppl_pages, new_page);


      //printf("setup offset %d\n", ofs_now);
      //printf("file address %x\n", file);
      /* Advance. */
      read_bytes -= page_read_bytes;
      zero_bytes -= page_zero_bytes;
      upage += PGSIZE;
      ofs_now += PGSIZE;
    }
  file_seek (file, ofs_now);
  return true;
}

/* Create a minimal stack by mapping a zeroed page at the top of */
static bool setup_stack (void **esp, char *f_name) 
{
  uint8_t *kpage;
  bool success = false;
  kpage = frame_alloc(true);
  if (kpage != NULL) 
    {
      success = install_page (((uint8_t *) PHYS_BASE) - PGSIZE, kpage, true);
      if (success)
        *esp = PHYS_BASE;
      else
        palloc_free_page (kpage);
    }

  /* filename parse */
  if (success) { 
      char* fn_copy = frame_alloc(false);
      if (fn_copy == NULL){
          palloc_free_page(fn_copy);
          thread_exit();
      }
      strlcpy(fn_copy, f_name, PGSIZE);
      int argc = calc_argc(fn_copy);
      if (argc == -1) {
          thread_exit();
      }
      int i = 0;
      char** arg_addr = (char**)malloc(argc * sizeof(char*));
      if (!arg_addr) {
          free(arg_addr);
          thread_exit();
          printf("no memory\n");
      }
      char* save_ptr, *token;

      //Save string to the stack
      for (token = strtok_r(fn_copy, " ", &save_ptr); token != NULL; 
              token = strtok_r (NULL, " ", &save_ptr)) {
          int temp_sl = strlen(token);
          *esp -= temp_sl + 1; 
          arg_addr[argc-1-i] = (char*)*esp;
          i++;
          int idx = 0;
          for (idx = 0; idx < temp_sl + 1; idx++) {
              *((char*)(*esp + idx)) = token[idx];
          }

          //hex_dump(0xbfffffc0, (void*)PHYS_BASE-64, 64, true);


      }

      //word align + argv[argc]
      *esp -= (size_t)(*esp)%4 + 4;
      *(int*)(*esp) = 0;
      *esp -= 4;

      //save pointer to argv & argc
      for (i=0; i<argc; i++) {
          *(char**)(*esp) = arg_addr[i];
          *esp -= 4;
      }
      *(char**)(*esp) = *esp + 4;
      *esp -= 4;
      *(int*)(*esp) = argc;
      *esp -= 4;
      *(int*)(*esp) = 0; //return address
      free(arg_addr);
      palloc_free_page(fn_copy);
      //Save 
  }


  return success;
}

/* Adds a mapping from user virtual address UPAGE to kernel
   virtual address KPAGE to the page table.
   If WRITABLE is true, the user process may modify the page;
   otherwise, it is read-only.
   UPAGE must not already be mapped.
   KPAGE should probably be a page obtained from the user pool
   with palloc_get_page().
   Returns true on success, false if UPAGE is already mapped or
   if memory allocation fails. */
static bool
install_page (void *upage, void *kpage, bool writable)
{
  struct thread *t = thread_current ();

  /* Verify that there's not already a page at that virtual
     address, then map our page there. */
  return (pagedir_get_page (t->pagedir, upage) == NULL
          && pagedir_set_page (t->pagedir, upage, kpage, writable));
}
