/* Copyright (C) 1991, 1995, 1996, 1997 Free Software Foundation, Inc.
   This file is part of the GNU C Library.

   The GNU C Library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 2.1 of the License, or (at your option) any later version.

   The GNU C Library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with the GNU C Library; if not, write to the Free
   Software Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA
   02111-1307 USA.  */

#include <errno.h>
#include <stddef.h>
#include <unistd.h>
#include "ros_syscall.h"
#include <string.h>
#include <fcntl.h>
#include <elf/elf.h>

/* Replace the current process, executing PATH with arguments ARGV and
   environment ENVP.  ARGV and ENVP are terminated by NULL pointers.  */
int
__execve (path, argv, envp)
     const char *path;
     char *const argv[];
     char *const envp[];
{
  procinfo_t pi;
  if(procinfo_pack_args(&pi,argv,envp))
  {
    errno = ENOMEM;
    return -1;
  }

  char name[MAX_PATH_LEN];
  if(strncpy(name,path,MAX_PATH_LEN) == MAX_PATH_LEN)
  {
    errno = ENAMETOOLONG;
    return -1;
  }

  return syscall(SYS_exec,(intptr_t)name,(intptr_t)&pi,0,0,0);
}
weak_alias (__execve, execve)