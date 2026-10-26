#ifndef BT_WAL_H
#define BT_WAL_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <sys/types.h>

#define BT_WAL_MAGIC 0x57414C312D425453ULL /* "WAL1-BTS" */
#define BT_WAL_VERSION 1
#define BT_WAL_DEFAULT_SEGMENT_SIZE (64 * 1024 * 1024) /* 64 MB */

typedef uint64_t bt_lsn_t;
typedef uint64_t bt_txid_t;

typedef enum {
    BT_WAL_OP_INSERT = 1,
    BT_WAL_OP_UPDATE = 2,
    BT_WAL_OP_DELETE = 3,
    BT_WAL_OP_SPLIT  = 4,
    BT_WAL_OP_MERGE  = 5,
    BT_WAL_OP_TX_BEGIN = 6,
    BT_WAL_OP_TX_COMMIT = 7,
    BT_WAL_OP_TX_ABORT = 8,
    BT_WAL_OP_CHECKPOINT = 9
} bt_wal_op_type_t;

typedef struct __attribute__((__packed__)) {
    uint64_t magic;
    uint32_t version;
    uint32_t checksum;
    bt_lsn_t start_lsn;
    uint64_t segment_id;
    uint32_t flags;
    uint32_t reserved;
} bt_wal_segment_header_t;

typedef struct __attribute__((__packed__)) {
    uint32_t length;
    uint32_t checksum;
    bt_lsn_t lsn;
    bt_txid_t txid;
    uint8_t op_type;
    uint8_t flags;
    uint16_t reserved;
    uint64_t page_offset;
    uint32_t payload_len;
    uint32_t undo_len;
} bt_wal_record_header_t;

typedef struct bt_wal bt_wal_t;

struct bt_wal {
    char *log_dir;
    int current_fd;
    uint64_t current_segment_id;
    bt_lsn_t committed_lsn;
    bt_lsn_t flushed_lsn;
    size_t current_offset;
    size_t segment_size;
    bool sync_on_commit;
    int mutex_lock; /* Primitive spinlock or futex word */
};

int bt_wal_init(bt_wal_t **wal, const char *log_dir, size_t segment_size, bool sync_on_commit);
void bt_wal_destroy(bt_wal_t *wal);

int bt_wal_append(bt_wal_t *wal, bt_txid_t txid, bt_wal_op_type_t op_type,
                  uint64_t page_offset, const void *payload, uint32_t payload_len,
                  const void *undo, uint32_t undo_len, bt_lsn_t *out_lsn);

int bt_wal_flush(bt_wal_t *wal, bt_lsn_t lsn);

int bt_wal_recover(bt_wal_t *wal, 
                   int (*redo_callback)(bt_wal_op_type_t op_type, uint64_t page_offset, const void *payload, uint32_t payload_len, bt_lsn_t lsn, void *arg),
                   int (*undo_callback)(bt_txid_t txid, uint64_t page_offset, const void *undo, uint32_t undo_len, bt_lsn_t lsn, void *arg),
                   void *callback_arg);

#endif /* BT_WAL_H */

#ifndef BT_WAL_IMPLEMENTATION_ONCE
#define BT_WAL_IMPLEMENTATION_ONCE

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <dirent.h>
#include <time.h>

static uint32_t bt_wal_crc32c(uint32_t crc, const void *buf, size_t len) {
    const uint8_t *p = buf;
    crc = ~crc;
    for (size_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int j = 0; j < 8; j++) {
            crc = (crc >> 1) ^ (-(int)(crc & 1) & 0x82F63B78U);
        }
    }
    return ~crc;
}

static void bt_wal_spin_lock(int *lock) {
    while (__atomic_test_and_set(lock, __ATOMIC_ACQUIRE)) {
        while (__atomic_load_n(lock, __ATOMIC_RELAXED)) {
            __builtin_ia32_pause();
        }
    }
}

static void bt_wal_spin_unlock(int *lock) {
    __atomic_clear(lock, __ATOMIC_RELEASE);
}

static int bt_wal_open_segment(bt_wal_t *wal, uint64_t segment_id, bool create) {
    char path[1024];
    int flags = O_RDWR | O_CREAT;
    if (!create) {
        flags = O_RDWR;
    }
    
    int ret = snprintf(path, sizeof(path), "%s/wal_%016lx.log", wal->log_dir, segment_id);
    if (ret < 0 || (size_t)ret >= sizeof(path)) {
        return -ENAMETOOLONG;
    }

    int fd = open(path, flags, 0666);
    if (fd < 0) {
        return -errno;
    }

    if (create) {
        struct stat st;
        if (fstat(fd, &st) == 0 && st.st_size == 0) {
            bt_wal_segment_header_t header;
            memset(&header, 0, sizeof(header));
            header.magic = BT_WAL_MAGIC;
            header.version = BT_WAL_VERSION;
            header.start_lsn = wal->committed_lsn + 1;
            header.segment_id = segment_id;
            header.checksum = bt_wal_crc32c(0, &header, offsetof(bt_wal_segment_header_t, checksum));

            if (write(fd, &header, sizeof(header)) != sizeof(header)) {
                int err = -errno;
                close(fd);
                return err;
            }
            wal->current_offset = sizeof(header);
        } else {
            struct stat st_check;
            if (fstat(fd, &st_check) == 0) {
                wal->current_offset = st_check.st_size;
            }
        }
    } else {
        struct stat st_check;
        if (fstat(fd, &st_check) == 0) {
            wal->current_offset = st_check.st_size;
        }
    }

    return fd;
}

int bt_wal_init(bt_wal_t **wal_out, const char *log_dir, size_t segment_size, bool sync_on_commit) {
    if (!wal_out || !log_dir) {
        return -EINVAL;
    }

    bt_wal_t *wal = (bt_wal_t *)malloc(sizeof(bt_wal_t));
    if (!wal) {
        return -ENOMEM;
    }

    wal->log_dir = strdup(log_dir);
    if (!wal->log_dir) {
        free(wal);
        return -ENOMEM;
    }

    struct stat st;
    if (stat(log_dir, &st) < 0) {
        if (mkdir(log_dir, 0777) < 0) {
            free(wal->log_dir);
            free(wal);
            return -errno;
        }
    }

    wal->segment_size = segment_size > 0 ? segment_size : BT_WAL_DEFAULT_SEGMENT_SIZE;
    wal->sync_on_commit = sync_on_commit;
    wal->committed_lsn = 0;
    wal->flushed_lsn = 0;
    wal->current_segment_id = 1;
    wal->mutex_lock = 0;
    wal->current_offset = 0;

    DIR *dir = opendir(log_dir);
    if (dir) {
        struct dirent *entry;
        uint64_t max_seg = 0;
        while ((entry = readdir(dir)) != NULL) {
            uint64_t seg_id = 0;
            if (sscanf(entry->d_name, "wal_%016lx.log", &seg_id) == 1) {
                if (seg_id > max_seg) {
                    max_seg = seg_id;
                }
            }
        }
        closedir(dir);
        if (max_seg > 0) {
            wal->current_segment_id = max_seg;
        }
    }

    int fd = bt_wal_open_segment(wal, wal->current_segment_id, true);
    if (fd < 0) {
        free(wal->log_dir);
        free(wal);
        return fd;
    }
    wal->current_fd = fd;

    *wal_out = wal;
    return 0;
}

void bt_wal_destroy(bt_wal_t *wal) {
    if (!wal) {
        return;
    }
    bt_wal_spin_lock(&wal->mutex_lock);
    if (wal->current_fd >= 0) {
        fsync(wal->current_fd);
        close(wal->current_fd);
        wal->current_fd = -1;
    }
    bt_wal_spin_unlock(&wal->mutex_lock);

    free(wal->log_dir);
    free(wal);
}

int bt_wal_append(bt_wal_t *wal, bt_txid_t txid, bt_wal_op_type_t op_type,
                  uint64_t page_offset, const void *payload, uint32_t payload_len,
                  const void *undo, uint32_t undo_len, bt_lsn_t *out_lsn) {
    if (!wal) {
        return -EINVAL;
    }

    bt_wal_spin_lock(&wal->mutex_lock);

    wal->committed_lsn++;
    bt_lsn_t lsn = wal->committed_lsn;

    uint32_t total_len = sizeof(bt_wal_record_header_t) + payload_len + undo_len;
    
    if (wal->current_offset + total_len > wal->segment_size) {
        if (wal->current_fd >= 0) {
            fsync(wal->current_fd);
            close(wal->current_fd);
        }
        wal->current_segment_id++;
        int new_fd = bt_wal_open_segment(wal, wal->current_segment_id, true);
        if (new_fd < 0) {
            bt_wal_spin_unlock(&wal->mutex_lock);
            return new_fd;
        }
        wal->current_fd = new_fd;
    }

    bt_wal_record_header_t header;
    memset(&header, 0, sizeof(header));
    header.length = total_len;
    header.lsn = lsn;
    header.txid = txid;
    header.op_type = (uint8_t)op_type;
    header.flags = 0;
    header.page_offset = page_offset;
    header.payload_len = payload_len;
    header.undo_len = undo_len;

    uint32_t crc = 0;
    crc = bt_wal_crc32c(crc, &header.lsn, sizeof(header.lsn));
    crc = bt_wal_crc32c(crc, &header.txid, sizeof(header.txid));
    crc = bt_wal_crc32c(crc, &header.op_type, sizeof(header.op_type));
    crc = bt_wal_crc32c(crc, &header.page_offset, sizeof(header.page_offset));
    crc = bt_wal_crc32c(crc, &header.payload_len, sizeof(header.payload_len));
    crc = bt_wal_crc32c(crc, &header.undo_len, sizeof(header.undo_len));
    if (payload && payload_len > 0) {
        crc = bt_wal_crc32c(crc, payload, payload_len);
    }
    if (undo && undo_len > 0) {
        crc = bt_wal_crc32c(crc, undo, undo_len);
    }
    header.checksum = crc;

    struct iovec iov[3];
    int iovcnt = 1;
    iov[0].iov_base = &header;
    iov[0].iov_len = sizeof(header);

    if (payload && payload_len > 0) {
        iov[iovcnt].iov_base = (void *)payload;
        iov[iovcnt].iov_len = payload_len;
        iovcnt++;
    }

    if (undo && undo_len > 0) {
        iov[iovcnt].iov_base = (void *)undo;
        iov[iovcnt].iov_len = undo_len;
        iovcnt++;
    }

    ssize_t written = writev(wal->current_fd, iov, iovcnt);
    if (written != (ssize_t)total_len) {
        int err = -errno;
        bt_wal_spin_unlock(&wal->mutex_lock);
        return err;
    }

    wal->current_offset += total_len;

    if (wal->sync_on_commit) {
        if (fdatasync(wal->current_fd) < 0) {
            int err = -errno;
            bt_wal_spin_unlock(&wal->mutex_lock);
            return err;
        }
        wal->flushed_lsn = lsn;
    }

    if (out_lsn) {
        *out_lsn = lsn;
    }

    bt_wal_spin_unlock(&wal->mutex_lock);
    return 0;
}

int bt_wal_flush(bt_wal_t *wal, bt_lsn_t lsn) {
    if (!wal) {
        return -EINVAL;
    }

    bt_wal_spin_lock(&wal->mutex_lock);
    if (lsn <= wal->flushed_lsn) {
        bt_wal_spin_unlock(&wal->mutex_lock);
        return 0;
    }

    if (wal->current_fd >= 0) {
        if (fdatasync(wal->current_fd) < 0) {
            int err = -errno;
            bt_wal_spin_unlock(&wal->mutex_lock);
            return err;
        }
    }
    wal->flushed_lsn = wal->committed_lsn;
    bt_wal_spin_unlock(&wal->mutex_lock);
    return 0;
}

int bt_wal_recover(bt_wal_t *wal, 
                   int (*redo_callback)(bt_wal_op_type_t op_type, uint64_t page_offset, const void *payload, uint32_t payload_len, bt_lsn_t lsn, void *arg),
                   int (*undo_callback)(bt_txid_t txid, uint64_t page_offset, const void *undo, uint32_t undo_len, bt_lsn_t lsn, void *arg),
                   void *callback_arg) {