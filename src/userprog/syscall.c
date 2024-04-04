#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"

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

void syscall_init(void)
{
  intr_register_int(0x30, 3, INTR_ON, syscall_handler, "syscall");
}

static void syscall_handler(struct intr_frame *f)
{
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
  int exit_code = *(int *)(f->esp + sizeof(uint32_t));
  thread_current()->exit_code = exit_code;
  thread_exit();
}
static void sys_write(struct intr_frame *f)
{
  int fd = *(int *)(f->esp + sizeof(uint32_t));
  char *buf = *(char **)(f->esp + 2 * sizeof(uint32_t));
  int size = *(int *)(f->esp + 3 * sizeof(uint32_t));

  if (fd == 1)
  {
    putbuf(buf, size);
    f->eax = size;
  }
}
static void sys_exec(struct intr_frame *f) {}
static void sys_wait(struct intr_frame *f) {}
static void sys_create(struct intr_frame *f) {}
static void sys_remove(struct intr_frame *f) {}
static void sys_open(struct intr_frame *f) {}
static void sys_close(struct intr_frame *f) {}
static void sys_filesize(struct intr_frame *f) {}
static void sys_read(struct intr_frame *f) {}
static void sys_seek(struct intr_frame *f) {}
static void sys_tell(struct intr_frame *f) {}
