/* ISC license. */

#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/mount.h>

#include <skalibs/types.h>
#include <skalibs/allreadwrite.h>
#include <skalibs/sgetopt.h>
#include <skalibs/strerr2.h>
#include <skalibs/stralloc.h>
#include <skalibs/env.h>
#include <skalibs/djbunix.h>

#include <s6/config.h>

#include <s6-linux-init/config.h>

#include "defaults.h"
#include "initctl.h"

#define USAGE "s6-linux-init [ -r ] [ -c basedir ] [ -p initpath ] [ -s envdumpdir ] [ -m umask ] [ -d devtmpfs ]"
#define dieusage() strerr_dieusage(100, USAGE)

#define BANNER "\n  s6-linux-init version " S6_LINUX_INIT_VERSION "\n\n"

static inline void run_stage2 (char const *basedir, char const **argv, unsigned int argc, char const *const *envp, size_t envlen, stralloc *envmodifs, int redirect)
{
  size_t dirlen = strlen(basedir) ;
  char const *childargv[argc + 2] ;
  char fn[dirlen + 1 + (sizeof(ENVSTAGE2) > sizeof(STAGE2) ? sizeof(ENVSTAGE2) : sizeof(STAGE2))] ;
  PROG = "s6-linux-init (child)" ;
  argv[0] = PROG ;
  memcpy(fn, basedir, dirlen) ;
  fn[dirlen] = '/' ;
  memcpy(fn + dirlen + 1, ENVSTAGE2, sizeof(ENVSTAGE2)) ;
  if (envdir(fn, envmodifs) == -1)
    strerr_warnwu2sys("envdir ", fn) ;
  memcpy(fn + dirlen + 1, STAGE2, sizeof(STAGE2)) ;
  childargv[0] = fn ;
  for (unsigned int i = 0 ; i < argc ; i++)
    childargv[i+1] = argv[i] ;
  childargv[argc + 1] = 0 ;
  setsid() ;
  fd_close(1) ;
  if (open(LOGFIFO, O_WRONLY) != 1)  /* blocks */
    strerr_diefu1sys(111, "open " LOGFIFO " for writing") ;
  if (fd_copy(1 + redirect, 2 - redirect) == -1)
    strerr_diefu1sys(111, "redirect output file descriptor") ;
  xpathexec_r(childargv, envp, envlen, envmodifs->s, envmodifs->len) ;
}

int main (int argc, char const **argv, char const *const *envp)
{
  mode_t mask = 0022 ;
  char const *basedir = BASEDIR ;
  char const *path = INITPATH ;
  char const *slashdev = 0 ;
  char const *envdumpdir = 0 ;
  stralloc envmodifs = STRALLOC_ZERO ;
  int redirect = 0 ;
  PROG = "s6-linux-init" ;

  if (getpid() != 1)
  {
    argv[0] = S6_LINUX_INIT_BINPREFIX "/s6-linux-init-telinit" ;
    pathexec_run(argv[0], argv, envp) ;
    strerr_dieexec(111, argv[0]) ;
  }

  {
    subgetopt_t l = SUBGETOPT_ZERO ;
    for (;;)
    {
      int opt = subgetopt_r(argc, argv, "rc:p:s:m:d:", &l) ;
      if (opt == -1) break ;
      switch (opt)
      {
        case 'r' : redirect = 1 ; break ;
        case 'c' : basedir = l.arg ; break ;
        case 'p' : path = l.arg ; break ;
        case 's' : envdumpdir = l.arg ; break ;
        case 'm' : if (!uint0_oscan(l.arg, &mask)) dieusage() ; break ;
        case 'd' : slashdev = l.arg ; break ;
        default : dieusage() ;
      }
    }
    argc -= l.ind ; argv += l.ind ;
  }

  allwrite(1, BANNER, sizeof(BANNER) - 1) ;
  if (chdir("/") == -1) strerr_diefu1sys(111, "chdir to /") ;
  umask(mask) ;
  setpgid(0, 0) ;
  fd_close(0) ;
  if (slashdev)
  {
    fd_close(1) ;
    fd_close(2) ;
    if (mount("dev", slashdev, "devtmpfs", MS_NOSUID | MS_NOEXEC, "") == -1)
    {
      int e = errno ;
      open("/dev/null", O_RDONLY) ;
      open("/dev/console", O_WRONLY) ;
      fd_copy(2, 1) ;
      errno = e ;
      strerr_diefu2sys(111, "mount ", slashdev) ;
    }
    if (open("/dev/console", O_WRONLY)
     || fd_copy(1, 0) == -1
     || fd_move(2, 0) == -1) return 111 ;
  }
  if (open("/dev/null", O_RDONLY)) strerr_diefu1sys(111, "open /dev/null") ;
  
  if (umount(S6_LINUX_INIT_TMPFS) == -1)
  {
    if (errno != EINVAL)
      strerr_warnwu1sys("umount " S6_LINUX_INIT_TMPFS) ;
  }
  if (mount("tmpfs", S6_LINUX_INIT_TMPFS, "tmpfs", MS_NODEV | MS_NOSUID, "mode=0755") == -1)
    strerr_diefu1sys(111, "mount tmpfs on " S6_LINUX_INIT_TMPFS) ;
  {
    size_t dirlen = strlen(basedir) ;
    char fn[dirlen + 1 + (sizeof(RUNIMAGE) > sizeof(ENVSTAGE1) ? sizeof(RUNIMAGE) : sizeof(ENVSTAGE1))] ;
    memcpy(fn, basedir, dirlen) ;
    fn[dirlen] = '/' ;
    memcpy(fn + dirlen + 1, RUNIMAGE, sizeof(RUNIMAGE)) ;
    if (!hiercopy(fn, S6_LINUX_INIT_TMPFS))
      strerr_diefu3sys(111, "copy ", fn, " to " S6_LINUX_INIT_TMPFS) ;
    memcpy(fn + dirlen + 1, ENVSTAGE1, sizeof(ENVSTAGE1)) ;
    if (envdir(fn, &envmodifs) == -1)
      strerr_warnwu2sys("envdir ", fn) ;
  }
  if (envdumpdir && !env_dump(envdumpdir, 0700, envp))
    strerr_warnwu2sys("dump kernel environment to ", envdumpdir) ;
  {
    int fdr = open_read(LOGFIFO) ;
    if (fdr == -1) strerr_diefu1sys(111, "open " LOGFIFO) ;
    fd_close(1) ;
    if (open(LOGFIFO, O_WRONLY) != 1) strerr_diefu1sys(111, "open " LOGFIFO) ;
    fd_close(fdr) ;
  }
  {
    static char const *const newargv[5] = { S6_EXTBINPREFIX "s6-svscan", "-st0", "--", S6_LINUX_INIT_TMPFS "/" SCANDIR, 0 } ;
    char const *newenvp[2] = { 0, 0 } ;
    pid_t pid ;
    size_t pathlen = path ? strlen(path) : 0 ;
    char pathvar[6 + pathlen] ;
    if (path)
    {
      if (setenv("PATH", path, 1) == -1)
        strerr_diefu1sys(111, "set initial PATH") ;
      memcpy(pathvar, "PATH=", 5) ;
      memcpy(pathvar + 5, path, pathlen + 1) ;
      newenvp[0] = pathvar ;
    }
    pid = fork() ;
    if (pid == -1) strerr_diefu1sys(111, "fork") ;
    if (!pid) run_stage2(basedir, argv, argc, newenvp, !!path, &envmodifs, redirect) ;
    if (fd_copy(2, 1) == -1)
      strerr_diefu1sys(111, "redirect output file descriptor") ;
    xpathexec_r(newargv, newenvp, !!path, envmodifs.s, envmodifs.len) ;
  }
}