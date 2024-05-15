#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "devices/input.h"
#include "devices/shutdown.h"
#include "threads/malloc.h"
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "filesys/filesys.h"
#include "filesys/file.h"
#include "userprog/process.h"

static void syscall_handler(struct intr_frame *);

static void sys_halt(struct intr_frame *f UNUSED);
static void sys_exit(struct intr_frame *f);
static void sys_write(struct intr_frame *f);
static void sys_exec(struct intr_frame *f);
static void sys_wait(struct intr_frame *f);
static void sys_create(struct intr_frame *f);
static void sys_remove(struct intr_frame *f);
static void sys_open(struct intr_frame *f);
static void sys_close(struct intr_frame *f);
static void sys_filesize(struct intr_frame *f);
static void sys_read(struct intr_frame *f);
static void sys_seek(struct intr_frame *f);
static void sys_tell(struct intr_frame *f);

/* Reads a byte at user virtual address UADDR.
   UADDR must be below PHYS_BASE.
   Returns the byte value if successful, -1 if a segfault
   occurred. */
static int
get_user(const uint8_t *uaddr)
{
  int result;
  asm("movl $1f, %0; movzbl %1, %0; 1:"
      : "=&a"(result) : "m"(*uaddr));
  return result;
}

/* Writes BYTE to user address UDST.
   UDST must be below PHYS_BASE.
   Returns true if successful, false if a segfault occurred. */
static bool
put_user(uint8_t *udst, uint8_t byte)
{
  int error_code;
  asm("movl $1f, %0; movb %b2, %1; 1:"
      : "=&a"(error_code), "=m"(*udst) : "q"(byte));
  return error_code != -1;
}

/* 检查读取size字节是否合法 */
static void check_read(void *p, size_t size)
{
  if (!is_user_vaddr(p))
  {
    thread_current()->exit_code = -1;
    thread_exit();
  }
  for (size_t i = 0; i < size; i++) // check if every byte is safe to read
  {
    if (get_user(p + i) == -1)
    {
      thread_current()->exit_code = -1;
      thread_exit();
    }
  }
}

/* 检查写入size字节是否合法 */
static void check_write(void *p, size_t size)
{
  if (!is_user_vaddr(p))
  {
    thread_current()->exit_code = -1;
    thread_exit();
  }
  for (size_t i = 0; i < size; i++) // check if every byte is safe to write
  {
    if (!put_user(p + i, 0))
    {
      thread_current()->exit_code = -1;
      thread_exit();
    }
  }
}

/* 检查读取字符串是否合法 */
static void check_read_str(char *p)
{
  if (!is_user_vaddr(p))
  {
    thread_current()->exit_code = -1;
    thread_exit();
  }

  uint8_t *_str = (uint8_t *)p;
  while (true)
  {
    int c = get_user(_str);
    if (c == -1)
    {
      thread_current()->exit_code = -1;
      thread_exit();
    }
    else if (c == '\0') // end of str
      return;
    _str++;
  }
  NOT_REACHED();
}

void syscall_init(void)
{
  intr_register_int(0x30, 3, INTR_ON, syscall_handler, "syscall");
}

static void syscall_handler(struct intr_frame *f)
{
  check_read(f->esp, sizeof(int));
#ifdef VM
  thread_current()->esp = f->esp;
#endif
  int syscall_type = *(int *)f->esp;
  switch (syscall_type)
  {
  case SYS_HALT:
    sys_halt(f);
    break;
  case SYS_EXIT:
    sys_exit(f);
    break;
  case SYS_WRITE:
    sys_write(f);
    break;
  case SYS_EXEC:
    sys_exec(f);
    break;
  case SYS_WAIT:
    sys_wait(f);
    break;
  case SYS_CREATE:
    sys_create(f);
    break;
  case SYS_REMOVE:
    sys_remove(f);
    break;
  case SYS_OPEN:
    sys_open(f);
    break;
  case SYS_CLOSE:
    sys_close(f);
    break;
  case SYS_FILESIZE:
    sys_filesize(f);
    break;
  case SYS_READ:
    sys_read(f);
    break;
  case SYS_SEEK:
    sys_seek(f);
    break;
  case SYS_TELL:
    sys_tell(f);
    break;
  case SYS_MMAP:
    PANIC("mmap not inplemented");
    break;
  case SYS_MUNMAP:
    PANIC("munmap not inplemented");
    break;
  default:
    PANIC("invalid syscall");
    break;
  }
}

static void sys_halt(struct intr_frame *f UNUSED)
{
  shutdown_power_off();
}
static void sys_exit(struct intr_frame *f)
{
  check_read(f->esp + sizeof(uint32_t), sizeof(int));
  int exit_code = *(int *)(f->esp + sizeof(uint32_t));
  thread_current()->exit_code = exit_code;
  thread_exit();
}
static void sys_write(struct intr_frame *f)
{
  check_read(f->esp + sizeof(uint32_t), sizeof(int));
  int fd = *(int *)(f->esp + sizeof(uint32_t));
  check_read(f->esp + 2 * sizeof(uint32_t), sizeof(int));
  char *buf = *(char **)(f->esp + 2 * sizeof(uint32_t));
  check_read_str(buf);
  check_read(f->esp + 3 * sizeof(uint32_t), sizeof(int));
  int size = *(int *)(f->esp + 3 * sizeof(uint32_t));

  if (fd == 1) // stdout
  {
    putbuf(buf, size);
    f->eax = size;
  }
  else
  {
    struct file_ *f_ = fd_to_file_(fd);
    if (f_ != NULL)
    {
      lock_acquire(&filesys_lock);
      f->eax = file_write(f_->f, buf, size);
      lock_release(&filesys_lock);
    }
    else
      f->eax = -1;
  }
}
static void sys_exec(struct intr_frame *f)
{
  check_read(f->esp + sizeof(uint32_t), sizeof(int));
  char *cmd = *(char **)(f->esp + sizeof(uint32_t));
  check_read_str(cmd);
  f->eax = process_execute(cmd);
}
static void sys_wait(struct intr_frame *f)
{
  check_read(f->esp + sizeof(uint32_t), sizeof(int));
  int pid = *(int *)(f->esp + sizeof(uint32_t));
  f->eax = process_wait(pid);
}
static void sys_create(struct intr_frame *f)
{
  check_read(f->esp + sizeof(uint32_t), sizeof(char *));
  char *fname = *(char **)(f->esp + sizeof(uint32_t));
  check_read_str(fname);
  check_read(f->esp + 2 * sizeof(uint32_t), sizeof(unsigned));
  unsigned fsize = *(unsigned *)(f->esp + 2 * sizeof(uint32_t));
  lock_acquire(&filesys_lock);
  f->eax = filesys_create(fname, fsize);
  lock_release(&filesys_lock);
}
static void sys_remove(struct intr_frame *f)
{
  check_read(f->esp + sizeof(uint32_t), sizeof(char *));
  char *fname = *(char **)(f->esp + sizeof(uint32_t));
  check_read_str(fname);
  lock_acquire(&filesys_lock);
  f->eax = filesys_remove(fname);
  lock_release(&filesys_lock);
}
static void sys_open(struct intr_frame *f)
{
  check_read(f->esp + sizeof(uint32_t), sizeof(char *));
  char *fname = *(char **)(f->esp + sizeof(uint32_t));
  check_read_str(fname);
  lock_acquire(&filesys_lock);
  struct file *file = filesys_open(fname);
  lock_release(&filesys_lock);

  if (file != NULL)
  {
    struct thread *t = thread_current();
    struct file_ *f_ = (struct file_ *)malloc(sizeof(struct file_));
    f_->fd = t->next_fd++;
    f_->f = file;
    list_insert_ordered(&t->file_list, &f_->elem, fd_cmp, NULL);
    f->eax = f_->fd;
  }
  else // 打开失败
    f->eax = -1;
}
static void sys_close(struct intr_frame *f)
{
  check_read(f->esp + sizeof(uint32_t), sizeof(int));
  int fd = *(int *)(f->esp + sizeof(uint32_t));
  struct file_ *f_ = fd_to_file_(fd);
  if (f_ != NULL)
  {
    lock_acquire(&filesys_lock);
    file_close(f_->f);
    lock_release(&filesys_lock);
    list_remove(&f_->elem);
    free(f_);
  }
}
static void sys_filesize(struct intr_frame *f)
{
  check_read(f->esp + sizeof(uint32_t), sizeof(int));
  int fd = *(int *)(f->esp + sizeof(uint32_t));
  struct file_ *f_ = fd_to_file_(fd);
  if (f_ != NULL)
  {
    lock_acquire(&filesys_lock);
    f->eax = file_length(f_->f);
    lock_release(&filesys_lock);
  }
  else
    f->eax = -1;
}
static void sys_read(struct intr_frame *f)
{
  check_read(f->esp + sizeof(uint32_t), sizeof(int));
  int fd = *(int *)(f->esp + sizeof(uint32_t));
  check_read(f->esp + 2 * sizeof(uint32_t), sizeof(char *));
  char *buf = *(char **)(f->esp + 2 * sizeof(uint32_t));
  check_read(f->esp + 3 * sizeof(uint32_t), sizeof(int));
  unsigned size = *(int *)(f->esp + 3 * sizeof(uint32_t));
  if (size == 0)
    f->eax = 0;
  else
  {
    check_write(buf, size);
    if (fd == 0) // stdin
    {
      for (size_t i = 0; i < size; i++)
      {
        *buf = (char)input_getc();
        buf++;
      }
      f->eax = size;
    }
    else
    {
      struct file_ *f_ = fd_to_file_(fd);
      if (f_ != NULL)
      {
        lock_acquire(&filesys_lock);
        f->eax = file_read(f_->f, buf, size);
        lock_release(&filesys_lock);
      }
      else
        f->eax = -1;
    }
  }
}
static void sys_seek(struct intr_frame *f)
{
  check_read(f->esp + sizeof(uint32_t), sizeof(int));
  int fd = *(int *)(f->esp + sizeof(uint32_t));
  check_read(f->esp + 2 * sizeof(uint32_t), sizeof(unsigned));
  unsigned off = *(int *)(f->esp + 2 * sizeof(uint32_t));
  struct file_ *f_ = fd_to_file_(fd);
  if (f_ != NULL)
  {
    lock_acquire(&filesys_lock);
    file_seek(f_->f, off);
    lock_release(&filesys_lock);
  }
}
static void sys_tell(struct intr_frame *f)
{
  check_read(f->esp + sizeof(uint32_t), sizeof(int));
  int fd = *(int *)(f->esp + sizeof(uint32_t));
  struct file_ *f_ = fd_to_file_(fd);
  if (f_ != NULL)
  {
    lock_acquire(&filesys_lock);
    f->eax = file_tell(f_->f);
    lock_release(&filesys_lock);
  }
  else
    f->eax = -1;
}
