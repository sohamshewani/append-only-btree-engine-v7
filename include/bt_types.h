#ifndef BT_TYPES_H
#define BT_TYPES_H

#define _GNU_SOURCE

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <sys/types.h>

#define BT_PAGE_SIZE 4096
#define BT_ALIGNMENT 4096
#define BT_MAGIC 0x42545245455616C55ULL /* "BTREELU" */
#define BT_MAX_KEY_SIZE 512
#define BT_MAX_VALUE_SIZE 1024
#define BT_HEADER_SIZE 64

typedef uint64_t bt_lsn_t;
typedef uint64_t bt_tx_id_t;
typedef uint64_t bt_offset_t;

typedef enum {
    BT_PAGE_LEAF = 0x01,
    BT_PAGE_INTERNAL = 0x02,
    BT_PAGE_OVERFLOW = 0x04,
    BT_PAGE_DIRTY = 0x08,
    BT_PAGE_COMMITTED = 0x10
} bt_page_flags_t;

typedef struct __attribute__((__aligned__(BT_ALIGNMENT))) {
    uint64_t magic;
    bt_lsn_t lsn;
    bt_tx_id_t tx_id;
    bt_offset_t self_offset;
    bt_offset_t right_sibling_offset;
    uint16_t flags;
    uint16_t key_count;
    uint16_t free_space_offset;
    uint16_t checksum;
    uint8_t reserved[24];
    uint8_t data[BT_PAGE_SIZE - BT_HEADER_SIZE];
} bt_page_t;

typedef struct {
    uint16_t offset;
    uint16_t length;
    uint16_t key_length;
    uint8_t flags;
    uint8_t reserved;
} __attribute__((__packed__)) bt_slot_t;

typedef struct {
    uint32_t state;
    int32_t waiters;
} bt_rwlatch_t;

typedef struct {
    bt_offset_t page_offset;
    uint32_t frame_id;
    _Atomic int32_t pin_count;
    uint32_t flags;
    bt_lsn_t page_lsn;
    bt_rwlatch_t latch;
    uint32_t lru_next;
    uint32_t lru_prev;
    uint32_t hash_next;
} bt_frame_desc_t;

typedef struct {
    uint8_t data[BT_MAX_KEY_SIZE];
    uint16_t length;
} bt_key_t;

typedef struct {
    bt_offset_t offset;
    uint32_t length;
    uint32_t flags;
} bt_value_t;

typedef struct {
    bt_key_t key;
    bt_value_t value;
} bt_entry_t;

typedef struct {
    _Atomic(void*) hazard_pointers[64];
    _Atomic uint64_t epoch;
} bt_hazard_registry_t;

typedef struct {
    int fd;
    char path[1024];
    bt_offset_t current_size;
    _Atomic uint64_t write_offset;
    bt_rwlatch_t segment_latch;
} bt_segment_t;

typedef struct {
    bt_segment_t active_segment;
    bt_segment_t wal_segment;
    bt_frame_desc_t *frames;
    uint8_t *buffer_pool_memory;
    uint32_t pool_size;
    uint32_t lru_head;
    uint32_t lru_tail;
    bt_hazard_registry_t hazard_registry;
    _Atomic bt_offset_t atomic_root_offset;
    _Atomic bt_lsn_t committed_lsn;
    bool recovery_in_progress;
} bt_engine_t;

typedef enum {
    BT_SUCCESS = 0,
    BT_ERR_IO = -1,
    BT_ERR_CORRUPT = -2,
    BT_ERR_NOMEM = -3,
    BT_ERR_FULL = -4,
    BT_ERR_NOTFOUND = -5,
    BT_ERR_LOCKED = -6,
    BT_ERR_INVAL = -7
} bt_status_t;

static inline void bt_rwlatch_init(bt_rwlatch_t *latch) {
    latch->state = 0;
    latch->waiters = 0;
}

static inline void bt_page_init(bt_page_t *page, uint16_t flags, bt_offset_t self_offset, bt_lsn_t lsn, bt_tx_id_t tx_id) {
    page->magic = BT_MAGIC;
    page->lsn = lsn;
    page->tx_id = tx_id;
    page->self_offset = self_offset;
    page->right_sibling_offset = 0;
    page->flags = flags;
    page->key_count = 0;
    page->free_space_offset = BT_PAGE_SIZE - BT_HEADER_SIZE;
    page->checksum = 0;
    __builtin_memset(page->reserved, 0, sizeof(page->reserved));
}

#endif /* BT_TYPES_H */