#include "userprog/syscall.h"
#include <stdio.h>
#include <stdlib.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "userprog/pagedir.h"
#include "filesys/filesys.h"
#include "threads/synch.h"
#include "userprog/process.h"
#include <string.h>
#include "filesys/off_t.h"
#include "filesys/file.h"
#include "filesys/inode.h"

static int curr_descriptor, saved_status;
static struct list file_descriptor_list;
static struct list executable_list;
struct lock rw_lock;

struct file_descriptor{
	struct list_elem file_elem;
	int num;
	struct file* open_file;
	int size;
	int thread_opener;
	bool is_directory;
};

struct exec_descriptor{
	struct list_elem exec_elem;
	char* file_name;
	int id;
};

//Prototypes
static void syscall_handler (struct intr_frame *);
void is_ptr_valid(const void *ptr);
int write (int fd, const void *buffer, unsigned size);
bool create(const char *file, unsigned initial_size);
int open (const char *file);
int read(int fd, void* buffer, unsigned size);
int wait(int id);
int exec(const char *cmd_line);
struct file_descriptor* find_fd(int fd);
int find_file_size(int fd);
void seek(int fd, unsigned position);
void add_exec_to_list (char* file_name, int id);
bool find_exec_in_list(char* f);
void close(int fd);
bool chdir(const char *dir);
bool mkdir(const char *dir);
bool readdir(int fd, char *name);
bool isdir(int fd);
int inumber(int fd);

void
syscall_init (void) 
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
  curr_descriptor = 2;
  saved_status = NULL;
  list_init(&file_descriptor_list);
  list_init(&executable_list);
  lock_init(&rw_lock);
}

static void
syscall_handler (struct intr_frame *f UNUSED) 
{
	is_ptr_valid(f->esp);

	//Check for valid syscall
	if (*(int*)f->esp < SYS_HALT || *(int*)f->esp > SYS_INUMBER)
	{
		exit(-1);
	}

	switch(*(int*)f->esp)
	{
		case SYS_WRITE:
		{
			is_ptr_valid((int*)f->esp + 1);
			is_ptr_valid((int*)f->esp + 2);
			is_ptr_valid((int*)f->esp + 3);

			int fd = *((int*)f->esp + 1);
			void* buffer = (void*)(*((int*)f->esp + 2));
			unsigned size = *((unsigned*)f->esp + 3);

			is_ptr_valid(buffer);
			f->eax = write(fd, buffer, size);
			break;
		}
		case SYS_CREATE:
		{
			is_ptr_valid((int*)f->esp + 1);
			is_ptr_valid((int*)f->esp + 2);

			void* filename = (void*)(*((int*)f->esp + 1));
			unsigned size = *((unsigned*)f->esp + 2);

			if (filename == NULL)
				exit(-1);
			is_ptr_valid(filename);

			f->eax = create(filename, size);
			break;
		}
		case SYS_READ:
		{
			is_ptr_valid((int*)f->esp + 1);
			is_ptr_valid((int*)f->esp + 2);
			is_ptr_valid((int*)f->esp + 3);

			int fd = *((int*)f->esp + 1);
			void* buffer = (void*)(*((int*)f->esp + 2));
			unsigned size = *((unsigned*)f->esp + 3);

			is_ptr_valid(buffer);
			f->eax = read(fd, buffer, size);

			break;
		}
		case SYS_OPEN:
		{
			is_ptr_valid((int*)f->esp + 1);
			void* filename = (void*)(*((int*)f->esp + 1));
			is_ptr_valid(filename);

			f->eax = open(filename);

			break;
		}
		case SYS_WAIT:
		{
			is_ptr_valid((int*)f->esp + 1);
			int pid = *((int*)f->esp + 1);

			f->eax = wait(pid);
			break;
		}
		case SYS_EXEC:
		{
			is_ptr_valid((int*)f->esp + 1);
			void* filename = (void*)(*((int*)f->esp + 1));
			is_ptr_valid(filename);

			f->eax = exec(filename);
			break;
		}
		case SYS_FILESIZE:
		{
			is_ptr_valid((int*)f->esp + 1);
			int fd = *((int*)f->esp + 1);

			f->eax = find_file_size(fd);
			break;
		}
		case SYS_SEEK:
		{
			is_ptr_valid((int*)f->esp + 1);
			is_ptr_valid((int*)f->esp + 2);

			int fd = *((int*)f->esp + 1);
			unsigned position = *((unsigned*)f->esp + 2);

			seek(fd, position);
			break;
		}
		case SYS_CLOSE:
		{
			is_ptr_valid((int*)f->esp + 1);
			int fd = *((int*)f->esp + 1);

			close(fd);
			break;
		}
		case SYS_EXIT:
		{
			is_ptr_valid((int*)f->esp + 1);
			int status = *((int*)f->esp + 1);

			exit(status);
			break;
		}
		case SYS_MKDIR:
		{
			is_ptr_valid((int*)f->esp + 1);
			void* filename = (void*)(*((int*)f->esp + 1));
			is_ptr_valid(filename);

			f->eax = mkdir(filename);
			break;
		}
		case SYS_ISDIR:
		{
			is_ptr_valid((int*)f->esp + 1);
			int fd = *((int*)f->esp + 1);

			isdir(fd);
			break;
		}
	}
}

//Exit the thread
void exit (int status)
{
	saved_status = status;
	printf("%s: exit(%d)\n", thread_current()->name, status);
	thread_exit();
}

//Check if it is a valid user pointer
void is_ptr_valid(const void *ptr)
{
	if (ptr < (void*)0x8048000 || ptr > PHYS_BASE || ptr == NULL)
		exit(-1);
	if (!is_user_vaddr(ptr))
		exit(-1);
	if (pagedir_get_page(thread_current()->pagedir, ptr) == NULL)
		exit(-1);
}

//Write to the stack
int write (int fd, const void *buffer, unsigned size)
{
	int size_written = 0;
	if (fd == STDOUT_FILENO)
	{
		putbuf(buffer, size);
	}
	else
	{
	  	struct file_descriptor* curr_descriptor = find_fd(fd);
	  	if (curr_descriptor != NULL){
	  		if (curr_descriptor->is_directory == true)
	  			return -1;
	  		size_written = file_write(curr_descriptor->open_file, buffer, size);
	  	}
	}

	return size_written;
}

//List finder
struct file_descriptor* find_fd(int fd){
	struct list_elem *it;
	struct file_descriptor *currFd;

	for (it = list_begin(&file_descriptor_list); it!=list_end(&file_descriptor_list); it = list_next(it))
	{
		currFd = list_entry(it, struct file_descriptor, file_elem);
		if (currFd->num == fd)
			return currFd;
	}

	return NULL;
}

///SYSCALL Functions
bool create(const char *file, unsigned initial_size)
{
  	if ((file!=NULL) && (file[0] == '\0')) {
  		return false;
  	}

  	bool success = filesys_create(file, initial_size);

  	return success;
}

int open (const char *file)
{
  	if ((file!=NULL) && (file[0] == '\0')) {
  		return -1;
  	}

  	struct file* opened_file = filesys_open(file);
  	if (opened_file == NULL)
  		return -1;

  	if (find_exec_in_list(file))
  		file_deny_write(opened_file);

  	//Create file descriptor struct
  	struct file_descriptor* fd = malloc(sizeof(struct file_descriptor));
  	curr_descriptor++;
  	fd->num = curr_descriptor;
  	fd->open_file = opened_file;
  	fd->is_directory = false;
  	fd->size = file_length(opened_file);
  	fd->thread_opener = thread_tid();

  	list_push_back(&file_descriptor_list, &(fd->file_elem));

  	return curr_descriptor;
}

int read(int fd, void* buffer, unsigned size)
{
	int size_read = 0;

	if (fd == STDOUT_FILENO)
		exit(-1);

  	struct file_descriptor* curr_descriptor = find_fd(fd);
  	if (curr_descriptor != NULL){
  		size_read = file_read(curr_descriptor->open_file, buffer, size);
  	}

  	return size_read;
}

int wait(int id)
{
	int return_code = process_wait(id);

	if (return_code == -1)
		return -1;
	if (saved_status == -1)
		return -1;
	if (saved_status != NULL)
		return saved_status;

	return saved_status;
}

int exec(const char *cmd_line)
{
	int id = process_execute(cmd_line);
	process_load_wait(id);

	if (thread_current()->failed == -99){
		id = -1;
	}
	return id;
}

int find_file_size(int fd)
{
	struct file_descriptor* curr_descriptor = find_fd(fd);
	if (curr_descriptor != NULL)
		return curr_descriptor->size;

	return -1;
}

void seek(int fd, unsigned position)
{
	struct file_descriptor* curr_descriptor = find_fd(fd);
	if (curr_descriptor != NULL)
		file_seek(curr_descriptor->open_file, position);
}

void add_exec_to_list(char* f, int id)
{
	struct exec_descriptor* e = malloc(sizeof(struct exec_descriptor));
	e->file_name = malloc(strlen(f) + 1);
	strlcpy(e->file_name, f, strlen(f)+1);
	e->id = id;
  	list_push_back(&executable_list, &(e->exec_elem));
}

bool find_exec_in_list(char* f)
{
	struct list_elem *it;
	struct exec_descriptor* currExe;
	for (it = list_begin(&executable_list); it!=list_end(&executable_list); it = list_next(it))
	{
		currExe = list_entry(it, struct exec_descriptor, exec_elem);
		if (strcmp(currExe->file_name, f) == 0)
			return true;
	}

	return false;
}

void close(int fd)
{
	struct file_descriptor* curr_descriptor = find_fd(fd);
	if (thread_tid() != curr_descriptor->thread_opener)
		return;
	if (curr_descriptor != NULL)
	{
		//Remove fd
		list_remove(&(curr_descriptor->file_elem));
		file_close(curr_descriptor->open_file);
	}
	else{
		exit(-1);
	}
}

bool chdir(const char *dir)
{
	return false;
}

bool mkdir(const char *dir)
{
	//Empty directory name
	if (strcmp(dir,"") == 0)
		return false;

	bool success = filesys_create(dir, 1);
  	return success;
}

bool isdir(int fd)
{
	struct file_descriptor* curr_descriptor = find_fd(fd);
	return curr_descriptor->is_directory;
}