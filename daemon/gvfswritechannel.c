#include <config.h>

#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>

#include <glib.h>
#include <glib-object.h>
#include <glib/gi18n.h>
#include <dbus-gmain.h>
#include <gvfswritechannel.h>
#include <gio/ginputstreamsocket.h>
#include <gio/goutputstreamsocket.h>
#include <gvfsdaemonprotocol.h>
#include <gvfsdaemonutils.h>
#include <gvfsjobwrite.h>
#include <gvfsjobseekwrite.h>
#include <gvfsjobclosewrite.h>

struct _GVfsWriteChannel
{
  GVfsChannel parent_instance;
};

G_DEFINE_TYPE (GVfsWriteChannel, g_vfs_write_channel, G_VFS_TYPE_CHANNEL)

static GVfsJob *write_channel_close          (GVfsChannel  *channel);
static GVfsJob *write_channel_handle_request (GVfsChannel  *channel,
					      guint32       command,
					      guint32       seq_nr,
					      guint32       arg1,
					      guint32       arg2,
					      gpointer      data,
					      gsize         data_len,
					      GError      **error);
  
static void
g_vfs_write_channel_finalize (GObject *object)
{
  GVfsWriteChannel *write_channel;

  write_channel = G_VFS_WRITE_CHANNEL (object);
  
  if (G_OBJECT_CLASS (g_vfs_write_channel_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_vfs_write_channel_parent_class)->finalize) (object);
}

static void
g_vfs_write_channel_class_init (GVfsWriteChannelClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GVfsChannelClass *channel_class = G_VFS_CHANNEL_CLASS (klass);

  gobject_class->finalize = g_vfs_write_channel_finalize;
  channel_class->close = write_channel_close;
  channel_class->handle_request = write_channel_handle_request;
}

static void
g_vfs_write_channel_init (GVfsWriteChannel *channel)
{
}

static GVfsJob *
write_channel_close (GVfsChannel *channel)
{
  return g_vfs_job_close_write_new (G_VFS_WRITE_CHANNEL (channel),
				    g_vfs_channel_get_backend_handle (channel),
				    g_vfs_channel_get_backend (channel));
} 

static GVfsJob *
write_channel_handle_request (GVfsChannel *channel,
			      guint32 command,
			      guint32 seq_nr,
			      guint32 arg1,
			      guint32 arg2,
			      gpointer data,
			      gsize data_len,
			      GError **error)
{
  GVfsJob *job;
  GSeekType seek_type;
  GVfsBackendHandle backend_handle;
  GVfsBackend *backend;
  GVfsWriteChannel *write_channel;

  write_channel = G_VFS_WRITE_CHANNEL (channel);
  backend_handle = g_vfs_channel_get_backend_handle (channel);
  backend = g_vfs_channel_get_backend (channel);
  
  job = NULL;
  switch (command)
    {
    case G_VFS_DAEMON_SOCKET_PROTOCOL_REQUEST_WRITE:
      job = g_vfs_job_write_new (write_channel,
				 backend_handle,
				 data, data_len,
				 backend);
      data = NULL; /* Pass ownership */
      break;
    case G_VFS_DAEMON_SOCKET_PROTOCOL_REQUEST_CLOSE:
      job = g_vfs_job_close_write_new (write_channel,
				       backend_handle,
				       backend);
      break;
    case G_VFS_DAEMON_SOCKET_PROTOCOL_REQUEST_SEEK_CUR:
    case G_VFS_DAEMON_SOCKET_PROTOCOL_REQUEST_SEEK_END:
    case G_VFS_DAEMON_SOCKET_PROTOCOL_REQUEST_SEEK_SET:
      seek_type = G_SEEK_SET;
      if (command == G_VFS_DAEMON_SOCKET_PROTOCOL_REQUEST_SEEK_END)
	seek_type = G_SEEK_END;
      else if (command == G_VFS_DAEMON_SOCKET_PROTOCOL_REQUEST_SEEK_CUR)
	seek_type = G_SEEK_CUR;
      
      job = g_vfs_job_seek_write_new (write_channel,
				      backend_handle,
				      seek_type,
				      ((goffset)arg1) | (((goffset)arg2) << 32),
				      backend);
      break;
      
    default:
      g_set_error (error, G_VFS_ERROR,
		   G_VFS_ERROR_INTERNAL_ERROR,
		   "Unknown stream command %d\n", command);
      break;
    }

  /* Ownership was passed */
  g_free (data);
  return job;
}

/* Might be called on an i/o thread
 */
void
g_vfs_write_channel_send_seek_offset (GVfsWriteChannel *write_channel,
				     goffset offset)
{
  GVfsDaemonSocketProtocolReply reply;
  GVfsChannel *channel;

  channel = G_VFS_CHANNEL (write_channel);
  
  reply.type = g_htonl (G_VFS_DAEMON_SOCKET_PROTOCOL_REPLY_SEEK_POS);
  reply.seq_nr = g_htonl (g_vfs_channel_get_current_seq_nr (channel));
  reply.arg1 = g_htonl (offset & 0xffffffff);
  reply.arg2 = g_htonl (offset >> 32);

  g_vfs_channel_send_reply (channel, &reply, NULL, 0);
}

/* Might be called on an i/o thread
 */
void
g_vfs_write_channel_send_closed (GVfsWriteChannel *write_channel)
{
  GVfsDaemonSocketProtocolReply reply;
  GVfsChannel *channel;

  channel = G_VFS_CHANNEL (write_channel);
  
  reply.type = g_htonl (G_VFS_DAEMON_SOCKET_PROTOCOL_REPLY_CLOSED);
  reply.seq_nr = g_htonl (g_vfs_channel_get_current_seq_nr (channel));
  reply.arg1 = g_htonl (0);
  reply.arg2 = g_htonl (0);

  g_vfs_channel_send_reply (channel, &reply, NULL, 0);
}

/* Might be called on an i/o thread
 */
void
g_vfs_write_channel_send_written (GVfsWriteChannel  *write_channel,
				  gsize bytes_written)
{
  GVfsDaemonSocketProtocolReply reply;
  GVfsChannel *channel;

  channel = G_VFS_CHANNEL (write_channel);

  reply.type = g_htonl (G_VFS_DAEMON_SOCKET_PROTOCOL_REPLY_WRITTEN);
  reply.seq_nr = g_htonl (g_vfs_channel_get_current_seq_nr (channel));
  reply.arg1 = g_htonl (bytes_written);
  reply.arg2 = 0;

  g_vfs_channel_send_reply (channel, &reply, NULL, 0);
}


GVfsWriteChannel *
g_vfs_write_channel_new (GVfsBackend *backend)
{
  return g_object_new (G_VFS_TYPE_WRITE_CHANNEL,
		       "backend", backend,
		       NULL);
}
