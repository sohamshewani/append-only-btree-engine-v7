#include "bt_engine.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/uio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <immintrin.h>

#define BT_PAGE_SIZE 4096
#define BT_MAGIC 0x4254524545303031ULL // "BTREE001"
#define BT_HASH_BUCKETS 1024
#define BT_MAX_HAZARD_POINTERS 64

typedef enum {
    BT_PAGE_LEAF = 0x01,
    BT_PAGE_INTERNAL = 0x02,
    BT_PAGE_OVERFLOW = 0x04,
    BT_PAGE_DIRTY = 0x08
} bt_page_flags_t;

typedef struct __attribute__((packed)) {
    uint64_t magic;
    uint64_t lsn;
    uint64_t tx_id;
    uint64_t next_page_offset;
    uint16_t flags;
    uint16_t num_slots;
    uint16_t free_space_offset;
    uint16_t reserved;
} bt_page_header_t;

typedef struct __attribute__((packed)) {
    uint16_t key_offset;
    uint16_t key_length;
    uint64_t value_offset;
    uint32_t value_length;
} bt_slot_t;

struct bt_page {
    bt_page_header_t header;
    uint8_t data[BT_PAGE_SIZE - sizeof(bt_page_header_t)];
};

typedef struct bt_frame {
    uint64_t page_offset;
    bt_page_t page;
    _Atomic uint32_t pin_count;
    uint32_t rw_latch; // 0: unlocked, 1: locked writer, >1: reader count
    bool dirty;
    uint64_t lsn;
    struct bt_frame *lru_prev;
    struct bt_frame *lru_next;
    struct bt_frame *hash_next;
} bt_frame_t;

typedef struct {
    bt_frame_t *buckets[BT_HASH_BUCKETS];
    bt_frame_t *lru_head;
    bt_frame_t *lru_tail;
    size_t capacity;
    size_t size;
    pthread_mutex_t mutex;
} bt_buffer_pool_t;

typedef struct {
    _Atomic(void*) ptr;
} bt_hazard_pointer_t;

struct bt_engine {
    int fd;
    int wal_fd;
    _Atomic uint64_t current_lsn;
    _Atomic uint64_t current_tx_id;
    bt_buffer_pool_t bp;
    _Atomic(bt_page_t*) root_ptr;
    bt_hazard_pointer_t hazards[BT_MAX_HAZARD_POINTERS];
    pthread_t flusher_thread;
    bool flusher_running;
    pthread_mutex_t flusher_mutex;
    pthread_cond_t flusher_cond;
};

static uint32_t bt_hash_offset(uint64_t offset) {
    offset = (~offset) + (offset << 21);
    offset = offset ^ (offset >> 24);
    offset = (offset + (offset << 3)) + (offset << 8);
    offset = offset ^ (offset >> 14);
    offset = (offset + (offset << 2)) + (offset << 4);
    offset = offset ^ (offset >> 28);
    offset = (offset + (offset << 31));
    return (uint32_t)(offset % BT_HASH_BUCKETS);
}

static uint32_t bt_crc32c(const void *data, size_t length) {
    uint32_t crc = 0xFFFFFFFF;
    const uint8_t *p = (const uint8_t *)data;
    for (size_t i = 0; i < length; i++) {
        crc = _mm_crc32_u8(crc, p[i]);
    }
    return ~crc;
}

bt_engine_t *bt_engine_open(const char *path, const char *wal_path, size_t pool_size) {
    bt_engine_t *engine = (bt_engine_t *)malloc(sizeof(bt_engine_t));
    if (!engine) {
        errno = ENOMEM;
        return NULL;
    }

    engine->fd = open(path, O_RDWR | O_CREAT | O_DIRECT, 0644);
    if (engine->fd < 0) {
        free(engine);
        return NULL;
    }

    engine->wal_fd = open(wal_path, O_RDWR | O_CREAT | O_SYNC, 0644);
    if (engine->wal_fd < 0) {
        close(engine->fd);
        free(engine);
        return NULL;
    }

    atomic_init(&engine->current_lsn, 1);
    atomic_init(&engine->current_tx_id, 1);
    atomic_init(&engine->root_ptr, NULL);

    pthread_mutex_init(&engine->bp.mutex, NULL);
    engine->bp.capacity = pool_size;
    engine->bp.size = 0;
    engine->bp.lru_head = NULL;
    engine->bp.lru_tail = NULL;
    for (int i = 0; i < BT_HASH_BUCKETS; i++) {
        engine->bp.buckets[i] = NULL;
    }

    for (int i = 0; i < BT_MAX_HAZARD_POINTERS; i++) {
        atomic_init(&engine->hazards[i].ptr, NULL);
    }

    return engine;
}

void bt_engine_close(bt_engine_t *engine) {
    if (!engine) return;

    pthread_mutex_lock(&engine->bp.mutex);
    bt_frame_t *curr = engine->bp.lru_head;
    while (curr) {
        bt_frame_t *next = curr->lru_next;
        if (curr->dirty) {
            lseek(engine->fd, curr->page_offset, SEEK_SET);
            write(engine->fd, &curr->page, BT_PAGE_SIZE);
        }
        free(curr);
        curr = next;
    }
    pthread_mutex_unlock(&engine->bp.mutex);

    pthread_mutex_destroy(&engine->bp.mutex);
    close(engine->fd);
    close(engine->wal_fd);
    free(engine);
}

static void bt_latch_wlock(bt_frame_t *frame) {
    while (1) {
        uint32_t expected = 0;
        if (__atomic_compare_exchange_n(&frame->rw_latch, &expected, 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
            break;
        }
        _mm_pause();
    }
}

static void bt_latch_wunlock(bt_frame_t *frame) {
    __atomic_store_n(&frame->rw_latch, 0, __ATOMIC_RELEASE);
}

static void bt_latch_rlock(bt_frame_t *frame) {
    while (1) {
        uint32_t val = __atomic_load_n(&frame->rw_latch, __ATOMIC_RELAXED);
        if (val == 1) {
            _mm_pause();
            continue;
        }
        if (__atomic_compare_exchange_n(&frame->rw_latch, &val, val + 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
            break;
        }
        _mm_pause();
    }
}

static void bt_latch_runlock(bt_frame_t *frame) {
    __atomic_fetch_sub(&frame->rw_latch, 1, __ATOMIC_RELEASE);
}

static bt_frame_t *bt_buffer_pool_fetch(bt_engine_t *engine, uint64_t page_offset) {
    pthread_mutex_lock(&engine->bp.mutex);
    uint32_t bucket = bt_hash_offset(page_offset);
    bt_frame_t *frame = engine->bp.buckets[bucket];

    while (frame) {
        if (frame->page_offset == page_offset) {
            atomic_fetch_add(&frame->pin_count, 1);
            pthread_mutex_unlock(&engine->bp.mutex);
            bt_latch_rlock(frame);
            return frame;
        }
        frame = frame->hash_next;
    }

    if (engine->bp.size >= engine->bp.capacity) {
        bt_frame_t *victim = engine->bp.lru_tail;
        while (victim) {
            if (atomic_load(&victim->pin_count) == 0 && victim->rw_latch == 0) {
                break;
            }
            victim = victim->lru_prev;
        }

        if (!victim) {
            pthread_mutex_unlock(&engine->bp.mutex);
            errno = EAGAIN;
            return NULL;
        }

        uint32_t old_bucket = bt_hash_offset(victim->page_offset);
        bt_frame_t **indirect = &engine->bp.buckets[old_bucket];
        while (*indirect && *indirect != victim) {
            indirect = &(*indirect)->hash_next;
        }
        if (*indirect) {
            *indirect = victim->hash_next;
        }

        if (victim->dirty) {
            lseek(engine->fd, victim->page_offset, SEEK_SET);
            write(engine->fd, &victim->page, BT_PAGE_SIZE);
            victim->dirty = false;
        }

        if (victim->lru_prev) victim->lru_prev->lru_next = victim->lru_next;
        if (victim->lru_next) victim->lru_next->lru_prev = victim->lru_prev;
        if (engine->bp.lru_head == victim) engine->bp.lru_head = victim->lru_next;
        if (engine->bp.lru_tail == victim) engine->bp.lru_tail = victim->lru_prev;

        frame = victim;
    } else {
        frame = (bt_frame_t *)malloc(sizeof(bt_frame_t));
        if (!frame) {
            pthread_mutex_unlock(&engine->bp.mutex);
            errno = ENOMEM;
            return NULL;
        }
        engine->bp.size++;
    }

    frame->page_offset = page_offset;
    frame->dirty = false;
    atomic_init(&frame->pin_count, 1);
    frame->rw_latch = 0;
    frame->lsn = 0;

    lseek(engine->fd, page_offset, SEEK_SET);
    ssize_t n = read(engine->fd, &frame->page, BT_PAGE_SIZE);
    if (n < BT_PAGE_SIZE) {
        memset(&frame->page, 0, BT_PAGE_SIZE);
        frame->page.header.magic = BT_MAGIC;
        frame->page.header.flags = BT_PAGE_LEAF | BT_PAGE_DIRTY;
        frame->page.header.free_space_offset = BT_PAGE_SIZE;
        frame->dirty = true;
    }

    frame->hash_next = engine->bp.buckets[bucket];
    engine->bp.buckets[bucket] = frame;

    frame->lru_next = engine->bp.lru_head;
    frame->lru_prev = NULL;
    if (engine->bp.lru_head) engine->bp.lru_head->lru_prev = frame;
    engine->bp.lru_head = frame;
    if (!engine->bp.lru_tail) engine->bp.lru_tail = frame;

    pthread_mutex_unlock(&engine->bp.mutex);
    bt_latch_rlock(frame);
    return frame;
}

static void bt_buffer_pool_unpin(bt_engine_t *engine, bt_frame_t *frame, bool is_writer) {
    if (is_writer) {
        bt_latch_wunlock(frame);
    } else {
        bt_latch_runlock(frame);
    }
    atomic_fetch_sub(&frame->pin_count, 1);
}

int bt_engine_insert(bt_engine_t *engine, const void *key, size_t key_len, const void *value, size_t val_len) {
    if (!engine || !key || key_len == 0) {
        errno = EINVAL;
        return -1;
    }

    uint64_t tx_id = atomic_fetch_add(&engine->current_tx_id, 1);
    uint64_t lsn = atomic_fetch_add(&engine->current_lsn, 1);

    struct {
        uint64_t lsn;
        uint64_t tx_id;
        uint32_t key_len;
        uint32_t val_len;
    } log_entry = {lsn, tx_id, (uint32_t)key_len, (uint32_t)val_len};

    struct iovec iov[3];
    iov[0].iov_base = &log_entry;
    iov[0].iov_len = sizeof(log_entry);
    iov[1].iov_base = (void *)key;
    iov[1].iov_len = key_len;
    iov[2].iov_base = (void *)value;
    iov[2].iov_len = val_len;

    pthread_mutex_lock(&engine->flusher_mutex);
    writev(engine->wal_fd, iov, 3);
    fdatasync(engine->wal_fd);
    pthread_mutex_unlock(&engine->flusher_mutex);

    pthread_mutex_lock(&engine->bp.mutex);
    bt_frame_t *frame = engine->bp.lru_head;
    if (!frame) {
        pthread_mutex_unlock(&engine->bp.mutex);
        frame = bt_buffer_pool_fetch(engine, 0);
    } else {
        atomic_fetch_add(&frame->pin_count, 1);
        pthread_mutex_unlock(&engine->bp.mutex);
        bt_latch_wlock(frame);
    }

    bt_page_header_t *hdr = &frame->page.header;
    size_t needed = sizeof(bt_slot_t) + key_len + val_len;
    size_t current_free = hdr->free_space_offset;

    if (current_free < sizeof(bt_page_header_t) + (hdr->num_slots + 1) * sizeof(bt_slot_t) + needed) {
        bt_buffer_pool_unpin(engine, frame, true);
        errno = ENOMEM;
        return -1;
    }

    uint16_t new_free_offset = current_free - val_len;
    memcpy(frame->page.data + (new_free_offset - sizeof(bt_page_header_t)), value, val_len);
    uint64_t val_file_offset = frame->page_offset + new_free_offset;

    new_free_offset -= key_len;
    memcpy(frame->page.data + (new_free_offset - sizeof(bt_page_header_t)), key, key_len);

    bt_slot_t *slot = (bt_slot_t *)(frame->page.data + hdr->num_slots * sizeof(bt_slot_t));
    slot->key_offset = new_free_offset;
    slot->key_length = (uint16_t)key_len;