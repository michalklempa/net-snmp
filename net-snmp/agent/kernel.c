
/*
 *  13 Jun 91  wsak (wk0x@andrew) added mips support
 */

#include <sys/types.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include "asn1.h"
#include "snmp_impl.h"

#ifndef NULL
#define NULL 0
#endif

static int kmem;
int swap, mem;

init_kmem(file)
    char *file;
{
  kmem = open(file, O_RDONLY);
  if (kmem < 0){
    fprintf(stderr, "cannot open ");
    perror(file);
    exit(1);
  }
  fcntl(kmem,F_SETFD,1);
  mem = open("/dev/mem",O_RDONLY);    
  if (mem < 0){
    fprintf(stderr, "cannot open ");
    perror("/dev/mem");
    exit(1);
  }
  fcntl(mem,F_SETFD,1);
#ifndef __alpha
#ifdef hpux
  swap = open("/dev/dmem",O_RDONLY);
#else
  swap = open("/dev/drum",0);
#endif
  if (swap < 0){
    fprintf(stderr, "cannot open ");
    perror("/dev/drum");
    exit(1);
  }
  fcntl(swap,F_SETFD,1);
#endif
}


/*
 *  Seek into the kernel for a value.
 */
off_t
klseek(base)
     off_t base;
{
  return (lseek(kmem, (off_t)base, 0));
}


/*
 *  Read from the kernel 
 */
int
klread(buf, buflen)
    char *buf;
    int buflen;
{
  return (read(kmem, buf, buflen));
}



/*
 *  klookup:
 *
 *  It seeks to the location  off  in kmem
 *  It does a read into  target  of  siz  bytes.
 *
 *  Return NULL on failure and 1 on sucess.
 *
 */



klookup(off, target, siz) 
     int     off;
     char   *target;
     int     siz;
{

  klseek(off);
  if (siz != klread(target, siz)) {
    ERROR("klread\n");
#ifdef EXIT_ON_BAD_KLREAD
    exit(-1);
#endif
    return(NULL);
  }

  return (1);

}

