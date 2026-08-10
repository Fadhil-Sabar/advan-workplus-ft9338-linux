#include <stdio.h>
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
  if (!fp_device_open_sync (device, NULL, &error))
    {
      fprintf (stderr, "Open failed: %s\n", error->message);
      g_error_free (error);
      return 2;
    }

  puts ("TOUCH_SENSOR_NOW");
  fflush (stdout);
  FpImage *image = fp_device_capture_sync (device, TRUE, NULL, &error);
  if (image == NULL)
    {
      fprintf (stderr, "Capture failed: %s\n", error->message);
      g_error_free (error);
    }
  else
    {
      printf ("CAPTURE_OK %ux%u\n",
              fp_image_get_width (image), fp_image_get_height (image));
      g_object_unref (image);
    }

  if (!fp_device_close_sync (device, NULL, &error))
    {
      fprintf (stderr, "Close failed: %s\n", error->message);
      g_error_free (error);
      return 3;
    }
  return image != NULL ? 0 : 4;
}
