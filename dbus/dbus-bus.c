/* -*- mode: C; c-file-style: "gnu" -*- */
/* dbus-bus.c  Convenience functions for communicating with the bus.
 *
 * Copyright (C) 2003  CodeFactory AB
 * Copyright (C) 2003  Red Hat, Inc.
 *
 * Licensed under the Academic Free License version 2.0
 * 
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */

#include "dbus-bus.h"
#include "dbus-protocol.h"
#include "dbus-internals.h"
#include "dbus-message.h"
#include <string.h>

/**
 * @defgroup DBusBus Message bus APIs
 * @ingroup DBus
 * @brief Functions for communicating with the message bus
 *
 * @todo right now the default address of the system bus is hardcoded,
 * so if you change it in the global config file suddenly you have to
 * set DBUS_SYSTEM_BUS_ADDRESS env variable.  Might be nice if the
 * client lib somehow read the config file, or if the bus on startup
 * somehow wrote out its address to a well-known spot, but might also
 * not be worth it.
 */

/**
 * @defgroup DBusBusInternals Message bus APIs internals
 * @ingroup DBusInternals
 * @brief Internals of functions for communicating with the message bus
 *
 * @{
 */

/**
 * Block of message-bus-related data we attach to each
 * #DBusConnection used with these convenience functions.
 *
 *
 * @todo get rid of most of these; they should be done
 * with DBusGProxy and the Qt equivalent, i.e. the same
 * way any other interface would be used.
 */
typedef struct
{
  DBusConnection *connection; /**< Connection we're associated with */
  char *base_service; /**< Base service name of this connection */

  unsigned int is_well_known : 1; /**< Is one of the well-known connections in our global array */
} BusData;

/** The slot we have reserved to store BusData.
 */
static dbus_int32_t bus_data_slot = -1;

/** Number of bus types */
#define N_BUS_TYPES 3

static DBusConnection *bus_connections[N_BUS_TYPES];
static char *bus_connection_addresses[N_BUS_TYPES] = { NULL, NULL, NULL };

static DBusBusType activation_bus_type = DBUS_BUS_ACTIVATION;

static dbus_bool_t initialized = FALSE;

/**
 * Lock for globals in this file
 */
_DBUS_DEFINE_GLOBAL_LOCK (bus);

static void
addresses_shutdown_func (void *data)
{
  int i;

  i = 0;
  while (i < N_BUS_TYPES)
    {
      if (bus_connections[i] != NULL)
        _dbus_warn ("dbus_shutdown() called but connections were still live!");
      
      dbus_free (bus_connection_addresses[i]);
      bus_connection_addresses[i] = NULL;
      ++i;
    }

  activation_bus_type = DBUS_BUS_ACTIVATION;
}

static dbus_bool_t
get_from_env (char           **connection_p,
              const char      *env_var)
{
  const char *s;
  
  _dbus_assert (*connection_p == NULL);
  
  s = _dbus_getenv (env_var);
  if (s == NULL || *s == '\0')
    return TRUE; /* successfully didn't use the env var */
  else
    {
      *connection_p = _dbus_strdup (s);
      return *connection_p != NULL;
    }
}

static dbus_bool_t
init_connections_unlocked (void)
{
  if (!initialized)
    {
      const char *s;
      int i;

      i = 0;
      while (i < N_BUS_TYPES)
        {
          bus_connections[i] = NULL;
          ++i;
        }

      /* Don't init these twice, we may run this code twice if
       * init_connections_unlocked() fails midway through.
       * In practice, each block below should contain only one
       * "return FALSE" or running through twice may not
       * work right.
       */
      
       if (bus_connection_addresses[DBUS_BUS_SYSTEM] == NULL)
         {
           _dbus_verbose ("Filling in system bus address...\n");
           
           if (!get_from_env (&bus_connection_addresses[DBUS_BUS_SYSTEM],
                              "DBUS_SYSTEM_BUS_ADDRESS"))
             return FALSE;
         }

                  
       if (bus_connection_addresses[DBUS_BUS_SYSTEM] == NULL)
         {
           /* Use default system bus address if none set in environment */
           bus_connection_addresses[DBUS_BUS_SYSTEM] =
             _dbus_strdup (DBUS_SYSTEM_BUS_DEFAULT_ADDRESS);
           if (bus_connection_addresses[DBUS_BUS_SYSTEM] == NULL)
             return FALSE;
           
           _dbus_verbose ("  used default system bus \"%s\"\n",
                          bus_connection_addresses[DBUS_BUS_SYSTEM]);
         }
       else
         _dbus_verbose ("  used env var system bus \"%s\"\n",
                        bus_connection_addresses[DBUS_BUS_SYSTEM]);
          
      if (bus_connection_addresses[DBUS_BUS_SESSION] == NULL)
        {
          _dbus_verbose ("Filling in session bus address...\n");
          
          if (!get_from_env (&bus_connection_addresses[DBUS_BUS_SESSION],
                             "DBUS_SESSION_BUS_ADDRESS"))
            return FALSE;
          _dbus_verbose ("  \"%s\"\n", bus_connection_addresses[DBUS_BUS_SESSION] ?
                         bus_connection_addresses[DBUS_BUS_SESSION] : "none set");
        }

      if (bus_connection_addresses[DBUS_BUS_ACTIVATION] == NULL)
        {
          _dbus_verbose ("Filling in activation bus address...\n");
          
          if (!get_from_env (&bus_connection_addresses[DBUS_BUS_ACTIVATION],
                             "DBUS_ACTIVATION_ADDRESS"))
            return FALSE;
          
          _dbus_verbose ("  \"%s\"\n", bus_connection_addresses[DBUS_BUS_ACTIVATION] ?
                         bus_connection_addresses[DBUS_BUS_ACTIVATION] : "none set");
        }


      if (bus_connection_addresses[DBUS_BUS_ACTIVATION] != NULL)
        {
          s = _dbus_getenv ("DBUS_ACTIVATION_BUS_TYPE");
              
          if (s != NULL)
            {
              _dbus_verbose ("Bus activation type was set to \"%s\"\n", s);
                  
              if (strcmp (s, "system") == 0)
                activation_bus_type = DBUS_BUS_SYSTEM;
              else if (strcmp (s, "session") == 0)
                activation_bus_type = DBUS_BUS_SESSION;
            }
        }
      else
        {
          /* Default to the session bus instead if available */
          if (bus_connection_addresses[DBUS_BUS_SESSION] != NULL)
            {
              bus_connection_addresses[DBUS_BUS_ACTIVATION] =
                _dbus_strdup (bus_connection_addresses[DBUS_BUS_SESSION]);
              if (bus_connection_addresses[DBUS_BUS_ACTIVATION] == NULL)
                return FALSE;
            }
        }
      
      /* If we return FALSE we have to be sure that restarting
       * the above code will work right
       */
      
      if (!_dbus_setenv ("DBUS_ACTIVATION_ADDRESS", NULL))
        return FALSE;

      if (!_dbus_setenv ("DBUS_ACTIVATION_BUS_TYPE", NULL))
        return FALSE;
      
      if (!_dbus_register_shutdown_func (addresses_shutdown_func,
                                         NULL))
        return FALSE;
      
      initialized = TRUE;
    }

  return initialized;
}

static void
bus_data_free (void *data)
{
  BusData *bd = data;
  
  if (bd->is_well_known)
    {
      int i;
      _DBUS_LOCK (bus);
      /* We may be stored in more than one slot */
      i = 0;
      while (i < N_BUS_TYPES)
        {
          if (bus_connections[i] == bd->connection)
            bus_connections[i] = NULL;
          
          ++i;
        }
      _DBUS_UNLOCK (bus);
    }
  
  dbus_free (bd->base_service);
  dbus_free (bd);

  dbus_connection_free_data_slot (&bus_data_slot);
}

static BusData*
ensure_bus_data (DBusConnection *connection)
{
  BusData *bd;

  if (!dbus_connection_allocate_data_slot (&bus_data_slot))
    return NULL;

  bd = dbus_connection_get_data (connection, bus_data_slot);
  if (bd == NULL)
    {      
      bd = dbus_new0 (BusData, 1);
      if (bd == NULL)
        {
          dbus_connection_free_data_slot (&bus_data_slot);
          return NULL;
        }

      bd->connection = connection;
      
      if (!dbus_connection_set_data (connection, bus_data_slot, bd,
                                     bus_data_free))
        {
          dbus_free (bd);
          dbus_connection_free_data_slot (&bus_data_slot);
          return NULL;
        }

      /* Data slot refcount now held by the BusData */
    }
  else
    {
      dbus_connection_free_data_slot (&bus_data_slot);
    }

  return bd;
}

/** @} */ /* end of implementation details docs */

/**
 * @addtogroup DBusBus
 * @{
 */

/**
 * Connects to a bus daemon and registers the client with it.
 * If a connection to the bus already exists, then that connection is returned.
 *
 * @todo alex thinks we should nullify the connection when we get a disconnect-message.
 *
 * @param type bus type
 * @param error address where an error can be returned.
 * @returns a DBusConnection
 */
DBusConnection *
dbus_bus_get (DBusBusType  type,
	      DBusError   *error)
{
  const char *address;
  DBusConnection *connection;
  BusData *bd;
  DBusBusType address_type;

  _dbus_return_val_if_fail (type >= 0 && type < N_BUS_TYPES, NULL);
  _dbus_return_val_if_error_is_set (error, NULL);

  _DBUS_LOCK (bus);

  if (!init_connections_unlocked ())
    {
      _DBUS_UNLOCK (bus);
      dbus_set_error (error, DBUS_ERROR_NO_MEMORY, NULL);
      return NULL;
    }

  /* We want to use the activation address even if the
   * activating bus is the session or system bus,
   * per the spec.
   */
  address_type = type;
  
  /* Use the real type of the activation bus for getting its
   * connection, but only if the real type's address is available. (If
   * the activating bus isn't a well-known bus then
   * activation_bus_type == DBUS_BUS_ACTIVATION)
   */
  if (type == DBUS_BUS_ACTIVATION &&
      bus_connection_addresses[activation_bus_type] != NULL)
    type = activation_bus_type;
  
  if (bus_connections[type] != NULL)
    {
      connection = bus_connections[type];
      dbus_connection_ref (connection);
      
      _DBUS_UNLOCK (bus);
      return connection;
    }

  address = bus_connection_addresses[address_type];
  if (address == NULL)
    {
      dbus_set_error (error, DBUS_ERROR_FAILED,
                      "Unable to determine the address of the message bus");
      _DBUS_UNLOCK (bus);
      return NULL;
    }

  connection = dbus_connection_open (address, error);
  
  if (!connection)
    {
      _DBUS_ASSERT_ERROR_IS_SET (error);
      _DBUS_UNLOCK (bus);
      return NULL;
    }

  /* By default we're bound to the lifecycle of
   * the message bus.
   */
  dbus_connection_set_exit_on_disconnect (connection,
                                          TRUE);
  
  if (!dbus_bus_register (connection, error))
    {
      _DBUS_ASSERT_ERROR_IS_SET (error);
      dbus_connection_disconnect (connection);
      dbus_connection_unref (connection);

      _DBUS_UNLOCK (bus);
      return NULL;
    }

  bus_connections[type] = connection;
  bd = ensure_bus_data (connection);
  _dbus_assert (bd != NULL);

  bd->is_well_known = TRUE;

  _DBUS_UNLOCK (bus);
  return connection;
}


/**
 * Registers a connection with the bus. This must be the first
 * thing an application does when connecting to the message bus.
 * If registration succeeds, the base service name will be set,
 * and can be obtained using dbus_bus_get_base_service().
 *
 * @todo if we get an error reply, it has to be converted into
 * DBusError and returned
 * 
 * @param connection the connection
 * @param error place to store errors
 * @returns #TRUE on success
 */
dbus_bool_t
dbus_bus_register (DBusConnection *connection,
                   DBusError      *error)
{
  DBusMessage *message, *reply;
  char *name;
  BusData *bd;
  dbus_bool_t retval;

  _dbus_return_val_if_fail (connection != NULL, FALSE);
  _dbus_return_val_if_error_is_set (error, FALSE);

  retval = FALSE;
  
  bd = ensure_bus_data (connection);
  if (bd == NULL)
    {
      _DBUS_SET_OOM (error);
      return FALSE;
    }

  if (bd->base_service != NULL)
    {
      _dbus_warn ("Attempt to register the same DBusConnection with the message bus, but it is already registered\n");
      /* This isn't an error, it's a programming bug. We'll be nice
       * and not _dbus_assert_not_reached()
       */
      return TRUE;
    }
  
  message = dbus_message_new_method_call (DBUS_SERVICE_ORG_FREEDESKTOP_DBUS,
                                          DBUS_PATH_ORG_FREEDESKTOP_DBUS,
                                          DBUS_INTERFACE_ORG_FREEDESKTOP_DBUS,
                                          "Hello"); 

  if (!message)
    {
      _DBUS_SET_OOM (error);
      return FALSE;
    }
  
  reply = dbus_connection_send_with_reply_and_block (connection, message, -1, error);

  dbus_message_unref (message);
  
  if (reply == NULL)
    goto out;
  else if (dbus_set_error_from_message (error, reply))
    goto out;
  else if (!dbus_message_get_args (reply, error,
                                   DBUS_TYPE_STRING, &name,
                                   DBUS_TYPE_INVALID))
    goto out;
  
  bd->base_service = name;

  retval = TRUE;
  
 out:
  if (reply)
    dbus_message_unref (reply);

  if (!retval)
    _DBUS_ASSERT_ERROR_IS_SET (error);
  
  return retval;
}


/**
 * Sets the base service name of the connection.
 * Can only be used if you registered with the
 * bus manually (i.e. if you did not call
 * dbus_bus_register()). Can only be called
 * once per connection.
 *
 * @param connection the connection
 * @param base_service the base service name
 * @returns #FALSE if not enough memory
 */
dbus_bool_t
dbus_bus_set_base_service (DBusConnection *connection,
                           const char     *base_service)
{
  BusData *bd;

  _dbus_return_val_if_fail (connection != NULL, FALSE);
  _dbus_return_val_if_fail (base_service != NULL, FALSE);
  
  bd = ensure_bus_data (connection);
  if (bd == NULL)
    return FALSE;

  _dbus_assert (bd->base_service == NULL);
  
  bd->base_service = _dbus_strdup (base_service);
  return bd->base_service != NULL;
}

/**
 * Gets the base service name of the connection.
 * Only possible after the connection has been registered
 * with the message bus.
 *
 * @param connection the connection
 * @returns the base service name
 */
const char*
dbus_bus_get_base_service (DBusConnection *connection)
{
  BusData *bd;

  _dbus_return_val_if_fail (connection != NULL, NULL);
  
  bd = ensure_bus_data (connection);
  if (bd == NULL)
    return NULL;
  
  return bd->base_service;
}

/**
 * Asks the bus to return the uid of a service.
 *
 * @param connection the connection
 * @param service_name the service name
 * @param error location to store the error
 * @returns a result code, -1 if error is set
 */ 
unsigned long
dbus_bus_get_unix_user (DBusConnection *connection,
                        const char     *service,
                        DBusError      *error)
{
  DBusMessage *message, *reply;
  dbus_uint32_t uid;

  _dbus_return_val_if_fail (connection != NULL, DBUS_UID_UNSET);
  _dbus_return_val_if_fail (service != NULL, DBUS_UID_UNSET);
  _dbus_return_val_if_error_is_set (error, DBUS_UID_UNSET);
  
  message = dbus_message_new_method_call (DBUS_SERVICE_ORG_FREEDESKTOP_DBUS,
                                          DBUS_PATH_ORG_FREEDESKTOP_DBUS,
                                          DBUS_INTERFACE_ORG_FREEDESKTOP_DBUS,
                                          "GetConnectionUnixUser");

  if (message == NULL)
    {
      _DBUS_SET_OOM (error);
      return DBUS_UID_UNSET;
    }
 
  if (!dbus_message_append_args (message,
				 DBUS_TYPE_STRING, service,
				 DBUS_TYPE_INVALID))
    {
      dbus_message_unref (message);
      _DBUS_SET_OOM (error);
      return DBUS_UID_UNSET;
    }
  
  reply = dbus_connection_send_with_reply_and_block (connection, message, -1,
                                                     error);
  
  dbus_message_unref (message);
  
  if (reply == NULL)
    {
      _DBUS_ASSERT_ERROR_IS_SET (error);
      return DBUS_UID_UNSET;
    }  

  if (dbus_set_error_from_message (error, reply))
    {
      _DBUS_ASSERT_ERROR_IS_SET (error);
      dbus_message_unref (reply);
      return DBUS_UID_UNSET;
    }
  
  if (!dbus_message_get_args (reply, error,
                              DBUS_TYPE_UINT32, &uid,
                              DBUS_TYPE_INVALID))
    {
      _DBUS_ASSERT_ERROR_IS_SET (error);
      dbus_message_unref (reply);
      return DBUS_UID_UNSET;
    }

  dbus_message_unref (reply);
  
  return (unsigned long) uid;
}


/**
 * Asks the bus to try to acquire a certain service.
 *
 * @todo these docs are not complete, need to document the
 * return value and flags
 * 
 * @todo if we get an error reply, it has to be converted into
 * DBusError and returned
 *
 * @param connection the connection
 * @param service_name the service name
 * @param flags flags
 * @param error location to store the error
 * @returns a result code, -1 if error is set
 */ 
int
dbus_bus_acquire_service (DBusConnection *connection,
			  const char     *service_name,
			  unsigned int    flags,
                          DBusError      *error)
{
  DBusMessage *message, *reply;
  dbus_uint32_t service_result;

  _dbus_return_val_if_fail (connection != NULL, 0);
  _dbus_return_val_if_fail (service_name != NULL, 0);
  _dbus_return_val_if_error_is_set (error, 0);
  
  message = dbus_message_new_method_call (DBUS_SERVICE_ORG_FREEDESKTOP_DBUS,
                                          DBUS_PATH_ORG_FREEDESKTOP_DBUS,
                                          DBUS_INTERFACE_ORG_FREEDESKTOP_DBUS,
                                          "AcquireService");

  if (message == NULL)
    {
      _DBUS_SET_OOM (error);
      return -1;
    }
 
  if (!dbus_message_append_args (message,
				 DBUS_TYPE_STRING, service_name,
				 DBUS_TYPE_UINT32, flags,
				 DBUS_TYPE_INVALID))
    {
      dbus_message_unref (message);
      _DBUS_SET_OOM (error);
      return -1;
    }
  
  reply = dbus_connection_send_with_reply_and_block (connection, message, -1,
                                                     error);
  
  dbus_message_unref (message);
  
  if (reply == NULL)
    {
      _DBUS_ASSERT_ERROR_IS_SET (error);
      return -1;
    }  

  if (dbus_set_error_from_message (error, reply))
    {
      _DBUS_ASSERT_ERROR_IS_SET (error);
      dbus_message_unref (reply);
      return -1;
    }
  
  if (!dbus_message_get_args (reply, error,
                              DBUS_TYPE_UINT32, &service_result,
                              DBUS_TYPE_INVALID))
    {
      _DBUS_ASSERT_ERROR_IS_SET (error);
      dbus_message_unref (reply);
      return -1;
    }

  dbus_message_unref (reply);
  
  return service_result;
}

/**
 * Checks whether a certain service exists.
 *
 * @param connection the connection
 * @param service_name the service name
 * @param error location to store any errors
 * @returns #TRUE if the service exists, #FALSE if not or on error
 */
dbus_bool_t
dbus_bus_service_exists (DBusConnection *connection,
			 const char     *service_name,
                         DBusError      *error)
{
  DBusMessage *message, *reply;
  dbus_bool_t exists;

  _dbus_return_val_if_fail (connection != NULL, FALSE);
  _dbus_return_val_if_fail (service_name != NULL, FALSE);
  _dbus_return_val_if_error_is_set (error, FALSE);
  
  message = dbus_message_new_method_call (DBUS_SERVICE_ORG_FREEDESKTOP_DBUS,
                                          DBUS_PATH_ORG_FREEDESKTOP_DBUS,
                                          DBUS_INTERFACE_ORG_FREEDESKTOP_DBUS,
                                          "ServiceExists");
  if (message == NULL)
    {
      _DBUS_SET_OOM (error);
      return FALSE;
    }
  
  if (!dbus_message_append_args (message,
				 DBUS_TYPE_STRING, service_name,
				 DBUS_TYPE_INVALID))
    {
      dbus_message_unref (message);
      _DBUS_SET_OOM (error);
      return FALSE;
    }
  
  reply = dbus_connection_send_with_reply_and_block (connection, message, -1, error);
  dbus_message_unref (message);

  if (reply == NULL)
    {
      _DBUS_ASSERT_ERROR_IS_SET (error);
      return FALSE;
    }

  if (!dbus_message_get_args (reply, error,
                              DBUS_TYPE_BOOLEAN, &exists,
                              DBUS_TYPE_INVALID))
    {
      _DBUS_ASSERT_ERROR_IS_SET (error);
      dbus_message_unref (reply);
      return FALSE;
    }
  
  dbus_message_unref (reply);
  return exists;
}

/**
 * Activates a given service
 *
 * @param connection the connection
 * @param service_name the service name
 * @param flags the flags
 * @param result a place to store the result of the activation, which will
 * be one of DBUS_ACTIVATION_REPLY_ACTIVATED or
 * DBUS_ACTIVATION_REPLY_ALREADY_ACTIVE if successful.  Pass NULL if you
 * don't care about the result.
 * @param error location to store any errors
 * @returns #TRUE if the activation succeeded, #FALSE if not
 *
 * @todo document what the flags do
 */
dbus_bool_t
dbus_bus_activate_service (DBusConnection *connection,
			   const char     *service_name,
			   dbus_uint32_t   flags,
			   dbus_uint32_t  *result,
			   DBusError      *error)
{
  DBusMessage *msg;
  DBusMessage *reply;

  msg = dbus_message_new_method_call (DBUS_SERVICE_ORG_FREEDESKTOP_DBUS,
                                      DBUS_PATH_ORG_FREEDESKTOP_DBUS,
                                      DBUS_INTERFACE_ORG_FREEDESKTOP_DBUS,
                                      "ActivateService");

  if (!dbus_message_append_args (msg, DBUS_TYPE_STRING, service_name,
			  	 DBUS_TYPE_UINT32, flags, DBUS_TYPE_INVALID))
    {
      dbus_message_unref (msg);
      _DBUS_SET_OOM (error);
      return FALSE;
    }

  reply = dbus_connection_send_with_reply_and_block (connection, msg,
                                                     -1, error);
  dbus_message_unref (msg);

  if (reply == NULL)
    {
      _DBUS_ASSERT_ERROR_IS_SET (error);
      return FALSE;
    }

  if (dbus_set_error_from_message (error, reply))
    {
      _DBUS_ASSERT_ERROR_IS_SET (error);
      dbus_message_unref (reply);
      return FALSE;
    }

  if (result != NULL &&
      !dbus_message_get_args (reply, error, DBUS_TYPE_UINT32,
	      		      result, DBUS_TYPE_INVALID))
    {
      _DBUS_ASSERT_ERROR_IS_SET (error);
      dbus_message_unref (reply);
      return FALSE;
    }
  
  dbus_message_unref (reply);
  return TRUE;
}

static void
send_no_return_values (DBusConnection *connection,
                       DBusMessage    *msg,
                       DBusError      *error)
{
  if (error)
    {
      /* Block to check success codepath */
      DBusMessage *reply;
      
      reply = dbus_connection_send_with_reply_and_block (connection, msg,
                                                         -1, error);
      
      if (reply == NULL)
        {
          _DBUS_ASSERT_ERROR_IS_SET (error);
          return;
        }

      if (dbus_set_error_from_message (error, reply))
        {
          _DBUS_ASSERT_ERROR_IS_SET (error);
          dbus_message_unref (reply);
          return;
        }

      dbus_message_unref (reply);
    }
  else
    {
      /* Silently-fail nonblocking codepath */
      if (!dbus_connection_send (connection, msg, NULL))
        return;
    }
}

/**
 * Adds a match rule to match messages going through the message bus.
 * The "rule" argument is the string form of a match rule.
 *
 * If you pass #NULL for the error, this function will not
 * block; the match thus won't be added until you flush the
 * connection, and if there's an error adding the match
 * (only possible error is lack of resources in the bus),
 * you won't find out about it.
 *
 * If you pass non-#NULL for the error this function will
 * block until it gets a reply.
 *
 * Normal API conventions would have the function return
 * a boolean value indicating whether the error was set,
 * but that would require blocking always to determine
 * the return value.
 * 
 * @param connection connection to the message bus
 * @param rule textual form of match rule
 * @param error location to store any errors
 */
void
dbus_bus_add_match (DBusConnection *connection,
                    const char     *rule,
                    DBusError      *error)
{
  DBusMessage *msg;

  msg = dbus_message_new_method_call (DBUS_SERVICE_ORG_FREEDESKTOP_DBUS,
                                      DBUS_PATH_ORG_FREEDESKTOP_DBUS,
                                      DBUS_INTERFACE_ORG_FREEDESKTOP_DBUS,
                                      "AddMatch");

  if (!dbus_message_append_args (msg, DBUS_TYPE_STRING, rule,
                                 DBUS_TYPE_INVALID))
    {
      dbus_message_unref (msg);
      _DBUS_SET_OOM (error);
      return;
    }

  send_no_return_values (connection, msg, error);

  dbus_message_unref (msg);
}

/**
 * Removes a previously-added match rule "by value" (the most
 * recently-added identical rule gets removed).  The "rule" argument
 * is the string form of a match rule.
 *
 * If you pass #NULL for the error, this function will not
 * block; otherwise it will. See detailed explanation in
 * docs for dbus_bus_add_match().
 * 
 * @param connection connection to the message bus
 * @param rule textual form of match rule
 * @param error location to store any errors
 */
void
dbus_bus_remove_match (DBusConnection *connection,
                       const char     *rule,
                       DBusError      *error)
{
  DBusMessage *msg;

  msg = dbus_message_new_method_call (DBUS_SERVICE_ORG_FREEDESKTOP_DBUS,
                                      DBUS_PATH_ORG_FREEDESKTOP_DBUS,
                                      DBUS_INTERFACE_ORG_FREEDESKTOP_DBUS,
                                      "RemoveMatch");

  if (!dbus_message_append_args (msg, DBUS_TYPE_STRING, rule,
                                 DBUS_TYPE_INVALID))
    {
      dbus_message_unref (msg);
      _DBUS_SET_OOM (error);
      return;
    }

  send_no_return_values (connection, msg, error);

  dbus_message_unref (msg);
}

/** @} */
