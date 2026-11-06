#define _GNU_SOURCE

/*
 * Small read-only LG110 command-submission tracer.
 *
 * This LD_PRELOAD library only observes userspace memory and forwards the
 * original ioctl/mmap/munmap calls.  It never modifies ioctl payloads, IBs,
 * GPU registers, or driver state.  Trace output goes to stderr.
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
#define LG110_HARD_MAX_IB_DWORDS 1024
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
    char *end = NULL;
    unsigned long parsed;

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

static void track_user_map(int fd, void *address, size_t length, off64_t offset)
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
        track_user_map(fd, result, length, offset);
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
