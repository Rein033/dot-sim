/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file khal_nvs_sim.c
 * @brief NVS on the host: one file per key, <data>/nvs/<namespace>/<key>,
 *        whose first byte is the type. Survives between runs of the same app.
 */
#include "khal_nvs.h"

#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "kdotsim.h"

#define T_U8   'b'
#define T_U64  'q'
#define T_STR  's'
#define T_BLOB 'x'
#define KEY_MAX 15U

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;

static bool name_ok(const char *s)
{
    if ( (NULL == s) || ('\0' == s[0]) || (strlen(s) > KEY_MAX) ) { return false; }
    for ( const char *p = s; *p; ++p )
    {
        if ( ('/' == *p) || ('.' == p[0] && p == s) ) { return false; }
    }
    return true;
}

static void path_of(char *out, size_t cap, const char *ns, const char *key)
{
    if ( NULL == key ) { (void)snprintf(out, cap, "%s/nvs/%s", kdotsim_data_dir(), ns); }
    else { (void)snprintf(out, cap, "%s/nvs/%s/%s", kdotsim_data_dir(), ns, key); }
}

kode_status_t khal_nvs_init(void)
{
    char p[600];
    (void)snprintf(p, sizeof(p), "%s/nvs", kdotsim_data_dir());
    (void)mkdir(p, 0755);
    return KODE_OK;
}

static kode_status_t put(const char *ns, const char *key, char type, const void *v, size_t len)
{
    if ( !name_ok(ns) || !name_ok(key) || ((NULL == v) && (0U != len)) ) { return KODE_DENIED; }
    char dir[600], p[700], tmp[720];
    (void)khal_nvs_init();
    path_of(dir, sizeof(dir), ns, NULL);
    (void)mkdir(dir, 0755);
    path_of(p, sizeof(p), ns, key);
    (void)snprintf(tmp, sizeof(tmp), "%s.tmp", p);
    pthread_mutex_lock(&s_lock);
    FILE *f = fopen(tmp, "wb");
    kode_status_t rc = KODE_IO_ERROR;
    if ( NULL != f )
    {
        if ( (fputc(type, f) != EOF) && (fwrite(v, 1U, len, f) == len) ) { rc = KODE_OK; }
        fclose(f);
        if ( (KODE_OK == rc) && (rename(tmp, p) != 0) ) { rc = KODE_IO_ERROR; }
    }
    pthread_mutex_unlock(&s_lock);
    return rc;
}

/* Reads a key of @p type: KODE_ERROR when it does not exist (as ESP_ERR_NVS_NOT_FOUND maps). */
static kode_status_t get(const char *ns, const char *key, char type, void *out, size_t *len, bool exact)
{
    if ( !name_ok(ns) || !name_ok(key) ) { return KODE_DENIED; }
    char p[700];
    path_of(p, sizeof(p), ns, key);
    pthread_mutex_lock(&s_lock);
    FILE *f = fopen(p, "rb");
    if ( NULL == f )
    {
        pthread_mutex_unlock(&s_lock);
        return KODE_DENIED;   /* not there: the SDK's "missing" */
    }
    kode_status_t rc = KODE_OK;
    int const t = fgetc(f);
    (void)fseek(f, 0, SEEK_END);
    long const total = ftell(f) - 1L;
    (void)fseek(f, 1L, SEEK_SET);
    if ( t != type )
    {
        rc = KODE_DENIED;
    }
    else if ( NULL == out )
    {
        if ( NULL != len ) { *len = (size_t)total; }
    }
    else if ( (exact && ((size_t)total != *len)) || ((size_t)total > *len) )
    {
        if ( NULL != len ) { *len = (size_t)total; }
        rc = KODE_ERROR;
    }
    else
    {
        *len = fread(out, 1U, (size_t)total, f);
    }
    fclose(f);
    pthread_mutex_unlock(&s_lock);
    return rc;
}

kode_status_t khal_nvs_get_str(const char *ns, const char *key, char *out, size_t cap)
{
    if ( (NULL == out) || (0U == cap) ) { return KODE_DENIED; }
    size_t len = cap - 1U;
    kode_status_t const rc = get(ns, key, T_STR, out, &len, false);
    if ( KODE_OK == rc ) { out[len] = '\0'; }
    return rc;
}

kode_status_t khal_nvs_set_str(const char *ns, const char *key, const char *value)
{
    if ( NULL == value ) { return KODE_DENIED; }
    return put(ns, key, T_STR, value, strlen(value));
}

kode_status_t khal_nvs_get_u8(const char *ns, const char *key, uint8_t *out)
{
    if ( NULL == out ) { return KODE_DENIED; }
    size_t len = 1U;
    return get(ns, key, T_U8, out, &len, true);
}

kode_status_t khal_nvs_set_u8(const char *ns, const char *key, uint8_t value)
{
    return put(ns, key, T_U8, &value, 1U);
}

kode_status_t khal_nvs_get_u64(const char *ns, const char *key, uint64_t *out)
{
    if ( NULL == out ) { return KODE_DENIED; }
    size_t len = 8U;
    return get(ns, key, T_U64, out, &len, true);
}

kode_status_t khal_nvs_set_u64(const char *ns, const char *key, uint64_t value)
{
    return put(ns, key, T_U64, &value, 8U);
}

kode_status_t khal_nvs_get_blob(const char *ns, const char *key, void *out, size_t *len)
{
    if ( NULL == len ) { return KODE_DENIED; }
    return get(ns, key, T_BLOB, out, len, false);
}

kode_status_t khal_nvs_set_blob(const char *ns, const char *key, const void *value, size_t len)
{
    return put(ns, key, T_BLOB, value, len);
}

kode_status_t khal_nvs_erase(const char *ns, const char *key)
{
    if ( !name_ok(ns) || !name_ok(key) ) { return KODE_DENIED; }
    char p[700];
    path_of(p, sizeof(p), ns, key);
    pthread_mutex_lock(&s_lock);
    int const r = unlink(p);
    pthread_mutex_unlock(&s_lock);
    (void)r;
    return KODE_OK;
}

/* The idx-th entry of a directory, in name order, skipping dot files and temporaries. */
static kode_status_t nth(const char *dir, uint16_t idx, char *out, size_t cap)
{
    struct dirent **list = NULL;
    int const n = scandir(dir, &list, NULL, alphasort);
    if ( n < 0 ) { return KODE_DENIED; }
    kode_status_t rc = KODE_DENIED;
    int seen = 0;
    for ( int i = 0; i < n; ++i )
    {
        const char *nm = list[i]->d_name;
        size_t const l = strlen(nm);
        bool const skip = ('.' == nm[0]) || ((l > 4U) && (0 == strcmp(nm + l - 4U, ".tmp")));
        if ( !skip )
        {
            if ( seen == (int)idx )
            {
                rc = (strlen(nm) < cap) ? KODE_OK : KODE_ERROR;
                if ( KODE_OK == rc ) { (void)snprintf(out, cap, "%s", nm); }
            }
            ++seen;
        }
        free(list[i]);
    }
    free(list);
    return rc;
}

kode_status_t khal_nvs_key(const char *ns, uint16_t idx, char *out, size_t cap)
{
    if ( !name_ok(ns) || (NULL == out) ) { return KODE_DENIED; }
    char d[600];
    path_of(d, sizeof(d), ns, NULL);
    return nth(d, idx, out, cap);
}

kode_status_t khal_nvs_namespace(uint16_t idx, char *out, size_t cap)
{
    if ( NULL == out ) { return KODE_DENIED; }
    char d[600];
    (void)snprintf(d, sizeof(d), "%s/nvs", kdotsim_data_dir());
    return nth(d, idx, out, cap);
}

kode_status_t khal_nvs_erase_namespace(const char *ns)
{
    if ( !name_ok(ns) ) { return KODE_DENIED; }
    char d[600];
    path_of(d, sizeof(d), ns, NULL);
    DIR *dir = opendir(d);
    if ( NULL == dir ) { return KODE_OK; }
    struct dirent *e;
    while ( (e = readdir(dir)) != NULL )
    {
        if ( '.' == e->d_name[0] ) { continue; }
        char p[900];
        (void)snprintf(p, sizeof(p), "%s/%s", d, e->d_name);
        (void)unlink(p);
    }
    closedir(dir);
    (void)rmdir(d);
    return KODE_OK;
}
