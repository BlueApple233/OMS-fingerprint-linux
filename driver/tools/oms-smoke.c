/*
 * OMS driver bring-up tool
 * Copyright (C) 2026 The OMS Linux driver contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/*
 * Small companion tool for bring-up and manual testing of the omsmoc driver
 * without fprintd.  It uses the public libfprint API only.
 *
 *   oms-smoke info                 open/close the device, print the driver state
 *   oms-smoke verify [seconds]     verify a print against the chip
 *   oms-smoke identify [seconds]   identify a finger through the chip
 *
 * The verify/identify commands wait for a finger press and stop after the
 * given number of seconds (default 20).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libfprint/fprint.h>
#include <glib-unix.h>

typedef enum {
  COMMAND_INFO,
  COMMAND_VERIFY,
  COMMAND_IDENTIFY,
  COMMAND_ENROLL,
  COMMAND_LIST,
  COMMAND_DELETE,
  COMMAND_CLEAR,
} SmokeCommand;

typedef struct
{
  GMainLoop      *loop;
  GCancellable   *cancellable;
  FpPrint        *print;
  SmokeCommand    command;
  guint           slot;
  gboolean        slot_given;
  guint           timeout_seconds;
  guint           timeout_id;
  int             ret_value;
} SmokeData;

static void
smoke_data_free (SmokeData *data)
{
  g_clear_handle_id (&data->timeout_id, g_source_remove);
  g_clear_object (&data->print);
  g_clear_object (&data->cancellable);
  g_clear_pointer (&data->loop, g_main_loop_unref);
  g_free (data);
}
G_DEFINE_AUTOPTR_CLEANUP_FUNC (SmokeData, smoke_data_free)

static void
on_device_closed (FpDevice     *device,
                  GAsyncResult *result,
                  gpointer      user_data)
{
  SmokeData *data = user_data;
  g_autoptr(GError) error = NULL;

  fp_device_close_finish (device, result, &error);
  if (error)
    g_warning ("Failed to close device: %s", error->message);

  g_main_loop_quit (data->loop);
}

static void
quit (FpDevice  *device,
      SmokeData *data)
{
  if (!fp_device_is_open (device))
    {
      g_main_loop_quit (data->loop);
      return;
    }

  fp_device_close (device, NULL, (GAsyncReadyCallback) on_device_closed, data);
}

static gboolean
on_timeout (gpointer user_data)
{
  SmokeData *data = user_data;

  g_print ("\nTimeout reached, cancelling.\n");
  data->timeout_id = 0;
  g_cancellable_cancel (data->cancellable);

  return G_SOURCE_REMOVE;
}

static gboolean
on_sigint (gpointer user_data)
{
  SmokeData *data = user_data;

  g_cancellable_cancel (data->cancellable);

  return G_SOURCE_CONTINUE;
}

static void
start_action (FpDevice  *device,
              SmokeData *data);

static void
on_verify_done (FpDevice     *device,
                GAsyncResult *result,
                gpointer      user_data)
{
  SmokeData *data = user_data;
  g_autoptr(FpPrint) matched_print = NULL;
  g_autoptr(FpPrint) scanned_print = NULL;
  g_autoptr(GError) error = NULL;
  gboolean matched = FALSE;

  if (!fp_device_verify_finish (device, result, &matched, &scanned_print, &error) &&
      error->domain != FP_DEVICE_RETRY)
    {
      g_warning ("Verify failed: %s", error->message);
      data->ret_value = EXIT_FAILURE;
      quit (device, data);
      return;
    }

  if (error)
    g_print ("Verify reported a retry condition: %s\n", error->message);

  if (matched)
    {
      g_print ("VERIFY: MATCH (finger presented was recognised)\n");
      data->ret_value = EXIT_SUCCESS;
      quit (device, data);
      return;
    }

  g_print ("VERIFY: no match, press the registered finger again? [Y/n] ");

  {
    char buffer[20];

    if (!fgets (buffer, sizeof (buffer), stdin) ||
        (buffer[0] != 'Y' && buffer[0] != 'y' && buffer[0] != '\n'))
      {
        data->ret_value = EXIT_FAILURE;
        quit (device, data);
        return;
      }
  }

  g_cancellable_reset (data->cancellable);
  start_action (device, data);
}

static void
on_identify_report (FpDevice  *device,
                    FpPrint   *match,
                    FpPrint   *print,
                    gpointer   user_data,
                    GError    *error)
{
  if (error)
    {
      g_print ("Identify report (retry): %s\n", error->message);
      return;
    }

  if (match)
    g_print ("Identify report: MATCH\n");
  else
    g_print ("Identify report: no match\n");
}

static void
on_identify_done (FpDevice     *device,
                  GAsyncResult *result,
                  gpointer      user_data)
{
  SmokeData *data = user_data;
  g_autoptr(FpPrint) matched_print = NULL;
  g_autoptr(FpPrint) scanned_print = NULL;
  g_autoptr(GError) error = NULL;

  if (!fp_device_identify_finish (device, result,
                                  &matched_print, &scanned_print, &error) &&
      error->domain != FP_DEVICE_RETRY)
    {
      g_warning ("Identify failed: %s", error->message);
      data->ret_value = EXIT_FAILURE;
      quit (device, data);
      return;
    }

  if (error)
    g_print ("Identify reported a retry condition: %s\n", error->message);

  if (matched_print)
    {
      g_print ("IDENTIFY: MATCH\n");
      data->ret_value = EXIT_SUCCESS;
      quit (device, data);
      return;
    }

  g_print ("IDENTIFY: no match, try again? [Y/n] ");

  {
    char buffer[20];

    if (!fgets (buffer, sizeof (buffer), stdin) ||
        (buffer[0] != 'Y' && buffer[0] != 'y' && buffer[0] != '\n'))
      {
        data->ret_value = EXIT_FAILURE;
        quit (device, data);
        return;
      }
  }

  g_cancellable_reset (data->cancellable);
  start_action (device, data);
}

static void
on_enroll_progress (FpDevice     *device,
                    gint          completed_stages,
                    FpPrint      *print,
                    gpointer      user_data,
                    GError       *error)
{
  if (error)
    g_print ("Enroll progress %d: %s\n", completed_stages, error->message);
  else
    g_print ("Enroll progress: %d/%d stages\n", completed_stages,
             fp_device_get_nr_enroll_stages (device));
}

static void
on_enroll_done (FpDevice     *device,
                GAsyncResult *result,
                gpointer      user_data)
{
  SmokeData *data = user_data;
  g_autoptr(FpPrint) print = NULL;
  g_autoptr(GError) error = NULL;

  print = fp_device_enroll_finish (device, result, &error);
  if (!print)
    {
      g_warning ("Enroll failed: %s", error->message);
      data->ret_value = EXIT_FAILURE;
      quit (device, data);
      return;
    }

  g_print ("ENROLL: done (finger %d, description '%s')\n",
           (int) fp_print_get_finger (print),
           fp_print_get_description (print) ? fp_print_get_description (print) : "");

  data->ret_value = EXIT_SUCCESS;
  quit (device, data);
}

static void
on_clear_done (FpDevice     *device,
               GAsyncResult *result,
               gpointer      user_data)
{
  SmokeData *data = user_data;
  g_autoptr(GError) error = NULL;

  if (!fp_device_clear_storage_finish (device, result, &error))
    {
      g_warning ("Clear storage failed: %s", error->message);
      data->ret_value = EXIT_FAILURE;
      quit (device, data);
      return;
    }

  g_print ("CLEAR: done\n");
  data->ret_value = EXIT_SUCCESS;
  quit (device, data);
}

static void
on_delete_done (FpDevice     *device,
                GAsyncResult *result,
                gpointer      user_data)
{
  SmokeData *data = user_data;
  g_autoptr(GError) error = NULL;

  if (!fp_device_delete_print_finish (device, result, &error))
    {
      g_warning ("Delete failed: %s", error->message);
      data->ret_value = EXIT_FAILURE;
      quit (device, data);
      return;
    }

  g_print ("DELETE: slot %u removed\n", data->slot);
  data->ret_value = EXIT_SUCCESS;
  quit (device, data);
}

static void
on_list_for_delete (FpDevice     *device,
                    GAsyncResult *result,
                    gpointer      user_data)
{
  SmokeData *data = user_data;
  g_autoptr(GPtrArray) prints = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree gchar *wanted = NULL;
  guint i;

  prints = fp_device_list_prints_finish (device, result, &error);
  if (!prints)
    {
      g_warning ("List failed: %s", error->message);
      data->ret_value = EXIT_FAILURE;
      quit (device, data);
      return;
    }

  wanted = g_strdup_printf ("Slot %u", data->slot);

  for (i = 0; i < prints->len; i++)
    {
      FpPrint *print = g_ptr_array_index (prints, i);
      g_autofree gchar *description = NULL;

      g_object_get (print, "description", &description, NULL);
      if (g_strcmp0 (description, wanted) == 0)
        {
          g_print ("Deleting template in slot %u\n", data->slot);
          fp_device_delete_print (device, print, data->cancellable,
                                  (GAsyncReadyCallback) on_delete_done, data);
          return;
        }
    }

  g_warning ("No template in slot %u", data->slot);
  data->ret_value = EXIT_FAILURE;
  quit (device, data);
}

static void
on_list_done (FpDevice     *device,
              GAsyncResult *result,
              gpointer      user_data)
{
  SmokeData *data = user_data;
  g_autoptr(GPtrArray) prints = NULL;
  g_autoptr(GError) error = NULL;
  guint i;

  prints = fp_device_list_prints_finish (device, result, &error);
  if (!prints)
    {
      g_warning ("List failed: %s", error->message);
      data->ret_value = EXIT_FAILURE;
      quit (device, data);
      return;
    }

  g_print ("%u template(s) on the device:\n", prints->len);
  for (i = 0; i < prints->len; i++)
    {
      FpPrint *print = g_ptr_array_index (prints, i);
      g_autofree gchar *description = NULL;

      g_object_get (print, "description", &description, NULL);
      g_print ("  - %s\n", description ? description : "(no description)");
    }

  data->ret_value = EXIT_SUCCESS;
  quit (device, data);
}

static void
on_verify_listed (FpDevice     *device,
                  GAsyncResult *result,
                  gpointer      user_data)
{
  SmokeData *data = user_data;
  g_autoptr(GPtrArray) prints = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree gchar *wanted = NULL;
  FpPrint *print = NULL;
  guint i;

  prints = fp_device_list_prints_finish (device, result, &error);
  if (!prints || prints->len == 0)
    {
      g_warning ("No prints on the device: %s", error ? error->message : "storage empty");
      data->ret_value = EXIT_FAILURE;
      quit (device, data);
      return;
    }

  if (data->slot_given)
    {
      wanted = g_strdup_printf ("Slot %u", data->slot);

      for (i = 0; i < prints->len; i++)
        {
          g_autofree gchar *description = NULL;

          g_object_get (g_ptr_array_index (prints, i), "description", &description, NULL);
          if (g_strcmp0 (description, wanted) == 0)
            break;
        }

      if (i == prints->len)
        {
          g_warning ("No template in slot %u", data->slot);
          data->ret_value = EXIT_FAILURE;
          quit (device, data);
          return;
        }
    }
  else
    {
      i = 0;
    }

  print = g_ptr_array_index (prints, i);
  g_print ("Press the finger enrolled in '%s' (timeout %u s)...\n",
           fp_print_get_description (print) ? fp_print_get_description (print) : "?",
           data->timeout_seconds);

  g_set_object (&data->print, print);
  fp_device_verify (device, print, data->cancellable,
                    NULL, NULL, NULL,
                    (GAsyncReadyCallback) on_verify_done, data);
}

static void
start_action (FpDevice  *device,
              SmokeData *data)
{
  data->timeout_id = g_timeout_add_seconds (data->timeout_seconds, on_timeout, data);

  switch (data->command)
    {
    case COMMAND_INFO:
      g_print ("Driver: %s\n", fp_device_get_driver (device));
      g_print ("Device: %s (%s)\n", fp_device_get_name (device),
               fp_device_get_device_id (device));
      g_print ("Scan type: %s\n",
               fp_device_get_scan_type (device) == FP_SCAN_TYPE_PRESS ? "press" : "swipe");
      g_print ("Enroll stages: %u\n", fp_device_get_nr_enroll_stages (device));
      data->ret_value = EXIT_SUCCESS;
      quit (device, data);
      break;

    case COMMAND_ENROLL:
      g_clear_object (&data->print);
      data->print = fp_print_new (device);
      fp_print_set_finger (data->print, FP_FINGER_LEFT_INDEX);
      g_print ("Enrolling a new finger, press and lift the finger for every "
               "stage (timeout %u s)...\n", data->timeout_seconds);
      fp_device_enroll (device, data->print, data->cancellable,
                        on_enroll_progress, data, NULL,
                        (GAsyncReadyCallback) on_enroll_done, data);
      break;

    case COMMAND_VERIFY:
      /* Verification needs a print that is stored on the device, as the
       * slot it refers to must match the slot the chip reports. */
      fp_device_list_prints (device, data->cancellable,
                             (GAsyncReadyCallback) on_verify_listed, data);
      break;

    case COMMAND_IDENTIFY:
      {
        g_autoptr(GPtrArray) gallery = g_ptr_array_new_with_free_func (g_object_unref);

        g_print ("Press any finger enrolled on the chip (timeout %u s)...\n",
                 data->timeout_seconds);
        fp_device_identify (device, gallery, data->cancellable,
                            on_identify_report, data, NULL,
                            (GAsyncReadyCallback) on_identify_done, data);
      }
      break;

    case COMMAND_LIST:
      fp_device_list_prints (device, data->cancellable,
                             (GAsyncReadyCallback) on_list_done, data);
      break;

    case COMMAND_DELETE:
      fp_device_list_prints (device, data->cancellable,
                             (GAsyncReadyCallback) on_list_for_delete, data);
      break;

    case COMMAND_CLEAR:
      g_print ("Clearing the chip storage (all templates!)...\n");
      fp_device_clear_storage (device, data->cancellable,
                               (GAsyncReadyCallback) on_clear_done, data);
      break;
    }
}

static void
on_device_opened (FpDevice     *device,
                  GAsyncResult *result,
                  gpointer      user_data)
{
  SmokeData *data = user_data;
  g_autoptr(GError) error = NULL;

  if (!fp_device_open_finish (device, result, &error))
    {
      g_warning ("Failed to open device: %s", error->message);
      quit (device, data);
      return;
    }

  start_action (device, data);
}

int
main (int   argc,
      char *argv[])
{
  g_autoptr(FpContext) ctx = NULL;
  g_autoptr(SmokeData) data = NULL;
  GPtrArray *devices;
  FpDevice *device = NULL;
  const char *command = "info";
  guint i;

  data = g_new0 (SmokeData, 1);
  data->ret_value = EXIT_FAILURE;
  data->timeout_seconds = 20;

  if (argc > 1)
    command = argv[1];

  if (g_strcmp0 (command, "info") == 0)
    data->command = COMMAND_INFO;
  else if (g_strcmp0 (command, "verify") == 0)
    data->command = COMMAND_VERIFY;
  else if (g_strcmp0 (command, "identify") == 0)
    data->command = COMMAND_IDENTIFY;
  else if (g_strcmp0 (command, "enroll") == 0)
    data->command = COMMAND_ENROLL;
  else if (g_strcmp0 (command, "list") == 0)
    data->command = COMMAND_LIST;
  else if (g_strcmp0 (command, "delete") == 0)
    data->command = COMMAND_DELETE;
  else if (g_strcmp0 (command, "clear") == 0)
    data->command = COMMAND_CLEAR;
  else
    {
      g_printerr ("Usage: %s [info|verify|identify|enroll|list|delete <slot>|clear] "
                  "[timeout seconds]\n", argv[0]);
      return EXIT_FAILURE;
    }

  if (data->command == COMMAND_DELETE)
    {
      if (argc < 3)
        {
          g_printerr ("delete needs a slot number\n");
          return EXIT_FAILURE;
        }
      data->slot = (guint) MAX (0, atoi (argv[2]));
      data->slot_given = TRUE;
      if (argc > 3)
        data->timeout_seconds = (guint) MAX (1, atoi (argv[3]));
    }
  else
    {
      int arg;

      for (arg = 2; arg < argc; arg++)
        {
          if (g_strcmp0 (argv[arg], "--slot") == 0 && arg + 1 < argc)
            {
              /* Note: increment first, MAX() would evaluate the argument twice. */
              arg++;
              data->slot = (guint) MAX (0, atoi (argv[arg]));
              data->slot_given = TRUE;
            }
          else if (atoi (argv[arg]) > 0)
            {
              data->timeout_seconds = (guint) atoi (argv[arg]);
            }
        }
    }

  ctx = fp_context_new ();
  devices = fp_context_get_devices (ctx);
  if (!devices || devices->len == 0)
    {
      g_printerr ("No fingerprint devices found.\n");
      return EXIT_FAILURE;
    }

  for (i = 0; i < devices->len; i++)
    {
      FpDevice *candidate = g_ptr_array_index (devices, i);

      if (g_strcmp0 (fp_device_get_driver (candidate), "omsmoc") == 0)
        {
          device = candidate;
          break;
        }
    }

  if (!device)
    {
      g_printerr ("No omsmoc device found (is the module connected?).\n");
      return EXIT_FAILURE;
    }

  data->loop = g_main_loop_new (NULL, FALSE);
  data->cancellable = g_cancellable_new ();
  g_unix_signal_add_full (G_PRIORITY_HIGH, SIGINT, on_sigint, data, NULL);

  fp_device_open (device, data->cancellable,
                  (GAsyncReadyCallback) on_device_opened, data);
  g_main_loop_run (data->loop);

  return data->ret_value;
}
