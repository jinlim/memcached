#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <dlfcn.h>
#include <string.h>
#include <assert.h>

#include "config_parser.h"
#include "genhash.h"
#include "bucket_engine.h"

#include <memcached/engine.h>

typedef union proxied_engine {
    ENGINE_HANDLE *v0;
    ENGINE_HANDLE_V1 *v1;
} proxied_engine_t;

typedef struct proxied_engine_handle {
    proxied_engine_t pe;
    int refcount;
    bool valid;
} proxied_engine_handle_t;

struct bucket_engine {
    ENGINE_HANDLE_V1 engine;
    bool initialized;
    bool has_default;
    bool auto_create;
    char *proxied_engine_path;
    char *admin_user;
    proxied_engine_t default_engine;
    genhash_t *engines;
    CREATE_INSTANCE new_engine;
    GET_SERVER_API get_server_api;
    SERVER_HANDLE_V1 *server;
};

ENGINE_ERROR_CODE create_instance(uint64_t interface,
                                  GET_SERVER_API gsapi,
                                  ENGINE_HANDLE **handle);

static const char* bucket_get_info(ENGINE_HANDLE* handle);
static ENGINE_ERROR_CODE bucket_initialize(ENGINE_HANDLE* handle,
                                           const char* config_str);
static void bucket_destroy(ENGINE_HANDLE* handle);
static ENGINE_ERROR_CODE bucket_item_allocate(ENGINE_HANDLE* handle,
                                              const void* cookie,
                                              item **item,
                                              const void* key,
                                              const size_t nkey,
                                              const size_t nbytes,
                                              const int flags,
                                              const rel_time_t exptime);
static ENGINE_ERROR_CODE bucket_item_delete(ENGINE_HANDLE* handle,
                                            const void* cookie,
                                            item* item);
static void bucket_item_release(ENGINE_HANDLE* handle,
                                const void *cookie,
                                item* item);
static ENGINE_ERROR_CODE bucket_get(ENGINE_HANDLE* handle,
                                    const void* cookie,
                                    item** item,
                                    const void* key,
                                    const int nkey);
static ENGINE_ERROR_CODE bucket_get_stats(ENGINE_HANDLE* handle,
                                          const void *cookie,
                                          const char *stat_key,
                                          int nkey,
                                          ADD_STAT add_stat);
static void bucket_reset_stats(ENGINE_HANDLE* handle, const void *cookie);
static ENGINE_ERROR_CODE bucket_store(ENGINE_HANDLE* handle,
                                      const void *cookie,
                                      item* item,
                                      uint64_t *cas,
                                      ENGINE_STORE_OPERATION operation);
static ENGINE_ERROR_CODE bucket_arithmetic(ENGINE_HANDLE* handle,
                                           const void* cookie,
                                           const void* key,
                                           const int nkey,
                                           const bool increment,
                                           const bool create,
                                           const uint64_t delta,
                                           const uint64_t initial,
                                           const rel_time_t exptime,
                                           uint64_t *cas,
                                           uint64_t *result);
static ENGINE_ERROR_CODE bucket_flush(ENGINE_HANDLE* handle,
                                      const void* cookie, time_t when);
static ENGINE_ERROR_CODE initalize_configuration(struct bucket_engine *me,
                                                 const char *cfg_str);
static ENGINE_ERROR_CODE bucket_unknown_command(ENGINE_HANDLE* handle,
                                                const void* cookie,
                                                protocol_binary_request_header *request,
                                                ADD_RESPONSE response);
struct bucket_engine bucket_engine = {
    .engine = {
        .interface = {
            .interface = 1
        },
        .get_info = bucket_get_info,
        .initialize = bucket_initialize,
        .destroy = bucket_destroy,
        .allocate = bucket_item_allocate,
        .remove = bucket_item_delete,
        .release = bucket_item_release,
        .get = bucket_get,
        .get_stats = bucket_get_stats,
        .reset_stats = bucket_reset_stats,
        .store = bucket_store,
        .arithmetic = bucket_arithmetic,
        .flush = bucket_flush,
        .unknown_command = bucket_unknown_command,
    },
    .initialized = false,
};

ENGINE_ERROR_CODE create_instance(uint64_t interface,
                                  GET_SERVER_API gsapi,
                                  ENGINE_HANDLE **handle) {
    if (interface != 1) {
        return ENGINE_ENOTSUP;
    }

    *handle = (ENGINE_HANDLE*)&bucket_engine;
    bucket_engine.get_server_api = gsapi;
    bucket_engine.server = gsapi(1);
    return ENGINE_SUCCESS;
}

static bool has_valid_bucket_name(const char *n) {
    bool rv = strlen(n) > 0;
    for (; *n; n++) {
        rv &= isalpha(*n) || isdigit(*n) || *n == '.';
    }
    return rv;
}

static ENGINE_ERROR_CODE create_bucket(struct bucket_engine *e,
                                       const char *bucket_name,
                                       const char *config,
                                       proxied_engine_handle_t **e_out) {
    if (!has_valid_bucket_name(bucket_name)) {
        return ENGINE_EINVAL;
    }
    *e_out = calloc(sizeof(proxied_engine_handle_t), 1);
    proxied_engine_handle_t *peh = *e_out;
    peh->refcount = 1;
    peh->valid = true;
    assert(peh);

    ENGINE_ERROR_CODE rv = e->new_engine(1, e->get_server_api, &peh->pe.v0);
    // This was already verified, but we'll check it anyway
    assert(peh->pe.v0->interface == 1);
    if (peh->pe.v1->initialize(peh->pe.v0, config) != ENGINE_SUCCESS) {

        peh->pe.v1->destroy(peh->pe.v0);
        fprintf(stderr, "Failed to initialize instance. Error code: %d\n",
                rv);
        return rv;
    }

    if (genhash_find(e->engines, bucket_name, strlen(bucket_name)) == NULL) {
        genhash_update(e->engines, bucket_name, strlen(bucket_name), peh, 0);
        rv = ENGINE_SUCCESS;
    } else {
        rv = ENGINE_KEY_EEXISTS;
    }

    return rv;
}

static inline proxied_engine_t *get_engine(ENGINE_HANDLE *h,
                                           const void *cookie) {
    struct bucket_engine *e = (struct bucket_engine*)h;
    proxied_engine_handle_t *peh = e->server->get_engine_specific(cookie);
    if (peh == NULL || !peh->valid) {
        const char *user = e->server->get_auth_data(cookie);
        if (user) {
            peh = genhash_find(e->engines, user, strlen(user));
            if (!peh && e->auto_create) {
                // XXX:  Need default config
                create_bucket(e, user, "", &peh);
            }
        }
        if (peh) {
            peh->refcount++;
        }
        e->server->store_engine_specific(cookie, peh);
    }

    proxied_engine_t *rv = NULL;
    if (peh) {
        rv = &peh->pe;
    } else {
        rv = e->default_engine.v0 ? &e->default_engine : NULL;
    }

    return rv;
}

static inline struct bucket_engine* get_handle(ENGINE_HANDLE* handle) {
    return (struct bucket_engine*)handle;
}

static const char* bucket_get_info(ENGINE_HANDLE* handle) {
    return "Bucket engine v0.1";
}

static int my_hash_eq(const void *k1, size_t nkey1,
                      const void *k2, size_t nkey2) {
    return nkey1 == nkey2 && memcmp(k1, k2, nkey1) == 0;
}

static void* hash_strdup(const void *k, size_t nkey) {
    void *rv = calloc(nkey, 1);
    assert(rv);
    memcpy(rv, k, nkey);
    return rv;
}

static void* noop_dup(const void* ob, size_t vlen) {
    return (void*)ob;
}

static void engine_free(void* ob) {
    proxied_engine_handle_t *peh = (proxied_engine_handle_t *)ob;
    peh->valid = false;
    if (--peh->refcount == 0) {
        peh->pe.v1->destroy(peh->pe.v0);
        free(ob);
    }
}

static ENGINE_HANDLE *load_engine(const char *soname, const char *config_str,
                                  CREATE_INSTANCE *create_out) {
    ENGINE_HANDLE *engine = NULL;
    /* Hack to remove the warning from C99 */
    union my_hack {
        CREATE_INSTANCE create;
        void* voidptr;
    } my_create = {.create = NULL };

    void *handle = dlopen(soname, RTLD_LAZY | RTLD_LOCAL);
    if (handle == NULL) {
        const char *msg = dlerror();
        fprintf(stderr, "Failed to open library \"%s\": %s\n",
                soname ? soname : "self",
                msg ? msg : "unknown error");
        return NULL;
    }

    void *symbol = dlsym(handle, "create_instance");
    if (symbol == NULL) {
        fprintf(stderr,
                "Could not find symbol \"create_instance\" in %s: %s\n",
                soname ? soname : "self",
                dlerror());
        return NULL;
    }
    my_create.voidptr = symbol;
    if (create_out) {
        *create_out = my_create.create;
    }

    /* request a instance with protocol version 1 */
    ENGINE_ERROR_CODE error = (*my_create.create)(1,
                                                  bucket_engine.get_server_api,
                                                  &engine);

    if (error != ENGINE_SUCCESS || engine == NULL) {
        fprintf(stderr, "Failed to create instance. Error code: %d\n", error);
        dlclose(handle);
        return NULL;
    }

    if (engine->interface == 1) {
        ENGINE_HANDLE_V1 *v1 = (ENGINE_HANDLE_V1*)engine;
        if (v1->initialize(engine, config_str) != ENGINE_SUCCESS) {
            v1->destroy(engine);
            fprintf(stderr, "Failed to initialize instance. Error code: %d\n",
                    error);
            dlclose(handle);
            return NULL;
        }
    } else {
        fprintf(stderr, "Unsupported interface level\n");
        dlclose(handle);
        return NULL;
    }

    return engine;
}

static ENGINE_ERROR_CODE bucket_initialize(ENGINE_HANDLE* handle,
                                           const char* config_str) {
    struct bucket_engine* se = get_handle(handle);

    assert(!se->initialized);

    ENGINE_ERROR_CODE ret = initalize_configuration(se, config_str);
    if (ret != ENGINE_SUCCESS) {
        return ret;
    }

    static struct hash_ops my_hash_ops = {
        .hashfunc = genhash_string_hash,
        .hasheq = my_hash_eq,
        .dupKey = hash_strdup,
        .dupValue = noop_dup,
        .freeKey = free,
        .freeValue = engine_free
    };

    se->engines = genhash_init(1, my_hash_ops);
    if (se->engines == NULL) {
        return ENGINE_ENOMEM;
    }

    // Load the engine and find the pointers to the item functions
    ENGINE_HANDLE *eh = load_engine(se->proxied_engine_path, "",
                                    &se->new_engine);
    if (!eh) {
        return ENGINE_FAILED;
    }
    ENGINE_HANDLE_V1 *hv1 = (ENGINE_HANDLE_V1*)eh;
    bucket_engine.engine.item_get_cas = hv1->item_get_cas;
    bucket_engine.engine.item_set_cas = hv1->item_set_cas;
    bucket_engine.engine.item_get_key = hv1->item_get_key;
    bucket_engine.engine.item_get_data = hv1->item_get_data;
    bucket_engine.engine.item_get_clsid = hv1->item_get_clsid;
    // Shut it back down.
    ENGINE_HANDLE_V1 *ehv1 = (ENGINE_HANDLE_V1*)eh;
    ehv1->destroy(eh);

    // Initialization is useful to know if we *can* start up an
    // engine, but we check flags here to see if we should have and
    // shut it down if not.
    if (se->has_default) {
        se->default_engine.v0 = load_engine(se->proxied_engine_path, "", NULL);
    }

    se->initialized = true;
    return ENGINE_SUCCESS;
}

static void bucket_destroy(ENGINE_HANDLE* handle) {
    struct bucket_engine* se = get_handle(handle);

    if (se->initialized) {
        proxied_engine_t *e = get_engine(handle, NULL);
        if (e) {
            e->v1->destroy(e->v0);
            e->v0 = NULL;
        }
        genhash_free(se->engines);
        se->engines = NULL;
        se->initialized = false;
    }
}

static ENGINE_ERROR_CODE bucket_item_allocate(ENGINE_HANDLE* handle,
                                              const void* cookie,
                                              item **item,
                                              const void* key,
                                              const size_t nkey,
                                              const size_t nbytes,
                                              const int flags,
                                              const rel_time_t exptime) {
    proxied_engine_t *e = get_engine(handle, cookie);
    if (e) {
        return e->v1->allocate(e->v0, cookie, item, key,
                               nkey, nbytes, flags, exptime);
    } else {
        return ENGINE_ENOMEM;
    }
}

static ENGINE_ERROR_CODE bucket_item_delete(ENGINE_HANDLE* handle,
                                            const void* cookie,
                                            item* item) {
    proxied_engine_t *e = get_engine(handle, cookie);
    if (e) {
        return e->v1->remove(e->v0, cookie, item);
    } else {
        return ENGINE_KEY_ENOENT;
    }
}

static void bucket_item_release(ENGINE_HANDLE* handle,
                                const void *cookie,
                                item* item) {
    proxied_engine_t *e = get_engine(handle, cookie);
    assert(e);
    if (e) {
        e->v1->release(e->v0, cookie, item);
    }
}

static ENGINE_ERROR_CODE bucket_get(ENGINE_HANDLE* handle,
                                    const void* cookie,
                                    item** item,
                                    const void* key,
                                    const int nkey) {
    proxied_engine_t *e = get_engine(handle, cookie);
    if (e) {
        return e->v1->get(e->v0, cookie, item, key, nkey);
    } else {
        return ENGINE_KEY_ENOENT;
    }
}

static ENGINE_ERROR_CODE bucket_get_stats(ENGINE_HANDLE* handle,
                                          const void* cookie,
                                          const char* stat_key,
                                          int nkey,
                                          ADD_STAT add_stat)
{
    proxied_engine_t *e = get_engine(handle, cookie);
    if (e) {
        return e->v1->get_stats(e->v0, cookie, stat_key, nkey, add_stat);
    } else {
        return ENGINE_FAILED;
    }
}

static ENGINE_ERROR_CODE bucket_store(ENGINE_HANDLE* handle,
                                      const void *cookie,
                                      item* item,
                                      uint64_t *cas,
                                      ENGINE_STORE_OPERATION operation) {
    proxied_engine_t *e = get_engine(handle, cookie);
    if (e) {
        return e->v1->store(e->v0, cookie, item, cas, operation);
    } else {
        return ENGINE_NOT_STORED;
    }
}

static ENGINE_ERROR_CODE bucket_arithmetic(ENGINE_HANDLE* handle,
                                           const void* cookie,
                                           const void* key,
                                           const int nkey,
                                           const bool increment,
                                           const bool create,
                                           const uint64_t delta,
                                           const uint64_t initial,
                                           const rel_time_t exptime,
                                           uint64_t *cas,
                                           uint64_t *result) {
    proxied_engine_t *e = get_engine(handle, cookie);
    if (e) {
        return e->v1->arithmetic(e->v0, cookie, key, nkey,
                                 increment, create, delta, initial,
                                 exptime, cas, result);
    } else {
        return ENGINE_KEY_ENOENT;
    }
}

static ENGINE_ERROR_CODE bucket_flush(ENGINE_HANDLE* handle,
                                      const void* cookie, time_t when) {
    proxied_engine_t *e = get_engine(handle, cookie);
    return e->v1->flush(e->v0, cookie, when);
}

static void bucket_reset_stats(ENGINE_HANDLE* handle, const void *cookie) {
    proxied_engine_t *e = get_engine(handle, cookie);
    if (e) {
        e->v1->reset_stats(e->v0, cookie);
    }
}

static ENGINE_ERROR_CODE initalize_configuration(struct bucket_engine *me,
                                                 const char *cfg_str) {
    ENGINE_ERROR_CODE ret = ENGINE_SUCCESS;

    me->auto_create = true;

    if (cfg_str != NULL) {
        struct config_item items[] = {
            { .key = "engine",
              .datatype = DT_STRING,
              .value.dt_string = &me->proxied_engine_path },
            { .key = "admin",
              .datatype = DT_STRING,
              .value.dt_string = &me->admin_user },
            { .key = "default",
              .datatype = DT_BOOL,
              .value.dt_bool = &me->has_default },
            { .key = "auto_create",
              .datatype = DT_BOOL,
              .value.dt_bool = &me->auto_create },
            { .key = "config_file",
              .datatype = DT_CONFIGFILE },
            { .key = NULL}
        };

        ret = parse_config(cfg_str, items, stderr);
    }

    return ret;
}

#define EXTRACT_KEY(req, out)                                       \
    char keyz[ntohs(req->message.header.request.keylen) + 1];       \
    memcpy(keyz, ((void*)request) + sizeof(req->message.header),    \
           ntohs(req->message.header.request.keylen));              \
    keyz[ntohs(req->message.header.request.keylen)] = 0x00;

static ENGINE_ERROR_CODE handle_create_bucket(ENGINE_HANDLE* handle,
                                       const void* cookie,
                                       protocol_binary_request_header *request,
                                       ADD_RESPONSE response) {
    struct bucket_engine *e = (struct bucket_engine*)handle;
    protocol_binary_request_create_bucket *breq =
        (protocol_binary_request_create_bucket*)request;

    EXTRACT_KEY(breq, keyz);

    size_t bodylen = ntohl(breq->message.header.request.bodylen)
        - ntohs(breq->message.header.request.keylen);
    assert(bodylen < (1 << 16)); // 64k ought to be enough for anybody
    char configz[bodylen + 1];
    memcpy(configz, ((void*)request) + sizeof(breq->message.header)
           + ntohs(breq->message.header.request.keylen), bodylen);
    configz[ntohs(breq->message.header.request.keylen)] = 0x00;

    proxied_engine_handle_t *peh = NULL;
    ENGINE_ERROR_CODE ret = create_bucket(e, keyz, configz, &peh);

    const char *msg = "";
    protocol_binary_response_status rc = PROTOCOL_BINARY_RESPONSE_SUCCESS;

    switch(ret) {
    case ENGINE_SUCCESS:
        // Defaults as above.
        break;
    case ENGINE_KEY_EEXISTS:
        msg = "Bucket exists";
        rc = PROTOCOL_BINARY_RESPONSE_KEY_EEXISTS;
        break;
    default:
        msg = "Error creating bucket";
        rc = PROTOCOL_BINARY_RESPONSE_NOT_STORED;
    }

    response(msg, strlen(msg), "", 0, "", 0, 0, rc, 0, cookie);

    return ENGINE_SUCCESS;
}

static ENGINE_ERROR_CODE handle_delete_bucket(ENGINE_HANDLE* handle,
                                              const void* cookie,
                                              protocol_binary_request_header *request,
                                              ADD_RESPONSE response) {
    struct bucket_engine *e = (struct bucket_engine*)handle;
    protocol_binary_request_delete_bucket *breq =
        (protocol_binary_request_delete_bucket*)request;

    EXTRACT_KEY(breq, keyz);

    int upd = genhash_delete_all(e->engines, keyz, strlen(keyz));

    assert(genhash_find(e->engines, keyz, strlen(keyz)) == NULL);

    if (upd > 0) {
        response("", 0, "", 0, "", 0, 0, 0, 0, cookie);
    } else {
        const char *msg = "Not found.";
        response(msg, strlen(msg),
                 "", 0, "", 0,
                 0, PROTOCOL_BINARY_RESPONSE_KEY_ENOENT,
                 0, cookie);
    }

    return ENGINE_SUCCESS;
}

struct bucket_list {
    char *name;
    struct bucket_list *next;
};

static void add_engine(const void *key, size_t nkey,
                  const void *val, size_t nval,
                  void *arg) {
    struct bucket_list **blist_ptr = (struct bucket_list **)arg;
    struct bucket_list *n = calloc(sizeof(struct bucket_list), 1);
    assert(n);
    n->name = strdup(key);
    assert(n->name);
    n->next = *blist_ptr;
    *blist_ptr = n;
}

static ENGINE_ERROR_CODE handle_list_buckets(ENGINE_HANDLE* handle,
                                             const void* cookie,
                                             protocol_binary_request_header *request,
                                             ADD_RESPONSE response) {
    struct bucket_engine *e = (struct bucket_engine*)handle;

    // Accumulate the current bucket list.
    struct bucket_list *blist = NULL;
    genhash_iter(e->engines, add_engine, &blist);

    int len = 0, n = 0;
    struct bucket_list *p = blist;
    while (p) {
        len += strlen(p->name);
        n++;
        p = p->next;
    }

    // Now turn it into a space-separated list.
    char *blist_txt = calloc(sizeof(char), n + len);
    assert(blist_txt);
    p = blist;
    while (p) {
        strcat(blist_txt, p->name);
        if (p->next) {
            strcat(blist_txt, " ");
        }
        free(p->name);
        struct bucket_list *tmp = p->next;
        free(p);
        p = tmp;
    }

    // Response body will be "" in the case of an empty response.
    // Otherwise, it needs to account for the trailing space of the
    // above append code.
    response("", 0, "", 0, blist_txt,
             n == 0 ? 0 : (sizeof(char) * n + len) - 1,
             0, 0, 0, cookie);
    free(blist_txt);

    return ENGINE_SUCCESS;
}

static bool authorized(ENGINE_HANDLE* handle,
                       const void* cookie) {
    struct bucket_engine *e = (struct bucket_engine*)handle;
    bool rv = false;
    if (e->admin_user) {
        const char *user = e->server->get_auth_data(cookie);
        if (user) {
            rv = strcmp(user, e->admin_user) == 0;
        }
    }
    return rv;
}

static ENGINE_ERROR_CODE handle_expand_bucket(ENGINE_HANDLE* handle,
                                              const void* cookie,
                                              protocol_binary_request_header *request,
                                              ADD_RESPONSE response) {
    struct bucket_engine *e = (struct bucket_engine*)handle;
    protocol_binary_request_delete_bucket *breq =
        (protocol_binary_request_delete_bucket*)request;

    EXTRACT_KEY(breq, keyz);

    proxied_engine_t *proxied = genhash_find(e->engines, keyz, strlen(keyz));

    ENGINE_ERROR_CODE rv = ENGINE_SUCCESS;
    if (proxied) {
        rv = proxied->v1->unknown_command(handle, cookie, request, response);
    } else {
        const char *msg = "Engine not found";
        response(msg, strlen(msg),
                 "", 0, "", 0,
                 0, PROTOCOL_BINARY_RESPONSE_KEY_ENOENT,
                 0, cookie);
    }

    return rv;
}

static inline bool is_admin_command(uint8_t opcode) {
    return opcode == CREATE_BUCKET
        || opcode == DELETE_BUCKET
        || opcode == LIST_BUCKETS
        || opcode == EXPAND_BUCKET;
}

static ENGINE_ERROR_CODE bucket_unknown_command(ENGINE_HANDLE* handle,
                                                const void* cookie,
                                                protocol_binary_request_header *request,
                                                ADD_RESPONSE response)
{
    if (is_admin_command(request->request.opcode)
        && !authorized(handle, cookie)) {
        return ENGINE_ENOTSUP;
    }

    ENGINE_ERROR_CODE rv = ENGINE_ENOTSUP;
    switch(request->request.opcode) {
    case CREATE_BUCKET:
        rv = handle_create_bucket(handle, cookie, request, response);
        break;
    case DELETE_BUCKET:
        rv = handle_delete_bucket(handle, cookie, request, response);
        break;
    case LIST_BUCKETS:
        rv = handle_list_buckets(handle, cookie, request, response);
        break;
    case EXPAND_BUCKET:
        rv = handle_expand_bucket(handle, cookie, request, response);
        break;
    default: {
        proxied_engine_t *e = get_engine(handle, cookie);
        if (e) {
            rv = e->v1->unknown_command(handle, cookie, request, response);
        } else {
            rv = ENGINE_ENOTSUP;
        }
    }
    }
    return rv;
}
