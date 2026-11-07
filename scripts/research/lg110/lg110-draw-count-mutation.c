#define _GNU_SOURCE

/*
 * Bounded LG110 tracer with explicitly opt-in draw-field experiments.
 * Passive by default. =count changes only word270 from 3 to 2; =topology
 * changes only word268 from 0x40000400 to 0x40000000; =word276 changes only
 * word276 from 2 to 1; =word274/=word275 each change only that word from 0 to 1.
 * =index32-to16 changes only word268 from 0x40000404 to 0x40000402 after
 * requiring an indexed UINT32 signature and nonzero index VA.
 * =first-index changes word269 0→3; =base-vertex changes word274 0→1;
 * =base-neg-to-zero changes word274 -1→0 with firstIndex=3. All three require
 * the indexed UINT16 signature and nonzero index VA. Exact stream
 * signatures,
 * old values, writable mappings, and post-store readback are mandatory.
 */

#include <dlfcn.h>
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>

#include "loonggpu_drm.h"

/* Installed vendor userspace uses IOWR for GEM_VA although the 1.0.2
 * public header says IOW.  The payload and command number are identical. */
#define LG110_IOCTL_GEM_VA_IOWR \
    DRM_IOWR(DRM_COMMAND_BASE + DRM_LOONGGPU_GEM_VA, struct drm_loonggpu_gem_va)

/* GSPKT layout/opcodes mirrored from the kernel-only loonggpu.h. */
#define GSPKT(op, n) (((op) & 0xFF) | ((n) & 0xFFFF) << 16)
#define GSPKT_NOP       0x80
#define GSPKT_WRITE     0x81
#define GSPKT_INDIRECT  0x82
#define GSPKT_FENCE     0x83
#define GSPKT_TRAP      0x84
#define GSPKT_POLL      0x85
#define GSPKT_WPOLL     0x86
#define GSPKT_READ      0x87
#define GSPKT_VM_BIND   0x8A
#define GSPKT_XDMA_COPY 0xC0

#define LG110_MAX_GEM           256
#define LG110_MAX_VA            512
#define LG110_MAX_USER_MAP      256
#define LG110_MAX_CHUNKS        32
#define LG110_DEFAULT_IB_DWORDS 32
#define LG110_HARD_MAX_IB_DWORDS 2048
#define LG110_HARD_MAX_SUBS     1000000U

struct gem_record {
    int used;
    int fd;
    uint32_t handle;
    uint64_t bo_size;
    uint64_t mmap_offset;
    int have_mmap_offset;
};

struct va_record {
    int used;
    int fd;
    uint32_t handle;
    uint64_t va;
    uint64_t size;
    uint64_t bo_offset;
};

struct user_map_record {
    int used;
    int fd;
    uint32_t handle;
    uintptr_t address;
    size_t length;
    int protection;
    uint64_t mmap_offset;
};

static struct gem_record gems[LG110_MAX_GEM];
static struct va_record vas[LG110_MAX_VA];
static struct user_map_record user_maps[LG110_MAX_USER_MAP];
static pthread_mutex_t state_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t config_once = PTHREAD_ONCE_INIT;
static unsigned int max_submissions = 64;
static unsigned int submission_count;
static unsigned int max_aux_records;
static unsigned int aux_record_count;
static unsigned int max_ib_dwords = LG110_DEFAULT_IB_DWORDS;
static uint64_t trace_va_address;
static unsigned int trace_va_dwords = 64;
static int mutate_draw_mode;
static int mutation_done;

static int (*real_ioctl)(int, unsigned long, ...);
static void *(*real_mmap)(void *, size_t, int, int, int, off_t);
static void *(*real_mmap64)(void *, size_t, int, int, int, off64_t);
static int (*real_munmap)(void *, size_t);
static __thread int in_hook;

/* POSIX specifies dlsym for function lookup; memcpy avoids a C cast warning. */
#define RESOLVE_REAL(function, symbol_name) do { \
    void *symbol_ = dlsym(RTLD_NEXT, symbol_name); \
    memcpy(&function, &symbol_, sizeof(function)); \
} while (0)

static void configure(void)
{
    const char *value = getenv("LG110_TRACE_MAX");
    const char *draw_value = getenv("LG110_MUTATE_DRAW_FIELD");
    char *end = NULL;
    unsigned long parsed;

    if (draw_value) {
        if (strcmp(draw_value, "count") == 0)
            mutate_draw_mode = 1;
        else if (strcmp(draw_value, "topology") == 0)
            mutate_draw_mode = 2;
        else if (strcmp(draw_value, "word276") == 0)
            mutate_draw_mode = 3;
        else if (strcmp(draw_value, "word274") == 0)
            mutate_draw_mode = 4;
        else if (strcmp(draw_value, "word275") == 0)
            mutate_draw_mode = 5;
        else if (strcmp(draw_value, "index32-to16") == 0)
            mutate_draw_mode = 6;
        else if (strcmp(draw_value, "first-index") == 0)
            mutate_draw_mode = 7;
        else if (strcmp(draw_value, "base-vertex") == 0)
            mutate_draw_mode = 8;
        else if (strcmp(draw_value, "base-neg-to-zero") == 0)
            mutate_draw_mode = 9;
        else {
            fprintf(stderr, "LG110DRAWMUTATE invalid field; passive only\n");
        }
    }

    if (value && *value) {
        parsed = strtoul(value, &end, 10);
        if (end && end != value && *end == '\0' && parsed <= LG110_HARD_MAX_SUBS)
            max_submissions = (unsigned int)parsed;
    }

    value = getenv("LG110_TRACE_AUX_MAX");
    end = NULL;
    if (value && *value) {
        parsed = strtoul(value, &end, 10);
        if (end && end != value && *end == '\0' && parsed <= LG110_HARD_MAX_SUBS)
            max_aux_records = (unsigned int)parsed;
    }

    value = getenv("LG110_TRACE_VA");
    end = NULL;
    if (value && *value) {
        unsigned long long address = strtoull(value, &end, 0);
        if (end && end != value && *end == '\0' && address)
            trace_va_address = (uint64_t)address;
    }

    value = getenv("LG110_TRACE_IB_DWORDS");
    end = NULL;
    if (value && *value) {
        parsed = strtoul(value, &end, 10);
        if (end && end != value && *end == '\0' && parsed > 0 &&
            parsed <= LG110_HARD_MAX_IB_DWORDS)
            max_ib_dwords = (unsigned int)parsed;
    }
}

static void log_line(const char *format, ...)
{
    /* Include IB metadata and newline in addition to all hexadecimal words. */
    char buffer[LG110_HARD_MAX_IB_DWORDS * 11 + 1024];
    va_list ap;
    int length;

    va_start(ap, format);
    length = vsnprintf(buffer, sizeof(buffer), format, ap);
    va_end(ap);
    if (length < 0)
        return;
    if ((size_t)length >= sizeof(buffer))
        length = (int)sizeof(buffer) - 1;
    buffer[length++] = '\n';
    (void)write(STDERR_FILENO, buffer, (size_t)length);
}

static void aux_log(const char *format, ...)
{
    char buffer[1024];
    va_list ap;
    int length;

    pthread_mutex_lock(&state_lock);
    if (aux_record_count >= max_aux_records)
        goto out;
    ++aux_record_count;
    va_start(ap, format);
    length = vsnprintf(buffer, sizeof(buffer), format, ap);
    va_end(ap);
    if (length < 0)
        goto out;
    if ((size_t)length >= sizeof(buffer))
        length = (int)sizeof(buffer) - 1;
    buffer[length++] = '\n';
    (void)write(STDERR_FILENO, buffer, (size_t)length);
out:
    pthread_mutex_unlock(&state_lock);
}

/* Read without dereferencing an untrusted userspace pointer in this process. */
static int read_user(uint64_t source, void *destination, size_t length)
{
    struct iovec local;
    struct iovec remote;
    ssize_t result;

    if (!length)
        return 0;
    if (!source || !destination)
        return -1;

    local.iov_base = destination;
    local.iov_len = length;
    remote.iov_base = (void *)(uintptr_t)source;
    remote.iov_len = length;
    result = syscall(SYS_process_vm_readv, getpid(), &local, 1, &remote, 1, 0);
    return result == (ssize_t)length ? 0 : -1;
}

static void invalidate_handle_locked(int fd, uint32_t handle)
{
    unsigned int i;

    for (i = 0; i < LG110_MAX_VA; ++i)
        if (vas[i].used && vas[i].fd == fd && vas[i].handle == handle)
            vas[i].used = 0;
    for (i = 0; i < LG110_MAX_USER_MAP; ++i)
        if (user_maps[i].used && user_maps[i].fd == fd &&
            user_maps[i].handle == handle)
            user_maps[i].used = 0;
}

static struct gem_record *find_gem_locked(int fd, uint32_t handle)
{
    unsigned int i;

    for (i = 0; i < LG110_MAX_GEM; ++i)
        if (gems[i].used && gems[i].fd == fd && gems[i].handle == handle)
            return &gems[i];
    return NULL;
}

static struct gem_record *alloc_gem_locked(int fd, uint32_t handle)
{
    struct gem_record *record = find_gem_locked(fd, handle);
    unsigned int i;

    if (record) {
        invalidate_handle_locked(fd, handle);
        memset(record, 0, sizeof(*record));
    } else {
        for (i = 0; i < LG110_MAX_GEM; ++i) {
            if (!gems[i].used) {
                record = &gems[i];
                break;
            }
        }
    }
    if (record) {
        memset(record, 0, sizeof(*record));
        record->used = 1;
        record->fd = fd;
        record->handle = handle;
    }
    return record;
}

static void track_gem_create(int fd, uint64_t bo_size, uint32_t handle)
{
    struct gem_record *record;

    pthread_mutex_lock(&state_lock);
    record = alloc_gem_locked(fd, handle);
    if (record)
        record->bo_size = bo_size;
    pthread_mutex_unlock(&state_lock);
}

static void track_gem_mmap(int fd, uint32_t handle, uint64_t mmap_offset)
{
    struct gem_record *record;

    pthread_mutex_lock(&state_lock);
    record = find_gem_locked(fd, handle);
    if (!record)
        record = alloc_gem_locked(fd, handle);
    if (record) {
        record->mmap_offset = mmap_offset;
        record->have_mmap_offset = 1;
    }
    pthread_mutex_unlock(&state_lock);
}

static struct va_record *alloc_va_locked(void)
{
    unsigned int i;

    for (i = 0; i < LG110_MAX_VA; ++i)
        if (!vas[i].used)
            return &vas[i];
    return NULL;
}

static int ranges_overlap_u64(uint64_t a, uint64_t a_size,
                              uint64_t b, uint64_t b_size)
{
    uint64_t a_end, b_end;

    if (!a_size || !b_size || a > UINT64_MAX - a_size ||
        b > UINT64_MAX - b_size)
        return 0;
    a_end = a + a_size;
    b_end = b + b_size;
    return a < b_end && b < a_end;
}

static void track_gem_va(int fd, const struct drm_loonggpu_gem_va *input)
{
    struct va_record *record = NULL;
    unsigned int i;

    pthread_mutex_lock(&state_lock);
    if (input->operation == LOONGGPU_VA_OP_MAP ||
        input->operation == LOONGGPU_VA_OP_REPLACE) {
        for (i = 0; i < LG110_MAX_VA; ++i) {
            if (vas[i].used && vas[i].fd == fd &&
                vas[i].handle == input->handle &&
                vas[i].va == input->va_address) {
                record = &vas[i];
                break;
            }
        }
        if (!record)
            record = alloc_va_locked();
        if (record) {
            memset(record, 0, sizeof(*record));
            record->used = 1;
            record->fd = fd;
            record->handle = input->handle;
            record->va = input->va_address;
            record->size = input->map_size;
            record->bo_offset = input->offset_in_bo;
        }
    } else if (input->operation == LOONGGPU_VA_OP_UNMAP ||
               input->operation == LOONGGPU_VA_OP_CLEAR) {
        for (i = 0; i < LG110_MAX_VA; ++i) {
            if (vas[i].used && vas[i].fd == fd &&
                vas[i].handle == input->handle &&
                ranges_overlap_u64(vas[i].va, vas[i].size,
                                   input->va_address, input->map_size))
                vas[i].used = 0;
        }
    }
    pthread_mutex_unlock(&state_lock);
}

static int ranges_overlap_user(uintptr_t a, size_t a_size,
                               uintptr_t b, size_t b_size)
{
    uintptr_t a_end, b_end;

    if (!a_size || !b_size || a > UINTPTR_MAX - a_size ||
        b > UINTPTR_MAX - b_size)
        return 0;
    a_end = a + a_size;
    b_end = b + b_size;
    return a < b_end && b < a_end;
}

static void invalidate_user_overlap_locked(uintptr_t address, size_t length)
{
    unsigned int i;

    for (i = 0; i < LG110_MAX_USER_MAP; ++i)
        if (user_maps[i].used &&
            ranges_overlap_user(user_maps[i].address, user_maps[i].length,
                                address, length))
            user_maps[i].used = 0;
}

static void track_user_map(int fd, void *address, size_t length, int protection, off64_t offset)
{
    struct gem_record *gem = NULL;
    struct user_map_record *map = NULL;
    uintptr_t start = (uintptr_t)address;
    unsigned int i;

    if (offset < 0)
        return;

    pthread_mutex_lock(&state_lock);
    for (i = 0; i < LG110_MAX_GEM; ++i) {
        if (gems[i].used && gems[i].fd == fd && gems[i].have_mmap_offset &&
            gems[i].mmap_offset == (uint64_t)offset) {
            gem = &gems[i];
            break;
        }
    }
    if (!gem)
        goto out;

    invalidate_user_overlap_locked(start, length);
    for (i = 0; i < LG110_MAX_USER_MAP; ++i)
        if (!user_maps[i].used) {
            map = &user_maps[i];
            break;
        }
    if (map) {
        memset(map, 0, sizeof(*map));
        map->used = 1;
        map->fd = fd;
        map->handle = gem->handle;
        map->address = start;
        map->length = length;
        map->protection = protection;
        map->mmap_offset = (uint64_t)offset;
    }
out:
    pthread_mutex_unlock(&state_lock);
}

static void track_user_unmap(void *address, size_t length)
{
    pthread_mutex_lock(&state_lock);
    invalidate_user_overlap_locked((uintptr_t)address, length);
    pthread_mutex_unlock(&state_lock);
}

static const char *chunk_name(uint32_t id)
{
    switch (id) {
    case LOONGGPU_CHUNK_ID_IB: return "IB";
    case LOONGGPU_CHUNK_ID_FENCE: return "FENCE";
    case LOONGGPU_CHUNK_ID_DEPENDENCIES: return "DEPENDENCIES";
    case LOONGGPU_CHUNK_ID_SYNCOBJ_IN: return "SYNCOBJ_IN";
    case LOONGGPU_CHUNK_ID_SYNCOBJ_OUT: return "SYNCOBJ_OUT";
    case LOONGGPU_CHUNK_ID_BO_HANDLES: return "BO_HANDLES";
    case LOONGGPU_CHUNK_ID_SYNCOBJ_TIMELINE_WAIT: return "SYNCOBJ_TIMELINE_WAIT";
    case LOONGGPU_CHUNK_ID_SYNCOBJ_TIMELINE_SIGNAL: return "SYNCOBJ_TIMELINE_SIGNAL";
    default: return "UNKNOWN";
    }
}

static const char *gspkt_name(uint32_t op)
{
    switch (op) {
    case GSPKT_NOP: return "NOP";
    case GSPKT_WRITE: return "WRITE";
    case GSPKT_INDIRECT: return "INDIRECT";
    case GSPKT_FENCE: return "FENCE";
    case GSPKT_TRAP: return "TRAP";
    case GSPKT_POLL: return "POLL";
    case GSPKT_WPOLL: return "WPOLL";
    case GSPKT_READ: return "READ";
    case GSPKT_VM_BIND: return "VM_BIND";
    case GSPKT_XDMA_COPY: return "XDMA_COPY";
    default: return "UNKNOWN";
    }
}

struct ib_source {
    int valid;
    const char *reason;
    uint32_t handle;
    uint64_t bo_offset;
    uintptr_t user_address;
    size_t available;
    int writable;
};

/* state_lock must be held. */
static struct ib_source resolve_ib_locked(int fd, uint64_t va_start)
{
    struct ib_source source;
    struct va_record *va = NULL;
    uint64_t delta;
    unsigned int i;

    memset(&source, 0, sizeof(source));
    for (i = 0; i < LG110_MAX_VA; ++i) {
        if (vas[i].used && vas[i].fd == fd && vas[i].size &&
            va_start >= vas[i].va && va_start - vas[i].va < vas[i].size) {
            va = &vas[i];
            break;
        }
    }
    if (!va) {
        source.reason = "no_va_mapping";
        return source;
    }

    delta = va_start - va->va;
    if (delta > UINT64_MAX - va->bo_offset) {
        source.reason = "bo_offset_overflow";
        return source;
    }
    source.handle = va->handle;
    source.bo_offset = va->bo_offset + delta;

    for (i = 0; i < LG110_MAX_USER_MAP; ++i) {
        struct user_map_record *map = &user_maps[i];

        if (!map->used || map->fd != fd || map->handle != source.handle ||
            source.bo_offset >= map->length)
            continue;
        if (source.bo_offset > UINTPTR_MAX - map->address) {
            source.reason = "user_address_overflow";
            return source;
        }
        source.user_address = map->address + (uintptr_t)source.bo_offset;
        source.available = map->length - (size_t)source.bo_offset;
        source.writable = (map->protection & PROT_WRITE) != 0;
        source.valid = 1;
        source.reason = "mapped";
        return source;
    }
    source.reason = "no_user_mapping";
    return source;
}

/* state_lock must be held; keeping it held prevents interleaved CS records. */
static void trace_ib_locked(unsigned int sequence, unsigned int index, int fd,
                            const struct drm_loonggpu_cs_chunk_ib *ib)
{
    struct ib_source source;
    uint32_t dwords[LG110_HARD_MAX_IB_DWORDS];
    size_t count;
    unsigned int i;
    char values[LG110_HARD_MAX_IB_DWORDS * 11 + 1];
    size_t used = 0;
    uint32_t op;

    log_line("LG110IB seq=%u chunk=%u va_start=0x%016" PRIx64
             " ib_bytes=%u ip_type=%u ip_instance=%u ring=%u flags=0x%08x",
             sequence, index, (uint64_t)ib->va_start, ib->ib_bytes,
             ib->ip_type, ib->ip_instance, ib->ring, ib->flags);

    source = resolve_ib_locked(fd, ib->va_start);
    if (!source.valid) {
        log_line("LG110IBDUMP seq=%u chunk=%u status=not_dumped reason=%s",
                 sequence, index, source.reason);
        return;
    }

    count = ib->ib_bytes / 4;
    if (count > max_ib_dwords)
        count = max_ib_dwords;
    if (count > source.available / 4)
        count = source.available / 4;
    if (!count) {
        log_line("LG110IBDUMP seq=%u chunk=%u status=not_dumped "
                 "reason=no_complete_dword handle=%u bo_offset=0x%" PRIx64,
                 sequence, index, source.handle, source.bo_offset);
        return;
    }
    if (read_user(source.user_address, dwords, count * sizeof(dwords[0]))) {
        /* process_vm_readv may reject DRM/PFNMAP VMAs.  A direct read is
         * permitted here only because a successful tracked mmap covers the
         * complete range; unresolved or unknown addresses never reach this. */
        memcpy(dwords, (const void *)source.user_address,
               count * sizeof(dwords[0]));
    }

    values[0] = '\0';
    for (i = 0; i < count; ++i) {
        int written = snprintf(values + used, sizeof(values) - used,
                               "%s0x%08x", i ? " " : "", dwords[i]);
        if (written < 0 || (size_t)written >= sizeof(values) - used)
            break;
        used += (size_t)written;
    }
    op = dwords[0] & 0xffU;
    log_line("LG110IBDUMP seq=%u chunk=%u status=ok handle=%u "
             "bo_offset=0x%" PRIx64 " user=0x%016" PRIxPTR
             " dwords=%u word0_op=%s(0x%02x) word0_n=%u data=%s",
             sequence, index, source.handle, source.bo_offset,
             source.user_address, (unsigned int)i, gspkt_name(op), op,
             (dwords[0] >> 16) & 0xffffU, values);
}

/* state_lock must be held. Dump only an explicitly selected, fully tracked VA. */
static void trace_extra_va_locked(unsigned int sequence, int fd)
{
    struct ib_source source;
    uint32_t dwords[256];
    char values[256 * 11 + 1];
    size_t count, used = 0;
    unsigned int i;
    void *temporary_map = NULL;
    size_t temporary_length = 0;

    if (!trace_va_address)
        return;
    source = resolve_ib_locked(fd, trace_va_address);
    if (!source.valid && source.handle) {
        union drm_loonggpu_gem_mmap mmap_args;
        long page_size = sysconf(_SC_PAGESIZE);
        size_t page_offset, delta, map_length;

        memset(&mmap_args, 0, sizeof(mmap_args));
        mmap_args.in.handle = source.handle;
        if (page_size > 0 && source.bo_offset <= SIZE_MAX - 1024 &&
            real_ioctl(fd, DRM_IOCTL_LOONGGPU_GEM_MMAP, &mmap_args) == 0) {
            page_offset = (size_t)source.bo_offset & ~((size_t)page_size - 1);
            delta = (size_t)source.bo_offset - page_offset;
            map_length = (delta + 1024 + (size_t)page_size - 1) &
                         ~((size_t)page_size - 1);
            if (!real_mmap)
                RESOLVE_REAL(real_mmap, "mmap");
            if (real_mmap && mmap_args.out.addr_ptr <= (uint64_t)INT64_MAX - page_offset) {
                temporary_map = real_mmap(NULL, map_length, PROT_READ, MAP_SHARED,
                    fd, (off_t)(mmap_args.out.addr_ptr + page_offset));
                if (temporary_map != MAP_FAILED) {
                    temporary_length = map_length;
                    source.user_address = (uintptr_t)temporary_map + delta;
                    source.available = map_length - delta;
                    source.valid = 1;
                    source.reason = "temporary_read_only_mapping";
                } else {
                    temporary_map = NULL;
                }
            }
        }
    }
    if (!source.valid) {
        log_line("LG110VADUMP seq=%u va=0x%016" PRIx64
                 " status=not_dumped reason=%s handle=%u bo_offset=0x%" PRIx64,
                 sequence, trace_va_address, source.reason, source.handle,
                 source.bo_offset);
        return;
    }
    count = trace_va_dwords;
    if (count > 256) count = 256;
    if (count > source.available / 4) count = source.available / 4;
    if (!count || (!temporary_map &&
                   read_user(source.user_address, dwords, count * sizeof(dwords[0])))) {
        log_line("LG110VADUMP seq=%u va=0x%016" PRIx64
                 " status=read_failed handle=%u bo_offset=0x%" PRIx64,
                 sequence, trace_va_address, source.handle, source.bo_offset);
        goto out;
    }
    if (temporary_map)
        memcpy(dwords, (const void *)source.user_address, count * sizeof(dwords[0]));
    values[0] = '\0';
    for (i = 0; i < count; ++i) {
        int written = snprintf(values + used, sizeof(values) - used,
                               "%s0x%08x", i ? " " : "", dwords[i]);
        if (written < 0 || (size_t)written >= sizeof(values) - used)
            break;
        used += (size_t)written;
    }
    log_line("LG110VADUMP seq=%u va=0x%016" PRIx64
             " status=ok handle=%u bo_offset=0x%" PRIx64 " dwords=%u data=%s",
             sequence, trace_va_address, source.handle, source.bo_offset, i, values);
out:
    if (temporary_map) {
        if (!real_munmap)
            RESOLVE_REAL(real_munmap, "munmap");
        if (real_munmap)
            real_munmap(temporary_map, temporary_length);
        else
            syscall(SYS_munmap, temporary_map, temporary_length);
    }
}

/* state_lock must be held. */
static void trace_cs_locked(unsigned int sequence, int fd,
                            const struct drm_loonggpu_cs_in *input,
                            int snapshot_valid, int result, int error_number,
                            int output_valid, uint64_t output_handle)
{
    uint64_t chunk_pointers[LG110_MAX_CHUNKS];
    unsigned int count;
    unsigned int i;

    if (!snapshot_valid) {
        log_line("LG110CS seq=%u fd=%d snapshot=read_failed ret=%d errno=%d",
                 sequence, fd, result, result == 0 ? 0 : error_number);
        return;
    }

    if (output_valid)
        log_line("LG110CS seq=%u fd=%d ctx=%u bo_list=%u num_chunks=%u "
                 "ret=%d errno=0 cs_handle=0x%016" PRIx64,
                 sequence, fd, input->ctx_id, input->bo_list_handle,
                 input->num_chunks, result, output_handle);
    else
        log_line("LG110CS seq=%u fd=%d ctx=%u bo_list=%u num_chunks=%u "
                 "ret=%d errno=%d cs_handle=unavailable",
                 sequence, fd, input->ctx_id, input->bo_list_handle,
                 input->num_chunks, result, result == 0 ? 0 : error_number);

    count = input->num_chunks;
    if (count > LG110_MAX_CHUNKS) {
        log_line("LG110CS seq=%u chunks_truncated=%u logged_chunks=%u",
                 sequence, count - LG110_MAX_CHUNKS, LG110_MAX_CHUNKS);
        count = LG110_MAX_CHUNKS;
    }
    if (!count)
        return;
    if (read_user(input->chunks, chunk_pointers,
                  count * sizeof(chunk_pointers[0]))) {
        log_line("LG110CS seq=%u status=chunk_pointer_read_failed", sequence);
        return;
    }

    for (i = 0; i < count; ++i) {
        struct drm_loonggpu_cs_chunk chunk;

        if (read_user(chunk_pointers[i], &chunk, sizeof(chunk))) {
            log_line("LG110CHUNK seq=%u chunk=%u status=descriptor_read_failed",
                     sequence, i);
            continue;
        }
        log_line("LG110CHUNK seq=%u chunk=%u id=0x%02x type=%s length_dw=%u",
                 sequence, i, chunk.chunk_id, chunk_name(chunk.chunk_id),
                 chunk.length_dw);

        if (chunk.chunk_id == LOONGGPU_CHUNK_ID_IB) {
            struct drm_loonggpu_cs_chunk_ib ib;

            if (chunk.length_dw < (sizeof(ib) + 3) / 4) {
                log_line("LG110IB seq=%u chunk=%u status=short_chunk",
                         sequence, i);
            } else if (read_user(chunk.chunk_data, &ib, sizeof(ib))) {
                log_line("LG110IB seq=%u chunk=%u status=data_read_failed",
                         sequence, i);
            } else {
                trace_ib_locked(sequence, i, fd, &ib);
            }
        } else if (chunk.chunk_id == LOONGGPU_CHUNK_ID_FENCE) {
            struct drm_loonggpu_cs_chunk_fence fence;

            if (chunk.length_dw < (sizeof(fence) + 3) / 4) {
                log_line("LG110FENCE seq=%u chunk=%u status=short_chunk",
                         sequence, i);
            } else if (read_user(chunk.chunk_data, &fence, sizeof(fence))) {
                log_line("LG110FENCE seq=%u chunk=%u status=data_read_failed",
                         sequence, i);
            } else {
                log_line("LG110FENCE seq=%u chunk=%u handle=%u offset=%u",
                         sequence, i, fence.handle, fence.offset);
            }
        } else if (chunk.chunk_id == LOONGGPU_CHUNK_ID_SYNCOBJ_IN ||
                   chunk.chunk_id == LOONGGPU_CHUNK_ID_SYNCOBJ_OUT ||
                   chunk.chunk_id == LOONGGPU_CHUNK_ID_SYNCOBJ_TIMELINE_WAIT ||
                   chunk.chunk_id == LOONGGPU_CHUNK_ID_SYNCOBJ_TIMELINE_SIGNAL) {
            log_line("LG110SYNCOBJ seq=%u chunk=%u type=%s entries=%u",
                     sequence, i, chunk_name(chunk.chunk_id),
                     chunk.length_dw /
                     (unsigned int)(sizeof(struct drm_loonggpu_cs_chunk_syncobj) / 4));
        }
    }
    trace_extra_va_locked(sequence, fd);
}

#define LG110_DRAW_DWORDS 672U
#define LG110_DRAW_SIGNATURE_DWORDS 281U

static int draw_word_selected(unsigned int index)
{
    return (index == 270U && mutate_draw_mode == 1) ||
           (index == 268U && (mutate_draw_mode == 2 || mutate_draw_mode == 6)) ||
           (index == 276U && mutate_draw_mode == 3) ||
           (index == 274U && (mutate_draw_mode == 4 || mutate_draw_mode == 8 ||
                              mutate_draw_mode == 9)) ||
           (index == 275U && mutate_draw_mode == 5) ||
           (index == 269U && mutate_draw_mode == 7);
}

static int draw_signature_matches(const uint32_t *words)
{
    static const struct signature_word {
        unsigned int index;
        uint32_t value;
    } signature[] = {
        {0, 0x00010088U}, {1, 0x00000003U}, {2, 0x00810089U},
        {3, 0x0000000fU}, {4, 0x41010f00U}, {6, 0x00000003U},
        {8, 0x00000001U}, {72, 0x00000810U}, {73, 0x008c0401U},
        {85, 0x01e00280U}, {86, 0x3f800000U}, {87, 0x00000000U},
        {88, 0x3e4ccccdU}, {89, 0x3ecccccdU}, {90, 0x3f800000U},
        {91, 0x7f800000U}, {264, 0x00010088U}, {265, 0x00000003U},
        {266, 0x00810089U}, {267, 0x0000000fU}, {268, 0x40000400U},
        {269, 0x00000000U}, {270, 0x00000003U}, {271, 0x00000000U},
        {272, 0x00000001U}, {273, 0x00000003U}, {274, 0x00000000U},
        {275, 0x00000000U}, {276, 0x00000002U}, {277, 0x00000000U},
    };
    size_t i;

    for (i = 0; i < sizeof(signature) / sizeof(signature[0]); ++i)
        if (!draw_word_selected(signature[i].index)) {
            uint32_t expected = signature[i].value;
            if (mutate_draw_mode >= 6 && signature[i].index == 268U)
                expected = 0x40000402U;
            if (mutate_draw_mode >= 6 && signature[i].index == 276U)
                expected = 0xffffffffU;
            if (mutate_draw_mode == 9 && signature[i].index == 269U)
                expected = 3U;
            if (words[signature[i].index] != expected)
                return 0;
        }
    return 1;
}

/* state_lock serializes the only permitted IB. No new ioctl is issued. */
static void maybe_mutate_draw_ib(int fd, const struct drm_loonggpu_cs_in *input)
{
    uint64_t chunk_pointers[LG110_MAX_CHUNKS];
    struct drm_loonggpu_cs_chunk_ib ib;
    struct ib_source source;
    uint32_t words[LG110_DRAW_SIGNATURE_DWORDS];
    unsigned int i;
    unsigned int ib_count = 0;

    if (mutation_done || !input->num_chunks ||
        input->num_chunks > LG110_MAX_CHUNKS)
        return;
    if (read_user(input->chunks, chunk_pointers,
                  input->num_chunks * sizeof(chunk_pointers[0])))
        return;
    memset(&ib, 0, sizeof(ib));
    for (i = 0; i < input->num_chunks; ++i) {
        struct drm_loonggpu_cs_chunk chunk;
        if (read_user(chunk_pointers[i], &chunk, sizeof(chunk)))
            return;
        if (chunk.chunk_id != LOONGGPU_CHUNK_ID_IB)
            continue;
        ++ib_count;
        if (ib_count != 1 || chunk.length_dw < (sizeof(ib) + 3) / 4 ||
            read_user(chunk.chunk_data, &ib, sizeof(ib)))
            return;
    }
    if (ib_count != 1 || ib.ip_type != LOONGGPU_HW_IP_GFX ||
        ib.ip_instance != 0 || ib.ring != 0 || ib.flags != 0 ||
        ib.ib_bytes != LG110_DRAW_DWORDS * sizeof(uint32_t))
        return;

    pthread_mutex_lock(&state_lock);
    if (!mutation_done) {
        source = resolve_ib_locked(fd, ib.va_start);
        if (source.valid && source.writable && source.available >= ib.ib_bytes &&
            source.user_address % sizeof(uint32_t) == 0) {
            if (read_user(source.user_address, words, sizeof(words)))
                memcpy(words, (const void *)source.user_address, sizeof(words));
            int guard_ok;
            if (mutate_draw_mode == 6) {
                guard_ok = words[270] == 3U && words[276] == 0xffffffffU &&
                           words[268] == 0x40000404U && words[280] != 0U;
            } else if (mutate_draw_mode >= 7) {
                guard_ok = words[270] == 3U && words[276] == 0xffffffffU &&
                           words[268] == 0x40000402U && words[280] != 0U;
                if (mutate_draw_mode == 7 || mutate_draw_mode == 8)
                    guard_ok = guard_ok && words[269] == 0U && words[274] == 0U;
                else
                    guard_ok = guard_ok && words[269] == 3U &&
                               words[274] == 0xffffffffU;
            } else {
                guard_ok = words[270] == 3U && words[276] == 2U &&
                           (mutate_draw_mode == 1 || words[268] == 0x40000400U);
            }
            if (draw_signature_matches(words) && guard_ok) {
                uint32_t *target = (uint32_t *)source.user_address;
                uint32_t observed;
                int patched;

                if (mutate_draw_mode == 1) {
                    target[270] = 2U;
                    __sync_synchronize();
                    observed = target[270];
                    patched = observed == 2U;
                    log_line("LG110DRAWMUTATE mode=count word=270 old=0x00000003 "
                             "new=0x00000002 observed=0x%08x patched=%d",
                             observed, patched ? 1 : 0);
                } else if (mutate_draw_mode == 2) {
                    target[268] = 0x40000000U;
                    __sync_synchronize();
                    observed = target[268];
                    patched = observed == 0x40000000U;
                    log_line("LG110DRAWMUTATE mode=topology word=268 old=0x40000400 "
                             "new=0x40000000 observed=0x%08x patched=%d",
                             observed, patched ? 1 : 0);
                } else if (mutate_draw_mode == 3) {
                    target[276] = 1U;
                    __sync_synchronize();
                    observed = target[276];
                    patched = observed == 1U;
                    log_line("LG110DRAWMUTATE mode=word276 word=276 old=0x00000002 "
                             "new=0x00000001 observed=0x%08x patched=%d",
                             observed, patched ? 1 : 0);
                } else if (mutate_draw_mode == 6) {
                    target[268] = 0x40000402U;
                    __sync_synchronize();
                    observed = target[268];
                    patched = observed == 0x40000402U;
                    log_line("LG110DRAWMUTATE mode=index32-to16 word=268 old=0x40000404 "
                             "new=0x40000402 observed=0x%08x patched=%d",
                             observed, patched ? 1 : 0);
                } else if (mutate_draw_mode == 7) {
                    target[269] = 3U;
                    __sync_synchronize();
                    observed = target[269];
                    patched = observed == 3U;
                    log_line("LG110DRAWMUTATE mode=first-index word=269 old=0x00000000 "
                             "new=0x00000003 observed=0x%08x patched=%d",
                             observed, patched ? 1 : 0);
                } else if (mutate_draw_mode == 8) {
                    target[274] = 1U;
                    __sync_synchronize();
                    observed = target[274];
                    patched = observed == 1U;
                    log_line("LG110DRAWMUTATE mode=base-vertex word=274 old=0x00000000 "
                             "new=0x00000001 observed=0x%08x patched=%d",
                             observed, patched ? 1 : 0);
                } else if (mutate_draw_mode == 9) {
                    target[274] = 0U;
                    __sync_synchronize();
                    observed = target[274];
                    patched = observed == 0U;
                    log_line("LG110DRAWMUTATE mode=base-neg-to-zero word=274 old=0xffffffff "
                             "new=0x00000000 observed=0x%08x patched=%d",
                             observed, patched ? 1 : 0);
                } else {
                    unsigned int index = mutate_draw_mode == 4 ? 274U : 275U;
                    target[index] = 1U;
                    __sync_synchronize();
                    observed = target[index];
                    patched = observed == 1U;
                    log_line("LG110DRAWMUTATE mode=word%u word=%u old=0x00000000 "
                             "new=0x00000001 observed=0x%08x patched=%d",
                             index, index, observed, patched ? 1 : 0);
                }
                mutation_done = 1;
            }
        }
    }
    pthread_mutex_unlock(&state_lock);
}

int ioctl(int fd, unsigned long request, ...)
{
    va_list ap;
    void *argument;
    int result;
    int saved_errno;
    int is_create = request == DRM_IOCTL_LOONGGPU_GEM_CREATE;
    int is_gem_mmap = request == DRM_IOCTL_LOONGGPU_GEM_MMAP;
    int is_va = request == DRM_IOCTL_LOONGGPU_GEM_VA ||
                request == LG110_IOCTL_GEM_VA_IOWR;
    int is_cs = request == DRM_IOCTL_LOONGGPU_CS;
    struct drm_loonggpu_gem_create_in create_input;
    struct drm_loonggpu_gem_mmap_in mmap_input;
    struct drm_loonggpu_gem_va va_input;
    struct drm_loonggpu_cs_in cs_input;
    int create_valid = 0, mmap_valid = 0, va_valid = 0, cs_valid = 0;

    va_start(ap, request);
    argument = va_arg(ap, void *);
    va_end(ap);

    if (in_hook)
        return (int)syscall(SYS_ioctl, fd, request, argument);

    in_hook = 1;
    pthread_once(&config_once, configure);
    if (!real_ioctl)
        RESOLVE_REAL(real_ioctl, "ioctl");

    if (is_create)
        create_valid = !read_user((uint64_t)(uintptr_t)argument,
                                  &create_input, sizeof(create_input));
    else if (is_gem_mmap)
        mmap_valid = !read_user((uint64_t)(uintptr_t)argument,
                                &mmap_input, sizeof(mmap_input));
    else if (is_va)
        va_valid = !read_user((uint64_t)(uintptr_t)argument,
                              &va_input, sizeof(va_input));
    else if (is_cs)
        cs_valid = !read_user((uint64_t)(uintptr_t)argument,
                              &cs_input, sizeof(cs_input));

    if (is_cs && cs_valid && mutate_draw_mode)
        maybe_mutate_draw_ib(fd, &cs_input);

    if (real_ioctl)
        result = real_ioctl(fd, request, argument);
    else
        result = (int)syscall(SYS_ioctl, fd, request, argument);
    saved_errno = errno;

    if (is_create && create_valid && result == 0) {
        struct drm_loonggpu_gem_create_out output;

        if (!read_user((uint64_t)(uintptr_t)argument, &output, sizeof(output)))
            track_gem_create(fd, create_input.bo_size, output.handle);
    } else if (is_gem_mmap && mmap_valid && result == 0) {
        struct drm_loonggpu_gem_mmap_out output;

        if (!read_user((uint64_t)(uintptr_t)argument, &output, sizeof(output)))
            track_gem_mmap(fd, mmap_input.handle, output.addr_ptr);
    } else if (is_va && va_valid && result == 0) {
        track_gem_va(fd, &va_input);
    } else if (is_cs) {
        uint64_t output_handle = 0;
        int output_valid = 0;
        unsigned int sequence = 0;

        if (result == 0)
            output_valid = !read_user((uint64_t)(uintptr_t)argument,
                                      &output_handle, sizeof(output_handle));
        pthread_mutex_lock(&state_lock);
        if (submission_count < max_submissions) {
            sequence = ++submission_count;
            if (sequence == 1)
                log_line("LG110TRACE version=1 pid=%ld max_submissions=%u "
                         "max_chunks=%u max_ib_dwords=%u",
                         (long)getpid(), max_submissions,
                         LG110_MAX_CHUNKS, max_ib_dwords);
            trace_cs_locked(sequence, fd, &cs_input, cs_valid, result,
                            saved_errno, output_valid, output_handle);
        }
        pthread_mutex_unlock(&state_lock);
    }

    if (is_va) {
        if (va_valid)
            aux_log("LG110VA fd=%d handle=%u operation=%u flags=0x%x "
                    "va=0x%016" PRIx64 " bo_offset=0x%016" PRIx64
                    " size=0x%016" PRIx64 " ret=%d errno=%d",
                    fd, va_input.handle, va_input.operation, va_input.flags,
                    (uint64_t)va_input.va_address,
                    (uint64_t)va_input.offset_in_bo, (uint64_t)va_input.map_size,
                    result, result == 0 ? 0 : saved_errno);
        else
            aux_log("LG110VA fd=%d snapshot=read_failed ret=%d errno=%d",
                    fd, result, result == 0 ? 0 : saved_errno);
    }

    errno = saved_errno;
    in_hook = 0;
    return result;
}

static void *mmap_common(void *address, size_t length, int protection,
                         int flags, int fd, off64_t offset, int use_mmap64)
{
    void *result;
    int saved_errno;

    if (in_hook)
        return (void *)syscall(SYS_mmap, address, length, protection,
                               flags, fd, (off_t)offset);

    in_hook = 1;
    pthread_once(&config_once, configure);
    if (use_mmap64) {
        if (!real_mmap64)
            RESOLVE_REAL(real_mmap64, "mmap64");
        if (real_mmap64)
            result = real_mmap64(address, length, protection, flags, fd, offset);
        else
            result = (void *)syscall(SYS_mmap, address, length, protection,
                                     flags, fd, (off_t)offset);
    } else {
        if (!real_mmap)
            RESOLVE_REAL(real_mmap, "mmap");
        if (real_mmap)
            result = real_mmap(address, length, protection, flags, fd, (off_t)offset);
        else
            result = (void *)syscall(SYS_mmap, address, length, protection,
                                     flags, fd, (off_t)offset);
    }
    saved_errno = errno;
    if (result != MAP_FAILED)
        track_user_map(fd, result, length, protection, offset);
    errno = saved_errno;
    in_hook = 0;
    return result;
}

void *mmap(void *address, size_t length, int protection, int flags, int fd,
           off_t offset)
{
    return mmap_common(address, length, protection, flags, fd,
                       (off64_t)offset, 0);
}

void *mmap64(void *address, size_t length, int protection, int flags, int fd,
             off64_t offset)
{
    return mmap_common(address, length, protection, flags, fd, offset, 1);
}

int munmap(void *address, size_t length)
{
    int result;
    int saved_errno;

    if (in_hook)
        return (int)syscall(SYS_munmap, address, length);

    in_hook = 1;
    if (!real_munmap)
        RESOLVE_REAL(real_munmap, "munmap");
    if (real_munmap)
        result = real_munmap(address, length);
    else
        result = (int)syscall(SYS_munmap, address, length);
    saved_errno = errno;
    if (result == 0)
        track_user_unmap(address, length);
    errno = saved_errno;
    in_hook = 0;
    return result;
}
