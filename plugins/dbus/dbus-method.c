#include "dbus-plugin.h"

extern int DBG_METHOD;                         /* debug flag for methods */

static OhmPlugin    *dbus_plugin;              /* this plugin */

static hash_table_t *sys_objects;              /* system bus object table */
static hash_table_t *sess_objects;             /* session bus object table */

static hash_table_t *object_add(DBusBusType, const char *);
static int           object_del(DBusBusType, const char *);
static hash_table_t *object_lookup(DBusBusType, const char *);
static void          object_purge(void *);

static void free_key(void *);


typedef struct {
    const char   *path;                        /* object path */
    bus_t        *bus;                         /* bus this object is on */
    hash_table_t *methods;                     /* object method table */
} object_t;


typedef struct {
    const char                    *interface;
    const char                    *member;
    const char                    *signature;
    DBusObjectPathMessageFunction  handler;
    void                          *data;
} method_t;


/********************
 * method_init
 ********************/
int
method_init(OhmPlugin *plugin)
{
    bus_t *system, *session;
    
    system  = bus_by_type(DBUS_BUS_SYSTEM);
    session = bus_by_type(DBUS_BUS_SESSION);
    
    system->objects  = hash_table_create(NULL, object_purge);
    session->objects = hash_table_create(NULL, object_purge);
    
    if (system->objects == NULL || session->objects == NULL) {
        OHM_ERROR("dbus: failed to create method object tables");
        method_exit();
        return FALSE;
    }
    
    dbus_plugin = plugin;
    return TRUE;
}


/********************
 * method_exit
 ********************/
void
method_exit(void)
{
    bus_t *system, *session;
    
    system  = bus_by_type(DBUS_BUS_SYSTEM);
    session = bus_by_type(DBUS_BUS_SESSION);
    
    if (system->objects) {
        hash_table_destroy(system->objects);
        system->objects = NULL;
    }

    if (session->objects) {
        hash_table_destroy(session->objects);
        session->objects = NULL;
    }
    
    dbus_plugin = NULL;
}


/********************
 * method_key
 ********************/
static inline char *
method_key(char *buf, size_t size,
           const char *interface, const char *member, const char *signature)
{
    snprintf(buf, size, "%s.%s/%s",
             interface ? interface : "",
             member    ? member    : "",
             signature ? signature : "");
    return buf;
}


/********************
 * method_purge
 ********************/
static void
method_purge(method_t *method)
{
#define MEMBER_FREE(member) if ((method)->member) FREE((method)->member)

    MEMBER_FREE(interface);
    MEMBER_FREE(member);
    MEMBER_FREE(signature);
    
    FREE(method);

#undef METHOD_FREE
}


/********************
 * method_add
 ********************/
int
method_add(DBusBusType type, const char *path, const char *interface,
           const char *member, const char *signature,
           DBusObjectPathMessageFunction handler, void *data)
{
    bus_t        *bus;
    object_t     *object;
    method_t     *method;
    char          key_buf[1024];
    const char   *key = NULL;

    if ((bus = bus_by_type(type)) == NULL)
        return FALSE;
    
    if (ALLOC_OBJ(method) == NULL)
        goto failed;

    method->interface = interface ? STRDUP(interface) : NULL;
    method->member    = STRDUP(member);
    method->signature = signature ? STRDUP(signature) : NULL;
    method->handler   = handler;
    method->data      = data;
    
    method_key(key_buf, sizeof(key_buf), interface, member, signature);
    key = key_buf;

    if ((object = object_lookup(bus, path)) == NULL) {
        if ((object = object_add(bus, path)) == NULL)
            goto failed;
    }
    else
        if (hash_table_lookup(object->methods, key) != NULL)
            goto failed;
            
    if ((key = STRDUP(key)) == NULL)
        goto failed;

    if (!hash_table_insert(object->methods, key, method))
        goto failed;
    
    OHM_DEBUG(DBG_METHOD, "dbus: registered handler %p for %s:%s",
              handler, path, key);

    g_object_ref(dbus_plugin);
    return TRUE;
    
 failed:
    method_purge(method);
    FREE(key);

    return FALSE;
}


/********************
 * method_del
 ********************/
int
method_del(DBusBusType type, const char *path, const char *interface,
           const char *member, const char *signature,
           DBusObjectPathMessageFunction handler)
{
    bus_t    *bus;
    object_t *object;
    method_t *method;
    
    if ((bus = bus_by_type(type)) == NULL)
        return FALSE;

    method_key(key, sizeof(key), interface, member, signature);
    
    if ((object = object_lookup(bus, path))                == NULL ||
        (method = hash_table_lookup(object->methods, key)) == NULL)
        return FALSE;
    
    if (method->handler != handler) {
        OHM_WARNING("dbus: %s:%s has handler %p instead of %p",
                    path, key, method->handler, handler);
        return FALSE;
    }

    hash_table_remove(object->methods, key);
    
    OHM_DEBUG(DBG_METHOD, "dbus: unregistered method %s:%s", path, key);

    if (hash_table_empty(object->methods)) {
        OHM_DEBUG(DBG_METHOD, "destroying object %s with no methods", path);
        object_unregister(object);
        object_del(object);
    }

    g_object_unref(dbus_plugin);
    return TRUE;
}


/********************
 * method_dispatch
 ********************/
DBusHandlerResult
method_dispatch(DBusConnection *c, DBusMessage *msg, void *data)
{
    const char *path      = dbus_message_get_path(msg);
    const char *interface = dbus_message_get_path(msg);
    const char *member    = dbus_message_get_member(msg);
    const char *signature = dbus_message_get_signature(msg);
    const char *sender    = dbus_message_get_sender(msg);
    bus_t      *bus       = bus_by_connection(c);
    object_t   *object    = (object_t *)data;
    method_t   *method;
    char        key[1024];


    if (bus == NULL)
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    
    OHM_DEBUG(DBG_METHOD, "dbus: got method call %s.%s(%s) for %s from %s",
              interface, member, signature, path ? path : NULL, sender);

    method_key(key, sizeof(key), interface, member, signature);
    if ((method = hash_table_lookup(object->methods, key)) != NULL) {
        OHM_DEBUG(DBG_METHOD, "dbus: routing to handler %p (%s)",
                  method->handler, key);
        return method->handler(c, msg, method->data);
    }

    method_key(key, sizeof(key), interface, member, NULL);
    if ((method = hash_table_lookup(object->methods, key)) != NULL) {
        OHM_DEBUG(DBG_METHOD, "dbus: routing to handler %p (%s)",
                  method->handler, key);
        return method->handler(c, msg, method->data);
    }

    if ((method = hash_table_lookup(object->methods, member)) != NULL) {
        OHM_DEBUG(DBG_METHOD, "dbus: routing to handler %p (%s)",
                  method->handler, member);
        return method->handler(c, msg, method->data);
    }

    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}


/********************
 * object_add
 ********************/
static object_t *
object_add(bus_t *bus, const char *path)
{
    object_t *object;
    char     *key;

    if (ALLOC_OBJ(object) == NULL)
        goto failed;
    
    object->bus = bus;

    if ((object->path = STRDUP(path)) == NULL)
        goto failed;
    
    if ((object->methods = hash_table_create(NULL, object_purge)) == NULL)
        goto failed;
    
    if (!hash_table_insert(bus->objects, object->path, object))
        goto failed;

    if (!object_register(object))
        goto failed;

    return object;

 failed:
    if (object) {
        if (object->methods)
            hash_table_unhash(bus->objects, object->path);
        object_purge(object);
    }
    
    return NULL;
}


/********************
 * object_del
 ********************/
static int
object_del(bus_t *bus, const char *path)
{
    return hash_table_remove(bus->objects, path);
}


/********************
 * object_register
 ********************/
int
object_register(object_t *object)
{
    bus_t *bus = object->bus;

    if (bus->conn != NULL)
        return dbus_connection_register_object_path(bus->conn, object->path,
                                                    method_dispatch, object);
    else
        return TRUE;                    /* will retry once we're connected */
}


/********************
 * object_unregister
 ********************/
static void
object_unregister(object_t *object)
{
    bus_t *bus = object->bus;
    
    if (bus->conn && object->path)
        dbus_connection_unregister_object_path(bus->conn, object->path);
}


/********************
 * object_lookup
 ********************/
static object_t *
object_lookup(bus_t *bus, const char *path)
{
    return hash_table_lookup(bus->objects, path);
}


/********************
 * object_purge
 ********************/
static void
object_purge(void *ptr)
{
    object_t *object = (object_t *)ptr;

    object_unregister(object);
    if (object->methods)
        hash_table_destroy(object->methods);
    FREE(object->path);
    FREE(object);
}


/********************
 * register_object
 ********************/
void
register_object(gpointer key, gpointer value, gpointer data)
{
    object_t *object = (object_t *)value;

    object_register(object);
}


/********************
 * method_bus_up
 ********************/
void
method_bus_up(bus_t *bus)
{
    hash_table_foreach(bus->objects, register_object, NULL);
}


/* 
 * Local Variables:
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 * vim:set expandtab shiftwidth=4:
 */
