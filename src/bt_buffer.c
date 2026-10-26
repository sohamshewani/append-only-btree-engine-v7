#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <sys/uio.h>
#include <sys/types.h>
#include <immintrin.h>

#define BT_PAGE_SIZE 4096
#define BT_PAGE_MAGIC 0x4254524550414745ULL // "BTREPAGE"
#define BT_BUFFER_HASH_SIZE 1024
#define BT_BUFFER_DEFAULT_FRAMES 256

typedef uint64_t lsn_t;
typedef uint64_t txid_t;

typedef struct bt_page_header {
    uint64_t magic;
    lsn_t lsn;
    txid_t txid;
    uint16_t flags;
    uint16_t lower;
    uint16_t upper;
    uint16_t special;
    uint32_t checksum;
    uint32_t unused;
} __attribute__((packed)) bt_page_header_t;

typedef struct bt_page {
    bt_page_header_t header;
    uint8_t data[BT_PAGE_SIZE - sizeof(bt_page_header_t)];
} __attribute__((packed)) bt_page_t;

typedef struct bt_frame {
    uint32_t frame_id;
    uint64_t page_offset;
    bt_page_t *page;
    int pin_count;
    bool is_dirty;
    bool is_valid;
    lsn_t page_lsn;
    pthread_rwlock_t rwlock;
    struct bt_frame *lru_prev;
    struct bt_frame *lru_next;
    struct bt_frame *hash_next;
} bt_frame_t;

typedef struct bt_buffer_pool {
    int fd;
    size_t capacity;
    bt_frame_t *frames;
    bt_frame_t *hash_table[BT_BUFFER_HASH_SIZE];
    bt_frame_t lru_head;
    bt_frame_t lru_tail;
    pthread_mutex_t latch;
    pthread_cond_t cv_evict;
    bool shutdown;
    pthread_t flusher_thread;
} bt_buffer_pool_t;

static uint32_t bt_buffer_hash(uint64_t offset) {
    offset = (~offset) + (offset << 21);
    offset = offset ^ (offset >> 24);
    offset = (offset + (offset << 3)) + (offset << 8);
    offset = offset ^ (offset >> 14);
    offset = (offset + (offset << 2)) + (offset << 4);
    offset = offset ^ (offset >> 28);
    offset = offset + (offset << 31);
    return (uint32_t)(offset % BT_BUFFER_HASH_SIZE);
}

static uint32_t bt_compute_crc32c(const uint8_t *data, size_t length) {
    uint32_t crc = 0xFFFFFFFF;
    size_t i = 0;
#if defined(__x86_64__)
    for (; i + 8 <= length; i += 8) {
        crc = (uint32_t)_mm_crc32_u64(crc, *(const uint64_t *)(data + i));
    }
    for (; i < length; i++) {
        crc = (uint32_t)_mm_crc32_u8(crc, data[i]);
    }
#else
    for (i = 0; i < length; i++) {
        crc = (crc >> 8) ^ (uint32_t)data[i];
    }
#endif
    return ~crc;
}

static void bt_lru_remove(bt_buffer_pool_t *pool, bt_frame_t *frame) {
    if (frame->lru_prev) {
        frame->lru_prev->lru_next = frame->lru_next;
    }
    if (frame->lru_next) {
        frame->lru_next->lru_prev = frame->lru_prev;
    }
    frame->lru_prev = NULL;
    frame->lru_next = NULL;
}

static void bt_lru_push_front(bt_buffer_pool_t *pool, bt_frame_t *frame) {
    frame->lru_prev = &pool->lru_head;
    frame->lru_next = pool->lru_head.lru_next;
    pool->lru_head.lru_next->lru_prev = frame;
    pool->lru_head.lru_next = frame;
}

static void bt_hash_remove(bt_buffer_pool_t *pool, bt_frame_t *frame) {
    uint32_t idx = bt_buffer_hash(frame->page_offset);
    bt_frame_t *curr = pool->hash_table[idx];
    bt_frame_t *prev = NULL;
    while (curr) {
        if (curr == frame) {
            if (prev) {
                prev->hash_next = curr->hash_next;
            } else {
                pool->hash_table[idx] = curr->hash_next;
            }
            curr->hash_next = NULL;
            return;
        }
        prev = curr;
        curr = curr->hash_next;
    }
}

static void bt_hash_insert(bt_buffer_pool_t *pool, bt_frame_t *frame) {
    uint32_t idx = bt_buffer_hash(frame->page_offset);
    frame->hash_next = pool->hash_table[idx];
    pool->hash_table[idx] = frame;
}

static bt_frame_t *bt_hash_find(bt_buffer_pool_t *pool, uint64_t offset) {
    uint32_t idx = bt_buffer_hash(offset);
    bt_frame_t *curr = pool->hash_table[idx];
    while (curr) {
        if (curr->page_offset == offset) {
            return curr;
        }
        curr = curr->hash_next;
    }
    return NULL;
}

int bt_buffer_pool_init(bt_buffer_pool_t *pool, int fd, size_t capacity) {
    if (!pool || capacity == 0) {
        return -EINVAL;
    }
    memset(pool, 0, sizeof(bt_buffer_pool_t));
    pool->fd = fd;
    pool->capacity = capacity;
    
    if (pthread_mutex_init(&pool->latch, NULL) != 0) {
        return -EAGAIN;
    }
    if (pthread_cond_init(&pool->cv_evict, NULL) != 0) {
        pthread_mutex_destroy(&pool->latch);
        return -EAGAIN;
    }

    pool->frames = (bt_frame_t *)malloc(capacity * sizeof(bt_frame_t));
    if (!pool->frames) {
        pthread_cond_destroy(&pool->cv_evict);
        pthread_mutex_destroy(&pool->latch);
        return -ENOMEM;
    }
    memset(pool->frames, 0, capacity * sizeof(bt_frame_t));

    for (size_t i = 0; i < capacity; i++) {
        void *mem = NULL;
        if (posix_memalign(&mem, BT_PAGE_SIZE, BT_PAGE_SIZE) != 0) {
            for (size_t j = 0; j < i; j++) {
                free(pool->frames[j].page);
                pthread_rwlock_destroy(&pool->frames[j].rwlock);
            }
            free(pool->frames);
            pthread_cond_destroy(&pool->cv_evict);
            pthread_mutex_destroy(&pool->latch);
            return -ENOMEM;
        }
        memset(mem, 0, BT_PAGE_SIZE);
        pool->frames[i].frame_id = (uint32_t)i;
        pool->frames[i].page = (bt_page_t *)mem;
        pool->frames[i].page_offset = (uint64_t)-1;
        if (pthread_rwlock_init(&pool->frames[i].rwlock, NULL) != 0) {
            free(mem);
            for (size_t j = 0; j < i; j++) {
                free(pool->frames[j].page);
                pthread_rwlock_destroy(&pool->frames[j].rwlock);
            }
            free(pool->frames);
            pthread_cond_destroy(&pool->cv_evict);
            pthread_mutex_destroy(&pool->latch);
            return -EAGAIN;
        }
    }

    pool->lru_head.lru_prev = NULL;
    pool->lru_head.lru_next = &pool->lru_tail;
    pool->lru_tail.lru_prev = &pool->lru_head;
    pool->lru_tail.lru_next = NULL;

    for (size_t i = 0; i < capacity; i++) {
        bt_lru_push_front(pool, &pool->frames[i]);
    }

    return 0;
}

static int bt_buffer_write_frame(bt_buffer_pool_t *pool, bt_frame_t *frame) {
    if (!frame->is_dirty) {
        return 0;
    }
    frame->page->header.checksum = 0;
    frame->page->header.checksum = bt_compute_crc32c((const uint8_t *)frame->page, BT_PAGE_SIZE);
    
    off_t offset = (off_t)frame->page_offset;
    ssize_t written = pwrite(pool->fd, frame->page, BT_PAGE_SIZE, offset);
    if (written != BT_PAGE_SIZE) {
        return errno ? -errno : -EIO;
    }
    frame->is_dirty = false;
    return 0;
}

static void *bt_buffer_flusher_main(void *arg) {
    bt_buffer_pool_t *pool = (bt_buffer_pool_t *)arg;
    while (true) {
        pthread_mutex_lock(&pool->latch);
        if (pool->shutdown) {
            pthread_mutex_unlock(&pool->latch);
            break;
        }
        
        for (size_t i = 0; i < pool->capacity; i++) {
            bt_frame_t *frame = &pool->frames[i];
            if (pthread_rwlock_trywrlock(&frame->rwlock) == 0) {
                if (frame->is_dirty && frame->pin_count == 0 && frame->is_valid) {
                    bt_buffer_write_frame(pool, frame);
                }
                pthread_rwlock_unlock(&frame->rwlock);
            }
        }
        
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 1;
        pthread_cond_timedwait(&pool->cv_evict, &pool->latch, &ts);
        pthread_mutex_unlock(&pool->latch);
    }
    return NULL;
}

int bt_buffer_pool_start_flusher(bt_buffer_pool_t *pool) {
    if (!pool) return -EINVAL;
    int err = pthread_create(&pool->flusher_thread, NULL, bt_buffer_flusher_main, pool);
    return err ? -err : 0;
}

int bt_buffer_pool_shutdown(bt_buffer_pool_t *pool) {
    if (!pool) return -EINVAL;
    
    pthread_mutex_lock(&pool->latch);
    pool->shutdown = true;
    pthread_cond_broadcast(&pool->cv_evict);
    pthread_mutex_unlock(&pool->latch);
    
    pthread_join(pool->flusher_thread, NULL);

    pthread_mutex_lock(&pool->latch);
    for (size_t i = 0; i < pool->capacity; i++) {
        bt_frame_t *frame = &pool->frames[i];
        pthread_rwlock_wrlock(&frame->rwlock);
        if (frame->is_dirty && frame->is_valid) {
            bt_buffer_write_frame(pool, frame);
        }
        pthread_rwlock_unlock(&frame->rwlock);
    }
    pthread_mutex_unlock(&pool->latch);

    for (size_t i = 0; i < pool->capacity; i++) {
        free(pool->frames[i].page);
        pthread_rwlock_destroy(&pool->frames[i].rwlock);
    }
    free(pool->frames);
    pthread_cond_destroy(&pool->cv_evict);
    pthread_mutex_destroy(&pool->latch);
    return 0;
}

static bt_frame_t *bt_buffer_evict_candidate(bt_buffer_pool_t *pool) {
    bt_frame_t *curr = pool->lru_tail.lru_prev;
    while (curr != &pool->lru_head) {
        if (curr->pin_count == 0) {
            if (pthread_rwlock_trywrlock(&curr->rwlock) == 0) {
                if (curr->pin_count == 0) {
                    return curr;
                }
                pthread_rwlock_unlock(&curr->rwlock);
            }
        }
        curr = curr->lru_prev;
    }
    return NULL;
}

int bt_buffer_fetch_page(bt_buffer_pool_t *pool, uint64_t page_offset, bt_page_t **out_page, bt_frame_t **out_frame) {
    if (!pool || !out_page || !out_frame) {
        return -EINVAL;
    }

    pthread_mutex_lock(&pool->latch);

    bt_frame_t *frame = bt_hash_find(pool, page_offset);
    if (frame) {
        pthread_rwlock_wrlock(&frame->rwlock);
        frame->pin_count++;
        bt_lru_remove(pool, frame);
        bt_lru_push_front(pool, frame);
        pthread_mutex_unlock(&pool->latch);

        *out_page = frame->page;
        *out_frame = frame;
        return 0;
    }

    while (true) {
        frame = bt_buffer_evict_candidate(pool);
        if (frame) {
            break;
        }
        pthread_cond_wait(&pool->cv_evict, &pool->latch);
    }

    if (frame->is_dirty && frame->is_valid) {
        int err = bt_buffer_write_frame(pool, frame);
        if (err < 0) {
            pthread_rwlock_unlock(&frame->rwlock);
            pthread_mutex_unlock(&pool->latch);
            return err;
        }
    }

    if (frame->is_valid) {
        bt_hash_remove(pool, frame);
        frame->is_valid = false;
    }

    frame->page_offset = page_offset;
    bt_hash_insert(pool, frame);
    bt_lru_remove(pool, frame);
    bt_lru_push_front(pool, frame);
    
    pthread_mutex_unlock(&pool->latch);

    off_t disk_offset = (off_t)page_offset;
    ssize_t nread = pread(pool->fd, frame->page, BT_PAGE_SIZE, disk_offset);
    if (nread == 0) {
        memset(frame->page, 0, BT_PAGE_SIZE);
        frame->page->header.magic = BT_PAGE_MAGIC;
        frame->page->header.lsn = 0;
        frame->page->header.flags = 0;
        frame->page->header.