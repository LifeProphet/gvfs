#include <config.h>

#include <string.h>

#include <glib.h>
#include <glib/gi18n-lib.h>
#include "gunixvolume.h"
#include "gunixvolumemonitor.h"
#include "gvolumepriv.h"
#include "gvolumemonitor.h"

struct _GUnixVolume {
  GObject parent;
  GVolumeMonitor *monitor;

  GUnixDrive *drive;
  char *name;
  char *icon;
  char *mountpoint;
};

static void g_unix_volue_volume_iface_init (GVolumeIface *iface);

G_DEFINE_TYPE_WITH_CODE (GUnixVolume, g_unix_volume, G_TYPE_OBJECT,
			 G_IMPLEMENT_INTERFACE (G_TYPE_VOLUME,
						g_unix_volue_volume_iface_init))


static void
g_unix_volume_finalize (GObject *object)
{
  GUnixVolume *volume;
  
  volume = G_UNIX_VOLUME (object);

  g_free (volume->name);
  g_free (volume->icon);
  g_free (volume->mountpoint);
  
  if (G_OBJECT_CLASS (g_unix_volume_parent_class)->finalize)
    (*G_OBJECT_CLASS (g_unix_volume_parent_class)->finalize) (object);
}

static void
g_unix_volume_class_init (GUnixVolumeClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  gobject_class->finalize = g_unix_volume_finalize;
}

static void
g_unix_volume_init (GUnixVolume *unix_volume)
{
}

static gboolean
is_in (const char *value, const char *set[])
{
  int i;
  for (i = 0; set[i] != NULL; i++)
    {
      if (strcmp (set[i], value) == 0)
	return TRUE;
    }
  return FALSE;
}

static char *
get_filesystem_volume_name (const char *fs_type)
{
  /* TODO: add translation table from gnome-vfs */
  return g_strdup_printf (_("%s volume"), fs_type);
}

static char *
type_to_icon (GUnixMountType type)
{
  const char *icon_name = NULL;
  
  switch (type)
    {
    case G_UNIX_MOUNT_TYPE_FLOPPY:
      icon_name = "gnome-dev-floppy";
      break;
    case G_UNIX_MOUNT_TYPE_CDROM:
      icon_name = "gnome-dev-cdrom";
      break;
    case G_UNIX_MOUNT_TYPE_NFS:
      icon_name = "gnome-fs-nfs";
      break;
    case G_UNIX_MOUNT_TYPE_ZIP:
      icon_name = "gnome-dev-zipdisk";
      break;
    case G_UNIX_MOUNT_TYPE_JAZ:
      icon_name = "gnome-dev-jazdisk";
      break;
    case G_UNIX_MOUNT_TYPE_MEMSTICK:
      icon_name = "gnome-dev-media-ms";
      break;
    case G_UNIX_MOUNT_TYPE_CF:
      icon_name = "gnome-dev-media-cf";
      break;
    case G_UNIX_MOUNT_TYPE_SM:
      icon_name = "gnome-dev-media-sm";
      break;
    case G_UNIX_MOUNT_TYPE_SDMMC:
      icon_name = "gnome-dev-media-sdmmc";
      break;
    case G_UNIX_MOUNT_TYPE_HD:
      icon_name = "gnome-dev-harddisk";
      break;
      
    case G_UNIX_MOUNT_TYPE_IPOD:
    case G_UNIX_MOUNT_TYPE_CAMERA:
    case G_UNIX_MOUNT_TYPE_UNKNOWN:
    default:
      icon_name = "gnome-dev-harddisk";
      break;
    }
  return g_strdup (icon_name);
}

GUnixVolume *
g_unix_volume_new (GVolumeMonitor *volume_monitor,
		   GUnixMount *mount)
{
  GUnixVolume *volume;
  GUnixDrive *drive;
  GUnixMountType type;
  char *volume_name;
  const char *ignore_fs[] = {
    "auto",
    "autofs",
    "devfs",
    "devpts",
    "kernfs",
    "linprocfs",
    "proc",
    "procfs",
    "ptyfs",
    "rootfs",
    "selinuxfs",
    "sysfs",
    "tmpfs",
    "usbfs",
    "nfsd",
    NULL
  };
  const char *ignore_devices[] = {
    "none",
    "sunrpc",
    "devpts",
    "nfsd",
    "/dev/loop",
    "/dev/vn",
    NULL
  };
  const char *ignore_mountpoints[] = {
    /* Includes all FHS 2.3 toplevel dirs */
    "/",
    "/bin",
    "/boot",
    "/dev",
    "/etc",
    "/home",
    "/lib",
    "/lib64",
    "/media",
    "/mnt",
    "/opt",
    "/root",
    "/sbin",
    "/srv",
    "/tmp",
    "/usr",
    "/var",
    "/proc",
    "/sbin",
    NULL
  };
  
  drive = g_unix_volume_monitor_lookup_drive_for_mountpoint (G_UNIX_VOLUME_MONITOR (volume_monitor),
							     mount->mount_path);

  if (drive == NULL)
    {
      /* No drive for volume. Ignore most internal things */
      
      if (is_in (mount->filesystem_type, ignore_fs))
	return NULL;
      
      if (is_in (mount->device_path, ignore_devices))
	return NULL;
      
      if (is_in (mount->mount_path, ignore_mountpoints))
	return NULL;

      if (g_str_has_prefix (mount->mount_path, "/dev") ||
	  g_str_has_prefix (mount->mount_path, "/proc") ||
	  g_str_has_prefix (mount->mount_path, "/sys"))
	return NULL;
    }
  
  volume = g_object_new (G_TYPE_UNIX_VOLUME, NULL);
  volume->monitor = volume_monitor;
  volume->drive = drive; /* TODO: Ownership? */
  volume->mountpoint = g_strdup (mount->mount_path);

  type = _g_guess_type_for_mount (mount->mount_path,
				  mount->device_path,
				  mount->filesystem_type);
  
  volume->icon = type_to_icon (type);
					  

  volume_name = NULL;
  if (type == G_UNIX_MOUNT_TYPE_CDROM)
    {
      /* Get CD type (audio/data) and volume name */
      
    }

  if (volume_name == NULL)
    {
      const char *name = strrchr (mount->mount_path, '/');
      if (name == NULL)
	if (mount->filesystem_type != NULL)
	  volume_name = g_strdup (get_filesystem_volume_name (mount->filesystem_type));
    }

  if (volume_name == NULL)
    {
      /* TODO: Use volume size as name? */
      volume_name = g_strdup (_("Unknown volume"));
    }
  
  g_print ("mountpoint: \n");
  g_print (" mountpoint: %s\n", mount->mount_path);
  g_print (" device: %s\n", mount->device_path);
  g_print (" fs_type: %s\n", mount->filesystem_type);
  g_print (" is_ro: %d\n", mount->is_read_only);
  volume->name = volume_name;

  return volume;
}

static char *
g_unix_volume_get_platform_id (GVolume *volume)
{
  GUnixVolume *unix_volume = G_UNIX_VOLUME (volume);

  return g_strdup (unix_volume->mountpoint);
}

static char *
g_unix_volume_get_icon (GVolume *volume)
{
  GUnixVolume *unix_volume = G_UNIX_VOLUME (volume);

  return g_strdup (unix_volume->icon);
}

static char *
g_unix_volume_get_name (GVolume *volume)
{
  GUnixVolume *unix_volume = G_UNIX_VOLUME (volume);
  
  return g_strdup (unix_volume->name);
}

gboolean
g_unix_volume_has_mountpoint (GUnixVolume *volume,
			      const char  *mountpoint)
{
  return strcmp (volume->mountpoint, mountpoint) == 0;
}

static void
g_unix_volue_volume_iface_init (GVolumeIface *iface)
{
  iface->get_platform_id = g_unix_volume_get_platform_id;
  iface->get_name = g_unix_volume_get_name;
  iface->get_icon = g_unix_volume_get_icon;
}