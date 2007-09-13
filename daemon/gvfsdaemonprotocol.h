#ifndef __G_VFS_DAEMON_PROTOCOL_H__
#define __G_VFS_DAEMON_PROTOCOL_H__

#include <gvfs/gfileinfo.h>

G_BEGIN_DECLS

#define G_VFS_DBUS_DAEMON_NAME "org.gtk.vfs.Daemon"

#define G_VFS_DBUS_MOUNTPOINT_INTERFACE "org.gtk.vfs.Mountpoint"
#define G_VFS_DBUS_ANNOUNCE_MOUNTPOINT "AnnounceMountpoint"
#define G_VFS_DBUS_OP_OPEN_FOR_READ "OpenForRead"
#define G_VFS_DBUS_OP_GET_INFO "GetInfo"

#define G_VFS_DBUS_MOUNTPOINT_TRACKER_INTERFACE "org.gtk.vfs.MountpointTracker"
#define G_VFS_DBUS_MOUNTPOINT_TRACKER_PATH "/org/gtk/vfs/MountpointTracker"
#define G_VFS_DBUS_LIST_MOUNT_POINTS "ListMountpoints"

#define G_VFS_DBUS_MOUNTPOINT_NAME "org.gtk.vfs.mount."
#define G_VFS_DBUS_MOUNTPOINT_PATH "/org/gtk/vfs/mount/"
#define G_VFS_DBUS_ERROR_SOCKET_FAILED "org.gtk.vfs.Error.SocketFailed"

#define G_VFS_DBUS_DAEMON_INTERFACE "org.gtk.vfs.Daemon"
#define G_VFS_DBUS_DAEMON_PATH "/org/gtk/vfs/Daemon"
#define G_VFS_DBUS_OP_GET_CONNECTION "GetConnection"
#define G_VFS_DBUS_OP_CANCEL "Cancel"


typedef struct {
  guint32 command;
  guint32 seq_nr;
  guint32 arg1;
  guint32 arg2;
} GVfsDaemonSocketProtocolRequest;

#define G_VFS_DAEMON_SOCKET_PROTOCOL_REQUEST_SIZE 16

#define G_VFS_DAEMON_SOCKET_PROTOCOL_REQUEST_READ 0
#define G_VFS_DAEMON_SOCKET_PROTOCOL_REQUEST_CLOSE 1
#define G_VFS_DAEMON_SOCKET_PROTOCOL_REQUEST_CANCEL 2
#define G_VFS_DAEMON_SOCKET_PROTOCOL_REQUEST_SEEK_CUR 3
#define G_VFS_DAEMON_SOCKET_PROTOCOL_REQUEST_SEEK_SET 4
#define G_VFS_DAEMON_SOCKET_PROTOCOL_REQUEST_SEEK_END 5

/*
read, readahead reply:
type, seek_generation, size, data

seek reply:
type, pos (64),

error:
type, code, size, data (size bytes, 2 strings: domain, message)
*/

typedef struct {
  guint32 type;
  guint32 seq_nr;
  guint32 arg1;
  guint32 arg2;
} GVfsDaemonSocketProtocolReply;

#define G_VFS_DAEMON_SOCKET_PROTOCOL_REPLY_SIZE 16

#define G_VFS_DAEMON_SOCKET_PROTOCOL_REPLY_DATA     0
#define G_VFS_DAEMON_SOCKET_PROTOCOL_REPLY_ERROR    1
#define G_VFS_DAEMON_SOCKET_PROTOCOL_REPLY_SEEK_POS 2
#define G_VFS_DAEMON_SOCKET_PROTOCOL_REPLY_CLOSED   3

char *g_dbus_get_file_info_signature (GFileInfoRequestFlags  requested);
void  g_dbus_append_file_info        (DBusMessageIter       *iter,
				      GFileInfoRequestFlags  requested,
				      GFileInfo             *file_info);

G_END_DECLS

#endif /* __G_VFS_DAEMON_PROTOCOL_H__ */
