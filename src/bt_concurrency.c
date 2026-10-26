#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <pthread.h>
#include <sched.h>
#include <errno.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <linux/futex.h>

#include "bt_concurrency.h"

/* 
 * Internal Futex Wrapper for Reader-Writer Latches
 * State representation:
 * Bits 0-30: Writer waiting / active count or reader count.
 * To keep it robust and simple for high-contention B-Tree nodes:
 * We use an atomic 32-bit integer for the latch state.
 * 0: Unlocked
 * -1 (0xFFFFFFFF): Write locked
 * >0: Number of active readers
 */

#define LATCH_UNLOCKED     0
#define LATCH_WRITE_LOCKED ((int32_t)0x80000000)
#define LATCH_READER_MASK  ((int32_t)0x7FFFFFFF)

static int futex_wake(void *addr, int val) {
    return (int)syscall(SYS_futex, addr, FUTEX_WAKE_PRIVATE, val, NULL, NULL, 0);
}

static int futex_wait(void *addr, int val, const struct timespec *timeout) {
    return (int)syscall(SYS_futex, addr, FUTEX_WAIT_PRIVATE, val, timeout, NULL, 0);
}

void bt_rwlatch_init(bt_rwlatch_t *latch) {
    atomic_init(&latch->state, LATCH_UNLOCKED);
    atomic_init(&latch->writer_waiting, 0);
}

void bt_rwlatch_destroy(bt_rwlatch_t *latch) {
    atomic_store(&latch->state, LATCH_UNLOCKED);
    atomic_store(&latch->writer_waiting, 0);
}

void bt_rwlatch_rdlock(bt_rwlatch_t *latch) {
    int32_t current;
    while (true) {
        current = atomic_load(&latch->state);
        // If write locked or writers are waiting (to avoid writer starvation), we back off and wait
        if ((current & LATCH_WRITE_LOCKED) || atomic_load(&latch->writer_waiting) > 0) {
            futex_wait((void *)&latch->state, current, NULL);
            continue;
        }
        
        // Try to increment reader count
        int32_t next = current + 1;
        if (atomic_compare_exchange_weak(&latch->state, &current, next)) {
            return;
        }
        // CAS failed, spin / retry via exponential backoff or immediate loop
        sched_yield();
    }
}

bool bt_rwlatch_try_rdlock(bt_rwlatch_t *latch) {
    int32_t current = atomic_load(&latch->state);
    if ((current & LATCH_WRITE_LOCKED) || atomic_load(&latch->writer_waiting) > 0) {
        return false;
    }
    int32_t next = current + 1;
    return atomic_compare_exchange_strong(&latch->state, &current, next);
}

void bt_rwlatch_rdunlock(bt_rwlatch_t *latch) {
    int32_t current, next;
    while (true) {
        current = atomic_load(&latch->state);
        if ((current & LATCH_READER_MASK) == 0) {
            // Unbalanced unlock panic safeguard
            return;
        }
        next = current - 1;
        if (atomic_compare_exchange_weak(&latch->state, &current, next)) {
            if (next == 0) {
                // Wake up waiting threads
                futex_wake((void *)&latch->state, INT32_MAX);
            }
            return;
        }
        sched_yield();
    }
}

void bt_rwlatch_wrlock(bt_rwlatch_t *latch) {
    atomic_fetch_add(&latch->writer_waiting, 1);
    
    int32_t current;
    while (true) {
        current = atomic_load(&latch->state);
        if (current == LATCH_UNLOCKED) {
            if (atomic_compare_exchange_weak(&latch->state, &current, LATCH_WRITE_LOCKED)) {
                atomic_fetch_sub(&latch->writer_waiting, 1);
                return;
            }
            continue;
        }
        futex_wait((void *)&latch->state, current, NULL);
    }
}

bool bt_rwlatch_try_wrlock(bt_rwlatch_t *latch) {
    int32_t current = LATCH_UNLOCKED;
    if (atomic_compare_exchange_strong(&latch->state, &current, LATCH_WRITE_LOCKED)) {
        return true;
    }
    return false;
}

void bt_rwlatch_wrunlock(bt_rwlatch_t *latch) {
    int32_t expected = LATCH_WRITE_LOCKED;
    if (atomic_compare_exchange_strong(&latch->state, &expected, LATCH_UNLOCKED)) {
        futex_wake((void *)&latch->state, INT32_MAX);
    }
}

/*
 * Hazard Pointer Registry Implementation
 * Thread-local hazard pointer slots to manage lock-free traversal of B-Tree nodes.
 */

static _Thread_local bt_hazard_slot_t *tls_hazard_slot = NULL;
static bt_hazard_registry_t g_hazard_registry = {
    .head = NULL,
    .count = 0
};
static pthread_mutex_t g_registry_mutex = PTHREAD_MUTEX_INITIALIZER;

void bt_hazard_init_registry(void) {
    pthread_mutex_lock(&g_registry_mutex);
    g_hazard_registry.head = NULL;
    g_hazard_registry.count = 0;
    pthread_mutex_unlock(&g_registry_mutex);
}

void bt_hazard_destroy_registry(void) {
    pthread_mutex_lock(&g_registry_mutex);
    bt_hazard_slot_t *curr = g_hazard_registry.head;
    while (curr != NULL) {
        bt_hazard_slot_t *next = curr->next;
        free(curr);
        curr = next;
    }
    g_hazard_registry.head = NULL;
    g_hazard_registry.count = 0;
    pthread_mutex_unlock(&g_registry_mutex);
}

bt_hazard_slot_t *bt_hazard_get_slot(void) {
    if (tls_hazard_slot != NULL) {
        return tls_hazard_slot;
    }

    pthread_mutex_lock(&g_registry_mutex);
    bt_hazard_slot_t *slot = (bt_hazard_slot_t *)malloc(sizeof(bt_hazard_slot_t));
    if (slot == NULL) {
        pthread_mutex_unlock(&g_registry_mutex);
        return NULL;
    }
    atomic_init(&slot->pointer, NULL);
    slot->active = true;
    slot->next = g_hazard_registry.head;
    g_hazard_registry.head = slot;
    g_hazard_registry.count++;
    pthread_mutex_unlock(&g_registry_mutex);

    tls_hazard_slot = slot;
    return slot;
}

void bt_hazard_release_slot(bt_hazard_slot_t *slot) {
    if (slot == NULL) return;
    atomic_store(&slot->pointer, NULL);
    slot->active = false;
    // We leave the node in the registry to avoid thread-safety overheads on destruction,
    // or re-use it via thread-local caching.
}

void bt_hazard_set(bt_hazard_slot_t *slot, void *node) {
    if (slot != NULL) {
        atomic_store(&slot->pointer, node);
    }
}

void bt_hazard_clear(bt_hazard_slot_t *slot) {
    if (slot != NULL) {
        atomic_store(&slot->pointer, NULL);
    }
}

bool bt_hazard_is_protected(void *node) {
    if (node == NULL) return false;

    pthread_mutex_lock(&g_registry_mutex);
    bt_hazard_slot_t *curr = g_hazard_registry.head;
    while (curr != NULL) {
        if (curr->active) {
            void *protected_ptr = atomic_load(&curr->pointer);
            if (protected_ptr == node) {
                pthread_mutex_unlock(&g_registry_mutex);
                return true;
            }
        }
        curr = curr->next;
    }
    pthread_mutex_unlock(&g_registry_mutex);
    return false;
}

/*
 * Epoch-Based / Deferred Reclamation Free List Node Wrapper
 */
typedef struct bt_reclaim_node {
    void *ptr;
    void (*free_func)(void *);
    struct bt_reclaim_node *next;
} bt_reclaim_node_t;

static bt_reclaim_node_t *g_reclaim_head = NULL;
static pthread_mutex_t g_reclaim_mutex = PTHREAD_MUTEX_INITIALIZER;

void bt_retire_node(void *node_ptr, void (*free_func)(void *)) {
    if (node_ptr == NULL) return;

    bt_reclaim_node_t *reclaim_node = (bt_reclaim_node_t *)malloc(sizeof(bt_reclaim_node_t));
    if (reclaim_node == NULL) {
        // Fallback: immediate free if memory is critically exhausted
        free_func(node_ptr);
        return;
    }
    reclaim_node->ptr = node_ptr;
    reclaim_node->free_func = free_func;
    reclaim_node->next = NULL;

    pthread_mutex_lock(&g_reclaim_mutex);
    reclaim_node->next = g_reclaim_head;
    g_reclaim_head = reclaim_node;
    pthread_mutex_unlock(&g_reclaim_mutex);
}

void bt_reclaim_scan(void) {
    pthread_mutex_lock(&g_reclaim_mutex);
    bt_reclaim_node_t *curr = g_reclaim_head;
    bt_reclaim_node_t *prev = NULL;

    while (curr != NULL) {
        if (!bt_hazard_is_protected(curr->ptr)) {
            // Safe to reclaim
            bt_reclaim_node_t *to_free = curr;
            if (prev == NULL) {
                g_reclaim_head = curr->next;
                curr = g_reclaim_head;
            } else {
                prev->next = curr->next;
                curr = prev->next;
            }
            to_free->free_func(to_free->ptr);
            free(to_free);
        } else {
            prev = curr;
            curr = curr->next;
        }
    }
    pthread_mutex_unlock(&g_reclaim_mutex);
}