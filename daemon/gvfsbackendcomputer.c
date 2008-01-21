/* -*- Mode: C; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* GIO - GLib Input, Output and Streaming Library
 * 
 * Copyright (C) 2006-2007 Red Hat, Inc.
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
 * You should have received a copy of the GNU Lesser General
 * Public License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place, Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 * Author: Alexander Larsson <alexl@redhat.com>
 */


#include <config.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>

#include <glib/gstdio.h>
#include <glib/gi18n.h>
#include <gio/gio.h>
#include <gio/gunixmounts.h>
#include <glib/gurifuncs.h>

#include "gvfsbackendcomputer.h"
#include "gvfsmonitor.h"
#include "gvfsjobopenforread.h"
#include "gvfsjobread.h"
#include "gvfsjobseekread.h"
#include "gvfsjobopenforwrite.h"
#include "gvfsjobwrite.h"
#include "gvfsjobclosewrite.h"
#include "gvfsjobseekwrite.h"
#include "gvfsjobsetdisplayname.h"
#include "gvfsjobmountmountable.h"
#include "gvfsjobqueryinfo.h"
#include "gvfsjobdelete.h"
#include "gvfsjobqueryfsinfo.h"
#include "gvfsjobqueryattributes.h"
#include "gvfsjobenumerate.h"
#include "gvfsjobcreatemonitor.h"
#include "gvfsdaemonprotocol.h"

typedef struct {
  char *filename;
  char *display_name;
  GIcon *icon;
  GFile *root;
  int prio;
  gboolean can_mount;
  gboolean can_unmount;
  gboolean can_eject;
  
  GDrive *drive;
  GVolume *volume;
  GMount *mount;
} ComputerFile;

static ComputerFile root = { "/" };

struct _GVfsBackendComputer
{
  GVfsBackend parent_instance;

  GVolumeMonitor *volume_monitor;

  GVfsMonitor *root_monitor;
  
  GList *files;
  
  guint recompute_idle_tag;
  
  GMountSpec *mount_spec;
};

G_DEFINE_TYPE (GVfsBackendComputer, g_vfs_backend_computer, G_VFS_TYPE_BACKEND);

static void
computer_file_free (ComputerFile *file)
{
  g_free (file->filename);
  g_free (file->display_name);
  if (file->icon)
    g_object_unref (file->icon);
  if (file->root)
    g_object_unref (file->root);
  
  if (file->drive)
    g_object_unref (file->drive);
  if (file->volume)
    g_object_unref (file->volume);
  if (file->mount)
    g_object_unref (file->mount);
  
  g_slice_free (ComputerFile, file);
}

/* Assumes filename equal */
static gboolean
computer_file_equal (ComputerFile *a,
                     ComputerFile *b)
{
  if (strcmp (a->display_name, b->display_name) != 0)
    return FALSE;

  if (!g_icon_equal (a->icon, b->icon))
    return FALSE;
      
  if ((a->root != NULL && b->root != NULL &&
       !g_file_equal (a->root, b->root)) ||
      (a->root != NULL && b->root == NULL) ||
      (a->root == NULL && b->root != NULL))
    return FALSE;

  if (a->prio != b->prio)
    return FALSE;

  if (a->can_mount != b->can_mount ||
      a->can_unmount != b->can_unmount ||
      a->can_eject != b->can_eject)
    return FALSE;

  return TRUE;
}

static void object_changed (GVolumeMonitor *monitor,
                            gpointer object,
                            GVfsBackendComputer *backend);

static void
g_vfs_backend_computer_finalize (GObject *object)
{
  GVfsBackendComputer *backend;

  backend = G_VFS_BACKEND_COMPUTER (object);

  if (backend->volume_monitor)
    {
      g_signal_handlers_disconnect_by_func(backend->volume_monitor, object_changed, backend);
      g_object_unref (backend->volume_monitor);
    }
  
  g_mount_spec_unref (backend->mount_spec);

  if (backend->recompute_idle_tag)
    {
      g_source_remove (backend->recompute_idle_tag);
      backend->recompute_idle_tag = 0;
    }

  g_object_unref (backend->root_monitor);
  
  if (G_OBJECT_CLASS (g_vfs_backend_computer_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_vfs_backend_computer_parent_class)->finalize) (object);
}

static void
g_vfs_backend_computer_init (GVfsBackendComputer *computer_backend)
{
  GVfsBackend *backend = G_VFS_BACKEND (computer_backend);
  GMountSpec *mount_spec;
  
  g_vfs_backend_set_display_name (backend, _("Computer"));
  g_vfs_backend_set_icon_name (backend, "gnome-fs-client");
  g_vfs_backend_set_user_visible (backend, FALSE);

  mount_spec = g_mount_spec_new ("computer");
  g_vfs_backend_set_mount_spec (backend, mount_spec);
  computer_backend->mount_spec = mount_spec;
}

static gboolean
filename_is_used (GList *files, const char *filename)
{
  ComputerFile *file;

  while (files != NULL)
    {
      file = files->data;
      
      if (file->filename == NULL)
        return FALSE;
      
      if (strcmp (file->filename, filename) == 0)
        return TRUE;

      files = files->next;
    }
  return FALSE;
}

static int
sort_file_by_filename (ComputerFile *a, ComputerFile *b)
{
  return strcmp (a->filename, b->filename);
}

static void
convert_slashes (char *str)
{
  char *s;

  while ((s = strchr (str, '/')) != NULL)
    *s = '\\';
}

static void
update_from_files (GVfsBackendComputer *backend,
                   GList *files)
{
  GList *old_files;
  GList *oldl, *newl;
  char *filename;
  ComputerFile *old, *new;
  int cmp;

  old_files = backend->files;
  backend->files = files;
  
  /* Generate change events */
  oldl = old_files;
  newl = files;
  while (oldl != NULL || newl != NULL)
    {
      if (oldl == NULL)
        {
          cmp = 1;
          new = newl->data;
          old = NULL;
        }
      else if (newl == NULL)
        {
          cmp = -1;
          new = NULL;
          old = oldl->data;
        }
      else
        {
          new = newl->data;
          old = oldl->data;
          cmp = strcmp (old->filename, new->filename);
        }
      
      if (cmp == 0)
        {
          if (!computer_file_equal (old, new))
            {
              filename = g_strconcat ("/", new->filename, NULL);
              g_vfs_monitor_emit_event (backend->root_monitor,
                                        G_FILE_MONITOR_EVENT_CHANGED,
                                        filename,
                                        NULL);
              g_free (filename);
            }
          
          oldl = oldl->next;
          newl = newl->next;
        }
      else if (cmp < 0)
        {
          filename = g_strconcat ("/", old->filename, NULL);
          g_vfs_monitor_emit_event (backend->root_monitor,
                                    G_FILE_MONITOR_EVENT_DELETED,
                                    filename,
                                    NULL);
          g_free (filename);
          oldl = oldl->next;
        }
      else
        {
          filename = g_strconcat ("/", new->filename, NULL);
          g_vfs_monitor_emit_event (backend->root_monitor,
                                    G_FILE_MONITOR_EVENT_CREATED,
                                    filename,
                                    NULL);
          g_free (filename);
          newl = newl->next;
        }
    }
  
  g_list_foreach (old_files, (GFunc)computer_file_free, NULL);
}

static void
recompute_files (GVfsBackendComputer *backend)
{
  GVolumeMonitor *volume_monitor;
  GList *drives, *volumes, *mounts, *l, *ll;
  GDrive *drive;
  GVolume *volume;
  GMount *mount;
  ComputerFile *file;
  GList *files;
  char *basename, *filename;
  const char *extension;
  int uniq;

  volume_monitor = backend->volume_monitor;

  files = NULL;
  
	/* first go through all connected drives */
	drives = g_volume_monitor_get_connected_drives (volume_monitor);
	for (l = drives; l != NULL; l = l->next)
    {
      drive = l->data;

      volumes = g_drive_get_volumes (drive);
      if (volumes != NULL)
        {
          for (ll = volumes; ll != NULL; ll = ll->next)
            {
              volume = ll->data;

              file = g_slice_new0 (ComputerFile);
              file->drive = g_object_ref (drive);
              file->volume = volume; /* Takes ref */
              file->mount = g_volume_get_mount (volume);
              file->prio = -3;
              files = g_list_prepend (files, file);
            }
        }
      else
        {
          /* No volume, single drive */
          
          file = g_slice_new0 (ComputerFile);
          file->drive = g_object_ref (drive);
          file->volume = NULL;
          file->mount = NULL;
          file->prio = -3;
          
          files = g_list_prepend (files, file);
        }
      
      g_object_unref (drive);
    }
	g_list_free (drives);
  
	/* add all volumes that is not associated with a drive */
	volumes = g_volume_monitor_get_volumes (volume_monitor);
	for (l = volumes; l != NULL; l = l->next)
    {
      volume = l->data;
      drive = g_volume_get_drive (volume);
      if (drive == NULL)
        {
          file = g_slice_new0 (ComputerFile);
          file->drive = NULL;
          file->volume = g_object_ref (volume);
          file->mount = g_volume_get_mount (volume);
          file->prio = -2;

          files = g_list_prepend (files, file);
        }
      else
        g_object_unref (drive);
      
      g_object_unref (volume);
    }
	g_list_free (volumes);

	/* add mounts that has no volume (/etc/mtab mounts, ftp, sftp,...) */
	mounts = g_volume_monitor_get_mounts (volume_monitor);
	for (l = mounts; l != NULL; l = l->next)
    {
      mount = l->data;
      volume = g_mount_get_volume (mount);
      if (volume == NULL)
        {
          file = g_slice_new0 (ComputerFile);
          file->drive = NULL;
          file->volume = NULL;
          file->mount = g_object_ref (mount);
          file->prio = -1;

          files = g_list_prepend (files, file);
        }
      else
        g_object_unref (volume);
      
      g_object_unref (mount);
    }
	g_list_free (mounts);

  files = g_list_reverse (files);
  
  for (l = files; l != NULL; l = l->next)
    {
      file = l->data;

      if (file->mount)
        {
          file->icon = g_mount_get_icon (file->mount);
          file->display_name = g_mount_get_name (file->mount);
          file->root = g_mount_get_root (file->mount);
          file->can_unmount = g_mount_can_unmount (file->mount);
          file->can_eject = g_mount_can_eject (file->mount);
        }
      else if (file->volume)
        {
          file->icon = g_volume_get_icon (file->volume);
          file->display_name = g_volume_get_name (file->volume);
          file->can_mount = g_volume_can_mount (file->volume);
          file->root = NULL;
          file->can_eject = g_volume_can_eject (file->volume);
        }
      else /* drive */
        {
          file->icon = g_drive_get_icon (file->drive);
          file->display_name = g_drive_get_name (file->drive);
          file->can_eject = g_drive_can_eject (file->drive);
        }

      if (file->drive)
        {
          basename = g_drive_get_name (file->drive);
          extension = ".drive";
        }
      else if (file->volume)
        {
          basename = g_volume_get_name (file->volume);
          extension = ".volume";
        }
      else /* mount */
        {
          basename = g_mount_get_name (file->mount);
          extension = ".mount";
        }

      convert_slashes (basename); /* No slashes in filenames */
      uniq = 1;
      filename = g_strconcat (basename, extension, NULL);
      while (filename_is_used (files, filename))
        {
          g_free (filename);
          filename = g_strdup_printf ("%s-%d%s",
                                      basename,
                                      uniq++,
                                      extension);
        }
      
      g_free (basename);
      file->filename = filename;
    }

  files = g_list_sort (files, (GCompareFunc)sort_file_by_filename);

  update_from_files (backend, files);
}

static gboolean
recompute_files_in_idle (GVfsBackendComputer *backend)
{
  backend->recompute_idle_tag = 0;

  recompute_files (backend);
  
  return FALSE;
}

static void
object_changed (GVolumeMonitor *monitor,
                gpointer object,
                GVfsBackendComputer *backend)
{
  if (backend->recompute_idle_tag == 0) 
    backend->recompute_idle_tag =
      g_idle_add ((GSourceFunc)recompute_files_in_idle,
                  backend);
}

static gboolean
try_mount (GVfsBackend *backend,
           GVfsJobMount *job,
           GMountSpec *mount_spec,
           GMountSource *mount_source,
           gboolean is_automount)
{
  GVfsBackendComputer *computer_backend = G_VFS_BACKEND_COMPUTER (backend);
  int i;
  char *signals[] = {
    "volume-added",
    "volume-removed",
    "volume-changed",
    "mount-added",
    "mount-removed",
    "mount-changed",
    "drive-connected",
    "drive-disconnected",
    "drive-changed",
    NULL
  };

  computer_backend->volume_monitor = g_volume_monitor_get ();

  /* TODO: connect all signals to object_changed */

  for (i = 0; signals[i] != NULL; i++)
    g_signal_connect_data (computer_backend->volume_monitor,
                           signals[i],
                           (GCallback)object_changed,
                           backend,
                           NULL, 0);

  computer_backend->root_monitor = g_vfs_monitor_new (backend);
  
  recompute_files (computer_backend);

  g_vfs_job_succeeded (G_VFS_JOB (job));

  return TRUE;
}

static ComputerFile *
lookup (GVfsBackendComputer *backend,
        GVfsJob *job,
        const char *filename)
{
  GList *l;
  ComputerFile *file;

  if (*filename != '/')
    goto out;

  while (*filename == '/')
    filename++;

  if (*filename == 0)
    return &root;
  
  if (strchr (filename, '/') != NULL)
    goto out;
  
  for (l = backend->files; l != NULL; l = l->next)
    {
      file = l->data;

      if (strcmp (file->filename, filename) == 0)
        return file;
    }

 out:
  g_vfs_job_failed (job, G_IO_ERROR,
                    G_IO_ERROR_NOT_FOUND,
                    _("File doesn't exist"));
  return NULL;
}


static gboolean
try_open_for_read (GVfsBackend *backend,
                   GVfsJobOpenForRead *job,
                   const char *filename)
{
  ComputerFile *file;

  file = lookup (G_VFS_BACKEND_COMPUTER (backend),
                 G_VFS_JOB (job), filename);

  if (file == &root)
    g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR,
                      G_IO_ERROR_IS_DIRECTORY,
                      _("Can't open directory"));
  else if (file != NULL)
    g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR,
                      G_IO_ERROR_NOT_SUPPORTED,
                      _("Can't open mountable file"));
  return TRUE;
}

static void
file_info_from_file (ComputerFile *file,
                     GFileInfo *info)
{
  char *uri;
  
  g_file_info_set_name (info, file->filename);
  g_file_info_set_display_name (info, file->display_name);

  if (file->icon)
    g_file_info_set_icon (info, file->icon);

  if (file->root)
    {
      uri = g_file_get_uri (file->root);

      g_file_info_set_attribute_string (info,
                                        G_FILE_ATTRIBUTE_STANDARD_TARGET_URI,
                                        uri);
      g_free (uri);
    }

  g_file_info_set_sort_order (info, file->prio);

  g_file_info_set_file_type (info, G_FILE_TYPE_MOUNTABLE);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_MOUNTABLE_CAN_MOUNT, file->can_mount);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_MOUNTABLE_CAN_UNMOUNT, file->can_unmount);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_MOUNTABLE_CAN_EJECT, file->can_eject);

  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE, FALSE);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_DELETE, FALSE);
  g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_TRASH, FALSE);
}

static gboolean
try_enumerate (GVfsBackend *backend,
               GVfsJobEnumerate *job,
               const char *filename,
               GFileAttributeMatcher *attribute_matcher,
               GFileQueryInfoFlags flags)
{
  ComputerFile *file;
  GList *l;
  GFileInfo *info;

  file = lookup (G_VFS_BACKEND_COMPUTER (backend),
                 G_VFS_JOB (job), filename);
  
  if (file != &root)
    {
      if (file != NULL)
        g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR,
                          G_IO_ERROR_NOT_DIRECTORY,
                          _("The file is not a directory"));
      return TRUE;
    }

  g_vfs_job_succeeded (G_VFS_JOB (job));
  
  /* Enumerate root */
  for (l = G_VFS_BACKEND_COMPUTER (backend)->files; l != NULL; l = l->next)
    {
      file = l->data;
      
      info = g_file_info_new ();
      
      file_info_from_file (file, info);
      g_vfs_job_enumerate_add_info (job, info);
      g_object_unref (info);
    }

  g_vfs_job_enumerate_done (job);
  
  return TRUE;
}

static gboolean
try_query_info (GVfsBackend *backend,
                GVfsJobQueryInfo *job,
                const char *filename,
                GFileQueryInfoFlags flags,
                GFileInfo *info,
                GFileAttributeMatcher *matcher)
{
  ComputerFile *file;

  file = lookup (G_VFS_BACKEND_COMPUTER (backend),
                 G_VFS_JOB (job), filename);

  if (file == &root)
    {
      GIcon *icon;
      
      g_file_info_set_name (info, "/");
      g_file_info_set_file_type (info, G_FILE_TYPE_DIRECTORY);
      g_file_info_set_display_name (info, _("Computer"));
      icon = g_themed_icon_new ("gnome-fs-client");
      g_file_info_set_icon (info, icon);
      g_object_unref (icon);
      g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_WRITE, FALSE);
      g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_DELETE, FALSE);
      g_file_info_set_attribute_boolean (info, G_FILE_ATTRIBUTE_ACCESS_CAN_TRASH, FALSE);
      g_file_info_set_content_type (info, "inode/directory");
    }
  else if (file != NULL)
    file_info_from_file (file, info);

  g_vfs_job_succeeded (G_VFS_JOB (job));
  
  return TRUE;
}

static gboolean
try_create_dir_monitor (GVfsBackend *backend,
                        GVfsJobCreateMonitor *job,
                        const char *filename,
                        GFileMonitorFlags flags)
{
  ComputerFile *file;
  GVfsBackendComputer *computer_backend;

  computer_backend = G_VFS_BACKEND_COMPUTER (backend);

  file = lookup (computer_backend,
                 G_VFS_JOB (job), filename);

  if (file != &root)
    {
      g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR,
                        G_IO_ERROR_NOT_SUPPORTED,
                        _("Can't open mountable file"));
      return TRUE;
    }
  
  g_vfs_job_create_monitor_set_monitor (job,
                                        computer_backend->root_monitor);
  g_vfs_job_succeeded (G_VFS_JOB (job));

  return TRUE;
}

static void
mount_volume_cb (GObject *source_object,
                 GAsyncResult *res,
                 gpointer user_data)
{
  GVfsJobMountMountable *job = user_data;
  GError *error;
  GMount *mount;
  GVolume *volume;
  GFile *root;
  char *uri;
  
  volume = G_VOLUME (source_object);

  /* TODO: We're leaking the GMountOperation here */
  
  error = NULL;
  if (g_volume_mount_finish (volume, res, &error))
    {
      mount = g_volume_get_mount (volume);

      if (mount)
        {
          root = g_mount_get_root (mount);
          uri = g_file_get_uri (root);
          g_vfs_job_mount_mountable_set_target_uri (job,
                                                    uri,
                                                    FALSE);
          g_free (uri);
          g_object_unref (root);
          g_object_unref (mount);
          g_vfs_job_succeeded (G_VFS_JOB (job));
        }
      else
        g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR,
                          G_IO_ERROR_FAILED,
                          _("Internal error: %s"), "No mount object for mounted volume");
    }
  else
    {
      g_vfs_job_failed_from_error  (G_VFS_JOB (job), error);
      g_error_free (error);
    }
}

static gboolean
try_mount_mountable (GVfsBackend *backend,
                     GVfsJobMountMountable *job,
                     const char *filename,
                     GMountSource *mount_source)
{
  ComputerFile *file;
  GMountOperation *mount_op;

  file = lookup (G_VFS_BACKEND_COMPUTER (backend),
                 G_VFS_JOB (job), filename);
  
  if (file == &root)
    g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR,
                      G_IO_ERROR_NOT_MOUNTABLE_FILE,
                      _("Not a mountable file"));
  else if (file != NULL)
    {
      if (file->volume)
        {
          mount_op = g_mount_source_get_operation (mount_source);
          g_volume_mount (file->volume,
                          mount_op,
                          G_VFS_JOB (job)->cancellable,
                          mount_volume_cb,
                          job);
        }
#if 0
      else if (file->drive)
        {
          /* TODO: Poll for media? */
        }
#endif
      else
        {
          g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR,
                            G_IO_ERROR_NOT_SUPPORTED,
                            _("Can't mount file"));
        }
    }
  
  return TRUE;
}

static void
unmount_mount_cb (GObject *source_object,
		  GAsyncResult *res,
		  gpointer user_data)
{
  GVfsJobMountMountable *job = user_data;
  GError *error;
  GMount *mount;
  
  mount = G_MOUNT (source_object);

  error = NULL;
  if (g_mount_unmount_finish (mount, res, &error))
    g_vfs_job_succeeded (G_VFS_JOB (job));
  else
    {
      g_vfs_job_failed_from_error  (G_VFS_JOB (job), error);
      g_error_free (error);
    }
}


static gboolean
try_unmount_mountable (GVfsBackend *backend,
		       GVfsJobUnmountMountable *job,
		       const char *filename,
		       GMountUnmountFlags flags)
{
  ComputerFile *file;

  file = lookup (G_VFS_BACKEND_COMPUTER (backend),
                 G_VFS_JOB (job), filename);
  
  if (file == &root)
    g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR,
                      G_IO_ERROR_NOT_MOUNTABLE_FILE,
                      _("Not a mountable file"));
  else if (file != NULL)
    {
      if (file->mount)
        {
          g_mount_unmount (file->mount,
                         flags,
                         G_VFS_JOB (job)->cancellable,
                         unmount_mount_cb,
                         job);
        }
      else
        {
          g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR,
                            G_IO_ERROR_NOT_SUPPORTED,
                            _("Can't unmount file"));
        }
    }
  
  return TRUE;
}

static void
eject_mount_cb (GObject *source_object,
                GAsyncResult *res,
                gpointer user_data)
{
  GVfsJobMountMountable *job = user_data;
  GError *error;
  GMount *mount;
  
  mount = G_MOUNT (source_object);

  error = NULL;
  if (g_mount_eject_finish (mount, res, &error))
    g_vfs_job_succeeded (G_VFS_JOB (job));
  else
    {
      g_vfs_job_failed_from_error  (G_VFS_JOB (job), error);
      g_error_free (error);
    }
}

static void
eject_volume_cb (GObject *source_object,
                 GAsyncResult *res,
                 gpointer user_data)
{
  GVfsJobMountMountable *job = user_data;
  GError *error;
  GVolume *volume;
  
  volume = G_VOLUME (source_object);

  error = NULL;
  if (g_volume_eject_finish (volume, res, &error))
    g_vfs_job_succeeded (G_VFS_JOB (job));
  else
    {
      g_vfs_job_failed_from_error  (G_VFS_JOB (job), error);
      g_error_free (error);
    }
}


static void
eject_drive_cb (GObject *source_object,
                GAsyncResult *res,
                gpointer user_data)
{
  GVfsJobMountMountable *job = user_data;
  GError *error;
  GDrive *drive;
  
  drive = G_DRIVE (source_object);

  error = NULL;
  if (g_drive_eject_finish (drive, res, &error))
    g_vfs_job_succeeded (G_VFS_JOB (job));
  else
    {
      g_vfs_job_failed_from_error  (G_VFS_JOB (job), error);
      g_error_free (error);
    }
}

static gboolean
try_eject_mountable (GVfsBackend *backend,
                     GVfsJobUnmountMountable *job,
                     const char *filename,
                     GMountUnmountFlags flags)
{
  ComputerFile *file;

  file = lookup (G_VFS_BACKEND_COMPUTER (backend),
                 G_VFS_JOB (job), filename);
  
  if (file == &root)
    g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR,
                      G_IO_ERROR_NOT_MOUNTABLE_FILE,
                      _("Not a mountable file"));
  else if (file != NULL)
    {
      if (file->mount)
        {
          g_mount_eject (file->mount,
                         flags,
                         G_VFS_JOB (job)->cancellable,
                         eject_mount_cb,
                         job);
        }
      else if (file->volume)
        {
          g_volume_eject (file->volume,
                          flags,
                          G_VFS_JOB (job)->cancellable,
                          eject_volume_cb,
                          job);
        }
      else if (file->drive)
        {
          g_drive_eject (file->drive,
                         flags,
                         G_VFS_JOB (job)->cancellable,
                         eject_drive_cb,
                         job);
        }
      else
        {
          g_vfs_job_failed (G_VFS_JOB (job), G_IO_ERROR,
                            G_IO_ERROR_NOT_SUPPORTED,
                            _("Can't eject file"));
        }
    }
  
  return TRUE;
}

static void
g_vfs_backend_computer_class_init (GVfsBackendComputerClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GVfsBackendClass *backend_class = G_VFS_BACKEND_CLASS (klass);
  
  gobject_class->finalize = g_vfs_backend_computer_finalize;

  backend_class->try_mount = try_mount;
  backend_class->try_open_for_read = try_open_for_read;
  backend_class->try_query_info = try_query_info;
  backend_class->try_enumerate = try_enumerate;
  backend_class->try_create_dir_monitor = try_create_dir_monitor;
  backend_class->try_mount_mountable = try_mount_mountable;
  backend_class->try_unmount_mountable = try_unmount_mountable;
  backend_class->try_eject_mountable = try_eject_mountable;
}