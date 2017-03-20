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
} KV;

struct KV_POD { // container for KVs with index for FIFO updating
  struct KV key_vals[KV_IN_PODS];
  int insert_index;
} KV_POD;

struct KV_STORE { // container for KV pods in shared store
  struct KV_POD kv_pods[PODS];
} KV_STORE;

int hash(char*);
int kv_store_create(char*);
int kv_store_write(char*, char*);
char *kv_store_read(char*);
char **kv_store_read_all(char*);
int kv_delete_db();

struct KV_STORE* store; // contains the kv-store
int pod_read_index[PODS]; // local index used for reading
char* kv_store_name; // contains name given to kv-store in create; used for del

// USER IS RESPONSIBLE FOR FREEING RETURNED CHAR**
char** kv_store_read_all(char* key) {
  char **value_list = malloc((KV_IN_PODS + 1) * sizeof(char*));
  int pod_no = hash(key);
  int tmp_index = pod_read_index[pod_no];
  pod_read_index[pod_no] = (store->kv_pods[pod_no].insert_index) % KV_IN_PODS;

  char *init_val = kv_store_read(key);
  int init_val_index = pod_read_index[pod_no] - 1;
  int val_list_index = 0;

  if (init_val == NULL) {
    return NULL;
  } else {
    value_list[val_list_index++] = init_val;
  }

  char *temp_val;
  if (strlen(key) > KEYSIZE-1) key[KEYSIZE-1] = '\0';

  for (int i = pod_read_index[pod_no]; i != init_val_index; i = (i+1) % KV_IN_PODS) {
    if (strcmp(key, (store->kv_pods[pod_no].key_vals[i].key)) == 0) {
      temp_val = malloc(strlen(store->kv_pods[pod_no].key_vals[i].value) + 1);
      strcpy(temp_val, (store->kv_pods[pod_no].key_vals[i].value));
      if (strcmp(init_val, temp_val) != 0) {
        value_list[val_list_index++] = temp_val;
      } else {
        free(temp_val);
      }
    }
  }
  value_list[val_list_index] = NULL;

  pod_read_index[pod_no] = tmp_index;
  return value_list;
}

int kv_store_write(char* key, char* value) {
  if (strlen(key) > KEYSIZE-1) key[KEYSIZE-1] = '\0';
  if (strlen(value) > VALSIZE-1) value[VALSIZE-1] = '\0';
  int pod_no = hash(key);

  int new_index = (store->kv_pods[pod_no].insert_index);
  struct KV new_kv;
  strcpy(new_kv.key, key);
  strcpy(new_kv.value, value);
  memcpy(&(store->kv_pods[pod_no].key_vals[(new_index++)%KV_IN_PODS]), &new_kv, sizeof(new_kv));
  memcpy(&(store->kv_pods[pod_no].insert_index), &new_index, sizeof(int));
  return 0;
}

char* kv_store_read(char* key) {
  if (strlen(key) > KEYSIZE-1) key[KEYSIZE-1] = '\0';
  int pod_no = hash(key);
  pod_read_index[pod_no] = pod_read_index[pod_no] % KV_IN_PODS;
  int original_read_index = pod_read_index[pod_no];

  //initial test- need to move read index past original so control enters subsequent while loop
  if (strcmp(key, (store->kv_pods[pod_no].key_vals[pod_read_index[pod_no]].key)) == 0) {
    char* value = malloc(strlen(store->kv_pods[pod_no].key_vals[pod_read_index[pod_no]].value) + 1);
    strcpy(value, (store->kv_pods[pod_no].key_vals[pod_read_index[pod_no]].value));
    pod_read_index[pod_no]++;
    return value;
  }

  while (original_read_index != ++pod_read_index[pod_no]%KV_IN_PODS) {
    pod_read_index[pod_no] = pod_read_index[pod_no] % KV_IN_PODS;
    if (strcmp(key, (store->kv_pods[pod_no].key_vals[pod_read_index[pod_no]].key)) == 0) {
      char* value = malloc(strlen(store->kv_pods[pod_no].key_vals[pod_read_index[pod_no]].value) + 1);
      strcpy(value, (store->kv_pods[pod_no].key_vals[pod_read_index[pod_no]++].value));
      return value;
    }
  }
  return NULL;
}

int kv_delete_db() {
  if (kv_store_name == NULL) return (-1); // if no store open, impossible to delete
  munmap(store, sizeof(*store));
  char* file_name = malloc((strlen(kv_store_name)+20) * sizeof(char));
  sprintf(file_name, "/dev/shm/%s", kv_store_name);
  int exit_code = remove(file_name);
  free(file_name);
  return exit_code;
}

int kv_store_create(char* name) {
  int fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, S_IRWXU);
  if (fd > -1) { //if new store
    store = mmap(NULL, sizeof(KV_STORE), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    ftruncate(fd, sizeof(KV_STORE));
    close(fd);
    for (int i = 0; i < PODS; i++) {
      store->kv_pods[i].insert_index = 0;
    }
    kv_store_name = name;
  }
  else { //if store exists try
    fd = shm_open(name, O_CREAT | O_RDWR, S_IRWXU);
    if (fd < 0) { //if shm_open still doesn't work
      printf("shm_open error\n");
      return -1;
    }
    store = mmap(NULL, sizeof(KV_STORE), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    ftruncate(fd, sizeof(KV_STORE));
    close(fd);

    kv_store_name = name;
  }

  if (fd < 0) { //if shm_open still doesn't work
    printf("shm_open error\n");
    return -1;
  }

  for (int i = 0; i < PODS; i++) {
    pod_read_index[i] = 0;
  }

  return 0;
}

int hash(char* key) {
  int hash = 0;
  for (int index = 0; index < strlen(key); index++) {
    hash += key[index];
  }
  return (hash%PODS);
}
