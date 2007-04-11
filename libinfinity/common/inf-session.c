/* infinote - Collaborative notetaking application
 * Copyright (C) 2007 Armin Burgmeier
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free
 * Software Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <libinfinity/common/inf-session.h>
#include <libinfinity/common/inf-connection-manager.h>
#include <libinfinity/common/inf-buffer.h>
#include <libinfinity/inf-marshal.h>

#include <string.h>

/* TODO: Set buffer to read-only during synchronization */

typedef struct _InfSessionSync InfSessionSync;
struct _InfSessionSync {
  InfXmlConnection* conn;
  guint messages_total;
  guint messages_sent;
  gboolean end_enqueued;
};

typedef struct _InfSessionPrivate InfSessionPrivate;
struct _InfSessionPrivate {
  InfConnectionManager* manager;
  InfBuffer* buffer;
  InfSessionStatus status;

  GHashTable* user_table;

  union {
    /* INF_SESSION_SYNCHRONIZING */
    struct {
      InfXmlConnection* conn;
      guint messages_total;
      guint messages_received;
      gchar* identifier;
    } sync;

    /* INF_SESSION_RUNNING */
    struct {
      GSList* syncs;
    } run;
  } shared;
};

typedef struct _InfSessionForeachUserData InfSessionForeachUserData;
struct _InfSessionForeachUserData {
  InfSessionForeachUserFunc func;
  gpointer user_data;
};

typedef struct _InfSessionXmlData InfSessionXmlData;
struct _InfSessionXmlData {
  InfSession* session;
  xmlNodePtr xml;
};

enum {
  PROP_0,

  PROP_CONNECTION_MANAGER,
  PROP_BUFFER,
  PROP_SYNC_CONNECTION,
  PROP_SYNC_IDENTIFIER,

  PROP_STATUS
};

enum {
  ADD_USER,
  REMOVE_USER,
  SYNCHRONIZATION_PROGRESS,
  SYNCHRONIZATION_COMPLETE,
  SYNCHRONIZATION_FAILED,

  LAST_SIGNAL
};

#define INF_SESSION_PRIVATE(obj) (G_TYPE_INSTANCE_GET_PRIVATE((obj), INF_TYPE_SESSION, InfSessionPrivate))

static GObjectClass* parent_class;
static guint session_signals[LAST_SIGNAL];
static GQuark inf_session_sync_error_quark;

/*
 * User table callbacks.
 */

static gboolean
inf_session_lookup_user_by_name_func(gpointer key,
                                     gpointer value,
                                     gpointer data)
{
  const gchar* user_name;
  user_name = inf_user_get_name(INF_USER(value));

  if(strcmp(user_name, (const gchar*)data) == 0) return TRUE;
  return FALSE;
}

static void
inf_session_foreach_user_func(gpointer key,
                              gpointer value,
                              gpointer user_data)
{
  InfSessionForeachUserData* data;
  data = (InfSessionForeachUserData*)user_data;

  data->func(INF_USER(value), data->user_data);
}

/*
 * Utility functions.
 */

static const gchar*
inf_session_sync_strerror(InfSessionSyncError errcode)
{
  switch(errcode)
  {
  case INF_SESSION_SYNC_ERROR_UNEXPECTED_NODE:
    return "Got unexpected XML node during synchronization";
  case INF_SESSION_SYNC_ERROR_ID_NOT_PRESENT:
    return "'id' attribute in user message is missing";
  case INF_SESSION_SYNC_ERROR_ID_IN_USE:
    return "User ID is already in use";
  case INF_SESSION_SYNC_ERROR_NAME_NOT_PRESENT:
    return "'name' attribute in user message is missing";
  case INF_SESSION_SYNC_ERROR_NAME_IN_USE:
    return "User Name is already in use";
  case INF_SESSION_SYNC_ERROR_CONNECTION_CLOSED:
    return "The connection was closed unexpectedly";
  case INF_SESSION_SYNC_ERROR_SENDER_CANCELLED:
    return "The sender cancelled the synchronization";
  case INF_SESSION_SYNC_ERROR_RECEIVER_CANCELLED:
    return "The receiver cancelled teh synchronization";
  case INF_SESSION_SYNC_ERROR_UNEXPECTED_BEGIN_OF_SYNC:
    return "Got begin-of-sync message, but synchronization is already "
           "in progress";
  case INF_SESSION_SYNC_ERROR_NUM_MESSAGES_MISSING:
    return "begin-of-sync message does not contain the number of messages "
           "to expect";
  case INF_SESSION_SYNC_ERROR_UNEXPECTED_END_OF_SYNC:
    return "Got end-of-sync message, but synchronization is still in progress";
  case INF_SESSION_SYNC_ERROR_EXPECTED_BEGIN_OF_SYNC:
    return "Expected begin-of-sync message as first message during "
           "synchronization";
  case INF_SESSION_SYNC_ERROR_EXPECTED_END_OF_SYNC:
    return "Expected end-of-sync message as last message during "
           "synchronization";
  case INF_SESSION_SYNC_ERROR_FAILED:
    return "An unknown synchronization error has occured";
  default:
    return "An error with unknown error code occured";
  }
}

static const gchar*
inf_session_get_sync_error_message(GQuark domain,
                                   guint code)
{
  if(domain == inf_session_sync_error_quark)
    return inf_session_sync_strerror(code);

  return "An error with unknown error domain occured";
}

static GSList*
inf_session_find_sync_item_by_connection(InfSession* session,
                                         InfXmlConnection* conn)
{
  InfSessionPrivate* priv;
  GSList* item;

  priv = INF_SESSION_PRIVATE(session);

  g_return_val_if_fail(priv->status == INF_SESSION_RUNNING, NULL);

  for(item = priv->shared.run.syncs; item != NULL; item = g_slist_next(item))
  {
    if( ((InfSessionSync*)item->data)->conn == conn)
      return item;
  }

  return NULL;
}

static InfSessionSync*
inf_session_find_sync_by_connection(InfSession* session,
                                    InfXmlConnection* conn)
{
  GSList* item;
  item = inf_session_find_sync_item_by_connection(session, conn);

  if(item == NULL) return NULL;
  return (InfSessionSync*)item->data;
}

/* Required by inf_session_release_connection() */
static void
inf_session_connection_notify_status_cb(InfXmlConnection* connection,
                                        const gchar* property,
                                        gpointer user_data);

static void
inf_session_release_connection(InfSession* session,
                               InfXmlConnection* connection)
{
  InfSessionPrivate* priv;
  GSList* item;
  gboolean result;

  priv = INF_SESSION_PRIVATE(session);

  switch(priv->status)
  {
  case INF_SESSION_SYNCHRONIZING:
    g_assert(priv->shared.sync.conn == connection);
    priv->shared.sync.conn = NULL;
    break;
  case INF_SESSION_RUNNING:
    item = inf_session_find_sync_item_by_connection(session, connection);
    g_assert(item != NULL);

    g_slice_free(InfSessionSync, (InfSessionSync*)item->data);
    priv->shared.run.syncs = g_slist_delete_link(
      priv->shared.run.syncs,
      item
    );

    break;
  case INF_SESSION_CLOSED:
  default:
    g_assert_not_reached();
    break;
  }

  /* If the connection was closed, the connection manager removes the
   * connection from itself automatically, so make sure that it has not
   * already done so. */
  result = inf_connection_manager_has_object(
    priv->manager,
    connection,
    INF_NET_OBJECT(session)
  );

  if(result == TRUE)
  {
    inf_connection_manager_remove_object(
      priv->manager,
      connection,
      INF_NET_OBJECT(session)
    );
  }

  g_signal_handlers_disconnect_by_func(
    G_OBJECT(connection),
    G_CALLBACK(inf_session_connection_notify_status_cb),
    session
  );

  g_object_unref(G_OBJECT(connection));
}

static void
inf_session_send_sync_error(InfSession* session,
                            GError* error)
{
  InfSessionPrivate* priv;
  xmlNodePtr node;
  gchar code_buf[16];

  priv = INF_SESSION_PRIVATE(session);

  g_return_if_fail(priv->status == INF_SESSION_SYNCHRONIZING);
  g_return_if_fail(priv->shared.sync.conn != NULL);

  node = xmlNewNode(NULL, (const xmlChar*)"sync-error");

  xmlNewProp(
     node,
    (const xmlChar*)"domain",
    (const xmlChar*)g_quark_to_string(error->domain)
  );

  sprintf(code_buf, "%u", (unsigned int)error->code);
  xmlNewProp(node, (const xmlChar*)"code", (const xmlChar*)code_buf);

  inf_connection_manager_send(
    priv->manager,
    priv->shared.sync.conn,
    INF_NET_OBJECT(session),
    node
  );
}

/*
 * Signal handlers.
 */
static void
inf_session_connection_notify_status_cb(InfXmlConnection* connection,
                                        const gchar* property,
                                        gpointer user_data)
{
  InfSession* session;
  InfSessionPrivate* priv;
  InfXmlConnectionStatus status;
  GError* error;

  session = INF_SESSION(user_data);
  priv = INF_SESSION_PRIVATE(session);
  error = NULL;

  g_object_get(G_OBJECT(connection), "status", &status, NULL);

  if(status == INF_XML_CONNECTION_CLOSED ||
     status == INF_XML_CONNECTION_CLOSING)
  {
    g_set_error(
      &error,
      inf_session_sync_error_quark,
      INF_SESSION_SYNC_ERROR_CONNECTION_CLOSED,
      "%s",
      inf_session_sync_strerror(INF_SESSION_SYNC_ERROR_CONNECTION_CLOSED)
    );

    switch(priv->status)
    {
    case INF_SESSION_SYNCHRONIZING:
      g_assert(connection == priv->shared.sync.conn);

      /* Release connection prior to session closure, otherwise,
       * inf_session_close would try to tell the synchronizer that the
       * session is closed, but this is rather senseless because the
       * communication channel just has been closed. */
      g_signal_emit(
        G_OBJECT(session),
        session_signals[SYNCHRONIZATION_FAILED],
        0,
        connection,
        error
      );

      inf_session_close(session);
      break;
    case INF_SESSION_RUNNING:
      g_assert(
        inf_session_find_sync_by_connection(session, connection) != NULL
      );

      g_signal_emit(
        G_OBJECT(session),
        session_signals[SYNCHRONIZATION_FAILED],
        0,
        connection,
        error
      );

      break;
    case INF_SESSION_CLOSED:
    default:
      g_assert_not_reached();
      break;
    }

    g_error_free(error);
  }
}

/*
 * GObject overrides.
 */

static void
inf_session_init_sync(InfSession* session)
{
  InfSessionPrivate* priv;
  priv = INF_SESSION_PRIVATE(session);

  /* RUNNING is initial state */
  if(priv->status == INF_SESSION_RUNNING)
  {
    priv->status = INF_SESSION_SYNCHRONIZING;
    priv->shared.sync.conn = NULL;
    priv->shared.sync.messages_total = 0;
    priv->shared.sync.messages_received = 0;
    priv->shared.sync.identifier = NULL;
  }
}

static void
inf_session_register_sync(InfSession* session)
{
  InfSessionPrivate* priv;
  priv = INF_SESSION_PRIVATE(session);

  /* Register NetObject when all requirements for initial synchronization
   * are met. */
  if(priv->status == INF_SESSION_SYNCHRONIZING &&
     priv->manager != NULL &&
     priv->shared.sync.conn != NULL &&
     priv->shared.sync.identifier != NULL)
  {
    inf_connection_manager_add_object(
      priv->manager,
      priv->shared.sync.conn,
      INF_NET_OBJECT(session),
      priv->shared.sync.identifier
    );

    g_signal_connect_after(
      G_OBJECT(priv->shared.sync.conn),
      "notify::status",
      G_CALLBACK(inf_session_connection_notify_status_cb),
      session
    );
  }
}

static void
inf_session_init(GTypeInstance* instance,
                 gpointer g_class)
{
  InfSession* session;
  InfSessionPrivate* priv;

  session = INF_SESSION(instance);
  priv = INF_SESSION_PRIVATE(session);

  priv->manager = NULL;
  priv->buffer = NULL;
  priv->status = INF_SESSION_RUNNING;

  priv->shared.run.syncs = NULL;

  priv->user_table = g_hash_table_new_full(
    NULL,
    NULL,
    NULL,
    (GDestroyNotify)g_object_unref
  );
}

static void
inf_session_dispose(GObject* object)
{
  InfSession* session;
  InfSessionPrivate* priv;

  session = INF_SESSION(object);
  priv = INF_SESSION_PRIVATE(session);

  /* Close session. This cancells all running synchronizations and tells
   * everyone that the session no longer exists. */
  inf_session_close(session);

  g_hash_table_remove_all(priv->user_table);

  g_object_unref(G_OBJECT(priv->buffer));
  priv->buffer = NULL;

  g_object_unref(G_OBJECT(priv->manager));
  priv->manager = NULL;

  G_OBJECT_CLASS(object)->dispose(object);
}

static void
inf_session_finalize(GObject* object)
{
  InfSession* session;
  InfSessionPrivate* priv;

  session = INF_SESSION(object);
  priv = INF_SESSION_PRIVATE(session);

  g_hash_table_destroy(priv->user_table);
  priv->user_table = NULL;

  G_OBJECT_CLASS(parent_class)->finalize(object);
}

static void
inf_session_set_property(GObject* object,
                         guint prop_id,
                         const GValue* value,
                         GParamSpec* pspec)
{
  InfSession* session;
  InfSessionPrivate* priv;
  InfXmlConnection* conn;
  gchar* identifier;

  session = INF_SESSION(object);
  priv = INF_SESSION_PRIVATE(session);

  switch(prop_id)
  {
  case PROP_CONNECTION_MANAGER:
    g_assert(priv->manager == NULL);
    priv->manager = INF_CONNECTION_MANAGER(g_value_dup_object(value));
    inf_session_register_sync(session);
    break;
  case PROP_BUFFER:
    g_assert(priv->buffer == NULL);
    priv->buffer = INF_BUFFER(g_value_dup_object(value));
    break;
  case PROP_SYNC_CONNECTION:
    conn = INF_XML_CONNECTION(g_value_get_object(value));
    if(conn != NULL)
    {
      inf_session_init_sync(session);
      priv->shared.sync.conn = conn;
      g_object_ref(G_OBJECT(priv->shared.sync.conn));
      inf_session_register_sync(session);
    }

    break;
  case PROP_SYNC_IDENTIFIER:
    identifier = g_value_dup_string(value);
    if(identifier != NULL)
    {
      inf_session_init_sync(session);
      priv->shared.sync.identifier = identifier;
      inf_session_register_sync(session);
    }

    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID(value, prop_id, pspec);
    break;
  }
}

static void
inf_session_get_property(GObject* object,
                         guint prop_id,
                         GValue* value,
                         GParamSpec* pspec)
{
  InfSession* session;
  InfSessionPrivate* priv;

  session = INF_SESSION(object);
  priv = INF_SESSION_PRIVATE(session);

  switch(prop_id)
  {
  case PROP_CONNECTION_MANAGER:
    g_value_set_object(value, G_OBJECT(priv->manager));
    break;
  case PROP_BUFFER:
    g_value_set_object(value, G_OBJECT(priv->buffer));
    break;
  case PROP_SYNC_CONNECTION:
    g_assert(priv->status == INF_SESSION_SYNCHRONIZING);
    g_value_set_object(value, G_OBJECT(priv->shared.sync.conn));
    break;
  case PROP_SYNC_IDENTIFIER:
    g_assert(priv->status == INF_SESSION_SYNCHRONIZING);
    g_value_set_string(value, priv->shared.sync.identifier);
    break;
  case PROP_STATUS:
    g_value_set_enum(value, priv->status);
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
    break;
  }
}

/*
 * VFunc implementations.
 */

static void
inf_session_to_xml_sync_impl_foreach_func(gpointer key,
                                          gpointer value,
                                          gpointer user_data)
{
  InfSessionXmlData* data;
  InfSessionClass* session_class;
  xmlNodePtr usernode;

  data = (InfSessionXmlData*)user_data;
  session_class = INF_SESSION_GET_CLASS(data->session);

  g_return_if_fail(session_class->user_to_xml != NULL);

  usernode = xmlNewNode(NULL, (const xmlChar*)"sync-user");
  session_class->user_to_xml(data->session, (InfUser*)value, usernode);

  xmlAddChild(data->xml, usernode);
}

static void
inf_session_to_xml_sync_impl(InfSession* session,
                             xmlNodePtr parent)
{
  InfSessionPrivate* priv;
  InfSessionXmlData data;

  priv = INF_SESSION_PRIVATE(session);
  data.session = session;
  data.xml = parent;

  g_hash_table_foreach(
    priv->user_table,
    inf_session_to_xml_sync_impl_foreach_func,
    &data
  );
}

static gboolean
inf_session_process_xml_sync_impl(InfSession* session,
                                  InfXmlConnection* connection,
                                  const xmlNodePtr xml,
                                  GError** error)
{
  InfSessionPrivate* priv;
  InfSessionClass* session_class;
  GArray* user_props;
  InfUser* user;
  GError* local_error;

  priv = INF_SESSION_PRIVATE(session);
  session_class = INF_SESSION_GET_CLASS(session);

  g_return_val_if_fail(session_class->get_xml_user_props != NULL, FALSE);

  g_return_val_if_fail(priv->status == INF_SESSION_SYNCHRONIZING, FALSE);
  g_return_val_if_fail(connection == priv->shared.sync.conn, FALSE);

  if(strcmp((const gchar*)xml->name, "sync-user") == 0)
  {
    user_props = session_class->get_xml_user_props(
      session,
      connection,
      xml
    );

    user = inf_session_add_user(
      session,
      (GParameter*)user_props->data,
      user_props->len,
      error
    );

    g_array_free(user_props, TRUE);

    if(user == NULL) return FALSE;
    return TRUE;
  }
  else if(strcmp((const gchar*)xml->name, "sync-cancel") == 0)
  {
    /* Synchronization was cancelled by remote site, so release connection
     * prior to closure, otherwise we would try to tell the remote site
     * that the session was closed, but there is no point in this because
     * it just was the other way around. */
    local_error = NULL;

    g_set_error(
      &local_error,
      inf_session_sync_error_quark,
      INF_SESSION_SYNC_ERROR_SENDER_CANCELLED,
      "%s",
      inf_session_sync_strerror(INF_SESSION_SYNC_ERROR_SENDER_CANCELLED)
    );

    g_signal_emit(
      G_OBJECT(session),
      session_signals[SYNCHRONIZATION_FAILED],
      0,
      connection,
      local_error
    );

    inf_session_close(session);
    g_error_free(local_error);

    /* Return FALSE, but do not set error because we handled it */
    return FALSE;
  }
  else
  {
    g_set_error(
      error,
      inf_session_sync_error_quark,
      INF_SESSION_SYNC_ERROR_UNEXPECTED_NODE,
      "%s",
      inf_session_sync_strerror(INF_SESSION_SYNC_ERROR_UNEXPECTED_NODE)
    );

    return FALSE;
  }
}

static GArray*
inf_session_get_xml_user_props_impl(InfSession* session,
                                    InfXmlConnection* conn,
                                    const xmlNodePtr xml)
{
  GArray* array;
  GParameter* parameter;
  xmlChar* name;
  xmlChar* id;

  array = g_array_sized_new(FALSE, FALSE, sizeof(GParameter), 16);

  name = xmlGetProp(xml, (const xmlChar*)"name");
  id = xmlGetProp(xml, (const xmlChar*)"id");

  if(id != NULL)
  {
    parameter = inf_session_get_user_property(array, "id");
    g_value_init(&parameter->value, G_TYPE_INT);
    g_value_set_uint(&parameter->value, strtoul((const gchar*)id, NULL, 10));
    xmlFree(id);
  }

  if(name != NULL)
  {
    parameter = inf_session_get_user_property(array, "name");
    g_value_init(&parameter->value, G_TYPE_STRING);
    g_value_set_string(&parameter->value, (const gchar*)name);
    xmlFree(name);
  }

  return array;
}

static gboolean
inf_session_validate_user_props_impl(InfSession* session,
                                     const GParameter* params,
                                     guint n_params,
                                     InfUser* exclude,
                                     GError** error)
{
  const GParameter* parameter;
  const gchar* name;
  InfUser* user;
  guint id;

  parameter = inf_session_lookup_user_property(params, n_params, "id");
  if(parameter == NULL)
  {
    g_set_error(
      error,
      inf_session_sync_error_quark,
      INF_SESSION_SYNC_ERROR_ID_NOT_PRESENT,
      "%s",
      inf_session_sync_strerror(INF_SESSION_SYNC_ERROR_ID_NOT_PRESENT)
    );

    return FALSE;
  }

  id = g_value_get_uint(&parameter->value);
  user = inf_session_lookup_user_by_id(session, id);

  if(user != NULL && user != exclude)
  {
    g_set_error(
      error,
      inf_session_sync_error_quark,
      INF_SESSION_SYNC_ERROR_ID_IN_USE,
      "%s",
      inf_session_sync_strerror(INF_SESSION_SYNC_ERROR_ID_IN_USE)
    );

    return FALSE;
  }

  parameter = inf_session_lookup_user_property(params, n_params, "name");
  if(parameter == NULL)
  {
    g_set_error(
      error,
      inf_session_sync_error_quark,
      INF_SESSION_SYNC_ERROR_NAME_NOT_PRESENT,
      "%s",
      inf_session_sync_strerror(INF_SESSION_SYNC_ERROR_NAME_NOT_PRESENT)
    );

    return FALSE;
  }

  name = g_value_get_string(&parameter->value);
  user = inf_session_lookup_user_by_name(session, name);

  if(user != NULL && user != exclude)
  {
    g_set_error(
      error,
      inf_session_sync_error_quark,
      INF_SESSION_SYNC_ERROR_NAME_IN_USE,
      "%s",
      inf_session_sync_strerror(INF_SESSION_SYNC_ERROR_NAME_IN_USE)
    );

    return FALSE;
  }

  return TRUE;
}

static void
inf_session_user_to_xml_impl(InfSession* session,
                             InfUser* user,
                             xmlNodePtr xml)
{
  gchar id[16];
  sprintf(id, "%u", inf_user_get_id(user));

  xmlNewProp(xml, (const xmlChar*)"id", (const xmlChar*)id);

  xmlNewProp(
    xml,
    (const xmlChar*)"name",
    (const xmlChar*)inf_user_get_name(user)
  );

  /* TODO: user status */
}

static void
inf_session_close_impl(InfSession* session)
{
  InfSessionPrivate* priv;
  InfSessionSync* sync;
  xmlNodePtr xml;
  GError* error;

  priv = INF_SESSION_PRIVATE(session);

  error = NULL;

  g_set_error(
    &error,
    inf_session_sync_error_quark,
    INF_SESSION_SYNC_ERROR_RECEIVER_CANCELLED,
    "%s",
    inf_session_sync_strerror(INF_SESSION_SYNC_ERROR_RECEIVER_CANCELLED)
  );

  switch(priv->status)
  {
  case INF_SESSION_SYNCHRONIZING:
    /* If we are getting the session synchronized and shared.sync.conn is
     * still set (it is not when a different error occured and the session
     * is therefore closed, see inf_session_net_object_received), then tell
     * the synchronizer. */
    if(priv->shared.sync.conn != NULL)
    {
      inf_session_send_sync_error(session, error);

      g_signal_emit(
        G_OBJECT(session),
        session_signals[SYNCHRONIZATION_FAILED],
        0,
        priv->shared.sync.conn,
        error
      );
    }

    g_free(priv->shared.sync.identifier);
    break;
  case INF_SESSION_RUNNING:
    while(priv->shared.run.syncs != NULL)
    {
      sync = (InfSessionSync*)priv->shared.run.syncs->data;

      /* If the sync-end message has already been enqueued within the
       * connection manager, we cannot cancel it anymore, so the remote
       * site will receive the full sync nevertheless, so we do not need
       * to cancel anything. */
      if(sync->end_enqueued == FALSE)
      {
        inf_connection_manager_cancel_outer(
          priv->manager,
          sync->conn,
          INF_NET_OBJECT(session)
        );

        xml = xmlNewNode(NULL, (const xmlChar*)"sync-cancel");
        inf_connection_manager_send(
          priv->manager,
          sync->conn,
          INF_NET_OBJECT(session),
          xml
        );

        /* We have to cancel the synchronization, so the synchronization
         * actually failed. */
        g_signal_emit(
          G_OBJECT(session),
          session_signals[SYNCHRONIZATION_FAILED],
          0,
          sync->conn,
          error
        );
      }
      else
      {
        inf_session_release_connection(session, sync->conn);
      }
    }

    break;
  case INF_SESSION_CLOSED:
  default:
    g_assert_not_reached();
    break;
  }

  g_error_free(error);

  priv->status = INF_SESSION_CLOSED;
  g_object_notify(G_OBJECT(session), "status");
}

/*
 * InfNetObject implementation.
 */
static gboolean
inf_session_handle_received_sync_message(InfSession* session,
                                         InfXmlConnection* connection,
                                         const xmlNodePtr node,
                                         GError** error)
{
  InfSessionClass* session_class;
  InfSessionPrivate* priv;
  xmlChar* num_messages;
  gboolean result;

  session_class = INF_SESSION_GET_CLASS(session);
  priv = INF_SESSION_PRIVATE(session);

  g_return_val_if_fail(session_class->process_xml_sync != NULL, FALSE);

  if(strcmp((const gchar*)node->name, "sync-begin") == 0)
  {
    if(priv->shared.sync.messages_total > 0)
    {
      g_set_error(
        error,
        inf_session_sync_error_quark,
        INF_SESSION_SYNC_ERROR_UNEXPECTED_BEGIN_OF_SYNC,
        "%s",
        inf_session_sync_strerror(
          INF_SESSION_SYNC_ERROR_UNEXPECTED_BEGIN_OF_SYNC
        )
      );

      return FALSE;
    }
    else
    {
      num_messages = xmlGetProp(node, (const xmlChar*)"num-messages");
      if(num_messages == NULL)
      {
        g_set_error(
          error,
          inf_session_sync_error_quark,
          INF_SESSION_SYNC_ERROR_NUM_MESSAGES_MISSING,
          "%s",
          inf_session_sync_strerror(
            INF_SESSION_SYNC_ERROR_NUM_MESSAGES_MISSING
          )
        );

        return FALSE;
      }
      else
      {
        /* 2 + [...] because we also count this initial sync-begin message
         * and the sync-end. This way, we can use a messages_total of 0 to
         * indicate that we did not yet get a sync-begin, even if the
         * whole sync does not contain any messages. */
        priv->shared.sync.messages_total = 2 + strtoul(
          (const gchar*)num_messages,
          NULL,
          0
        );

        priv->shared.sync.messages_received = 1;
        xmlFree(num_messages);

        g_signal_emit(
          G_OBJECT(session),
          session_signals[SYNCHRONIZATION_PROGRESS],
          0,
          connection,
          1.0 / (double)priv->shared.sync.messages_total
        );
 
        return TRUE;
      }
    }
  }
  else if(strcmp((const gchar*)node->name, "sync-end") == 0)
  {
    ++ priv->shared.sync.messages_received;
    if(priv->shared.sync.messages_received != priv->shared.sync.messages_total)
    {
      g_set_error(
        error,
        inf_session_sync_error_quark,
        INF_SESSION_SYNC_ERROR_UNEXPECTED_END_OF_SYNC,
        "%s",
        inf_session_sync_strerror(
          INF_SESSION_SYNC_ERROR_UNEXPECTED_END_OF_SYNC
        )
      );

      return FALSE;
    }
    else
    {
      /* Synchronization complete */
      g_signal_emit(
        G_OBJECT(session),
        session_signals[SYNCHRONIZATION_COMPLETE],
        0,
        connection
      );

      return TRUE;
    }
  }
  else
  {
    if(priv->shared.sync.messages_received == 0)
    {
      g_set_error(
        error,
        inf_session_sync_error_quark,
        INF_SESSION_SYNC_ERROR_EXPECTED_BEGIN_OF_SYNC,
        "%s",
        inf_session_sync_strerror(
          INF_SESSION_SYNC_ERROR_EXPECTED_BEGIN_OF_SYNC
        )
      );

      return FALSE;
    }
    else if(priv->shared.sync.messages_received ==
            priv->shared.sync.messages_total - 1)
    {
      g_set_error(
        error,
        inf_session_sync_error_quark,
        INF_SESSION_SYNC_ERROR_EXPECTED_END_OF_SYNC,
        "%s",
        inf_session_sync_strerror(INF_SESSION_SYNC_ERROR_EXPECTED_END_OF_SYNC)
      );

      return FALSE;
    }
    else
    {
      result = session_class->process_xml_sync(
        session,
        connection,
        node,
        error
      );

      if(result == FALSE) return FALSE;

      ++ priv->shared.sync.messages_received;

      g_signal_emit(
        G_OBJECT(session),
        session_signals[SYNCHRONIZATION_PROGRESS],
        0,
        connection,
        (double)priv->shared.sync.messages_received /
          (double)priv->shared.sync.messages_total
      );

      return TRUE;
    }
  }
}

static void
inf_session_net_object_sent(InfNetObject* net_object,
                            InfXmlConnection* connection,
                            const xmlNodePtr node)
{
  InfSessionSync* sync;

  sync = inf_session_find_sync_by_connection(
    INF_SESSION(net_object),
    connection
  );

  /* This can be any message from some session that is not related to
   * the synchronization, so do not assert here. */
  if(sync != NULL)
  {
    ++ sync->messages_sent;

    if(sync->messages_sent < sync->messages_total)
    {
      g_signal_emit(
        G_OBJECT(net_object),
        session_signals[SYNCHRONIZATION_PROGRESS],
        0,
        connection,
        (gdouble)sync->messages_sent / (gdouble)sync->messages_total
      );
    }
    else
    {
      /* TODO: Actually, we are not sure how far the client is with
       * processing the sync. He could still detect an error in which case
       * the whole synchronization process actually failed. Perhaps he should
       * ACK the end-of-sync before emitting synchronization-complete here. */
      g_signal_emit(
        G_OBJECT(net_object),
        session_signals[SYNCHRONIZATION_COMPLETE],
        0,
        connection
      );

      /* The default signal handler removes this sync from the list */
    }
  }
}

static void
inf_session_net_object_enqueued(InfNetObject* net_object,
                                InfXmlConnection* connection,
                                const xmlNodePtr node)
{
  InfSessionSync* sync;

  if(strcmp((const gchar*)node->name, "sync-end") == 0)
  {
    /* Remember when the last synchronization messages is enqueued because
     * we cannot cancel any synchronizations beyond that point. */
    sync = inf_session_find_sync_by_connection(
      INF_SESSION(net_object),
      connection
    );

    /* This should really be in the list if the node's name is sync-end,
     * otherwise most probably someone else sent a sync-end message via
     * this net_object. */
    g_assert(sync != NULL);
    g_assert(sync->end_enqueued == FALSE);

    sync->end_enqueued = TRUE;
  }
}

static void
inf_session_net_object_received(InfNetObject* net_object,
                                InfXmlConnection* connection,
                                const xmlNodePtr node)
{
  InfSessionClass* session_class;
  InfSession* session;
  InfSessionPrivate* priv;
  gboolean result;
  GQuark domain;
  InfSessionSyncError code;
  xmlChar* domain_attr;
  xmlChar* code_attr;
  GError* error;

  session = INF_SESSION(net_object);
  priv = INF_SESSION_PRIVATE(session);

  error = NULL;

  switch(priv->status)
  {
  case INF_SESSION_SYNCHRONIZING:
    g_assert(connection == priv->shared.sync.conn);

    result = inf_session_handle_received_sync_message(
      session,
      connection,
      node,
      &error
    );

    if(result == FALSE && error != NULL)
    {
      inf_session_send_sync_error(session, error);

      g_signal_emit(
        G_OBJECT(session),
        session_signals[SYNCHRONIZATION_FAILED],
        0,
        connection,
        error
      );

      g_error_free(error);
      inf_session_close(session);
    }

    break;
  case INF_SESSION_RUNNING:
    if(strcmp((const gchar*)node->name, "sync-error") == 0)
    {
      /* There was an error during synchronization, cancel remaining
       * messages. Note that even if the end was already enqueued, this
       * should do no further harm. */
      inf_connection_manager_cancel_outer(
        priv->manager,
        connection,
        INF_NET_OBJECT(session)
      );

      domain_attr = xmlGetProp(node, (const xmlChar*)"domain");
      code_attr = xmlGetProp(node, (const xmlChar*)"code");

      if(domain_attr != NULL && code_attr != NULL)
      {
        domain = g_quark_from_string((const gchar*)domain_attr);
        code = strtoul((const gchar*)code_attr, NULL, 0);

        g_set_error(
          &error,
          domain,
          code,
          "%s",
          inf_session_get_sync_error_message(domain, code)
        );
      }
      else
      {
        g_set_error(
          &error,
          inf_session_sync_error_quark,
          INF_SESSION_SYNC_ERROR_FAILED,
          "%s",
          inf_session_sync_strerror(INF_SESSION_SYNC_ERROR_FAILED)
        );
      }

      if(domain_attr != NULL) xmlFree(domain_attr);
      if(code_attr != NULL) xmlFree(code_attr);

      g_signal_emit(
        G_OBJECT(session),
        session_signals[SYNCHRONIZATION_FAILED],
        0,
        connection,
        error
      );

      g_error_free(error);
    }
    else
    {
      session_class = INF_SESSION_GET_CLASS(session);
      g_return_if_fail(session_class->process_xml_run != NULL);

      session_class->process_xml_run(session, connection, node);
    }

    break;
  case INF_SESSION_CLOSED:
  default:
    g_assert_not_reached();
    break;
  }
}

/*
 * Default signal handlers.
 */

static void
inf_session_add_user_handler(InfSession* session,
                             InfUser* user)
{
  InfSessionPrivate* priv;
  guint user_id;

  g_return_if_fail(INF_IS_SESSION(session));
  g_return_if_fail(INF_IS_USER(user));

  user_id = inf_user_get_id(user);
  g_return_if_fail(user_id > 0);

  priv = INF_SESSION_PRIVATE(session);

  g_return_if_fail(
    g_hash_table_lookup(priv->user_table, GUINT_TO_POINTER(user_id)) == NULL
  );

  g_hash_table_insert(priv->user_table, GUINT_TO_POINTER(user_id), user);
}

static void
inf_session_remove_user_handler(InfSession* session,
                                InfUser* user)
{
  InfSessionPrivate* priv;
  guint user_id;

  g_return_if_fail(INF_IS_SESSION(session));
  g_return_if_fail(INF_IS_USER(user));

  priv = INF_SESSION_PRIVATE(session);
  user_id = inf_user_get_id(user);

  g_return_if_fail(
    g_hash_table_lookup(priv->user_table, GUINT_TO_POINTER(user_id)) == user
  );

  g_hash_table_remove(priv->user_table, GUINT_TO_POINTER(user_id));
}

static void
inf_session_synchronization_complete_handler(InfSession* session,
                                             InfXmlConnection* connection)
{
  InfSessionPrivate* priv;
  priv = INF_SESSION_PRIVATE(session);

  switch(priv->status)
  {
  case INF_SESSION_SYNCHRONIZING:
    g_assert(connection == priv->shared.sync.conn);

    inf_session_release_connection(session, connection);
    g_free(priv->shared.sync.identifier);

    priv->status = INF_SESSION_RUNNING;
    priv->shared.run.syncs = NULL;

    g_object_notify(G_OBJECT(session), "status");
    break;
  case INF_SESSION_RUNNING:
    g_assert(
      inf_session_find_sync_by_connection(session, connection) != NULL
    );

    inf_session_release_connection(session, connection);
    break;
  case INF_SESSION_CLOSED:
  default:
    g_assert_not_reached();
    break;
  }
}

static void
inf_session_synchronization_failed_handler(InfSession* session,
                                           InfXmlConnection* connection,
                                           const GError* error)
{
  InfSessionPrivate* priv;
  priv = INF_SESSION_PRIVATE(session);

  switch(priv->status)
  {
  case INF_SESSION_SYNCHRONIZING:
    g_assert(connection == priv->shared.sync.conn);

    inf_session_release_connection(session, connection);
    /* TODO: Think about calling inf_session_close here. However, make sure
     * that this works even if inf_session_close emitted the
     * synchronization_failed signal. */
    break;
  case INF_SESSION_RUNNING:
    g_assert(
      inf_session_find_sync_by_connection(session, connection) != NULL
    );

    inf_session_release_connection(session, connection);
    break;
  case INF_SESSION_CLOSED:
  default:
    g_assert_not_reached();
    break;
  }
}

/*
 * Gype registration.
 */

static void
inf_session_class_init(gpointer g_class,
                       gpointer class_data)
{
  GObjectClass* object_class;
  InfSessionClass* session_class;

  object_class = G_OBJECT_CLASS(g_class);
  session_class = INF_SESSION_CLASS(g_class);

  parent_class = G_OBJECT_CLASS(g_type_class_peek_parent(g_class));
  g_type_class_add_private(g_class, sizeof(InfSessionPrivate));

  object_class->dispose = inf_session_dispose;
  object_class->finalize = inf_session_finalize;
  object_class->set_property = inf_session_set_property;
  object_class->get_property = inf_session_get_property;

  session_class->to_xml_sync = inf_session_to_xml_sync_impl;
  session_class->process_xml_sync = inf_session_process_xml_sync_impl;
  session_class->process_xml_run = NULL;

  session_class->get_xml_user_props = inf_session_get_xml_user_props_impl;
  session_class->validate_user_props = inf_session_validate_user_props_impl;

  session_class->user_to_xml = inf_session_user_to_xml_impl;
  session_class->user_new = NULL;

  session_class->close = inf_session_close_impl;

  session_class->add_user = inf_session_add_user_handler;
  session_class->remove_user = inf_session_remove_user_handler;
  session_class->synchronization_progress = NULL;
  session_class->synchronization_complete =
    inf_session_synchronization_complete_handler;
  session_class->synchronization_failed =
    inf_session_synchronization_failed_handler;

  inf_session_sync_error_quark = g_quark_from_static_string(
    "INF_SESSION_SYNC_ERROR"
  );

  g_object_class_install_property(
    object_class,
    PROP_CONNECTION_MANAGER,
    g_param_spec_object(
      "connection-manager",
      "Connection manager",
      "The connection manager used for sending requests",
      INF_TYPE_CONNECTION_MANAGER,
      G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY
    )
  );

  g_object_class_install_property(
    object_class,
    PROP_BUFFER,
    g_param_spec_object(
      "buffer",
      "Buffer",
      "The buffer in which the document content is stored",
      INF_TYPE_BUFFER,
      G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY
    )
  );

  g_object_class_install_property(
    object_class,
    PROP_SYNC_CONNECTION,
    g_param_spec_object(
      "sync-connection",
      "Synchronizing connection",
      "Connection which synchronizes the initial session state",
      INF_TYPE_XML_CONNECTION,
      G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY
    )
  );

  g_object_class_install_property(
    object_class,
    PROP_SYNC_IDENTIFIER,
    g_param_spec_string(
      "sync-identifier",
      "Synchronization identifier",
      "Identifier for the connection manager for the initial synchronization",
      NULL,
      G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY
    )
  );

  g_object_class_install_property(
    object_class,
    PROP_STATUS,
    g_param_spec_enum(
      "status",
      "Session Status",
      "Current status of the session",
      INF_TYPE_SESSION_STATUS,
      INF_SESSION_RUNNING,
      G_PARAM_READABLE
    )
  );

  session_signals[ADD_USER] = g_signal_new(
    "add-user",
    G_OBJECT_CLASS_TYPE(object_class),
    G_SIGNAL_RUN_LAST,
    G_STRUCT_OFFSET(InfSessionClass, add_user),
    NULL, NULL,
    inf_marshal_VOID__OBJECT,
    G_TYPE_NONE,
    1,
    INF_TYPE_USER
  );

  session_signals[REMOVE_USER] = g_signal_new(
    "remove-user",
    G_OBJECT_CLASS_TYPE(object_class),
    G_SIGNAL_RUN_LAST,
    G_STRUCT_OFFSET(InfSessionClass, remove_user),
    NULL, NULL,
    inf_marshal_VOID__OBJECT,
    G_TYPE_NONE,
    1,
    INF_TYPE_USER
  );

  session_signals[SYNCHRONIZATION_PROGRESS] = g_signal_new(
    "synchronization-progress",
    G_OBJECT_CLASS_TYPE(object_class),
    G_SIGNAL_RUN_LAST,
    G_STRUCT_OFFSET(InfSessionClass, synchronization_progress),
    NULL, NULL,
    inf_marshal_VOID__OBJECT_DOUBLE,
    G_TYPE_NONE,
    2,
    INF_TYPE_XML_CONNECTION,
    G_TYPE_DOUBLE
  );

  session_signals[SYNCHRONIZATION_COMPLETE] = g_signal_new(
    "synchronization-complete",
    G_OBJECT_CLASS_TYPE(object_class),
    G_SIGNAL_RUN_LAST,
    G_STRUCT_OFFSET(InfSessionClass, synchronization_complete),
    NULL, NULL,
    inf_marshal_VOID__OBJECT,
    G_TYPE_NONE,
    1,
    INF_TYPE_XML_CONNECTION
  );

  session_signals[SYNCHRONIZATION_FAILED] = g_signal_new(
    "synchronization-failed",
    G_OBJECT_CLASS_TYPE(object_class),
    G_SIGNAL_RUN_LAST,
    G_STRUCT_OFFSET(InfSessionClass, synchronization_failed),
    NULL, NULL,
    inf_marshal_VOID__OBJECT_POINTER,
    G_TYPE_NONE,
    2,
    INF_TYPE_XML_CONNECTION,
    G_TYPE_POINTER /* actually a GError* */
  );
}

static void
inf_session_net_object_init(gpointer g_iface,
                            gpointer iface_data)
{
  InfNetObjectIface* iface;
  iface = (InfNetObjectIface*)g_iface;

  iface->sent = inf_session_net_object_sent;
  iface->enqueued = inf_session_net_object_enqueued;
  iface->received = inf_session_net_object_received;
}

GType
inf_session_status_get_type(void)
{
  static GType session_status_type = 0;

  if(!session_status_type)
  {
    static const GEnumValue session_status_type_values[] = {
      {
        INF_SESSION_SYNCHRONIZING,
        "INF_SESSION_SYNCHRONIZING",
        "synchronizing"
      }, {
        INF_SESSION_RUNNING,
        "INF_SESSION_RUNNING",
        "running"
      }, {
        INF_SESSION_CLOSED,
        "INF_SESSION_CLOSED",
        "closed"
      }, {
        0,
        NULL,
        NULL
      }
    };

    session_status_type = g_enum_register_static(
      "InfSessionStatus",
      session_status_type_values
    );
  }

  return session_status_type;
}

GType
inf_session_get_type(void)
{
  static GType session_type = 0;

  if(!session_type)
  {
    static const GTypeInfo session_type_info = {
      sizeof(InfSessionClass),  /* class_size */
      NULL,                     /* base_init */
      NULL,                     /* base_finalize */
      inf_session_class_init,   /* class_init */
      NULL,                     /* class_finalize */
      NULL,                     /* class_data */
      sizeof(InfSession),       /* instance_size */
      0,                        /* n_preallocs */
      inf_session_init,         /* instance_init */
      NULL                      /* value_table */
    };

    static const GInterfaceInfo net_object_info = {
      inf_session_net_object_init,
      NULL,
      NULL
    };

    session_type = g_type_register_static(
      G_TYPE_OBJECT,
      "InfSession",
      &session_type_info,
      0
    );

    g_type_add_interface_static(
      session_type,
      INF_TYPE_NET_OBJECT,
      &net_object_info
    );
  }

  return session_type;
}

/*
 * Public API.
 */

/** inf_session_lookup_user_property:
 *
 * @array: A #GArray containing #GParameter values.
 *
 * Looks up the parameter with the given name in @array.
 *
 * Return Value: A #GParameter, or %NULL.
 **/
const GParameter*
inf_session_lookup_user_property(const GParameter* params,
                                 guint n_params,
                                 const gchar* name)
{
  guint i;

  g_return_val_if_fail(params != NULL || n_params == 0, NULL);
  g_return_val_if_fail(name != NULL, NULL);

  for(i = 0; i < n_params; ++ i)
    if(strcmp(params[i].name, name) == 0)
      return &params[i];

  return NULL;
}

/** inf_session_get_user_property:
 *
 * @array: A #GArray containing #GParameter values.
 *
 * Looks up the paremeter with the given name in @array. If there is no such
 * parameter, a new one will be created.
 *
 * Return Value: A #GParameter.
 **/
GParameter*
inf_session_get_user_property(GArray* array,
                              const gchar* name)
{
  GParameter* parameter;
  guint i;

  g_return_val_if_fail(array != NULL, NULL);
  g_return_val_if_fail(name != NULL, NULL);

  for(i = 0; i < array->len; ++ i)
    if(strcmp(g_array_index(array, GParameter, i).name, name) == 0)
      return &g_array_index(array, GParameter, i);

  g_array_set_size(array, array->len + 1);
  parameter = &g_array_index(array, GParameter, array->len - 1);

  parameter->name = name;
  return parameter;
}

/** inf_session_close:
 *
 * @session: A #InfSession.
 *
 * Closes a running session. When a session is closed, it unrefs all
 * connections and no longer handles requests.
 */
void
inf_session_close(InfSession* session)
{
  InfSessionClass* session_class;

  g_return_if_fail(INF_IS_SESSION(session));

  session_class = INF_SESSION_GET_CLASS(session);
  g_return_if_fail(session_class->close != NULL);

  session_class->close(session);
}

/** inf_session_get_connection_manager:
 *
 * @session: A #InfSession.
 *
 * Returns the connection manager for @session.
 *
 * Return Value: A #InfConnectionManager.
 **/
InfConnectionManager*
inf_session_get_connection_manager(InfSession* session)
{
  g_return_val_if_fail(INF_IS_SESSION(session), NULL);
  return INF_SESSION_PRIVATE(session)->manager;
}

/** inf_session_get_buffer:
 *
 * @session: A #InfSession.
 *
 * Returns the buffer used by @session.
 *
 * Return Value: A #InfBuffer.
 **/
InfBuffer*
inf_session_get_buffer(InfSession* session)
{
  g_return_val_if_fail(INF_IS_SESSION(session), NULL);
  return INF_SESSION_PRIVATE(session)->buffer;
}

/** inf_session_add_user:
 *
 * @session A #InfSession.
 * @params: Construction parameters for the #InfUser (or derived) object.
 * @n_params: Number of parameters.
 * @error: Location to store error information.
 *
 * Adds a user to @session. The user object is constructed via the
 * user_new vfunc of #InfSessionClass. This will create a new #InfUser
 * object by default, but may be overridden by subclasses to create
 * different kinds of users. This function should only be used by types
 * inheriting directly from #InfSession.
 *
 * Return Value: The new #InfUser, or %NULL in case of an error.
 **/
InfUser*
inf_session_add_user(InfSession* session,
                     const GParameter* params,
                     guint n_params,
                     GError** error)
{
  InfSessionClass* session_class;
  InfUser* user;
  gboolean result;

  g_return_val_if_fail(INF_IS_SESSION(session), NULL);
  session_class = INF_SESSION_GET_CLASS(session);

  g_return_val_if_fail(session_class->validate_user_props != NULL, NULL);
  g_return_val_if_fail(session_class->user_new != NULL, NULL);

  result = session_class->validate_user_props(
    session,
    params,
    n_params,
    NULL,
    error
  );

  if(result == TRUE)
  {
    user = session_class->user_new(session, params, n_params);
    g_signal_emit(G_OBJECT(session), session_signals[ADD_USER], 0, user);

    return user;
  }

  return NULL;
}

/** inf_session_remove_user:
 *
 * @session: A #InfSession.
 * @user: A #InfUser contained in @session.
 *
 * Removes @user from @session. This function will most likely only be useful
 * to types inheriting from #InfSession to remove a #InfUser from @session.
 **/
void
inf_session_remove_user(InfSession* session,
                        InfUser* user)
{
  InfSessionPrivate* priv;
  guint user_id;

  g_return_if_fail(INF_IS_SESSION(session));
  g_return_if_fail(INF_IS_USER(user));

  priv = INF_SESSION_PRIVATE(session);
  user_id = inf_user_get_id(user);

  g_return_if_fail(
    g_hash_table_lookup(priv->user_table, GUINT_TO_POINTER(user_id)) == user
  );

  g_object_ref(G_OBJECT(user));
  g_signal_emit(G_OBJECT(session), session_signals[REMOVE_USER], 0, user);
  g_object_unref(G_OBJECT(user));
}

/** inf_session_lookup_user_by_id:
 *
 * @session: A #InfSession.
 * @user_id: User ID to lookup.
 *
 * Returns the #InfUser with the given User ID in session.
 *
 * Return Value: A #InfUser, or %NULL.
 **/
InfUser*
inf_session_lookup_user_by_id(InfSession* session,
                              guint user_id)
{
  InfSessionPrivate* priv;

  g_return_val_if_fail(INF_IS_SESSION(session), NULL);

  priv = INF_SESSION_PRIVATE(priv);

  return INF_USER(
    g_hash_table_lookup(priv->user_table, GUINT_TO_POINTER(user_id))
  );
}

/** inf_session_lookup_user_by_name:
 *
 * @session: A #InfSession.
 * @name: User name to lookup.
 *
 * Returns an #InfUser with the given name if there is one.
 *
 * Return Value: A #InfUser, or %NULL.
 **/
InfUser*
inf_session_lookup_user_by_name(InfSession* session,
                                const gchar* name)
{
  InfSessionPrivate* priv;
  InfUser* user;

  g_return_val_if_fail(INF_IS_SESSION(session), NULL);
  g_return_val_if_fail(name != NULL, NULL);

  priv = INF_SESSION_PRIVATE(session);

  user = g_hash_table_find(
    priv->user_table,
    inf_session_lookup_user_by_name_func,
    (gpointer)name
  );

  return user;
}

/** inf_session_foreach_user:
 *
 * @session: A #InfSession.
 * @func: The function to call for each user.
 * @user_data: User data to pass to the function.
 *
 * Calls the given function for each user in the session. You should not
 * add or remove uses while this function is being executed.
 **/
void
inf_session_foreach_user(InfSession* session,
                         InfSessionForeachUserFunc func,
                         gpointer user_data)
{
  InfSessionPrivate* priv;
  InfSessionForeachUserData data;

  g_return_if_fail(INF_IS_SESSION(session));
  g_return_if_fail(func != NULL);

  priv = INF_SESSION_PRIVATE(session);

  data.func = func;
  data.user_data = user_data;

  g_hash_table_foreach(
    priv->user_table,
    inf_session_foreach_user_func,
    &data
  );
}

/** inf_session_synchronize_to:
 *
 * @session: A #InfSession with state %INF_SESSION_RUNNING.
 * @connection: A #InfConnection.
 * @identifier: A Session identifier.
 *
 * Initiates a synchronization to @connection. On the other end of
 * connection, a new session with the sync-connection and sync-identifier
 * construction properties set should have been created. @identifier is
 * used as an identifier for this synchronization in the connection
 * manager.
 *
 * A synchronization can only be initiated if @session is in state
 * %INF_SESSION_RUNNING.
 **/
void
inf_session_synchronize_to(InfSession* session,
                           InfXmlConnection* connection,
                           const gchar* identifier)
{
  InfSessionPrivate* priv;
  InfSessionClass* session_class;
  InfSessionSync* sync;
  xmlNodePtr messages;
  xmlNodePtr xml;
  gchar num_messages_buf[16];

  g_return_if_fail(INF_IS_SESSION(session));
  g_return_if_fail(INF_IS_XML_CONNECTION(connection));
  g_return_if_fail(identifier != NULL);

  priv = INF_SESSION_PRIVATE(session);

  g_return_if_fail(priv->status == INF_SESSION_RUNNING);
  g_return_if_fail(
    inf_session_find_sync_by_connection(session, connection) == NULL
  );

  session_class = INF_SESSION_GET_CLASS(session);
  g_return_if_fail(session_class->to_xml_sync != NULL);

  sync = g_slice_new(InfSessionSync);
  sync->conn = connection;
  sync->messages_sent = 0;
  sync->messages_total = 2; /* including sync-begin and sync-end */
  sync->end_enqueued = FALSE;

  g_object_ref(G_OBJECT(connection));
  priv->shared.run.syncs = g_slist_prepend(priv->shared.run.syncs, sync);

  g_signal_connect_after(
    G_OBJECT(connection),
    "notify::status",
    G_CALLBACK(inf_session_connection_notify_status_cb),
    session
  );

  inf_connection_manager_add_object(
    priv->manager,
    connection,
    INF_NET_OBJECT(session),
    identifier
  );

  /* Name is irrelevant because the node is only used to collect the child
   * nodes via the to_xml_sync vfunc. */
  messages = xmlNewNode(NULL, NULL);
  session_class->to_xml_sync(session, messages);

  for(xml = messages->children; xml != NULL; xml = xml->next)
    ++ sync->messages_total;

  sprintf(num_messages_buf, "%u", sync->messages_total - 2);

  xml = xmlNewNode(NULL, (const xmlChar*)"sync-begin");

  xmlNewProp(
    xml,
    (const xmlChar*)"num-messages",
    (const xmlChar*)num_messages_buf
  );

  inf_connection_manager_send(
    priv->manager,
    connection,
    INF_NET_OBJECT(session),
    xml
  );

  inf_connection_manager_send_multiple(
    priv->manager,
    connection,
    INF_NET_OBJECT(session),
    messages->children
  );

  xmlFreeNode(messages);

  xml = xmlNewNode(NULL, (const xmlChar*)"sync-end");

  inf_connection_manager_send(
    priv->manager,
    connection,
    INF_NET_OBJECT(session),
    xml
  );
}

/** inf_session_get_synchronization_status:
 *
 * @session: A #InfSession.
 * @connection: A #InfConnection.
 *
 * If @session is in status %INF_SESSION_SYNCHRONIZING, this always returns
 * %INF_SESSION_SYNC_IN_PROGRESS if @connection is the connection with which
 * the session is synchronized, and %INF_SESSION_SYNC_NONE otherwise.
 *
 * If @session is in status %INF_SESSION_RUNNING, this returns the status
 * of the synchronization to @connection. %INF_SESSION_SYNC_NONE is returned,
 * when there is currently no synchronization ongoing to @connection,
 * %INF_SESSION_SYNC_IN_PROGRESS is returned, if there is one, and
 * %INF_SESSION_SYNC_END_ENQUEUED is returned if the synchronization can no
 * longer be cancelled but is not yet complete and might still fail.
 *
 * If @session is in status $INF_SESSION_CLOSED, this always returns
 * %INF_SESSION_SYNC_NONE.
 *
 * Return Value: The synchronization status of @connection.
 **/
InfSessionSyncStatus
inf_session_get_synchronization_status(InfSession* session,
                                       InfXmlConnection* connection)
{
  InfSessionPrivate* priv;
  InfSessionSync* sync;

  g_return_val_if_fail(INF_IS_SESSION(session), INF_SESSION_SYNC_NONE);

  g_return_val_if_fail(
    INF_IS_XML_CONNECTION(connection),
    INF_SESSION_SYNC_NONE
  );

  priv = INF_SESSION_PRIVATE(session);

  switch(priv->status)
  {
  case INF_SESSION_SYNCHRONIZING:
    if(connection == priv->shared.sync.conn)
      return INF_SESSION_SYNC_IN_PROGRESS;
    return INF_SESSION_SYNC_NONE;
  case INF_SESSION_RUNNING:
    sync = inf_session_find_sync_by_connection(session, connection);
    if(sync == NULL) return INF_SESSION_SYNC_NONE;

    if(sync->end_enqueued == TRUE) return INF_SESSION_SYNC_END_ENQUEUED;
    return INF_SESSION_SYNC_IN_PROGRESS;
  case INF_SESSION_CLOSED:
    return INF_SESSION_SYNC_NONE;
  default:
    g_assert_not_reached();
    break;
  }
}