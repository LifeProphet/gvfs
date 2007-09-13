#include <config.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <stdlib.h>

#include <glib.h>
#include <glib/gi18n.h>
#include <glib-object.h>
#include <dbus-gmain.h>
#include <gvfsdaemon.h>
#include <gvfsdaemonprotocol.h>
#include <gvfsdaemonutils.h>
#include <gvfsjobopenforread.h>
#include <gdbusutils.h>

G_DEFINE_TYPE (GVfsDaemon, g_vfs_daemon, G_TYPE_OBJECT);

enum {
  PROP_0,
};

typedef struct {
  char *obj_path;
  DBusObjectPathMessageFunction callback;
  gpointer data;
} RegisteredPath;

struct _GVfsDaemonPrivate
{
  GMutex *lock;
  gboolean main_daemon;

  GThreadPool *thread_pool;
  DBusConnection *session_bus;
  GHashTable *registered_paths;
  GList *jobs;
  GList *job_sources;

  gint mount_counter;
};

typedef struct {
  GVfsDaemon *daemon;
  char *socket_dir;
  guint io_watch;
  DBusServer *server;
  
  gboolean got_dbus_connection;
  gboolean got_fd_connection;
  int fd;
  DBusConnection *conn;
} NewConnectionData;

static void g_vfs_daemon_get_property (GObject    *object,
				       guint       prop_id,
				       GValue     *value,
				       GParamSpec *pspec);
static void g_vfs_daemon_set_property (GObject         *object,
				       guint            prop_id,
				       const GValue    *value,
				       GParamSpec      *pspec);
static DBusHandlerResult daemon_message_func      (DBusConnection *conn,
						   DBusMessage    *message,
						   gpointer        data);
static DBusHandlerResult peer_to_peer_filter_func (DBusConnection *conn,
						   DBusMessage    *message,
						   gpointer        data);

static void
registered_path_free (RegisteredPath *data)
{
  g_free (data->obj_path);
  g_free (data);
}

static void
g_vfs_daemon_finalize (GObject *object)
{
  GVfsDaemon *daemon;

  daemon = G_VFS_DAEMON (object);

  g_assert (daemon->priv->jobs == NULL);

  g_hash_table_destroy (daemon->priv->registered_paths);
  g_mutex_free (daemon->priv->lock);

  if (G_OBJECT_CLASS (g_vfs_daemon_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_vfs_daemon_parent_class)->finalize) (object);
}

static void
g_vfs_daemon_class_init (GVfsDaemonClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  
  g_type_class_add_private (klass, sizeof (GVfsDaemonPrivate));
  
  gobject_class->finalize = g_vfs_daemon_finalize;
  gobject_class->set_property = g_vfs_daemon_set_property;
  gobject_class->get_property = g_vfs_daemon_get_property;
}

static void
job_handler_callback (gpointer       data,
		      gpointer       user_data)
{
  GVfsJob *job = G_VFS_JOB (data);

  g_vfs_job_run (job);
}

static void
g_vfs_daemon_init (GVfsDaemon *daemon)
{
  gint max_threads = 1; /* TODO: handle max threads */
  
  daemon->priv = G_TYPE_INSTANCE_GET_PRIVATE (daemon,
					      G_TYPE_VFS_DAEMON,
					      GVfsDaemonPrivate);
  daemon->priv->lock = g_mutex_new ();
  daemon->priv->session_bus = dbus_bus_get (DBUS_BUS_SESSION, NULL);
  daemon->priv->thread_pool = g_thread_pool_new (job_handler_callback,
						 daemon,
						 max_threads,
						 FALSE, NULL);
  /* TODO: verify thread_pool != NULL in a nicer way */
  g_assert (daemon->priv->thread_pool != NULL);

  daemon->priv->mount_counter = 0;
  
  daemon->priv->jobs = NULL;
  daemon->priv->registered_paths =
    g_hash_table_new_full (g_str_hash, g_str_equal,
			   NULL, (GDestroyNotify)registered_path_free);

  if (!dbus_connection_add_filter (daemon->priv->session_bus,
				   daemon_message_func, daemon, NULL))
    _g_dbus_oom ();
}

static void
g_vfs_daemon_set_property (GObject         *object,
			   guint            prop_id,
			   const GValue    *value,
			   GParamSpec      *pspec)
{
  GVfsDaemon *daemon;
  
  daemon = G_VFS_DAEMON (object);
  
  switch (prop_id)
    {
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
g_vfs_daemon_get_property (GObject    *object,
			   guint       prop_id,
			   GValue     *value,
			   GParamSpec *pspec)
{
  GVfsDaemon *daemon;
  
  daemon = G_VFS_DAEMON (object);

  switch (prop_id)
    {
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

GVfsDaemon *
g_vfs_daemon_new (gboolean main_daemon, gboolean replace)
{
  GVfsDaemon *daemon;
  DBusConnection *conn;
  DBusError error;
  unsigned int flags;
  int ret;

  g_print ("main_daemon: %d, replace %d\n", main_daemon, replace);
  
  dbus_error_init (&error);
  conn = dbus_bus_get (DBUS_BUS_SESSION, &error);
  if (!conn)
    {
      g_printerr ("Failed to connect to the D-BUS daemon: %s\n",
		  error.message);
      
      dbus_error_free (&error);
      return NULL;
    }

  dbus_connection_setup_with_g_main (conn, NULL);
  
  daemon = g_object_new (G_TYPE_VFS_DAEMON, NULL);
  daemon->priv->main_daemon = main_daemon;

  /* Request name only after we've installed the message filter */
  if (main_daemon)
    {
      flags = DBUS_NAME_FLAG_ALLOW_REPLACEMENT | DBUS_NAME_FLAG_DO_NOT_QUEUE;
      if (replace)
	flags |= DBUS_NAME_FLAG_REPLACE_EXISTING;

      g_print ("requesting name\n");
      ret = dbus_bus_request_name (conn, G_VFS_DBUS_DAEMON_NAME, flags, &error);
      if (ret == -1)
	{
	  g_printerr ("Failed to acquire daemon name: %s", error.message);
	  dbus_error_free (&error);
	  
	  g_object_unref (daemon);
	  daemon = NULL;
	}
      else if (ret == DBUS_REQUEST_NAME_REPLY_EXISTS)
	{
	  g_printerr ("VFS daemon already running, exiting.\n");
	  g_object_unref (daemon);
	  daemon = NULL;
	}
      else if (ret != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER)
	{
	  g_printerr ("Not primary owner of the service, exiting.\n");
	  g_object_unref (daemon);
	  daemon = NULL;
	}
    }

  dbus_connection_unref (conn);
  
  return daemon;
}

static void
job_source_new_job_callback (GVfsJobSource *job_source,
			     GVfsJob *job,
			     GVfsDaemon *daemon)
{
  g_vfs_daemon_queue_job (daemon, job);
}

static void
job_source_closed_callback (GVfsJobSource *job_source,
			    GVfsDaemon *daemon)
{
  g_mutex_lock (daemon->priv->lock);
  
  daemon->priv->job_sources = g_list_remove (daemon->priv->job_sources,
					     job_source);
  
  g_signal_handlers_disconnect_by_func (job_source,
					(GCallback)job_source_new_job_callback,
					daemon);
  g_signal_handlers_disconnect_by_func (job_source,
					(GCallback)job_source_closed_callback,
					daemon);
  
  g_object_unref (job_source);
  
  g_mutex_unlock (daemon->priv->lock);
}

void
g_vfs_daemon_add_job_source (GVfsDaemon *daemon,
			     GVfsJobSource *job_source)
{
  g_print ("Added new job source %p (%s)\n", job_source, g_type_name_from_instance ((gpointer)job_source));
  
  g_mutex_lock (daemon->priv->lock);

  g_object_ref (job_source);
  daemon->priv->job_sources = g_list_append (daemon->priv->job_sources,
					     job_source);
  g_signal_connect (job_source, "new_job",
		    (GCallback)job_source_new_job_callback, daemon);
  g_signal_connect (job_source, "closed",
		    (GCallback)job_source_closed_callback, daemon);
  
  g_mutex_unlock (daemon->priv->lock);
}

char *
g_vfs_daemon_register_mount (GVfsDaemon                    *daemon,
			     DBusObjectPathMessageFunction  callback,
			     gpointer                       user_data)
{
  RegisteredPath *data;
  int id;

  id = ++daemon->priv->mount_counter;

  data = g_new0 (RegisteredPath, 1);
  data->obj_path = g_strdup_printf ("/org/gtk/vfs/mount/%d", id);;
  data->callback = callback;
  data->data = user_data;

  g_hash_table_insert (daemon->priv->registered_paths, data->obj_path,
		       data);

  return g_strdup (data->obj_path);
}

/* NOTE: Might be emitted on a thread */
static void
job_new_source_callback (GVfsJob *job,
			 GVfsJobSource *job_source,
			 GVfsDaemon *daemon)
{
  g_vfs_daemon_add_job_source (daemon, job_source);
}

/* NOTE: Might be emitted on a thread */
static void
job_finished_callback (GVfsJob *job, 
		       GVfsDaemon *daemon)
{

  g_signal_handlers_disconnect_by_func (job,
					(GCallback)job_new_source_callback,
					daemon);
  g_signal_handlers_disconnect_by_func (job,
					(GCallback)job_finished_callback,
					daemon);

  g_mutex_lock (daemon->priv->lock);
  daemon->priv->jobs = g_list_remove (daemon->priv->jobs, job);
  g_mutex_unlock (daemon->priv->lock);
  
  g_object_unref (job);
}

void
g_vfs_daemon_queue_job (GVfsDaemon *daemon,
			GVfsJob *job)
{
  g_print ("Queued new job %p (%s)\n", job, g_type_name_from_instance ((gpointer)job));
  
  g_object_ref (job);
  g_signal_connect (job, "finished", (GCallback)job_finished_callback, daemon);
  g_signal_connect (job, "new_source", (GCallback)job_new_source_callback, daemon);
  
  g_mutex_lock (daemon->priv->lock);
  daemon->priv->jobs = g_list_prepend (daemon->priv->jobs, job);
  g_mutex_unlock (daemon->priv->lock);
  
  /* Can we start the job immediately / async */
  if (!g_vfs_job_try (job))
    {
      /* Couldn't finish / run async, queue worker thread */
      g_thread_pool_push (daemon->priv->thread_pool, job, NULL); /* TODO: Check error */
    }
}

static void
new_connection_data_free (void *memory)
{
  NewConnectionData *data = memory;
  
  /* Remove the socket and dir after connected */
  if (data->socket_dir) 
    rmdir (data->socket_dir);

  if (data->io_watch)
    g_source_remove (data->io_watch);

  g_free (data->socket_dir);
  g_free (data);
}

static void
daemon_peer_connection_setup (GVfsDaemon *daemon,
			      DBusConnection *dbus_conn,
			      NewConnectionData *data)
{
  /* We wait until we have the extra fd */
  if (!data->got_fd_connection)
    return;

  if (data->fd == -1)
    {
      /* The fd connection failed, abort the whole thing */
      g_warning (_("Failed to accept client: %s"), _("accept of extra fd failed"));
      dbus_connection_unref (dbus_conn);
      goto error_out;
    }
  
  dbus_connection_setup_with_g_main (dbus_conn, NULL);
  if (!dbus_connection_add_filter (dbus_conn, peer_to_peer_filter_func, daemon, NULL) ||
      !dbus_connection_add_filter (dbus_conn, daemon_message_func, daemon, NULL))
    {
      g_warning (_("Failed to accept client: %s"), _("object registration failed"));
      dbus_connection_unref (dbus_conn);
      close (data->fd);
      goto error_out;
    }
  
  dbus_connection_add_fd_send_fd (dbus_conn, data->fd);

 error_out:
  new_connection_data_free (data);
}

#ifdef __linux__
#define USE_ABSTRACT_SOCKETS
#endif

static void
randomize_string (char tmp[9])
{
  int i;
  const char chars[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";

  for (i = 0; i < 8; i++)
    tmp[i] = chars[g_random_int_range (0, strlen(chars))];
  
  tmp[8] = '\0';
}

#ifndef USE_ABSTRACT_SOCKETS
static gboolean
test_safe_socket_dir (const char *dirname)
{
  struct stat statbuf;
  
  if (g_stat (dirname, &statbuf) != 0)
    return FALSE;
	
#ifndef G_PLATFORM_WIN32
  if (statbuf.st_uid != getuid ())
    return FALSE;
  
  if ((statbuf.st_mode & (S_IRWXG|S_IRWXO)) ||
      !S_ISDIR (statbuf.st_mode))
    return FALSE;
#endif

  return TRUE;
}


static char *
create_socket_dir (void)
{
  char *dirname;
  long iteration = 0;
  char *safe_dir;
  gchar tmp[9];
  int i;
  
  safe_dir = NULL;
  do
    {
      g_free (safe_dir);

      randomize_string (tmp);
		
      dirname = g_strdup_printf ("gvfs-%s-%s",
				 g_get_user_name (), tmp);
      safe_dir = g_build_filename (g_get_tmp_dir (), dirname, NULL);
      g_free (dirname);
      
      if (g_mkdir (safe_dir, 0700) < 0)
	{
	  switch (errno)
	    {
	    case EACCES:
	      g_error ("I can't write to '%s', daemon init failed",
		       safe_dir);
	      break;
	      
	    case ENAMETOOLONG:
	      g_error ("Name '%s' too long your system is broken",
		       safe_dir);
	      break;
	      
	    case ENOMEM:
#ifdef ELOOP
	    case ELOOP:
#endif
	    case ENOSPC:
	    case ENOTDIR:
	    case ENOENT:
	      g_error ("Resource problem creating '%s'", safe_dir);
	      break;
	      
	    default: /* carry on going */
	      break;
	    }
	}
      /* Possible race - so we re-scan. */
      
      if (iteration++ == 1000) 
	g_error ("Cannot find a safe socket path in '%s'", g_get_tmp_dir ());
    }
  while (!test_safe_socket_dir (safe_dir));

  return safe_dir;
}
#endif

static void
generate_addresses (char **address1,
		    char **address2,
		    char **folder)
{
  *address1 = NULL;
  *address2 = NULL;
  *folder = NULL;

#ifdef USE_ABSTRACT_SOCKETS
  {
    gchar  tmp[9];

    randomize_string (tmp);
    *address1 = g_strdup_printf ("unix:abstract=/dbus-vfs-daemon/socket-%s", tmp);

    randomize_string (tmp);
    *address2 = g_strdup_printf ("unix:abstract=/dbus-vfs-daemon/socket-%s", tmp);
  }
#else
  {
    char *dir;
    
    dir = create_socket_dir ();
    *address1 = g_strdup_printf ("unix:path=%s/socket1", dir);
    *address2 = g_strdup_printf ("unix:path=%s/socket2", dir);
    *folder = dir;
  }
#endif
}

static void
daemon_new_connection_func (DBusServer     *server,
			    DBusConnection *conn,
			    gpointer        user_data)
{
  NewConnectionData *data;

  data = user_data;
  data->got_dbus_connection = TRUE;

  /* Take ownership */
  data->conn = dbus_connection_ref (conn);

  daemon_peer_connection_setup (data->daemon, conn, data);

  /* Kill the server, no more need for it */
  dbus_server_disconnect (server);
  dbus_server_unref (server);
}

static int
unix_socket_at (const char *address)
{
  int fd;
  const char *path;
  size_t path_len;
  struct sockaddr_un addr;

  fd = socket (PF_UNIX, SOCK_STREAM, 0);
  if (fd == -1)
    return -1;

#ifdef USE_ABSTRACT_SOCKETS  
  path = address + strlen ("unix:abstract=");
#else
  path = address + strlen ("unix:path=");
#endif
    
  memset (&addr, 0, sizeof (addr));
  addr.sun_family = AF_UNIX;
  path_len = strlen (path);

#ifdef USE_ABSTRACT_SOCKETS
  addr.sun_path[0] = '\0'; /* this is what says "use abstract" */
  path_len++; /* Account for the extra nul byte added to the start of sun_path */

  strncpy (&addr.sun_path[1], path, path_len);
#else /* USE_ABSTRACT_SOCKETS */
  strncpy (addr.sun_path, path, path_len);
  unlink (path);
#endif /* ! USE_ABSTRACT_SOCKETS */
  
  if (bind (fd, (struct sockaddr*) &addr, 
	    G_STRUCT_OFFSET (struct sockaddr_un, sun_path) + path_len) < 0)
    {
      close (fd);
      return -1;
    }

  if (listen (fd, 30 /* backlog */) < 0)
    {
      close (fd);
      return -1;
    }
  
  return fd;
}

static gboolean
accept_new_fd_client (GIOChannel  *channel,
		      GIOCondition cond,
		      gpointer     callback_data)
{
  NewConnectionData *data = callback_data;
  int fd;
  int new_fd;
  struct sockaddr_un addr;
  socklen_t addrlen;

  data->got_fd_connection = TRUE;
  
  fd = g_io_channel_unix_get_fd (channel);
  
  addrlen = sizeof (addr);
  new_fd = accept (fd, (struct sockaddr *) &addr, &addrlen);

  data->fd = new_fd;
  data->io_watch = 0;

  /* Did we already accept the dbus connection, if so, finish it now */
  if (data->got_dbus_connection)
    daemon_peer_connection_setup (data->daemon,
				  data->conn,
				  data);
  else if (data->fd == -1)
    {
      /* Didn't accept a dbus connection, and there is no need for one now */
      g_warning (_("Failed to accept client: %s"), _("accept of extra fd failed"));
      dbus_server_disconnect (data->server);
      dbus_server_unref (data->server);
      new_connection_data_free (data);
    }
  
  return FALSE;
}

static void
daemon_handle_get_connection (DBusConnection *conn,
			      DBusMessage *message,
			      GVfsDaemon *daemon)
{
  DBusServer    *server;
  DBusError      error;
  DBusMessage   *reply;
  gchar         *address1;
  gchar         *address2;
  NewConnectionData *data;
  GIOChannel *channel;
  char *socket_dir;
  int fd;
  
  generate_addresses (&address1, &address2, &socket_dir);

  data = g_new (NewConnectionData, 1);
  data->daemon = daemon;
  data->socket_dir = socket_dir;
  data->got_fd_connection = FALSE;
  data->got_dbus_connection = FALSE;
  data->fd = -1;
  data->conn = NULL;
  
  dbus_error_init (&error);
  server = dbus_server_listen (address1, &error);
  if (!server)
    {
      reply = dbus_message_new_error_printf (message,
					     G_VFS_DBUS_ERROR_SOCKET_FAILED,
					     "Failed to create new socket: %s", 
					     error.message);
      dbus_error_free (&error);
      if (reply)
	{
	  dbus_connection_send (conn, reply, NULL);
	  dbus_message_unref (reply);
	}
      
      goto error_out;
    }
  data->server = server;

  dbus_server_set_new_connection_function (server,
					   daemon_new_connection_func,
					   data, NULL);
  dbus_server_setup_with_g_main (server, NULL);
  
  fd = unix_socket_at (address2);
  if (fd == -1)
    goto error_out;

  channel = g_io_channel_unix_new (fd);
  g_io_channel_set_close_on_unref (channel, TRUE);
  data->io_watch = g_io_add_watch (channel, G_IO_IN | G_IO_HUP, accept_new_fd_client, data);
  g_io_channel_unref (channel);
    
  reply = dbus_message_new_method_return (message);
  if (reply == NULL)
    _g_dbus_oom ();

  if (!dbus_message_append_args (reply,
				 DBUS_TYPE_STRING, &address1,
				 DBUS_TYPE_STRING, &address2,
				 DBUS_TYPE_INVALID))
    _g_dbus_oom ();
  
  dbus_connection_send (conn, reply, NULL);
  
  dbus_message_unref (reply);

  g_free (address1);
  g_free (address2);

  return;

 error_out:
  g_free (data);
  g_free (address1);
  g_free (address2);
  if (socket_dir)
    {
      rmdir (socket_dir);
      g_free (socket_dir);
    }
}

static DBusHandlerResult
daemon_message_func (DBusConnection *conn,
		     DBusMessage    *message,
		     gpointer        data)
{
  GVfsDaemon *daemon = data;
  RegisteredPath *registered_path;
  const char *path;

  g_print ("daemon_message_func\n");
  
  if (dbus_message_is_signal (message, DBUS_INTERFACE_DBUS, "NameLost"))
    {
      char *name;
      if (dbus_message_get_args (message, NULL,
				 DBUS_TYPE_STRING, &name,
				 DBUS_TYPE_INVALID) &&
	  strcmp (name, G_VFS_DBUS_DAEMON_NAME) == 0)
	{
	  /* Someone else got the name (i.e. someone used --replace), exit */
	  if (daemon->priv->main_daemon)
	    exit (1);
	}
    }
  
  if (dbus_message_is_method_call (message,
				   G_VFS_DBUS_DAEMON_INTERFACE,
				   G_VFS_DBUS_OP_GET_CONNECTION))
    {
      daemon_handle_get_connection (conn, message, daemon);
      return DBUS_HANDLER_RESULT_HANDLED;
    }

  if (dbus_message_is_method_call (message,
				   G_VFS_DBUS_DAEMON_INTERFACE,
				   G_VFS_DBUS_OP_CANCEL))
    {
      GList *l;
      dbus_uint32_t serial;
      GVfsJob *job_to_cancel = NULL;
      
      g_print ("Got cancel dbus call\n");

      if (dbus_message_get_args (message, NULL, 
				 DBUS_TYPE_UINT32, &serial,
				 DBUS_TYPE_INVALID))
	{
	  g_mutex_lock (daemon->priv->lock);
	  for (l = daemon->priv->jobs; l != NULL; l = l->next)
	    {
	      GVfsJob *job = l->data;
	      
	      if (G_IS_VFS_JOB_DBUS (job) &&
		  g_vfs_job_dbus_is_serial (G_VFS_JOB_DBUS (job),
					    conn, serial))
		{
		  job_to_cancel = g_object_ref (job);
		  break;
		}
	    }
	  g_mutex_unlock (daemon->priv->lock);


	  if (job_to_cancel)
	    {
	      g_vfs_job_cancel (job_to_cancel);
	      g_object_unref (job_to_cancel);
	    }
	}
      
      return DBUS_HANDLER_RESULT_HANDLED;
    }

  path = dbus_message_get_path (message);
  registered_path = NULL;
  if (path != NULL)
    registered_path = g_hash_table_lookup (daemon->priv->registered_paths, path);
  
  if (registered_path)
    return registered_path->callback (conn, message, registered_path->data);
  else
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}


/* Only called for peer-to-peer connections */
static DBusHandlerResult
peer_to_peer_filter_func (DBusConnection *conn,
			  DBusMessage    *message,
			  gpointer        data)
{
  if (dbus_message_is_signal (message,
			      DBUS_INTERFACE_LOCAL,
			      "Disconnected"))
    {
      /* The peer-to-peer connection was disconnected */
      dbus_connection_unref (conn);
      return DBUS_HANDLER_RESULT_HANDLED;
    }
  
  return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}
