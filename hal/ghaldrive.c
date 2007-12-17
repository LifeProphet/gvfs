/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*- */

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
 * Author: David Zeuthen <davidz@redhat.com>
 */

#include <config.h>

#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include <glib.h>
#include <glib/gi18n-lib.h>

#include "ghalvolumemonitor.h"
#include "ghaldrive.h"
#include "ghalvolume.h"


struct _GHalDrive {
  GObject parent;

  GVolumeMonitor  *volume_monitor; /* owned by volume monitor */
  GList           *volumes;        /* entries in list are owned by volume_monitor */

  char *name;
  char *icon;
  char *device_path;

  gboolean can_eject;
  gboolean can_poll_for_media;
  gboolean is_media_check_automatic;
  gboolean has_media;
  gboolean uses_removable_media;

  HalDevice *device;
  HalPool *pool;
};

static void g_hal_drive_drive_iface_init (GDriveIface *iface);

#define _G_IMPLEMENT_INTERFACE_DYNAMIC(TYPE_IFACE, iface_init)       { \
  const GInterfaceInfo g_implement_interface_info = { \
    (GInterfaceInitFunc) iface_init, NULL, NULL \
  }; \
  g_type_module_add_interface (type_module, g_define_type_id, TYPE_IFACE, &g_implement_interface_info); \
}

G_DEFINE_DYNAMIC_TYPE_EXTENDED (GHalDrive, g_hal_drive, G_TYPE_OBJECT, 0,
                                _G_IMPLEMENT_INTERFACE_DYNAMIC (G_TYPE_DRIVE,
                                                                g_hal_drive_drive_iface_init))

static void
g_hal_drive_finalize (GObject *object)
{
  GList *l;
  GHalDrive *drive;
  
  drive = G_HAL_DRIVE (object);

  for (l = drive->volumes; l != NULL; l = l->next)
    {
      GHalVolume *volume = l->data;
      g_hal_volume_unset_drive (volume, drive);
    }

  g_free (drive->device_path);
  if (drive->device != NULL)
    g_object_unref (drive->device);
  if (drive->pool != NULL)
    g_object_unref (drive->pool);

  g_free (drive->name);
  g_free (drive->icon);
  
  if (G_OBJECT_CLASS (g_hal_drive_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_hal_drive_parent_class)->finalize) (object);
}

static void
g_hal_drive_class_init (GHalDriveClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  gobject_class->finalize = g_hal_drive_finalize;
}

static void
g_hal_drive_class_finalize (GHalDriveClass *klass)
{
}

static void
g_hal_drive_init (GHalDrive *hal_drive)
{
}

static char *
_drive_get_description (HalDevice *d)
{
  char *s = NULL;
  const char *drive_type;
  const char *drive_bus;

  drive_type = hal_device_get_property_string (d, "storage.drive_type");
  drive_bus = hal_device_get_property_string (d, "storage.bus");

  if (strcmp (drive_type, "cdrom") == 0)
    {
      const char *first;
      const char *second;
      
      first = _("CD-ROM");
      if (hal_device_get_property_bool (d, "storage.cdrom.cdr"))
        first = _("CD-R");
      if (hal_device_get_property_bool (d, "storage.cdrom.cdrw"))
        first = _("CD-RW");
      
      second = NULL;
      if (hal_device_get_property_bool (d, "storage.cdrom.dvd"))
        second = _("DVD-ROM");
      if (hal_device_get_property_bool (d, "storage.cdrom.dvdplusr"))
        second = _("DVD+R");
      if (hal_device_get_property_bool (d, "storage.cdrom.dvdplusrw"))
        second = _("DVD+RW");
      if (hal_device_get_property_bool (d, "storage.cdrom.dvdr"))
        second = _("DVD-R");
      if (hal_device_get_property_bool (d, "storage.cdrom.dvdrw"))
        second = _("DVD-RW");
      if (hal_device_get_property_bool (d, "storage.cdrom.dvdram"))
        second = _("DVD-RAM");
      if ((hal_device_get_property_bool (d, "storage.cdrom.dvdr")) &&
          (hal_device_get_property_bool (d, "storage.cdrom.dvdplusr")))
        second = _("DVD�R");
      if (hal_device_get_property_bool (d, "storage.cdrom.dvdrw") &&
          hal_device_get_property_bool (d, "storage.cdrom.dvdplusrw"))
        second = _("DVD�RW");
      if (hal_device_get_property_bool (d, "storage.cdrom.hddvd"))
        second = _("HDDVD");
      if (hal_device_get_property_bool (d, "storage.cdrom.hddvdr"))
        second = _("HDDVD-r");
      if (hal_device_get_property_bool (d, "storage.cdrom.hddvdrw"))
        second = _("HDDVD-RW");
      if (hal_device_get_property_bool (d, "storage.cdrom.bd"))
        second = _("Blu-ray");
      if (hal_device_get_property_bool (d, "storage.cdrom.bdr"))
        second = _("Blu-ray-R");
      if (hal_device_get_property_bool (d, "storage.cdrom.bdre"))
        second = _("Blu-ray-RE");
      
      if (second != NULL) {
        s = g_strdup_printf (_("%s/%s Drive"), first, second);
      } else {
        s = g_strdup_printf (_("%s Drive"), first);
      }
    } 
  else if (strcmp (drive_type, "floppy") == 0)
    s = g_strdup (_("Floppy Drive"));
  else if (strcmp (drive_type, "disk") == 0)
    {
      if (drive_bus != NULL)
        {
          if (strcmp (drive_bus, "linux_raid") == 0)
            s = g_strdup (_("Software RAID Drive"));
          if (strcmp (drive_bus, "usb") == 0)
            s = g_strdup (_("USB Drive"));
          if (strcmp (drive_bus, "ide") == 0)
            s = g_strdup (_("ATA Drive"));
          if (strcmp (drive_bus, "scsi") == 0)
            s = g_strdup (_("SCSI Drive"));
          if (strcmp (drive_bus, "ieee1394") == 0)
            s = g_strdup (_("FireWire Drive"));
        } 
    } 
  else if (strcmp (drive_type, "tape") == 0)
    s = g_strdup (_("Tape Drive"));
  else if (strcmp (drive_type, "compact_flash") == 0)
    s = g_strdup (_("CompactFlash Drive"));
  else if (strcmp (drive_type, "memory_stick") == 0)
    s = g_strdup (_("MemoryStick Drive"));
  else if (strcmp (drive_type, "smart_media") == 0)
    s = g_strdup (_("SmartMedia Drive"));
  else if (strcmp (drive_type, "sd_mmc") == 0)
    s = g_strdup (_("SD/MMC Drive"));
  else if (strcmp (drive_type, "zip") == 0)
    s = g_strdup (_("Zip Drive"));
  else if (strcmp (drive_type, "jaz") == 0)
    s = g_strdup (_("Jaz Drive"));
  else if (strcmp (drive_type, "flashkey") == 0)
    s = g_strdup (_("Thumb Drive"));

  if (s == NULL)
    s = g_strdup (_("Mass Storage Drive"));

  return s;
}

char *
_drive_get_icon (HalDevice *d)
{
  char *s = NULL;
  const char *drive_type;
  const char *drive_bus;

  drive_type = hal_device_get_property_string (d, "storage.drive_type");
  drive_bus = hal_device_get_property_string (d, "storage.bus");
  
  if (strcmp (drive_type, "disk") == 0) {
    if (strcmp (drive_bus, "ide") == 0)
      s = g_strdup ("drive-removable-media-ata");
    else if (strcmp (drive_bus, "scsi") == 0)
      s = g_strdup ("drive-removable-media-scsi");
    else if (strcmp (drive_bus, "ieee1394") == 0)
      s = g_strdup ("drive-removable-media-ieee1394");
    else if (strcmp (drive_bus, "usb") == 0)
      s = g_strdup ("drive-removable-media-usb");
    else
      s = g_strdup ("drive-removable-media");
  }
  else if (strcmp (drive_type, "cdrom") == 0)
    {
      /* TODO: maybe there's a better heuristic than this */
      if (hal_device_get_property_int (d, "storage.cdrom.write_speed") > 0)
        s = g_strdup ("drive-optical-recorder");
      else
        s = g_strdup ("drive-optical");
    }
  else if (strcmp (drive_type, "floppy") == 0)
    s = g_strdup ("drive-removable-media-floppy");
  else if (strcmp (drive_type, "tape") == 0)
    s = g_strdup ("drive-removable-media-tape");
  else if (strcmp (drive_type, "compact_flash") == 0)
    s = g_strdup ("drive-removable-media-flash-cf");
  else if (strcmp (drive_type, "memory_stick") == 0)
    s = g_strdup ("drive-removable-media-flash-ms");
  else if (strcmp (drive_type, "smart_media") == 0)
    s = g_strdup ("drive-removable-media-flash-sm");
  else if (strcmp (drive_type, "sd_mmc") == 0)
    s = g_strdup ("drive-removable-media-flash-sd");

  if (s == NULL)
    s = g_strdup ("drive-removable-media");
  
  return s;
}

static void
_do_update_from_hal (GHalDrive *d)
{
  d->name = _drive_get_description (d->device);
  d->icon = _drive_get_icon (d->device);
  
  d->uses_removable_media = hal_device_get_property_bool (d->device, "storage.removable");
  if (d->uses_removable_media)
    {
      d->has_media = hal_device_get_property_bool (d->device, "storage.removable.media_available");
      d->is_media_check_automatic = hal_device_get_property_bool (d->device, "storage.media_check_enabled");
      d->can_poll_for_media = hal_device_has_interface (d->device, "org.freedesktop.Hal.Device.Storage.Removable");
      d->can_eject = hal_device_get_property_bool (d->device, "storage.requires_eject");
    }
  else
    {
      d->has_media = TRUE;
      d->is_media_check_automatic = FALSE;
      d->can_poll_for_media = FALSE;
      d->can_eject = FALSE;
    }
}

static void
_update_from_hal (GHalDrive *d, gboolean emit_changed)
{
  char *old_name;
  char *old_icon;
  gboolean old_uses_removable_media;
  gboolean old_has_media;
  gboolean old_is_media_check_automatic;
  gboolean old_can_poll_for_media;
  gboolean old_can_eject;

  old_name = g_strdup (d->name);
  old_icon = g_strdup (d->icon);
  old_uses_removable_media = d->uses_removable_media;
  old_has_media = d->has_media;
  old_is_media_check_automatic = d->is_media_check_automatic;
  old_can_poll_for_media = d->can_poll_for_media;
  old_can_eject = d->can_eject;

  g_free (d->name);
  g_free (d->icon);
  _do_update_from_hal (d);

  if (emit_changed)
    {
      if (old_uses_removable_media != d->uses_removable_media ||
          old_has_media != d->has_media ||
          old_is_media_check_automatic != d->is_media_check_automatic ||
          old_can_poll_for_media != d->can_poll_for_media ||
          old_can_eject != d->can_eject ||
          old_name == NULL || 
          old_icon == NULL ||
          strcmp (old_name, d->name) != 0 ||
          strcmp (old_icon, d->icon) != 0)
        {
          g_signal_emit_by_name (d, "changed");
          if (d->volume_monitor != NULL)
            g_signal_emit_by_name (d->volume_monitor, "drive_changed", d);
        }
    }
  g_free (old_name);
  g_free (old_icon);
}

static void
hal_changed (HalDevice *device, const char *key, gpointer user_data)
{
  GHalDrive *hal_drive = G_HAL_DRIVE (user_data);
  
  //g_warning ("volhal modifying %s (property %s changed)", hal_drive->device_path, key);
  _update_from_hal (hal_drive, TRUE);
}

GHalDrive *
g_hal_drive_new (GVolumeMonitor       *volume_monitor,
                  HalDevice            *device,
                  HalPool              *pool)
{
  GHalDrive *drive;

  drive = g_object_new (G_TYPE_HAL_DRIVE, NULL);
  drive->volume_monitor = volume_monitor;
  g_object_add_weak_pointer (G_OBJECT (volume_monitor), (gpointer) &(drive->volume_monitor));
  drive->device_path = g_strdup (hal_device_get_property_string (device, "block.device"));
  drive->device = g_object_ref (device);
  drive->pool = g_object_ref (pool);

  drive->name = g_strdup_printf ("Drive for %s", drive->device_path);
  drive->icon = g_strdup_printf ("drive-removable-media");

  g_signal_connect_object (device, "hal_property_changed", (GCallback) hal_changed, drive, 0);

  _update_from_hal (drive, FALSE);

  return drive;
}

void 
g_hal_drive_disconnected (GHalDrive *drive)
{
  GList *l;

  for (l = drive->volumes; l != NULL; l = l->next)
    {
      GHalVolume *volume = l->data;
      g_hal_volume_unset_drive (volume, drive);
    }
}

void 
g_hal_drive_set_volume (GHalDrive *drive, 
                         GHalVolume *volume)
{

  if (g_list_find (drive->volumes, volume) != NULL)
    return;

  drive->volumes = g_list_prepend (drive->volumes, volume);
  
  /* TODO: Emit changed in idle to avoid locking issues */
  g_signal_emit_by_name (drive, "changed");
  if (drive->volume_monitor != NULL)
    g_signal_emit_by_name (drive->volume_monitor, "drive_changed", drive);
}

void 
g_hal_drive_unset_volume (GHalDrive *drive, 
                           GHalVolume *volume)
{
  GList *l;

  l = g_list_find (drive->volumes, volume);
  if (l != NULL)
    {
      drive->volumes = g_list_delete_link (drive->volumes, l);

      /* TODO: Emit changed in idle to avoid locking issues */
      g_signal_emit_by_name (drive, "changed");
      if (drive->volume_monitor != NULL)
        g_signal_emit_by_name (drive->volume_monitor, "drive_changed", drive);
    }
}

gboolean 
g_hal_drive_has_udi (GHalDrive *drive, const char *udi)
{
  return strcmp (udi, hal_device_get_udi (drive->device)) == 0;
}

static GIcon *
g_hal_drive_get_icon (GDrive *drive)
{
  GHalDrive *hal_drive = G_HAL_DRIVE (drive);

  return g_themed_icon_new (hal_drive->icon);
}

static char *
g_hal_drive_get_name (GDrive *drive)
{
  GHalDrive *hal_drive = G_HAL_DRIVE (drive);
  
  return g_strdup (hal_drive->name);
}

static GList *
g_hal_drive_get_volumes (GDrive *drive)
{
  GHalDrive *hal_drive = G_HAL_DRIVE (drive);
  GList *l;

  l = g_list_copy (hal_drive->volumes);
  g_list_foreach (l, (GFunc) g_object_ref, NULL);

  return l;
}

static gboolean
g_hal_drive_has_volumes (GDrive *drive)
{
  GHalDrive *hal_drive = G_HAL_DRIVE (drive);
  return g_list_length (hal_drive->volumes) > 0;
}

static gboolean
g_hal_drive_is_media_removable (GDrive *drive)
{
  GHalDrive *hal_drive = G_HAL_DRIVE (drive);
  return hal_drive->uses_removable_media;
}

static gboolean
g_hal_drive_has_media (GDrive *drive)
{
  GHalDrive *hal_drive = G_HAL_DRIVE (drive);
  return hal_drive->has_media;
}

static gboolean
g_hal_drive_is_media_check_automatic (GDrive *drive)
{
  GHalDrive *hal_drive = G_HAL_DRIVE (drive);
  return hal_drive->is_media_check_automatic;
}

static gboolean
g_hal_drive_can_eject (GDrive *drive)
{
  GHalDrive *hal_drive = G_HAL_DRIVE (drive);
  return hal_drive->can_eject;
}

static gboolean
g_hal_drive_can_poll_for_media (GDrive *drive)
{
  GHalDrive *hal_drive = G_HAL_DRIVE (drive);
  return hal_drive->can_poll_for_media;
}


typedef struct {
  GObject *object;
  GAsyncReadyCallback callback;
  gpointer user_data;
  GCancellable *cancellable;
} SpawnOp;

static void 
spawn_cb (GPid pid, gint status, gpointer user_data)
{
  SpawnOp *data = user_data;
  GSimpleAsyncResult *simple;
  
  /* TODO: how do we report an error back to the caller while telling
   * him that we already have shown an error dialog to the user?
   *
   * G_IO_ERROR_FAILED_NO_UI or something?
   */

  simple = g_simple_async_result_new (data->object,
                                      data->callback,
                                      data->user_data,
                                      NULL);
  g_simple_async_result_complete (simple);
  g_object_unref (simple);
  g_free (data);
}

static void
g_hal_drive_eject (GDrive              *drive,
                   GCancellable        *cancellable,
                   GAsyncReadyCallback  callback,
                   gpointer             user_data)
{
  GHalDrive *hal_drive = G_HAL_DRIVE (drive);
  SpawnOp *data;
  GPid child_pid;
  GError *error;
  char *argv[] = {"gnome-mount", "-e", "-b", "-d", NULL, NULL};

  argv[4] = hal_drive->device_path;
  
  data = g_new0 (SpawnOp, 1);
  data->object = G_OBJECT (drive);
  data->callback = callback;
  data->user_data = user_data;
  data->cancellable = cancellable;
  
  error = NULL;
  if (!g_spawn_async (NULL,         /* working dir */
                      argv,
                      NULL,         /* envp */
                      G_SPAWN_DO_NOT_REAP_CHILD|G_SPAWN_SEARCH_PATH,
                      NULL,         /* child_setup */
                      NULL,         /* user_data for child_setup */
                      &child_pid,
                      &error)) {
    GSimpleAsyncResult *simple;
    simple = g_simple_async_result_new_from_error (data->object,
                                                   data->callback,
                                                   data->user_data,
                                                   error);
    g_simple_async_result_complete (simple);
    g_object_unref (simple);
    g_error_free (error);
    g_free (data);
    return;
  }
  
  g_child_watch_add (child_pid, spawn_cb, data);
}

static gboolean
g_hal_drive_eject_finish (GDrive        *drive,
                          GAsyncResult  *result,
                          GError       **error)
{
  return TRUE;
}

typedef struct {
  GObject *object;
  GAsyncReadyCallback callback;
  gpointer user_data;
  GCancellable *cancellable;
} PollOp;

static void
poll_for_media_cb (DBusPendingCall *pending_call, void *user_data)
{
  PollOp *data = (PollOp *) user_data;
  GSimpleAsyncResult *simple;
  DBusMessage *reply;
  
  reply = dbus_pending_call_steal_reply (pending_call);
  
  if (dbus_message_get_type (reply) == DBUS_MESSAGE_TYPE_ERROR)
    {
      GError *error;
      DBusError dbus_error;
      
      dbus_error_init (&dbus_error);
      dbus_set_error_from_message (&dbus_error, reply);
      error = g_error_new (G_IO_ERROR,
                           G_IO_ERROR_FAILED,
                           "Cannot invoke CheckForMedia on HAL: %s: %s", dbus_error.name, dbus_error.message);
      simple = g_simple_async_result_new_from_error (data->object,
                                                     data->callback,
                                                     data->user_data,
                                                     error);
      g_simple_async_result_complete (simple);
      g_object_unref (simple);
      g_error_free (error);
      dbus_error_free (&dbus_error);
      goto out;
    }

  /* TODO: parse reply and extract result? 
   * (the result is whether the media availability state changed) 
   */
  
  simple = g_simple_async_result_new (data->object,
                                      data->callback,
                                      data->user_data,
                                      NULL);
  g_simple_async_result_complete (simple);
  g_object_unref (simple);
  
 out:
  dbus_message_unref (reply);
  dbus_pending_call_unref (pending_call);
}


static void
g_hal_drive_poll_for_media (GDrive              *drive,
                            GCancellable        *cancellable,
                            GAsyncReadyCallback  callback,
                            gpointer             user_data)
{
  GHalDrive *hal_drive = G_HAL_DRIVE (drive);
  DBusConnection *con;
  DBusMessage *msg;
  DBusPendingCall *pending_call;
  PollOp *data;

  data = g_new0 (PollOp, 1);
  data->object = G_OBJECT (drive);
  data->callback = callback;
  data->user_data = user_data;
  data->cancellable = cancellable;

  //g_warning ("Rescanning udi %s", hal_device_get_udi (hal_drive->device));
  
  con = hal_pool_get_dbus_connection (hal_drive->pool);
  
  msg = dbus_message_new_method_call ("org.freedesktop.Hal", 
                                      hal_device_get_udi (hal_drive->device),
                                      "org.freedesktop.Hal.Device.Storage.Removable",
                                      "CheckForMedia");
  
  if (!dbus_connection_send_with_reply (con, msg, &pending_call, -1))
    {
      GError *error;
      GSimpleAsyncResult *simple;
      error = g_error_new_literal (G_IO_ERROR,
                                   G_IO_ERROR_FAILED,
                                   "Cannot invoke CheckForMedia on HAL");
      simple = g_simple_async_result_new_from_error (data->object,
                                                     data->callback,
                                                     data->user_data,
                                                     error);
      g_simple_async_result_complete (simple);
      g_object_unref (simple);
      g_error_free (error);
      g_free (data);
      return;
    }

  dbus_pending_call_set_notify (pending_call,
                                poll_for_media_cb,
                                data,
                                (DBusFreeFunction) g_free);
}

static gboolean
g_hal_drive_poll_for_media_finish (GDrive        *drive,
                                   GAsyncResult  *result,
                                   GError       **error)
{
  //g_warning ("poll finish");
  return TRUE;
}


static void
g_hal_drive_drive_iface_init (GDriveIface *iface)
{
  iface->get_name = g_hal_drive_get_name;
  iface->get_icon = g_hal_drive_get_icon;
  iface->has_volumes = g_hal_drive_has_volumes;
  iface->get_volumes = g_hal_drive_get_volumes;
  iface->is_media_removable = g_hal_drive_is_media_removable;
  iface->has_media = g_hal_drive_has_media;
  iface->is_media_check_automatic = g_hal_drive_is_media_check_automatic;
  iface->can_eject = g_hal_drive_can_eject;
  iface->can_poll_for_media = g_hal_drive_can_poll_for_media;
  iface->eject = g_hal_drive_eject;
  iface->eject_finish = g_hal_drive_eject_finish;
  iface->poll_for_media = g_hal_drive_poll_for_media;
  iface->poll_for_media_finish = g_hal_drive_poll_for_media_finish;
}

void 
g_hal_drive_register (GIOModule *module)
{
  g_hal_drive_register_type (G_TYPE_MODULE (module));
}
