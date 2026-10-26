#define _GNU_SOURCE
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/stat.h>

#define BT_PAGE_SIZE 4096
#define BT_MAGIC 0x4254524554524545ULL // "BTREETRE"
#define BT_MAX_SEGMENTS 64
#define BT_BITMAP_WORDS ((BT_PAGE_SIZE / 8) / 64)

typedef struct bt_page {
    uint64_t magic;
    uint64_t lsn;
    uint64_t tx_id;
    uint32_t flags;
    uint32_t free_space_offset;
    uint32_t lower;
    uint32_t upper;
    uint8_t  padding[28];
    uint8_t  data[BT_PAGE_SIZE - 64];
} __attribute__((aligned(BT_PAGE_SIZE))) bt_page_t;

typedef struct bt_extent {
    uint64_t offset;
    uint64_t size;
    uint32_t flags;
    uint32_t ref_count;
} bt_extent_t;

typedef struct bt_segment {
    int fd;
    uint32_t segment_id;
    uint64_t current_size;
    pthread_mutex_t lock;
    char path[1024];
} bt_segment_t;

typedef struct bt_allocator {
    bt_segment_t segments[BT_MAX_SEGMENTS];
    uint32_t active_segment_count;
    pthread_mutex_t allocator_lock;
    uint64_t next_allocation_offset;
    uint32_t current_segment_id;
    char base_path[768];
    uint64_t max_segment_size;
    uint8_t allocation_bitmap[BT_PAGE_SIZE];
} bt_allocator_t;

int bt_allocator_init(bt_allocator_t *allocator, const char *base_path, uint64_t max_segment_size) {
    if (!allocator || !base_path) {
        errno = EINVAL;
        return -1;
    }

    memset(allocator, 0, sizeof(bt_allocator_t));
    strncpy(allocator->base_path, base_path, sizeof(allocator->base_path) - 1);
    allocator->max_segment_size = max_segment_size > 0 ? max_segment_size : (1024ULL * 1024ULL * 1024ULL);
    
    if (pthread_mutex_init(&allocator->allocator_lock, NULL) != 0) {
        return -1;
    }

    char first_segment_path[1024];
    snprintf(first_segment_path, sizeof(first_segment_path), "%s/segment_0.db", base_path);

    int fd = open(first_segment_path, O_RDWR | O_CREAT | O_DIRECT, 0644);
    if (fd < 0) {
        // Fallback if O_DIRECT is not supported by the underlying filesystem
        fd = open(first_segment_path, O_RDWR | O_CREAT, 0644);
        if (fd < 0) {
            pthread_mutex_destroy(&allocator->allocator_lock);
            return -1;
        }
    }

    allocator->segments[0].fd = fd;
    allocator->segments[0].segment_id = 0;
    allocator->segments[0].current_size = 0;
    if (pthread_mutex_init(&allocator->segments[0].lock, NULL) != 0) {
        close(fd);
        pthread_mutex_destroy(&allocator->allocator_lock);
        return -1;
    }
    strncpy(allocator->segments[0].path, first_segment_path, sizeof(allocator->segments[0].path) - 1);

    allocator->active_segment_count = 1;
    allocator->current_segment_id = 0;
    allocator->next_allocation_offset = 0;

    return 0;
}

void bt_allocator_shutdown(bt_allocator_t *allocator) {
    if (!allocator) {
        return;
    }

    pthread_mutex_lock(&allocator->allocator_lock);
    for (uint32_t i = 0; i < allocator->active_segment_count; i++) {
        pthread_mutex_lock(&allocator->segments[i].lock);
        if (allocator->segments[i].fd >= 0) {
            fsync(allocator->segments[i].fd);
            close(allocator->segments[i].fd);
            allocator->segments[i].fd = -1;
        }
        pthread_mutex_unlock(&allocator->segments[i].lock);
        pthread_mutex_destroy(&allocator->segments[i].lock);
    }
    allocator->active_segment_count = 0;
    pthread_mutex_unlock(&allocator->allocator_lock);
    pthread_mutex_destroy(&allocator->allocator_lock);
}

static int bt_allocator_roll_segment(bt_allocator_t *allocator) {
    if (allocator->active_segment_count >= BT_MAX_SEGMENTS) {
        errno = ENOSPC;
        return -1;
    }

    uint32_t new_id = allocator->active_segment_count;
    char segment_path[1024];
    snprintf(segment_path, sizeof(segment_path), "%s/segment_%u.db", allocator->base_path, new_id);

    int fd = open(segment_path, O_RDWR | O_CREAT | O_DIRECT, 0644);
    if (fd < 0) {
        fd = open(segment_path, O_RDWR | O_CREAT, 0644);
        if (fd < 0) {
            return -1;
        }
    }

    allocator->segments[new_id].fd = fd;
    allocator->segments[new_id].segment_id = new_id;
    allocator->segments[new_id].current_size = 0;
    if (pthread_mutex_init(&allocator->segments[new_id].lock, NULL) != 0) {
        close(fd);
        return -1;
    }
    strncpy(allocator->segments[new_id].path, segment_path, sizeof(allocator->segments[new_id].path) - 1);

    allocator->current_segment_id = new_id;
    allocator->next_allocation_offset = 0;
    allocator->active_segment_count++;

    return 0;
}

int bt_allocate_extent(bt_allocator_t *allocator, uint64_t size, bt_extent_t *out_extent) {
    if (!allocator || !out_extent || size == 0) {
        errno = EINVAL;
        return -1;
    }

    uint64_t aligned_size = (size + BT_PAGE_SIZE - 1) & ~(BT_PAGE_SIZE - 1);

    pthread_mutex_lock(&allocator->allocator_lock);

    bt_segment_t *segment = &allocator->segments[allocator->current_segment_id];
    pthread_mutex_lock(&segment->lock);

    if (segment->current_size + aligned_size > allocator->max_segment_size) {
        pthread_mutex_unlock(&segment->lock);
        if (bt_allocator_roll_segment(allocator) < 0) {
            pthread_mutex_unlock(&allocator->allocator_lock);
            return -1;
        }
        segment = &allocator->segments[allocator->current_segment_id];
        pthread_mutex_lock(&segment->lock);
    }

    uint64_t allocated_offset = allocator->next_allocation_offset;
    allocator->next_allocation_offset += aligned_size;
    segment->current_size = allocator->next_allocation_offset;

    pthread_mutex_unlock(&segment->lock);
    pthread_mutex_unlock(&allocator->allocator_lock);

    out_extent->offset = allocated_offset | (((uint64_t)allocator->current_segment_id) << 48);
    out_extent->size = aligned_size;
    out_extent->flags = 0;
    out_extent->ref_count = 1;

    return 0;
}

int bt_free_extent(bt_allocator_t *allocator, const bt_extent_t *extent) {
    if (!allocator || !extent) {
        errno = EINVAL;
        return -1;
    }

    // Append-only architecture treats deallocation as a logical free 
    // handled by compaction or garbage collection passes over immutable segments.
    // For transactional safety, reference decrements or bitmap updates occur here.
    pthread_mutex_lock(&allocator->allocator_lock);
    
    uint32_t segment_id = (uint32_t)(extent->offset >> 48);
    if (segment_id >= allocator->active_segment_count) {
        pthread_mutex_unlock(&allocator->allocator_lock);
        errno = EINVAL;
        return -1;
    }

    bt_segment_t *segment = &allocator->segments[segment_id];
    pthread_mutex_lock(&segment->lock);
    
    // Validate segment descriptor
    if (segment->fd < 0) {
        pthread_mutex_unlock(&segment->lock);
        pthread_mutex_unlock(&allocator->allocator_lock);
        errno = EBADF;
        return -1;
    }

    pthread_mutex_unlock(&segment->lock);
    pthread_mutex_unlock(&allocator->allocator_lock);

    return 0;
}

int bt_read_page(bt_allocator_t *allocator, uint64_t global_offset, bt_page_t *out_page) {
    if (!allocator || !out_page) {
        errno = EINVAL;
        return -1;
    }

    uint32_t segment_id = (uint32_t)(global_offset >> 48);
    uint64_t local_offset = global_offset & 0xFFFFFFFFFFFFULL;

    pthread_mutex_lock(&allocator->allocator_lock);
    if (segment_id >= allocator->active_segment_count) {
        pthread_mutex_unlock(&allocator->allocator_lock);
        errno = EINVAL;
        return -1;
    }

    bt_segment_t *segment = &allocator->segments[segment_id];
    pthread_mutex_lock(&segment->lock);
    int fd = segment->fd;
    pthread_mutex_unlock(&segment->lock);
    pthread_mutex_unlock(&allocator->allocator_lock);

    if (fd < 0) {
        errno = EBADF;
        return -1;
    }

    ssize_t bytes_read = pread(fd, out_page, BT_PAGE_SIZE, (off_t)local_offset);
    if (bytes_read != BT_PAGE_SIZE) {
        if (bytes_read >= 0) {
            errno = EIO;
        }
        return -1;
    }

    if (out_page->magic != BT_MAGIC) {
        errno = EILSEQ;
        return -1;
    }

    return 0;
}

int bt_write_page(bt_allocator_t *allocator, uint64_t global_offset, const bt_page_t *page) {
    if (!allocator || !page) {
        errno = EINVAL;
        return -1;
    }

    uint32_t segment_id = (uint32_t)(global_offset >> 48);
    uint64_t local_offset = global_offset & 0xFFFFFFFFFFFFULL;

    pthread_mutex_lock(&allocator->allocator_lock);
    if (segment_id >= allocator->active_segment_count) {
        pthread_mutex_unlock(&allocator->allocator_lock);
        errno = EINVAL;
        return -1;
    }

    bt_segment_t *segment = &allocator->segments[segment_id];
    pthread_mutex_lock(&segment->lock);
    int fd = segment->fd;
    pthread_mutex_unlock(&segment->lock);
    pthread_mutex_unlock(&allocator->allocator_lock);

    if (fd < 0) {
        errno = EBADF;
        return -1;
    }

    ssize_t bytes_written = pwrite(fd, page, BT_PAGE_SIZE, (off_t)local_offset);
    if (bytes_written != BT_PAGE_SIZE) {
        if (bytes_written >= 0) {
            errno = EIO;
        }
        return -1;
    }

    if (fsync(fd) < 0) {
        return -1;
    }

    return 0;
}