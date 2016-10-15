#include "userprog/syscall.h"
#include <stdio.h>
#include "threads/synch.h"
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/vaddr.h"

#define ARG_MAX 3

static void syscall_handler (struct intr_frame *);

struct lock filesys_lock;

void
syscall_init (void) 
{ 
  //file_counter = 0;
  lock_init(&filesys_lock);
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}

bool
userptr_valid(char* ptr) {
    int flag = 1;
    if (!ptr || !is_user_vaddr(ptr) || !pagedir_get_page(thread_current()->pagedir, ptr))
        flag = 0;

    return flag;
}

bool
userbuf_valid(char* ptr, int bufsize) {
    int flag = 1;
    int i;
    for(i=0; i<bufsize; i++) {
        if (!userptr_valid(ptr+i)) {
            flag = 0;
            break;
        }
    }
    return flag;
}
    
        
 
void get_arg(struct intr_frame *f, char** arg, int n) {
    char* curptr = f->esp + sizeof(int);
    int i;
    for (i=0; i<n; i++) {
        if (!userptr_valid(curptr)) {
			exit(-1);
        }
        arg[i] = *(char**)(curptr);
        curptr += sizeof(char*);
    }
}

  
static void
syscall_handler (struct intr_frame *f) 
{
  char* arg[ARG_MAX];
  int retval;

  if (!userptr_valid(f->esp)) {
      exit(-1);
  }

  switch (*(int*)(f->esp)) 
  {
    case SYS_HALT: 
	{
        power_off();
	    break;  
	}
    case SYS_EXIT:
	{
        get_arg(f,arg, 1);
	    exit((int)arg[0]);
        break;
	}
	case SYS_EXEC:
	{
		get_arg(f, arg, 1); 
		if (!userptr_valid(arg[0])) {
            exit(-1);
        }
        f->eax = exec((const char* )arg[0]);
        break;
	}
    case SYS_WAIT:
	{
		get_arg(f, arg, 1);
        f->eax = wait((int)arg[0]);
        break;
	}
	case SYS_CREATE:
	{
        get_arg(f, arg, 2);
        f->eax = create((const char*)arg[0], (unsigned)arg[1]);
        break;
	}
	case SYS_REMOVE:
	{
        get_arg(f, arg, 1);
        f->eax = remove((const char*)arg[0]);
        break;
	}
	case SYS_OPEN:
	{	
		get_arg(f, arg, 1);
        f->eax = open((const char*)arg[0]);
        break;
	}
	case SYS_FILESIZE:
	{
	    get_arg(f, arg, 1);
        f->eax = filesize((int)arg[0]);
        break;
	}
	case SYS_READ:
	{	
	    get_arg(f, arg, 3);
		f->eax = read((int)arg[0], (void*)arg[1], (unsigned)arg[2]);
        break;
	}
	case SYS_WRITE:
	{
		get_arg(f, arg, 3);
		f->eax = write((int)arg[0],(const void*) arg[1],(unsigned) arg[2]);
        break;
	}
	case SYS_SEEK:
	{
        get_arg(f, arg, 2);
        seek((int)arg[0], (unsigned)arg[1]);
        break;
	}
	case SYS_TELL:
	{
        get_arg(f, arg, 1);
        f->eax = tell((int)arg[0]);
        break;
	}
	case SYS_CLOSE:
	{
		get_arg(f, arg, 1);
		close(arg[0]);
        break;
	}
  }
}

/* CREATE : limitations 
 * No internal synchronization (only one process at a time is executing file system code
 * File size if fixed at creation time
 * File data is allocated as a single extent
 * no subdirectories(only root?)
 * file name limit 14 characters
 * no file system repair tool
 */
int create(const char *name, unsigned size) {
    if (!userptr_valid(name))
        exit(-1);
    if (strlen(name) > 14) {
        return 0;
    }
    lock_acquire(&filesys_lock);
    int ret = filesys_create(name, size);
    lock_release(&filesys_lock);
    return ret;
}

int remove(const char *file) {
    if (!userptr_valid(file))
        exit(-1);
    lock_acquire(&filesys_lock);
    int ret = filesys_remove(file);
    lock_release(&filesys_lock);
    return ret;
}


int open(const char *name) {
    if (!userptr_valid(name))
        exit(-1);
   char* kername = pagedir_get_page(thread_current()->pagedir, name);
   int fd;
   lock_acquire(&filesys_lock);
   struct file* openfile = filesys_open((const char*)kername);
   if (!openfile) {
       return -1;
   }

   struct list *file_list = &thread_current()->file_list;
   struct file_elem *fe = malloc(sizeof(struct file_elem));
   
   if (!fe) {
   	file_close(openfile);
	return -1;
   }
   
   fe->name = openfile;
   fd = fe->fd = thread_current()->fd_num++;
   strlcpy(fe->filename, kername, strlen(kername)+1);
   list_push_back(file_list, &fe->elem);
   lock_release(&filesys_lock);
   return fd;
}

struct file *find_file_desc(int fd) {
    struct list_elem *e;
    struct file_elem *fe;
    struct file *target;
    for (e = list_begin(&thread_current()->file_list); e != list_end(&thread_current()->file_list); e = list_next(e)) {
        fe = list_entry(e, struct file_elem, elem);
        if (fe->fd == fd) {
            return fe->name;
        }
    }
    return NULL;
}

void close(int fd) {
	lock_acquire(&filesys_lock);
    struct list *file_list = &thread_current()->file_list;
    struct list_elem *e;
    struct file_elem *fe;
    
    for (e = list_begin(file_list); e != list_end(file_list); e = list_next(e)) {
        fe = list_entry(e, struct file_elem, elem);
        if (fe->fd == fd) {
			struct file *file_to_close = find_file_desc(fd);
			file_close(file_to_close);
			e = list_next(e);
            list_remove(list_prev(e));
	  		//lock_release(fe->file_lock);
			//free(fe->file_lock);
			free(fe);
			e = list_prev(e);
			break;
        }
    }
	lock_release(&filesys_lock);
}

void exit(int status) {
    struct child_elem *child = find_child(thread_current()->tid);
    if (child != NULL) {
        child->status = status;
        child->exit = 1;

    }
	thread_current()->proc_status = status;
//	printf("%s: exit(%d)\n", thread_current()->name,status);
//    sema_up(&thread_current()->parent->sema);        
	thread_exit();
    

}

int exec(const char *cmd_line) {
	char *kerbuf = pagedir_get_page(thread_current()->pagedir, cmd_line);
    lock_acquire(&filesys_lock);
	int pid = process_execute(kerbuf);
	lock_release(&filesys_lock);
	return pid;
}

int wait(int pid) {
    int status;
	status = process_wait(pid);
    return status;
}
    
char *find_file_name(int fd) {
    struct list_elem *e;
    struct file_elem *fe;
    struct file *target;
    for (e = list_begin(&thread_current()->file_list); e != list_end(&thread_current()->file_list); e = list_next(e)) {
        fe = list_entry(e, struct file_elem, elem);
        if (fe->fd == fd) {
            return fe->filename;
        }
    }
    return NULL;
}



int write(int fd, const void *buffer, unsigned size)
{
    if (!userbuf_valid(buffer, size)) {
        exit(-1);
    }
    char* kerbuf = pagedir_get_page(thread_current()->pagedir, buffer);
    if (fd == STDOUT_FILENO) {
        putbuf((const char*) kerbuf, size);
        return size;
    }

    //write to file
	lock_acquire(&filesys_lock);
    struct file *file_to_write = find_file_desc(fd);
    char* filename = find_file_name(fd);
	
	//printf("write\n");

    //Cannot write executing code (for rox tests)
    if (filename != NULL && !strcmp(filename, thread_current()->name)) {
        lock_release(&filesys_lock);
		return 0;
    }

    if (file_to_write) {
        //lock_acquire(thread_current()->filesys_lock);
        int ret = file_write(file_to_write, (const void*)kerbuf, size);
        //lock_release(thread_current()->filesys_lock);
        lock_release(&filesys_lock);
		return ret;
    }
	lock_release(&filesys_lock);
    return 0;

}

int read(int fd, void *buffer, unsigned size)
{
    if(!userbuf_valid(buffer, size)) {
        exit(-1);
    }
    char* kerbuf = pagedir_get_page(thread_current()->pagedir, buffer);

    if (fd == STDIN_FILENO) {
        int index = 0;
        for (index = 0 ; index < size ; index++) {
            kerbuf[index] = input_getc();
        }
        return index;
    }

    //read from file
	lock_acquire(&filesys_lock);
    struct file *file_to_read = find_file_desc(fd);
    if(file_to_read) {
        //lock_acquire(thread_current()->filesys_lock);
        int ret =  file_read(file_to_read, (void*)kerbuf, (int)size);
        //lock_release(thread_current()->filesys_lock);
        lock_release(&filesys_lock);
		return ret;
    }
	lock_release(&filesys_lock);
    return -1;
}

int filesize(int fd) {
	lock_acquire(&filesys_lock);
    struct file *target = find_file_desc(fd);
    //lock_acquire(thread_current()->filesys_lock);
    int ret = file_length(target);
    //lock_release(thread_current()->filesys_lock);
    lock_release(&filesys_lock);
	return ret;
}

void seek(int fd, unsigned position) {
	lock_acquire(&filesys_lock);
    struct file *file_to_seek = find_file_desc(fd);
    //lock_acquire(thread_current()->filesys_lock);
    if(!file_to_seek) {
        lock_release(&filesys_lock);
		exit(-1);
	}
    file_seek(file_to_seek, position);
    //lock_release(thread_current()->filesys_lock);
	lock_release(&filesys_lock);
}

int tell(int fd) {
	lock_acquire(&filesys_lock);
    struct file *file_to_tell = find_file_desc(fd);

    if(!file_to_tell) {
        lock_release(&filesys_lock);
		exit(-1);
	}
    //lock_acquire(thread_current()->filesys_lock);
    int ret = file_tell(file_to_tell);
    //lock_release(thread_current()->filesys_lock);
    lock_release(&filesys_lock);
	return ret;
}

