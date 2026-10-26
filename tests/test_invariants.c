#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>

/* ========================================================================= */
/* APPEND-ONLY B-TREE ENGINE IMPLEMENTATION (Embedded for testing completeness)*/
/* ========================================================================= */

#define BTREE_ORDER 4 /* Small order to force splits and height increases */
#define PAGE_SIZE 4096
#define MAGIC_HEADER 0x425452455355434BULL /* "BTRESUCK" */

typedef uint64_t lsn_t;

typedef struct {
    uint64_t magic;
    uint64_t root_offset;
    uint64_t next_free_offset;
    lsn_t last_lsn;
} __attribute__((packed)) btree_header_t;

typedef struct {
    bool is_leaf;
    uint16_t num_keys;
    uint64_t keys[BTREE_ORDER - 1];
    uint64_t children[BTREE_ORDER]; /* Child offsets if internal, Value offsets if leaf */
} __attribute__((packed)) btree_node_disk_t;

typedef struct {
    int fd;
    pthread_rwlock_t rwlock;
    btree_header_t header;
    bool dirty_header;
} btree_engine_t;

static uint64_t align_up(uint64_t offset, uint64_t alignment) {
    return (offset + alignment - 1) & ~(alignment - 1);
}

static int pwrite_all(int fd, const void *buf, size_t count, off_t offset) {
    size_t written = 0;
    while (written < count) {
        ssize_t n = pwrite(fd, (const char *)buf + written, count - written, offset + written);
        if (n <= 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        written += n;
    }
    return 0;
}

static int pread_all(int fd, void *buf, size_t count, off_t offset) {
    size_t read_bytes = 0;
    while (read_bytes < count) {
        ssize_t n = pread(fd, (char *)buf + read_bytes, count - read_bytes, offset + read_bytes);
        if (n <= 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        read_bytes += n;
    }
    return 0;
}

btree_engine_t *btree_open(const char *path) {
    int fd = open(path, O_RDWR | O_CREAT, 0644);
    if (fd < 0) return NULL;

    btree_engine_t *engine = malloc(sizeof(btree_engine_t));
    if (!engine) {
        close(fd);
        return NULL;
    }

    engine->fd = fd;
    pthread_rwlock_init(&engine->rwlock, NULL);
    engine->dirty_header = false;

    struct stat st;
    if (fstat(fd, &st) < 0) {
        free(engine);
        close(fd);
        return NULL;
    }

    if (st.st_size == 0) {
        /* Initialize new file */
        engine->header.magic = MAGIC_HEADER;
        engine->header.root_offset = 0;
        engine->header.next_free_offset = PAGE_SIZE;
        engine->header.last_lsn = 0;

        btree_node_disk_t root;
        memset(&root, 0, sizeof(root));
        root.is_leaf = true;
        root.num_keys = 0;

        if (pwrite_all(fd, &engine->header, sizeof(engine->header), 0) < 0 ||
            pwrite_all(fd, &root, sizeof(root), PAGE_SIZE) < 0) {
            free(engine);
            close(fd);
            return NULL;
        }
    } else {
        if (pread_all(fd, &engine->header, sizeof(engine->header), 0) < 0 ||
            engine->header.magic != MAGIC_HEADER) {
            free(engine);
            close(fd);
            return NULL;
        }
    }

    return engine;
}

void btree_close(btree_engine_t *engine) {
    if (!engine) return;
    if (engine->dirty_header) {
        pwrite_all(engine->fd, &engine->header, sizeof(engine->header), 0);
    }
    pthread_rwlock_destroy(&engine->rwlock);
    close(engine->fd);
    free(engine);
}

static uint64_t btree_alloc_node(btree_engine_t *engine) {
    uint64_t offset = engine->header.next_free_offset;
    engine->header.next_free_offset = align_up(offset + sizeof(btree_node_disk_t), PAGE_SIZE);
    engine->dirty_header = true;
    return offset;
}

static int btree_read_node(btree_engine_t *engine, uint64_t offset, btree_node_disk_t *node) {
    return pread_all(engine->fd, node, sizeof(btree_node_disk_t), offset);
}

static int btree_write_node(btree_engine_t *engine, uint64_t offset, const btree_node_disk_t *node) {
    return pwrite_all(engine->fd, node, sizeof(btree_node_disk_t), offset);
}

static int btree_search_node(const btree_node_disk_t *node, uint64_t key, uint16_t *idx) {
    uint16_t low = 0, high = node->num_keys;
    while (low < high) {
        uint16_t mid = low + (high - low) / 2;
        if (node->keys[mid] >= key) {
            high = mid;
        } else {
            low = mid + 1;
        }
    }
    *idx = low;
    if (low < node->num_keys && node->keys[low] == key) {
        return 0; /* Found */
    }
    return -1; /* Not found */
}

bool btree_get(btree_engine_t *engine, uint64_t key, uint64_t *val_out) {
    pthread_rwlock_rdlock(&engine->rwlock);

    if (engine->header.root_offset == 0) {
        pthread_rwlock_unlock(&engine->rwlock);
        return false;
    }

    uint64_t curr_offset = engine->header.root_offset;
    btree_node_disk_t node;

    while (1) {
        if (btree_read_node(engine, curr_offset, &node) < 0) {
            pthread_rwlock_unlock(&engine->rwlock);
            return false;
        }

        uint16_t idx;
        int found = btree_search_node(&node, key, &idx);

        if (node.is_leaf) {
            if (found == 0) {
                *val_out = node.children[idx];
                pthread_rwlock_unlock(&engine->rwlock);
                return true;
            }
            pthread_rwlock_unlock(&engine->rwlock);
            return false;
        } else {
            if (found == 0) {
                idx++; /* In internal nodes, if key matches at idx, go to right child */
            }
            curr_offset = node.children[idx];
            if (curr_offset == 0) {
                pthread_rwlock_unlock(&engine->rwlock);
                return false;
            }
        }
    }
}

typedef struct {
    bool split;
    uint64_t promoted_key;
    uint64_t new_child_offset;
} btree_insert_result_t;

static btree_insert_result_t btree_insert_recursive(btree_engine_t *engine, uint64_t curr_offset, uint64_t key, uint64_t val) {
    btree_node_disk_t node;
    btree_read_node(engine, curr_offset, &node);

    uint16_t idx;
    int found = btree_search_node(&node, key, &idx);
    if (node.is_leaf) {
        if (found == 0) {
            /* Update existing key */
            node.children[idx] = val;
            btree_write_node(engine, curr_offset, &node);
            return (btree_insert_result_t){ .split = false };
        }

        if (node.num_keys < BTREE_ORDER - 1) {
            /* Shift keys and values */
            for (int i = node.num_keys; i > idx; i--) {
                node.keys[i] = node.keys[i - 1];
                node.children[i] = node.children[i - 1];
            }
            node.keys[idx] = key;
            node.children[idx] = val;
            node.num_keys++;
            btree_write_node(engine, curr_offset, &node);
            return (btree_insert_result_t){ .split = false };
        } else {
            /* Split leaf node */
            uint64_t new_node_offset = btree_alloc_node(engine);
            btree_node_disk_t new_node;
            memset(&new_node, 0, sizeof(new_node));
            new_node.is_leaf = true;

            int total_keys = BTREE_ORDER;
            /* Create temporary arrays */
            uint64_t temp_keys[BTREE_ORDER];
            uint64_t temp_vals[BTREE_ORDER];

            int t_idx = 0;
            for (int i = 0; i < node.num_keys; i++) {
                if (t_idx == idx) {
                    temp_keys[t_idx] = key;
                    temp_vals[t_idx] = val;
                    t_idx++;
                }
                temp_keys[t_idx] = node.keys[i];
                temp_vals[t_idx] = node.children[i];
                t_idx++;
            }
            if (t_idx == idx) {
                temp_keys[t_idx] = key;
                temp_vals[t_idx] = val;
            }

            int mid = total_keys / 2;
            node.num_keys = mid;
            for (int i = 0; i < mid; i++) {
                node.keys[i] = temp_keys[i];
                node.children[i] = temp_vals[i];
            }

            new_node.num_keys = total_keys - mid;
            for (int i = 0; i < new_node.num_keys; i++) {
                new_node.keys[i] = temp_keys[mid + i];
                new_node.children[i] = temp_vals[mid + i];
            }

            btree_write_node(engine, curr_offset, &node);
            btree_write_node(engine, new_node_offset, &new_node);

            return (btree_insert_result_t){
                .split = true,
                .promoted_key = new_node.keys[0],
                .new_child_offset = new_node_offset
            };
        }
    } else {
        if (found == 0) idx++;
        uint64_t child_offset = node.children[idx];
        btree_insert_result_t res = btree_insert_recursive(engine, child_offset, key, val);

        if (!res.split) {
            return (btree_insert_