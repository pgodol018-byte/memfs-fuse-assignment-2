#include <sys/stat.h>
#include "locks.h"

#ifndef _NODE_H
#define _NODE_H

struct node {
  struct stat   vstat;
  void         *data;
  unsigned int  fd_count;
  int           delete_on_close;
  struct rwmutex lock;
};

int getnoderelativeto(const char *path, struct node *root, struct node **node);

#endif

