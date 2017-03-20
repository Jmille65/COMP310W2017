#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <unistd.h>
#include <string.h>
#include <semaphore.h>

#define PODS 256
#define KV_IN_PODS 256
#define KEYSIZE 32
#define VALSIZE 256

struct KV { // contains key and value strings
  char key[KEYSIZE];
  char value[VALSIZE];
};

struct KV_POD { // container for KVs with index for FIFO updating
  struct KV key_vals[KV_IN_PODS];
  int insert_index;
};

struct KV_STORE { // container for KV pods in shared store
  struct KV_POD kv_pods[PODS];
};

int hash(char*);
int kv_store_create(char*);
int kv_store_write(char*, char*);
char *kv_store_read(char*);
char **kv_store_read_all(char*);
int kv_delete_db();

struct KV_STORE* store; // contains the kv-store
int pod_read_index[PODS]; // local index used for reading
