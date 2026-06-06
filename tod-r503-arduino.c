#define FP_COMPONENT "r503_arduino"

#include "tod-r503-arduino.h"
#include "fpi-device.h"
#include "fpi-print.h"

#include <gio/gio.h>
#include <gio/gunixsocketaddress.h>
#include <string.h>

#define R503_ARDUINOD_SOCKET "/run/r503-arduinod.sock"
#define SOCKET_FLUSH_TIMEOUT_MS 100

typedef struct _FpiDeviceR503Arduino FpiDeviceR503Arduino;

typedef struct {
  gchar *id_str;
  gchar *username;
  FpFinger finger;
} R503PrintRecord;

static void r503_print_record_free(R503PrintRecord *rec) {
  if (!rec)
    return;
  g_free(rec->id_str);
  g_free(rec->username);
  g_free(rec);
}

struct _FpiDeviceR503Arduino {
  FpDevice parent;

  GSocketConnection *connection;
  GDataInputStream *input;
  GOutputStream *output;

  GHashTable *prints;
};

typedef struct {
  FpDeviceClass parent;
} FpiDeviceR503ArduinoClass;

G_DEFINE_TYPE(FpiDeviceR503Arduino, fpi_device_r503_arduino, FP_TYPE_DEVICE)

#define FPI_TYPE_DEVICE_R503_ARDUINO (fpi_device_r503_arduino_get_type())
#define FPI_DEVICE_R503_ARDUINO(obj)                                           \
  (G_TYPE_CHECK_INSTANCE_CAST((obj), FPI_TYPE_DEVICE_R503_ARDUINO,             \
                              FpiDeviceR503Arduino))
#define FPI_IS_DEVICE_R503_ARDUINO(obj)                                        \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj), FPI_TYPE_DEVICE_R503_ARDUINO))

static const char *r503_get_socket_path(void) {
  const char *env = g_getenv("R503_ARDUINO_SOCKET");
  return env && env[0] != '\0' ? env : R503_ARDUINOD_SOCKET;
}

static GError *r503_make_error_proto(const char *msg) {
  return fpi_device_error_new_msg(FP_DEVICE_ERROR_PROTO, "%s", msg);
}

static GError *r503_err_to_gerror(const char *line) {
  if (!g_str_has_prefix(line, "ERR:"))
    return NULL;
  if (g_str_has_prefix(line, "ERR:TIMEOUT"))
    return fpi_device_retry_new(FP_DEVICE_RETRY_GENERAL);
  if (g_str_has_prefix(line, "ERR:CANCELLED"))
    return g_error_new(G_IO_ERROR, G_IO_ERROR_CANCELLED, "cancelled");
  if (g_str_has_prefix(line, "ERR:LIBRARY_FULL"))
    return fpi_device_error_new(FP_DEVICE_ERROR_DATA_FULL);
  if (g_str_has_prefix(line, "ERR:INVALID_ID"))
    return fpi_device_error_new(FP_DEVICE_ERROR_DATA_INVALID);
  if (g_str_has_prefix(line, "ERR:DELETE_FAILED"))
    return fpi_device_error_new(FP_DEVICE_ERROR_DATA_NOT_FOUND);
  return fpi_device_error_new_msg(FP_DEVICE_ERROR_GENERAL, "error: %s", line);
}

static FpPrint *r503_make_print_for_id(FpDevice *device, const char *id_str) {
  FpPrint *print = fp_print_new(device);
  fpi_print_set_type(print, FPI_PRINT_RAW);
  fpi_print_set_device_stored(print, TRUE);
  g_object_set(print, "fpi-data", g_variant_new_string(id_str), NULL);
  return print;
}

static void r503_trim_line(char *line) {
  if (!line)
    return;
  size_t len = strlen(line);
  if (len > 0 && line[len - 1] == '\r')
    line[len - 1] = '\0';
}

static void r503_disconnect(FpiDeviceR503Arduino *self) {
  g_clear_object(&self->input);
  g_clear_object(&self->output);
  g_clear_object(&self->connection);
  if (self->prints)
    g_hash_table_remove_all(self->prints);
}

typedef struct {
  FpiDeviceR503Arduino *self;
  GAsyncReadyCallback callback;
  gpointer user_data;
  GCancellable *cancellable;
  guint timeout_id;
} FlushData;

static void flush_data_free(FlushData *f) {
  if (f->timeout_id)
    g_source_remove(f->timeout_id);
  g_clear_object(&f->cancellable);
  g_free(f);
}

static gboolean flush_timeout_cb(gpointer user_data) {
  FlushData *f = user_data;
  f->timeout_id = 0;
  g_cancellable_cancel(f->cancellable);
  return G_SOURCE_REMOVE;
}

static void flush_read_cb(GObject *source_object, GAsyncResult *res,
                          gpointer user_data) {
  FlushData *f = user_data;
  GDataInputStream *stream = G_DATA_INPUT_STREAM(source_object);
  gsize len = 0;
  g_autofree char *line = NULL;
  g_autoptr(GError) error = NULL;

  line = g_data_input_stream_read_line_finish(stream, res, &len, &error);
  r503_trim_line(line);

  if (error || !line) {
    g_task_return_boolean(G_TASK(f->user_data), TRUE);
    return;
  }

  g_data_input_stream_read_line_async(f->self->input, G_PRIORITY_DEFAULT,
                                      f->cancellable, flush_read_cb, f);
}

static void r503_flush_async(FpiDeviceR503Arduino *self,
                             GCancellable *cancellable,
                             GAsyncReadyCallback callback, gpointer user_data) {
  GTask *task = g_task_new(self, cancellable, callback, user_data);
  FlushData *f = g_new0(FlushData, 1);

  f->self = self;
  f->cancellable = g_cancellable_new();
  if (cancellable)
    g_cancellable_connect(f->cancellable, G_CALLBACK(g_cancellable_cancel),
                          g_object_ref(cancellable), g_object_unref);
  f->user_data = g_object_ref(task);
  f->timeout_id = g_timeout_add(SOCKET_FLUSH_TIMEOUT_MS, flush_timeout_cb, f);

  if (!self->input) {
    g_task_return_boolean(task, TRUE);
    flush_data_free(f);
    return;
  }

  g_data_input_stream_read_line_async(self->input, G_PRIORITY_DEFAULT,
                                      f->cancellable, flush_read_cb, f);
}

static gboolean r503_flush_finish(FpiDeviceR503Arduino *self, GAsyncResult *res,
                                  GError **error) {
  (void)self;
  return g_task_propagate_boolean(G_TASK(res), error);
}

typedef struct {
  FpDevice *device;
  GTask *task;
  char *command;
} CmdWriteData;

static void cmd_write_data_free(CmdWriteData *c) {
  g_clear_object(&c->task);
  g_free(c->command);
  g_free(c);
}

static void cmd_write_cb(GObject *source_object, GAsyncResult *res,
                         gpointer user_data) {
  CmdWriteData *c = user_data;
  GOutputStream *stream = G_OUTPUT_STREAM(source_object);
  gsize written = 0;
  g_autoptr(GError) error = NULL;

  if (!g_output_stream_write_all_finish(stream, res, &written, &error)) {
    g_task_return_error(c->task, g_steal_pointer(&error));
    cmd_write_data_free(c);
    return;
  }

  g_task_return_boolean(c->task, TRUE);
  cmd_write_data_free(c);
}

static void r503_write_cmd_async(FpDevice *device, const char *command,
                                 GCancellable *cancellable,
                                 GAsyncReadyCallback callback,
                                 gpointer user_data) {
  GTask *task = g_task_new(device, cancellable, callback, user_data);
  CmdWriteData *c = g_new0(CmdWriteData, 1);
  FpiDeviceR503Arduino *self = FPI_DEVICE_R503_ARDUINO(device);

  c->device = device;
  c->task = g_object_ref(task);
  c->command = g_strdup(command);

  if (!self->output) {
    g_task_return_error(task, r503_make_error_proto("not connected"));
    cmd_write_data_free(c);
    return;
  }

  g_output_stream_write_all_async(self->output, command, strlen(command),
                                  G_PRIORITY_DEFAULT, cancellable, cmd_write_cb,
                                  c);
}

static gboolean r503_write_cmd_finish(FpDevice *device, GAsyncResult *res,
                                      GError **error) {
  (void)device;
  return g_task_propagate_boolean(G_TASK(res), error);
}

typedef struct {
  FpDevice *device;
  GAsyncReadyCallback callback;
  gpointer user_data;
} ReadData;

static void read_line_cb(GObject *source_object, GAsyncResult *res,
                         gpointer user_data) {
  ReadData *r = user_data;
  GDataInputStream *stream = G_DATA_INPUT_STREAM(source_object);
  gsize len = 0;
  g_autofree char *line = NULL;
  g_autoptr(GError) error = NULL;

  line = g_data_input_stream_read_line_finish(stream, res, &len, &error);
  r503_trim_line(line);

  GTask *task = g_task_new(r->device, NULL, r->callback, r->user_data);

  if (error)
    g_task_return_error(task, g_steal_pointer(&error));
  else if (!line)
    g_task_return_error(task,
                        r503_make_error_proto("unexpected EOF from daemon"));
  else {
    g_task_set_task_data(task, g_steal_pointer(&line), g_free);
    g_task_return_boolean(task, TRUE);
  }

  g_object_unref(task);
  g_free(r);
}

static void r503_read_line_async(FpDevice *device, GCancellable *cancellable,
                                 GAsyncReadyCallback callback,
                                 gpointer user_data) {
  FpiDeviceR503Arduino *self = FPI_DEVICE_R503_ARDUINO(device);
  ReadData *r = g_new0(ReadData, 1);

  r->device = device;
  r->callback = callback;
  r->user_data = user_data;

  if (!self->input) {
    GTask *task = g_task_new(device, NULL, callback, user_data);
    g_task_return_error(task, r503_make_error_proto("not connected"));
    g_object_unref(task);
    g_free(r);
    return;
  }

  g_data_input_stream_read_line_async(self->input, G_PRIORITY_DEFAULT,
                                      cancellable, read_line_cb, r);
}

static char *r503_read_line_finish(FpDevice *device, GAsyncResult *res,
                                   GError **error) {
  (void)device;
  if (!g_task_propagate_boolean(G_TASK(res), error))
    return NULL;
  return g_strdup(g_task_get_task_data(G_TASK(res)));
}

typedef void (*R503ResponseCb)(FpDevice *device, char *line, GError *error,
                               gpointer user_data);

typedef struct {
  FpDevice *device;
  char *command;
  R503ResponseCb cb;
  gpointer user_data;
  GCancellable *cancellable;
} OpChain;

static void op_chain_free(OpChain *op) {
  g_free(op->command);
  g_clear_object(&op->cancellable);
  g_free(op);
}

static void op_chain_do_cmd(OpChain *op);

static void op_chain_flush_cb(GObject *source, GAsyncResult *res,
                              gpointer user_data) {
  OpChain *op = user_data;
  GError *error = NULL;

  if (!r503_flush_finish(FPI_DEVICE_R503_ARDUINO(op->device), res, &error)) {
    op->cb(op->device, NULL, g_steal_pointer(&error), op->user_data);
    op_chain_free(op);
    return;
  }

  op_chain_do_cmd(op);
}

static void op_chain_read_cb(GObject *source, GAsyncResult *res,
                             gpointer user_data) {
  OpChain *op = user_data;
  g_autofree char *line = NULL;
  GError *error = NULL;

  line = r503_read_line_finish(op->device, res, &error);
  op->cb(op->device, line, g_steal_pointer(&error), op->user_data);
  op_chain_free(op);
}

static void op_chain_write_cb(GObject *source, GAsyncResult *res,
                              gpointer user_data) {
  OpChain *op = user_data;
  GError *error = NULL;

  if (!r503_write_cmd_finish(op->device, res, &error)) {
    op->cb(op->device, NULL, g_steal_pointer(&error), op->user_data);
    op_chain_free(op);
    return;
  }

  r503_read_line_async(op->device, op->cancellable, op_chain_read_cb, op);
}

static void op_chain_do_cmd(OpChain *op) {
  gsize cmd_len = strlen(op->command);
  g_autofree char *cmd_nl = g_malloc(cmd_len + 2);
  memcpy(cmd_nl, op->command, cmd_len);
  cmd_nl[cmd_len] = '\n';
  cmd_nl[cmd_len + 1] = '\0';

  r503_write_cmd_async(op->device, cmd_nl, op->cancellable, op_chain_write_cb,
                       op);
}

static void r503_do_cmd_async(FpDevice *device, const char *command,
                              GCancellable *cancellable,
                              R503ResponseCb callback, gpointer user_data) {
  OpChain *op = g_new0(OpChain, 1);
  op->device = device;
  op->command = g_strdup(command);
  op->cb = callback;
  op->user_data = user_data;
  op->cancellable = cancellable ? g_object_ref(cancellable) : NULL;

  r503_flush_async(FPI_DEVICE_R503_ARDUINO(device), op->cancellable,
                   op_chain_flush_cb, op);
}

static void r503_record_print(FpiDeviceR503Arduino *self, const char *id_str,
                              FpPrint *template) {
  R503PrintRecord *rec;
  const char *username;

  if (!self->prints)
    return;

  rec = g_new0(R503PrintRecord, 1);
  rec->id_str = g_strdup(id_str);
  rec->finger = fp_print_get_finger(template);
  username = fp_print_get_username(template);
  rec->username = g_strdup(username ? username : "");

  g_hash_table_insert(self->prints, rec->id_str, rec);
}

typedef struct {
  FpDevice *device;
  GPtrArray *res;
} ListBuildCtx;

static void r503_list_foreach_known(gpointer key, gpointer value,
                                    gpointer user_data) {
  ListBuildCtx *ctx = user_data;
  R503PrintRecord *rec = value;
  FpPrint *print = r503_make_print_for_id(ctx->device, rec->id_str);

  if (rec->username && rec->username[0] != '\0')
    fp_print_set_username(print, rec->username);
  fp_print_set_finger(print, rec->finger);

  g_ptr_array_add(ctx->res, print);
}

static GPtrArray *r503_parse_list_response(FpDevice *device, const char *line,
                                           GError **error) {
  GPtrArray *prints = g_ptr_array_new_with_free_func(g_object_unref);

  if (!g_str_has_prefix(line, "OK:LIST")) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                "unexpected LIST response: %s", line);
    return prints;
  }

  const char *p = line + strlen("OK:LIST");
  if (*p == '\0')
    return prints;

  if (*p != ',') {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                "malformed LIST response: %s", line);
    return prints;
  }
  p++;

  while (*p != '\0') {
    char *end;
    gint64 id = g_ascii_strtoll(p, &end, 10);
    if (end == p)
      break;

    gchar *id_str = g_strdup_printf("%" G_GINT64_FORMAT, id);
    g_ptr_array_add(prints, r503_make_print_for_id(device, id_str));
    g_free(id_str);

    p = end;
    if (*p == ',')
      p++;
  }

  return prints;
}

static gboolean r503_parse_enroll_response(const char *line, gint64 *out_id,
                                           GError **error) {
  if (!g_str_has_prefix(line, "OK:ENROLLED,ID,")) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                "unexpected ENROLL response: %s", line);
    return FALSE;
  }
  *out_id = g_ascii_strtoll(line + strlen("OK:ENROLLED,ID,"), NULL, 10);
  return TRUE;
}

static gboolean r503_parse_match_id(const char *line, gint64 *out_id) {
  if (!g_str_has_prefix(line, "OK:VERIFIED,ID,"))
    return FALSE;
  *out_id = g_ascii_strtoll(line + strlen("OK:VERIFIED,ID,"), NULL, 10);
  return TRUE;
}

static FpPrint *r503_find_print_by_id(GPtrArray *prints, gint64 id) {
  for (guint i = 0; i < prints->len; i++) {
    FpPrint *print = g_ptr_array_index(prints, i);
    g_autoptr(GVariant) data = NULL;
    g_object_get(print, "fpi-data", &data, NULL);
    if (data) {
      const char *id_str = g_variant_get_string(data, NULL);
      if (g_ascii_strtoll(id_str, NULL, 10) == id)
        return print;
    }
  }
  return NULL;
}

static void r503_open_cb(GObject *source, GAsyncResult *res,
                         gpointer user_data) {
  FpDevice *device = FP_DEVICE(user_data);
  FpiDeviceR503Arduino *self = FPI_DEVICE_R503_ARDUINO(device);
  GSocketClient *client = G_SOCKET_CLIENT(source);
  g_autoptr(GError) error = NULL;

  self->connection = g_socket_client_connect_finish(client, res, &error);
  if (!self->connection) {
    fpi_device_open_complete(device, g_steal_pointer(&error));
    return;
  }

  self->input = g_data_input_stream_new(
      g_io_stream_get_input_stream(G_IO_STREAM(self->connection)));
  g_data_input_stream_set_newline_type(self->input,
                                       G_DATA_STREAM_NEWLINE_TYPE_LF);

  self->output = g_object_ref(
      g_io_stream_get_output_stream(G_IO_STREAM(self->connection)));

  fpi_device_open_complete(device, NULL);
}

static void r503_open(FpDevice *device) {
  g_autoptr(GSocketClient) client = g_socket_client_new();
  GSocketAddress *addr =
      G_SOCKET_ADDRESS(g_unix_socket_address_new(r503_get_socket_path()));

  g_socket_client_connect_async(client, G_SOCKET_CONNECTABLE(addr),
                                fpi_device_get_cancellable(device),
                                r503_open_cb, device);
  g_object_unref(addr);
}

static void r503_close(FpDevice *device) {
  r503_disconnect(FPI_DEVICE_R503_ARDUINO(device));
  fpi_device_close_complete(device, NULL);
}

static void r503_probe(FpDevice *device) {
  fpi_device_probe_complete(device, "r503-arduino",
                            "R503 Arduino Fingerprint Scanner", NULL);
}

typedef struct {
  FpDevice *device;
} EnrollCtx;

static void enroll_step_cb(GObject *source, GAsyncResult *res,
                           gpointer user_data);
static void enroll_step(EnrollCtx *ctx) {
  r503_read_line_async(ctx->device, fpi_device_get_cancellable(ctx->device),
                       enroll_step_cb, ctx);
}

static void enroll_step_cb(GObject *source, GAsyncResult *res,
                           gpointer user_data) {
  g_autofree char *line = NULL;
  g_autoptr(GError) error = NULL;
  EnrollCtx *ctx = user_data;
  FpDevice *device = ctx->device;

  line = r503_read_line_finish(device, res, &error);
  if (!line) {
    fpi_device_enroll_complete(device, NULL, g_steal_pointer(&error));
    g_free(ctx);
    return;
  }

  if (g_str_has_prefix(line, "OK:PLACE_FINGER")) {
    fpi_device_report_finger_status_changes(device, FP_FINGER_STATUS_NEEDED,
                                            FP_FINGER_STATUS_NONE);
    enroll_step(ctx);
    return;
  }

  if (g_str_has_prefix(line, "OK:REMOVE_FINGER")) {
    fpi_device_report_finger_status_changes(device, FP_FINGER_STATUS_NONE,
                                            FP_FINGER_STATUS_PRESENT);
    fpi_device_enroll_progress(device, 1, NULL, NULL);
    enroll_step(ctx);
    return;
  }

  if (g_str_has_prefix(line, "OK:ENROLLED,ID,")) {
    gint64 id;
    FpPrint *print;
    fpi_device_get_enroll_data(device, &print);
    if (!print || !r503_parse_enroll_response(line, &id, &error)) {
      fpi_device_enroll_complete(device, NULL, g_steal_pointer(&error));
      g_free(ctx);
      return;
    }
    gchar *id_str = g_strdup_printf("%" G_GINT64_FORMAT, id);
    fpi_print_set_type(print, FPI_PRINT_RAW);
    fpi_print_set_device_stored(print, TRUE);
    g_object_set(print, "fpi-data", g_variant_new_string(id_str), NULL);
    r503_record_print(FPI_DEVICE_R503_ARDUINO(device), id_str, print);
    fpi_device_report_finger_status_changes(device, FP_FINGER_STATUS_NONE,
                                            FP_FINGER_STATUS_PRESENT);
    fpi_device_enroll_complete(device, g_object_ref(print), NULL);
    g_free(id_str);
    g_free(ctx);
    return;
  }

  if (g_str_has_prefix(line, "ERR:")) {
    GError *err = r503_err_to_gerror(line);
    fpi_device_enroll_complete(device, NULL, g_steal_pointer(&err));
    g_free(ctx);
    return;
  }

  enroll_step(ctx);
}

static void r503_enroll_cmd_cb(FpDevice *device, char *line, GError *error,
                               gpointer user_data) {
  (void)user_data;

  if (error || !line) {
    if (!error)
      error = r503_make_error_proto("no response to ENROLL");
    fpi_device_enroll_complete(device, NULL, g_steal_pointer(&error));
    return;
  }

  if (g_str_has_prefix(line, "OK:PLACE_FINGER")) {
    EnrollCtx *ctx = g_new0(EnrollCtx, 1);
    ctx->device = device;
    fpi_device_report_finger_status_changes(device, FP_FINGER_STATUS_NEEDED,
                                            FP_FINGER_STATUS_NONE);
    enroll_step(ctx);
    return;
  }

  if (g_str_has_prefix(line, "ERR:")) {
    GError *err = r503_err_to_gerror(line);
    fpi_device_enroll_complete(device, NULL, g_steal_pointer(&err));
    return;
  }

  error = r503_make_error_proto("unexpected ENROLL response");
  fpi_device_enroll_complete(device, NULL, g_steal_pointer(&error));
}

static void r503_enroll(FpDevice *device) {
  r503_do_cmd_async(device, "ENROLL", fpi_device_get_cancellable(device),
                    r503_enroll_cmd_cb, device);
}

typedef struct {
  FpDevice *device;
} VerifyCtx;

static void verify_step_cb(GObject *source, GAsyncResult *res,
                           gpointer user_data);
static void verify_step(VerifyCtx *ctx) {
  r503_read_line_async(ctx->device, fpi_device_get_cancellable(ctx->device),
                       verify_step_cb, ctx);
}

static void verify_step_cb(GObject *source, GAsyncResult *res,
                           gpointer user_data) {
  g_autofree char *line = NULL;
  g_autoptr(GError) error = NULL;
  VerifyCtx *ctx = user_data;
  FpDevice *device = ctx->device;

  line = r503_read_line_finish(device, res, &error);
  if (!line) {
    fpi_device_verify_report(device, FPI_MATCH_ERROR, NULL,
                             g_steal_pointer(&error));
    fpi_device_verify_complete(device, NULL);
    g_free(ctx);
    return;
  }

  if (g_str_has_prefix(line, "OK:PLACE_FINGER")) {
    fpi_device_report_finger_status_changes(device, FP_FINGER_STATUS_NEEDED,
                                            FP_FINGER_STATUS_NONE);
    verify_step(ctx);
    return;
  }

  gint64 id;
  if (r503_parse_match_id(line, &id)) {
    FpPrint *verify_print = NULL;
    fpi_device_get_verify_data(device, &verify_print);
    fpi_device_report_finger_status_changes(device, FP_FINGER_STATUS_NONE,
                                            FP_FINGER_STATUS_PRESENT);
    fpi_device_verify_report(device, FPI_MATCH_SUCCESS, verify_print, NULL);
    fpi_device_verify_complete(device, NULL);
    g_free(ctx);
    return;
  }

  if (g_strcmp0(line, "ERR:NO_MATCH") == 0) {
    fpi_device_report_finger_status_changes(device, FP_FINGER_STATUS_NONE,
                                            FP_FINGER_STATUS_PRESENT);
    fpi_device_verify_report(device, FPI_MATCH_FAIL, NULL, NULL);
    fpi_device_verify_complete(device, NULL);
    g_free(ctx);
    return;
  }

  if (g_str_has_prefix(line, "ERR:")) {
    GError *err = r503_err_to_gerror(line);
    fpi_device_verify_report(device, FPI_MATCH_ERROR, NULL,
                             g_steal_pointer(&err));
    fpi_device_verify_complete(device, NULL);
    g_free(ctx);
    return;
  }

  verify_step(ctx);
}

static void r503_verify_cmd_cb(FpDevice *device, char *line, GError *error,
                               gpointer user_data) {
  (void)user_data;
  VerifyCtx *ctx = g_new0(VerifyCtx, 1);
  ctx->device = device;

  if (error || !line) {
    if (!error)
      error = r503_make_error_proto("no response to VERIFY");
    fpi_device_verify_report(device, FPI_MATCH_ERROR, NULL,
                             g_steal_pointer(&error));
    fpi_device_verify_complete(device, NULL);
    g_free(ctx);
    return;
  }

  if (g_str_has_prefix(line, "OK:PLACE_FINGER")) {
    fpi_device_report_finger_status_changes(device, FP_FINGER_STATUS_NEEDED,
                                            FP_FINGER_STATUS_NONE);
    verify_step(ctx);
    return;
  }

  error = r503_make_error_proto("unexpected VERIFY response");
  fpi_device_verify_report(device, FPI_MATCH_ERROR, NULL,
                           g_steal_pointer(&error));
  fpi_device_verify_complete(device, NULL);
  g_free(ctx);
}

static void r503_verify(FpDevice *device) {
  r503_do_cmd_async(device, "VERIFY", fpi_device_get_cancellable(device),
                    r503_verify_cmd_cb, device);
}

typedef struct {
  FpDevice *device;
  GPtrArray *gallery;
} IdentifyCtx;

static void identify_step_cb(GObject *source, GAsyncResult *res,
                             gpointer user_data);
static void identify_step(IdentifyCtx *ctx) {
  r503_read_line_async(ctx->device, fpi_device_get_cancellable(ctx->device),
                       identify_step_cb, ctx);
}

static void identify_step_cb(GObject *source, GAsyncResult *res,
                             gpointer user_data) {
  g_autofree char *line = NULL;
  g_autoptr(GError) error = NULL;
  IdentifyCtx *ctx = user_data;
  FpDevice *device = ctx->device;

  line = r503_read_line_finish(device, res, &error);
  if (!line) {
    fpi_device_identify_report(device, NULL, NULL, g_steal_pointer(&error));
    fpi_device_identify_complete(device, NULL);
    g_ptr_array_unref(ctx->gallery);
    g_free(ctx);
    return;
  }

  if (g_str_has_prefix(line, "OK:PLACE_FINGER")) {
    fpi_device_report_finger_status_changes(device, FP_FINGER_STATUS_NEEDED,
                                            FP_FINGER_STATUS_NONE);
    identify_step(ctx);
    return;
  }

  gint64 id;
  if (r503_parse_match_id(line, &id)) {
    FpPrint *match = r503_find_print_by_id(ctx->gallery, id);
    gchar *id_str = g_strdup_printf("%" G_GINT64_FORMAT, id);
    FpPrint *new_print = r503_make_print_for_id(device, id_str);
    fpi_device_report_finger_status_changes(device, FP_FINGER_STATUS_NONE,
                                            FP_FINGER_STATUS_PRESENT);
    fpi_device_identify_report(device, match, new_print, NULL);
    fpi_device_identify_complete(device, NULL);
    g_free(id_str);
    g_ptr_array_unref(ctx->gallery);
    g_free(ctx);
    return;
  }

  if (g_strcmp0(line, "ERR:NO_MATCH") == 0) {
    fpi_device_report_finger_status_changes(device, FP_FINGER_STATUS_NONE,
                                            FP_FINGER_STATUS_PRESENT);
    fpi_device_identify_report(device, NULL, NULL, NULL);
    fpi_device_identify_complete(device, NULL);
    g_ptr_array_unref(ctx->gallery);
    g_free(ctx);
    return;
  }

  if (g_str_has_prefix(line, "ERR:")) {
    GError *err = r503_err_to_gerror(line);
    fpi_device_identify_report(device, NULL, NULL, g_steal_pointer(&err));
    fpi_device_identify_complete(device, NULL);
    g_ptr_array_unref(ctx->gallery);
    g_free(ctx);
    return;
  }

  identify_step(ctx);
}

static void r503_identify_cmd_cb(FpDevice *device, char *line, GError *error,
                                 gpointer user_data) {
  (void)user_data;
  IdentifyCtx *ctx = g_new0(IdentifyCtx, 1);
  ctx->device = device;
  fpi_device_get_identify_data(device, &ctx->gallery);
  if (ctx->gallery)
    g_ptr_array_ref(ctx->gallery);
  else
    ctx->gallery = g_ptr_array_new();

  if (error || !line) {
    if (!error)
      error = r503_make_error_proto("no response to IDENTIFY");
    fpi_device_identify_report(device, NULL, NULL, g_steal_pointer(&error));
    fpi_device_identify_complete(device, NULL);
    g_ptr_array_unref(ctx->gallery);
    g_free(ctx);
    return;
  }

  if (g_str_has_prefix(line, "OK:PLACE_FINGER")) {
    fpi_device_report_finger_status_changes(device, FP_FINGER_STATUS_NEEDED,
                                            FP_FINGER_STATUS_NONE);
    identify_step(ctx);
    return;
  }

  error = r503_make_error_proto("unexpected IDENTIFY response");
  fpi_device_identify_report(device, NULL, NULL, g_steal_pointer(&error));
  fpi_device_identify_complete(device, NULL);
  g_ptr_array_unref(ctx->gallery);
  g_free(ctx);
}

static void r503_identify(FpDevice *device) {
  r503_do_cmd_async(device, "VERIFY", fpi_device_get_cancellable(device),
                    r503_identify_cmd_cb, device);
}

static void r503_list_cb(FpDevice *device, char *line, GError *error,
                         gpointer user_data) {
  (void)user_data;
  FpiDeviceR503Arduino *self = FPI_DEVICE_R503_ARDUINO(device);

  if (self->prints && g_hash_table_size(self->prints) > 0) {
    ListBuildCtx ctx = {.device = device,
                        .res = g_ptr_array_new_with_free_func(g_object_unref)};
    g_hash_table_foreach(self->prints, r503_list_foreach_known, &ctx);
    fpi_device_list_complete(device, ctx.res, NULL);
    return;
  }

  if (error || !line) {
    if (!error)
      error = r503_make_error_proto("no response to LIST");
    fpi_device_list_complete(device, NULL, g_steal_pointer(&error));
    return;
  }

  g_autoptr(GPtrArray) prints = r503_parse_list_response(device, line, &error);
  if (error) {
    fpi_device_list_complete(device, NULL, g_steal_pointer(&error));
    return;
  }

  fpi_device_list_complete(device, g_steal_pointer(&prints), NULL);
}

static void r503_list(FpDevice *device) {
  r503_do_cmd_async(device, "LIST", fpi_device_get_cancellable(device),
                    r503_list_cb, device);
}

static void r503_delete_cb(FpDevice *device, char *line, GError *error,
                           gpointer user_data) {
  const char *id_str = user_data;

  if (error || !line) {
    if (!error)
      error = r503_make_error_proto("no response to DELETE");
    fpi_device_delete_complete(device, g_steal_pointer(&error));
    return;
  }

  if (g_str_has_prefix(line, "OK:DELETED,ID,")) {
    FpiDeviceR503Arduino *self = FPI_DEVICE_R503_ARDUINO(device);
    if (self->prints)
      g_hash_table_remove(self->prints, id_str);
    fpi_device_delete_complete(device, NULL);
    return;
  }

  GError *err = r503_err_to_gerror(line);
  if (err)
    fpi_device_delete_complete(device, g_steal_pointer(&err));
  else {
    err = fpi_device_error_new_msg(FP_DEVICE_ERROR_GENERAL, "delete error: %s",
                                   line);
    fpi_device_delete_complete(device, g_steal_pointer(&err));
  }
}

static void r503_delete(FpDevice *device) {
  FpPrint *print = NULL;
  g_autoptr(GVariant) data = NULL;
  const char *id_str = NULL;
  g_autofree char *cmd = NULL;

  fpi_device_get_delete_data(device, &print);
  g_object_get(print, "fpi-data", &data, NULL);
  if (data)
    id_str = g_variant_get_string(data, NULL);

  if (!id_str || id_str[0] == '\0') {
    fpi_device_delete_complete(
        device, fpi_device_error_new(FP_DEVICE_ERROR_DATA_INVALID));
    return;
  }

  cmd = g_strdup_printf("DELETE:%s", id_str);
  r503_do_cmd_async(device, cmd, fpi_device_get_cancellable(device),
                    r503_delete_cb, (gpointer)id_str);
}

static void r503_clear_cb(FpDevice *device, char *line, GError *error,
                          gpointer user_data) {
  (void)user_data;

  if (error || !line) {
    if (!error)
      error = r503_make_error_proto("no response to CLEAR");
    fpi_device_clear_storage_complete(device, g_steal_pointer(&error));
    return;
  }

  if (g_strcmp0(line, "OK:CLEARED") == 0) {
    FpiDeviceR503Arduino *self = FPI_DEVICE_R503_ARDUINO(device);
    if (self->prints)
      g_hash_table_remove_all(self->prints);
    fpi_device_clear_storage_complete(device, NULL);
    return;
  }

  error = fpi_device_error_new_msg(FP_DEVICE_ERROR_GENERAL, "clear error: %s",
                                   line);
  fpi_device_clear_storage_complete(device, g_steal_pointer(&error));
}

static void r503_clear_storage(FpDevice *device) {
  r503_do_cmd_async(device, "CLEAR", fpi_device_get_cancellable(device),
                    r503_clear_cb, device);
}

static void r503_cancel(FpDevice *device) {
  FpiDeviceR503Arduino *self = FPI_DEVICE_R503_ARDUINO(device);
  if (!self->output)
    return;
  g_output_stream_write_all(self->output, "CANCEL\n", 7, NULL, NULL, NULL);
}

static const FpIdEntry driver_ids[] = {
    {.virtual_envvar = "R503_ARDUINO_DEVICE"}, {.virtual_envvar = NULL}};

static void fpi_device_r503_arduino_finalize(GObject *object);

static void
fpi_device_r503_arduino_class_init(FpiDeviceR503ArduinoClass *klass) {
  FpDeviceClass *dev_class = FP_DEVICE_CLASS(klass);
  GObjectClass *object_class = G_OBJECT_CLASS(klass);

  object_class->finalize = fpi_device_r503_arduino_finalize;

  dev_class->id = FP_COMPONENT;
  dev_class->full_name = "R503 Arduino Fingerprint Scanner";
  dev_class->type = FP_DEVICE_TYPE_VIRTUAL;
  dev_class->id_table = driver_ids;
  dev_class->nr_enroll_stages = 1;
  dev_class->scan_type = FP_SCAN_TYPE_PRESS;

  dev_class->probe = r503_probe;
  dev_class->open = r503_open;
  dev_class->close = r503_close;
  dev_class->enroll = r503_enroll;
  dev_class->verify = r503_verify;
  dev_class->identify = r503_identify;
  dev_class->list = r503_list;
  dev_class->delete = r503_delete;
  dev_class->clear_storage = r503_clear_storage;
  dev_class->cancel = r503_cancel;

  fpi_device_class_auto_initialize_features(dev_class);
}

static void fpi_device_r503_arduino_init(FpiDeviceR503Arduino *self) {
  self->prints = g_hash_table_new_full(g_str_hash, g_str_equal, NULL,
                                       (GDestroyNotify)r503_print_record_free);
}

static void fpi_device_r503_arduino_finalize(GObject *object) {
  FpiDeviceR503Arduino *self = FPI_DEVICE_R503_ARDUINO(object);
  g_clear_pointer(&self->prints, g_hash_table_destroy);
  G_OBJECT_CLASS(fpi_device_r503_arduino_parent_class)->finalize(object);
}

G_MODULE_EXPORT GType fpi_tod_shared_driver_get_type(void) {
  return fpi_device_r503_arduino_get_type();
}
