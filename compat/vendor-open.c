#include <stdio.h>
#include <unistd.h>
#include <libfprint/fprint.h>

int
main (void)
{
  g_autoptr(FpContext) context = fp_context_new ();
  GPtrArray *devices = fp_context_get_devices (context);
  GError *error = NULL;

  if (devices->len == 0)
    {
      fprintf (stderr, "No fingerprint device found\n");
      return 1;
    }

  FpDevice *device = g_ptr_array_index (devices, 0);
  printf ("Opening %s (%s)\n",
          fp_device_get_name (device), fp_device_get_driver (device));

  if (!fp_device_open_sync (device, NULL, &error))
    {
      fprintf (stderr, "Open failed: %s\n", error->message);
      g_error_free (error);
      return 2;
    }

  puts ("OPEN_OK; keeping device active for 3 seconds");
  sleep (3);

  if (!fp_device_close_sync (device, NULL, &error))
    {
      fprintf (stderr, "Close failed: %s\n", error->message);
      g_error_free (error);
      return 3;
    }

  puts ("CLOSE_OK");
  return 0;
}
