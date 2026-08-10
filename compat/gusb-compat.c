#define _GNU_SOURCE
#include <dlfcn.h>
#include <glib.h>
#include <gusb.h>

typedef GPtrArray *(*GetInterfacesFn) (GUsbDevice *, GError **);

#define FORWARD_U8(wrapper, symbol, type)                                  \
  guint8 wrapper (type *object)                                            \
  {                                                                        \
    typedef guint8 (*Fn) (type *);                                         \
    static Fn current;                                                     \
    if (G_UNLIKELY (current == NULL))                                      \
      current = (Fn) dlvsym (RTLD_NEXT, symbol, "LIBGUSB_0.2.8");          \
    return current != NULL ? current (object) : 0;                         \
  }

GPtrArray *
g_usb_device_get_interfaces_compat (GUsbDevice *device, GError **error)
{
  static GetInterfacesFn current;

  if (G_UNLIKELY (current == NULL))
    current = (GetInterfacesFn) dlvsym (RTLD_NEXT,
                                        "g_usb_device_get_interfaces",
                                        "LIBGUSB_0.2.8");

  if (G_UNLIKELY (current == NULL))
    {
      g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                           "compatible libgusb symbol was not found");
      return NULL;
    }

  return current (device, error);
}

__asm__ (".symver g_usb_device_get_interfaces_compat,"
         "g_usb_device_get_interfaces@LIBGUSB_0.1.0");

FORWARD_U8 (g_usb_interface_get_protocol_compat,
            "g_usb_interface_get_protocol", GUsbInterface)
FORWARD_U8 (g_usb_interface_get_subclass_compat,
            "g_usb_interface_get_subclass", GUsbInterface)
FORWARD_U8 (g_usb_interface_get_class_compat,
            "g_usb_interface_get_class", GUsbInterface)
FORWARD_U8 (g_usb_interface_get_number_compat,
            "g_usb_interface_get_number", GUsbInterface)

guint16
g_usb_device_get_release_compat (GUsbDevice *device)
{
  typedef guint16 (*Fn) (GUsbDevice *);
  static Fn current;
  if (G_UNLIKELY (current == NULL))
    current = (Fn) dlvsym (RTLD_NEXT, "g_usb_device_get_release",
                           "LIBGUSB_0.2.8");
  return current != NULL ? current (device) : 0;
}

__asm__ (".symver g_usb_interface_get_protocol_compat,"
         "g_usb_interface_get_protocol@LIBGUSB_0.1.0");
__asm__ (".symver g_usb_interface_get_subclass_compat,"
         "g_usb_interface_get_subclass@LIBGUSB_0.1.0");
__asm__ (".symver g_usb_interface_get_class_compat,"
         "g_usb_interface_get_class@LIBGUSB_0.1.0");
__asm__ (".symver g_usb_interface_get_number_compat,"
         "g_usb_interface_get_number@LIBGUSB_0.1.0");
__asm__ (".symver g_usb_device_get_release_compat,"
         "g_usb_device_get_release@LIBGUSB_0.1.0");
