#include <stdio.h>
#include <inttypes.h>
#include <time.h>
#include <string.h>
#include <malloc.h>
#include <pthread.h>
#include <assert.h>

#include "statcache.h"
#include "fusedav.h"

#include <ne_uri.h>

#define CACHE_SIZE 2049
#define CACHE_TIMEOUT 60

struct dir_entry {
    struct dir_entry *next;
    int is_dir;
    char filename[];
};

struct cache_entry {
    struct {
        int valid;
        uint32_t hash;
        char *filename;
        time_t dead;
        struct stat st;
    } stat_info;

    struct {
        int valid, filling, in_use, valid2;
        uint32_t hash;
        char *filename;
        struct dir_entry *entries, *entries2;
        time_t dead, dead2;
    } dir_info;
};

static struct cache_entry *cache = NULL;
static pthread_mutex_t stat_cache_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t dir_cache_mutex = PTHREAD_MUTEX_INITIALIZER;

static uint32_t calc_hash(const char *s) {
    uint32_t h = 0;

    for (; *s; s++) {
        h ^= * (uint8_t*) s;
        h = (h << 8) | (h  >> 24);
    }

    return h;
}

int stat_cache_get(const char *fn, struct stat *st) {
    uint32_t h;
    struct cache_entry *ce;
    int r = -1;

    if (debug)
        fprintf(stderr, "CGET: %s\n", fn);
    
    assert(cache);
    
    h = calc_hash(fn);
    ce = cache + (h % CACHE_SIZE);

    pthread_mutex_lock(&stat_cache_mutex);
    
    if (ce->stat_info.valid &&
        ce->stat_info.filename &&
        ce->stat_info.hash == h &&
        !strcmp(ce->stat_info.filename, fn) &&
        time(NULL) <= ce->stat_info.dead) {
        
        *st = ce->stat_info.st;
        r = 0;
    }

    pthread_mutex_unlock(&stat_cache_mutex);
    
    return r;
}

void stat_cache_set(const char *fn, const struct stat*st) {
    uint32_t h;
    struct cache_entry *ce;

    if (debug)
        fprintf(stderr, "CSET: %s\n", fn);
    assert(cache);
    
    h = calc_hash(fn);
    ce = cache + (h % CACHE_SIZE);

    pthread_mutex_lock(&stat_cache_mutex);

    if (!ce->stat_info.filename || ce->stat_info.hash != h || strcmp(ce->stat_info.filename, fn)) {
        free(ce->stat_info.filename);
        ce->stat_info.filename = strdup(fn);
        ce->stat_info.hash = h;
    }
        
    ce->stat_info.st = *st;
    ce->stat_info.dead = time(NULL)+CACHE_TIMEOUT;
    ce->stat_info.valid = 1;

    pthread_mutex_unlock(&stat_cache_mutex);
}

void stat_cache_invalidate(const char*fn) {
    uint32_t h;
    struct cache_entry *ce;

    assert(cache);
    
    h = calc_hash(fn);
    ce = cache + (h % CACHE_SIZE);

    pthread_mutex_lock(&stat_cache_mutex);

    ce->stat_info.valid = 0;
    free(ce->stat_info.filename);
    ce->stat_info.filename = NULL;
    
    pthread_mutex_unlock(&stat_cache_mutex);
}

static void free_dir_entries(struct dir_entry *de) {

    while (de) {
        struct dir_entry *next = de->next;
        free(de);
        de = next;
    }
}


void dir_cache_begin(const char *fn) {
    uint32_t h;
    struct cache_entry *ce;
    struct dir_entry *de = NULL, *de2 = NULL;
    assert(cache);
    
    h = calc_hash(fn);
    ce = cache + (h % CACHE_SIZE);
    
    pthread_mutex_lock(&dir_cache_mutex);

    if (!ce->dir_info.filling) {
        
        if (!ce->dir_info.filename || ce->dir_info.hash != h || strcmp(ce->dir_info.filename, fn)) {
            free(ce->dir_info.filename);
            ce->dir_info.filename = strdup(fn);
            ce->dir_info.hash = h;

            de = ce->dir_info.entries;
            ce->dir_info.entries = NULL;
            ce->dir_info.valid = 0;
        }

        de2 = ce->dir_info.entries2;
        ce->dir_info.entries2 = NULL;
        ce->dir_info.valid2 = 0;
        ce->dir_info.filling = 1;
    }
    
    pthread_mutex_unlock(&dir_cache_mutex);
    free_dir_entries(de);
    free_dir_entries(de2);
}

void dir_cache_finish(const char *fn, int success) {
    uint32_t h;
    struct cache_entry *ce;
    struct dir_entry *de = NULL;
    assert(cache);
    
    h = calc_hash(fn);
    ce = cache + (h % CACHE_SIZE);
    
    pthread_mutex_lock(&dir_cache_mutex);
    
    if (ce->dir_info.filling &&
        ce->dir_info.filename &&
        ce->dir_info.hash == h &&
        !strcmp(ce->dir_info.filename, fn)) {

        assert(!ce->dir_info.valid2);

        if (success) {
            
            ce->dir_info.valid2 = 1;
            ce->dir_info.filling = 0;
            ce->dir_info.dead2 = time(NULL)+CACHE_TIMEOUT;
            
            if (!ce->dir_info.in_use) {
                de = ce->dir_info.entries;
                ce->dir_info.entries = ce->dir_info.entries2;
                ce->dir_info.entries2 = NULL;
                ce->dir_info.dead = ce->dir_info.dead2;
                ce->dir_info.valid2 = 0;
                ce->dir_info.valid = 1;
            }
            
        } else {
            ce->dir_info.filling = 0;
            de = ce->dir_info.entries2;
            ce->dir_info.entries2 = NULL;
        }
    }

    pthread_mutex_unlock(&dir_cache_mutex);
    free_dir_entries(de);
}

void dir_cache_add(const char *fn, const char *subdir, int is_dir) {
    uint32_t h;
    struct cache_entry *ce;
    assert(cache);
    
    h = calc_hash(fn);
    ce = cache + (h % CACHE_SIZE);
    
    pthread_mutex_lock(&dir_cache_mutex);
    
    if (ce->dir_info.filling &&
        ce->dir_info.filename &&
        ce->dir_info.hash == h &&
        !strcmp(ce->dir_info.filename, fn)) {

        struct dir_entry *n;

        assert(!ce->dir_info.valid2);

        n = malloc(sizeof(struct dir_entry) + strlen(subdir) + 1);
        assert(n);

        strcpy(n->filename, subdir);
        n->is_dir = is_dir;
        
        n->next = ce->dir_info.entries2;
        ce->dir_info.entries2 = n;
    }

    pthread_mutex_unlock(&dir_cache_mutex);
}

int dir_cache_enumerate(const char *fn, void (*f) (const char*fn, const char *subdir, int is_dir, void *user), void *user) {
    uint32_t h;
    struct cache_entry *ce;
    struct dir_entry *de = NULL;
    assert(cache && f);
    int r = -1;
    
    h = calc_hash(fn);
    ce = cache + (h % CACHE_SIZE);
    
    pthread_mutex_lock(&dir_cache_mutex);
    
    if (ce->dir_info.valid &&
        ce->dir_info.filename &&
        ce->dir_info.hash == h &&
        !strcmp(ce->dir_info.filename, fn) &&
        time(NULL) <= ce->dir_info.dead) {

        ce->dir_info.in_use = 1;
        pthread_mutex_unlock(&dir_cache_mutex);

        for (de = ce->dir_info.entries; de; de = de->next)
            f(fn, de->filename, de->is_dir, user);

        pthread_mutex_lock(&dir_cache_mutex);
        ce->dir_info.in_use = 0;

        if (ce->dir_info.valid2) {
            de = ce->dir_info.entries;
            ce->dir_info.entries = ce->dir_info.entries2;
            ce->dir_info.entries2 = NULL;
            ce->dir_info.dead = ce->dir_info.dead2;
            ce->dir_info.valid2 = 0;
            ce->dir_info.valid = 1;
        }

        r = 0;
    }
    
    pthread_mutex_unlock(&dir_cache_mutex);
    free_dir_entries(de);

    return r;
}   

void dir_cache_invalidate(const char*fn) {
    uint32_t h;
    struct cache_entry *ce;
    struct dir_entry *de = NULL;
    assert(cache && fn);
    
    h = calc_hash(fn);
    ce = cache + (h % CACHE_SIZE);
    pthread_mutex_lock(&dir_cache_mutex);
    
    if (ce->dir_info.valid &&
        ce->dir_info.filename &&
        ce->dir_info.hash == h &&
        !strcmp(ce->dir_info.filename, fn)) {

        ce->dir_info.valid = 0;
        de = ce->dir_info.entries;
        ce->dir_info.entries = NULL;
    }
    
    pthread_mutex_unlock(&dir_cache_mutex);
    free_dir_entries(de);
}

void dir_cache_invalidate_parent(const char *fn) {
    char *p;

    if ((p = ne_path_parent(fn))) {
        int l = strlen(p);

        if (strcmp(p, "/") && l) {
            if (p[l-1] == '/')
                p[l-1] = 0;
        }
        
        dir_cache_invalidate(p);
        free(p);
    } else
        dir_cache_invalidate(fn);
}

void cache_free(void) {
    uint32_t h;
    struct cache_entry *ce;

    if (!cache)
        return;

    for (h = 0, ce = cache; h < CACHE_SIZE; h++, ce++) {
        free(ce->stat_info.filename);
        free(ce->dir_info.filename);
        free_dir_entries(ce->dir_info.entries);
        free_dir_entries(ce->dir_info.entries2);
    }

    memset(cache, 0, sizeof(struct cache_entry)*CACHE_SIZE);
}

void cache_alloc(void) {
    
    if (cache)
        return;

    cache = malloc(sizeof(struct cache_entry)*CACHE_SIZE);
    memset(cache, 0, sizeof(struct cache_entry)*CACHE_SIZE);
}

