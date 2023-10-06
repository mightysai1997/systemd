/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <errno.h>
#include <stdlib.h>
#include <sys/mman.h>

#include "alloc-util.h"
#include "errno-util.h"
#include "fd-util.h"
#include "hashmap.h"
#include "list.h"
#include "log.h"
#include "macro.h"
#include "memory-util.h"
#include "mmap-cache.h"
#include "sigbus.h"

typedef struct Window Window;
typedef struct Context Context;

typedef enum WindowFlag {
        WINDOW_KEEP_ALWAYS  = 1u << (_MMAP_CACHE_CONTEXT_MAX + 0),
        WINDOW_INVALIDATED  = 1u << (_MMAP_CACHE_CONTEXT_MAX + 1),

        _WINDOW_UNUSED_MASK = WINDOW_INVALIDATED - 1,
} WindowFlag;

#define WINDOW_UNUSED(w) (((w)->flags & _WINDOW_UNUSED_MASK) == 0)

struct Window {
        MMapFileDescriptor *fd;

        WindowFlag flags;

        void *ptr;
        uint64_t offset;
        size_t size;

        LIST_FIELDS(Window, windows);
        LIST_FIELDS(Window, unused);
};

struct MMapFileDescriptor {
        MMapCache *cache;

        int fd;
        int prot;
        bool sigbus;

        LIST_HEAD(Window, windows);
};

struct MMapCache {
        unsigned n_ref;
        unsigned n_windows;

        unsigned n_context_cache_hit;
        unsigned n_window_list_hit;
        unsigned n_missed;

        Hashmap *fds;

        LIST_HEAD(Window, unused);
        Window *last_unused;

        Window *windows_by_context[_MMAP_CACHE_CONTEXT_MAX];
};

#define WINDOWS_MIN 64

#if ENABLE_DEBUG_MMAP_CACHE
/* Tiny windows increase mmap activity and the chance of exposing unsafe use. */
# define WINDOW_SIZE (page_size())
#else
# define WINDOW_SIZE (8ULL*1024ULL*1024ULL)
#endif

MMapCache* mmap_cache_new(void) {
        MMapCache *m;

        m = new(MMapCache, 1);
        if (!m)
                return NULL;

        *m = (MMapCache) {
                .n_ref = 1,
        };

        return m;
}

static Window* window_unlink(Window *w) {
        assert(w);

        MMapCache *m = mmap_cache_fd_cache(w->fd);

        if (w->ptr)
                munmap(w->ptr, w->size);

        if (WINDOW_UNUSED(w)) {
                if (m->last_unused == w)
                        m->last_unused = w->unused_prev;
                LIST_REMOVE(unused, m->unused, w);
        } else {
                for (unsigned i = 0; i < _MMAP_CACHE_CONTEXT_MAX; i++)
                        if (FLAGS_SET(w->flags, 1u << i))
                                assert_se(TAKE_PTR(m->windows_by_context[i]) == w);
        }

        return LIST_REMOVE(windows, w->fd->windows, w);
}

static void window_invalidate(Window *w) {
        assert(w);
        assert(w->fd);

        if (FLAGS_SET(w->flags, WINDOW_INVALIDATED))
                return;

        /* Replace the window with anonymous pages. This is useful when we hit a SIGBUS and want to make sure
         * the file cannot trigger any further SIGBUS, possibly overrunning the sigbus queue. */

        assert_se(mmap(w->ptr, w->size, w->fd->prot, MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED, -1, 0) == w->ptr);
        w->flags |= WINDOW_INVALIDATED;
}

static Window* window_free(Window *w) {
        if (!w)
                return NULL;

        window_unlink(w);
        w->fd->cache->n_windows--;

        return mfree(w);
}

static bool window_matches(Window *w, MMapFileDescriptor *f, uint64_t offset, size_t size) {
        assert(size > 0);

        return
                w &&
                f == w->fd &&
                offset >= w->offset &&
                offset + size <= w->offset + w->size;
}

static bool window_matches_by_addr(Window *w, MMapFileDescriptor *f, void *addr, size_t size) {
        assert(size > 0);

        return
                w &&
                f == w->fd &&
                (uint8_t*) addr >= (uint8_t*) w->ptr &&
                (uint8_t*) addr + size <= (uint8_t*) w->ptr + w->size;
}

static Window* window_add(MMapFileDescriptor *f, uint64_t offset, size_t size, void *ptr) {
        MMapCache *m = mmap_cache_fd_cache(f);
        Window *w;

        if (!m->last_unused || m->n_windows <= WINDOWS_MIN) {
                /* Allocate a new window */
                w = new(Window, 1);
                if (!w)
                        return NULL;
                m->n_windows++;
        } else
                /* Reuse an existing one */
                w = window_unlink(m->last_unused);

        *w = (Window) {
                .fd = f,
                .offset = offset,
                .size = size,
                .ptr = ptr,
        };

        return LIST_PREPEND(windows, f->windows, w);
}

static void context_detach_window(MMapCache *m, MMapCacheContext c) {
        Window *w;

        assert(m);
        assert(c >= 0 && c < _MMAP_CACHE_CONTEXT_MAX);

        w = TAKE_PTR(m->windows_by_context[c]);
        if (!w)
                return; /* Nothing attached. */

        assert(FLAGS_SET(w->flags, 1u << c));
        w->flags &= ~(1u << c);

        if (WINDOW_UNUSED(w)) {
                /* Not used anymore? */
#if ENABLE_DEBUG_MMAP_CACHE
                /* Unmap unused windows immediately to expose use-after-unmap by SIGSEGV. */
                window_free(w);
#else
                LIST_PREPEND(unused, m->unused, w);
                if (!m->last_unused)
                        m->last_unused = w;
#endif
        }
}

static void context_attach_window(MMapCache *m, MMapCacheContext c, Window *w) {
        assert(m);
        assert(c >= 0 && c < _MMAP_CACHE_CONTEXT_MAX);
        assert(w);

        if (m->windows_by_context[c] == w)
                return; /* Already attached. */

        context_detach_window(m, c);

        m->windows_by_context[c] = w;
        w->flags |= (1u << c);
}

static MMapCache* mmap_cache_free(MMapCache *m) {
        if (!m)
                return NULL;

        /* All windows are owned by fds, and each fd takes a reference of MMapCache. So, when this is called,
         * all fds are already freed, and hence there is no window. */

        assert(hashmap_isempty(m->fds));
        hashmap_free(m->fds);

        assert(!m->unused);
        assert(m->n_windows == 0);

        return mfree(m);
}

DEFINE_TRIVIAL_REF_UNREF_FUNC(MMapCache, mmap_cache, mmap_cache_free);

static int mmap_try_harder(MMapFileDescriptor *f, void *addr, int flags, uint64_t offset, size_t size, void **ret) {
        MMapCache *m = mmap_cache_fd_cache(f);

        assert(ret);

        for (;;) {
                void *ptr;

                ptr = mmap(addr, size, f->prot, flags, f->fd, offset);
                if (ptr != MAP_FAILED) {
                        *ret = ptr;
                        return 0;
                }
                if (errno != ENOMEM)
                        return negative_errno();

                /* When failed with ENOMEM, try again after making a room by freeing an unused window. */

                if (!m->last_unused)
                        return -ENOMEM; /* no free window, propagate the original error. */

                window_free(m->last_unused);
        }
}

static int add_mmap(
                MMapFileDescriptor *f,
                uint64_t offset,
                size_t size,
                struct stat *st,
                Window **ret) {

        uint64_t woffset, wsize;
        Window *w;
        void *d;
        int r;

        assert(f);
        assert(size > 0);
        assert(ret);

        woffset = offset & ~((uint64_t) page_size() - 1ULL);
        wsize = size + (offset - woffset);
        wsize = PAGE_ALIGN(wsize);

        if (wsize < WINDOW_SIZE) {
                uint64_t delta;

                delta = PAGE_ALIGN((WINDOW_SIZE - wsize) / 2);

                if (delta > offset)
                        woffset = 0;
                else
                        woffset -= delta;

                wsize = WINDOW_SIZE;
        }

        if (st) {
                /* Memory maps that are larger then the files underneath have undefined behavior. Hence,
                 * clamp things to the file size if we know it */

                if (woffset >= (uint64_t) st->st_size)
                        return -EADDRNOTAVAIL;

                if (woffset + wsize > (uint64_t) st->st_size)
                        wsize = PAGE_ALIGN(st->st_size - woffset);
        }

        r = mmap_try_harder(f, NULL, MAP_SHARED, woffset, wsize, &d);
        if (r < 0)
                return r;

        w = window_add(f, woffset, wsize, d);
        if (!w) {
                (void) munmap(d, wsize);
                return -ENOMEM;
        }

        *ret = w;
        return 0;
}

int mmap_cache_fd_get(
                MMapFileDescriptor *f,
                MMapCacheContext c,
                bool keep_always,
                uint64_t offset,
                size_t size,
                struct stat *st,
                void **ret) {

        MMapCache *m = mmap_cache_fd_cache(f);
        Window *w;
        int r;

        assert(size > 0);
        assert(c >= 0 && c < _MMAP_CACHE_CONTEXT_MAX);
        assert(ret);

        if (f->sigbus)
                return -EIO;

        /* Check whether the current context is the right one already */
        if (window_matches(m->windows_by_context[c], f, offset, size)) {
                m->n_context_cache_hit++;
                w = m->windows_by_context[c];
                goto found;
        }

        /* Drop the reference to the window, since it's unnecessary now */
        context_detach_window(m, c);

        /* Search for a matching mmap */
        LIST_FOREACH(windows, i, f->windows)
                if (window_matches(i, f, offset, size)) {
                        m->n_window_list_hit++;
                        w = i;

                        /* The window may be tagged as unused, let's drop the tag. */
                        if (WINDOW_UNUSED(w)) {
                                LIST_REMOVE(unused, m->unused, w);
                                if (m->last_unused == w)
                                        m->last_unused = w->unused_prev;
                        }

                        goto found;
                }

        m->n_missed++;

        /* Create a new mmap */
        r = add_mmap(f, offset, size, st, &w);
        if (r < 0)
                return r;

found:
        if (keep_always)
                w->flags |= WINDOW_KEEP_ALWAYS;

        context_attach_window(m, c, w);
        *ret = (uint8_t*) w->ptr + (offset - w->offset);
        return 0;
}

int mmap_cache_fd_pin(
                MMapFileDescriptor *f,
                MMapCacheContext c,
                void *addr,
                size_t size) {

        MMapCache *m = mmap_cache_fd_cache(f);
        Window *w;

        assert(addr);
        assert(c >= 0 && c < _MMAP_CACHE_CONTEXT_MAX);
        assert(size > 0);

        if (f->sigbus)
                return -EIO;

        /* Check if the current context is the right one. */
        if (window_matches_by_addr(m->windows_by_context[c], f, addr, size)) {
                m->n_context_cache_hit++;
                w = m->windows_by_context[c];
                goto found;
        }

        /* Search for a matching mmap. */
        LIST_FOREACH(windows, i, f->windows)
                if (window_matches_by_addr(i, f, addr, size)) {
                        m->n_window_list_hit++;
                        w = i;
                        goto found;
                }

        m->n_missed++;
        return -EADDRNOTAVAIL; /* Not found. */

found:
        if (FLAGS_SET(w->flags, WINDOW_KEEP_ALWAYS))
                return 0; /* The window will never unmapped. */

        /* Attach the window to the 'pinning' context. */
        context_attach_window(m, MMAP_CACHE_CONTEXT_PIN, w);
        return 1;
}

void mmap_cache_stats_log_debug(MMapCache *m) {
        assert(m);

        log_debug("mmap cache statistics: %u context cache hit, %u window list hit, %u miss",
                  m->n_context_cache_hit, m->n_window_list_hit, m->n_missed);
}

static void mmap_cache_process_sigbus(MMapCache *m) {
        bool found = false;
        MMapFileDescriptor *f;
        int r;

        assert(m);

        /* Iterate through all triggered pages and mark their files as invalidated. */
        for (;;) {
                bool ours;
                void *addr;

                r = sigbus_pop(&addr);
                if (_likely_(r == 0))
                        break;
                if (r < 0) {
                        log_error_errno(r, "SIGBUS handling failed: %m");
                        abort();
                }

                ours = false;
                HASHMAP_FOREACH(f, m->fds) {
                        LIST_FOREACH(windows, w, f->windows)
                                if (window_matches_by_addr(w, f, addr, 1)) {
                                        found = ours = f->sigbus = true;
                                        break;
                                }

                        if (ours)
                                break;
                }

                /* Didn't find a matching window, give up. */
                if (!ours) {
                        log_error("Unknown SIGBUS page, aborting.");
                        abort();
                }
        }

        /* The list of triggered pages is now empty. Now, let's remap all windows of the triggered file to
         * anonymous maps, so that no page of the file in question is triggered again, so that we can be sure
         * not to hit the queue size limit. */
        if (_likely_(!found))
                return;

        HASHMAP_FOREACH(f, m->fds) {
                if (!f->sigbus)
                        continue;

                LIST_FOREACH(windows, w, f->windows)
                        window_invalidate(w);
        }
}

bool mmap_cache_fd_got_sigbus(MMapFileDescriptor *f) {
        assert(f);

        mmap_cache_process_sigbus(f->cache);

        return f->sigbus;
}

int mmap_cache_add_fd(MMapCache *m, int fd, int prot, MMapFileDescriptor **ret) {
        _cleanup_free_ MMapFileDescriptor *f = NULL;
        MMapFileDescriptor *existing;
        int r;

        assert(m);
        assert(fd >= 0);

        existing = hashmap_get(m->fds, FD_TO_PTR(fd));
        if (existing) {
                if (existing->prot != prot)
                        return -EEXIST;
                if (ret)
                        *ret = existing;
                return 0;
        }

        f = new(MMapFileDescriptor, 1);
        if (!f)
                return -ENOMEM;

        *f = (MMapFileDescriptor) {
                .fd = fd,
                .prot = prot,
        };

        r = hashmap_ensure_put(&m->fds, NULL, FD_TO_PTR(fd), f);
        if (r < 0)
                return r;
        assert(r > 0);

        f->cache = mmap_cache_ref(m);

        if (ret)
                *ret = f;

        TAKE_PTR(f);
        return 1;
}

MMapFileDescriptor* mmap_cache_fd_free(MMapFileDescriptor *f) {
        if (!f)
                return NULL;

        /* Make sure that any queued SIGBUS are first dispatched, so that we don't end up with a SIGBUS entry
         * we cannot relate to any existing memory map. */

        mmap_cache_process_sigbus(f->cache);

        while (f->windows)
                window_free(f->windows);

        assert_se(hashmap_remove(f->cache->fds, FD_TO_PTR(f->fd)) == f);

        /* Unref the cache at the end. Otherwise, the assertions in mmap_cache_free() may be triggered. */
        f->cache = mmap_cache_unref(f->cache);

        return mfree(f);
}

MMapCache* mmap_cache_fd_cache(MMapFileDescriptor *f) {
        assert(f);
        return ASSERT_PTR(f->cache);
}
