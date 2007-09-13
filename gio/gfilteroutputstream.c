#include <config.h>
#include <glib/gi18n-lib.h>


#include "gfilteroutputstream.h"
#include "goutputstream.h"

/* TODO: Real P_() */
#define P_(_x) (_x)

struct _GFilterOutputStreamPrivate {
  int dummy;
};

enum {
  PROP_0,
  PROP_BASE_STREAM
};

static void     g_filter_output_stream_set_property (GObject      *object,
                                                     guint         prop_id,
                                                     const GValue *value,
                                                     GParamSpec   *pspec);

static void     g_filter_output_stream_get_property (GObject    *object,
                                                     guint       prop_id,
                                                     GValue     *value,
                                                     GParamSpec *pspec);
static void     g_filter_output_stream_dispose      (GObject *object);


static gssize   g_filter_output_stream_write        (GOutputStream *stream,
                                                     void          *buffer,
                                                     gsize          count,
                                                     GCancellable  *cancellable,
                                                     GError       **error);
static gboolean g_filter_output_stream_flush        (GOutputStream    *stream,
                                                     GCancellable  *cancellable,
                                                     GError          **error);
static gboolean g_filter_output_stream_close        (GOutputStream  *stream,
                                                     GCancellable   *cancellable,
                                                     GError        **error);
static void     g_filter_output_stream_write_async  (GOutputStream        *stream,
                                                     void                 *buffer,
                                                     gsize                 count,
                                                     int                   io_priority,
                                                     GCancellable         *cancellable,
                                                     GAsyncReadyCallback   callback,
                                                     gpointer              data);
static gssize   g_filter_output_stream_write_finish (GOutputStream        *stream,
                                                     GAsyncResult         *result,
                                                     GError              **error);
static void     g_filter_output_stream_flush_async  (GOutputStream        *stream,
                                                     int                   io_priority,
                                                     GCancellable         *cancellable,
                                                     GAsyncReadyCallback   callback,
                                                     gpointer              data);
static gboolean g_filter_output_stream_flush_finish (GOutputStream        *stream,
                                                     GAsyncResult         *result,
                                                     GError              **error);
static void     g_filter_output_stream_close_async  (GOutputStream        *stream,
                                                     int                   io_priority,
                                                     GCancellable         *cancellable,
                                                     GAsyncReadyCallback   callback,
                                                     gpointer              data);
static gboolean g_filter_output_stream_close_finish (GOutputStream        *stream,
                                                     GAsyncResult         *result,
                                                     GError              **error);



G_DEFINE_TYPE (GFilterOutputStream, g_filter_output_stream, G_TYPE_OUTPUT_STREAM)



static void
g_filter_output_stream_class_init (GFilterOutputStreamClass *klass)
{
  GObjectClass *object_class;
  GOutputStreamClass *ostream_class;

  g_type_class_add_private (klass, sizeof (GFilterOutputStreamPrivate));

  object_class = G_OBJECT_CLASS (klass);
  object_class->get_property = g_filter_output_stream_get_property;
  object_class->set_property = g_filter_output_stream_set_property;
  object_class->dispose      = g_filter_output_stream_dispose;
    
  ostream_class = G_OUTPUT_STREAM_CLASS (klass);
  ostream_class->write = g_filter_output_stream_write;
  ostream_class->flush = g_filter_output_stream_flush;
  ostream_class->close = g_filter_output_stream_close;
  ostream_class->write_async  = g_filter_output_stream_write_async;
  ostream_class->write_finish = g_filter_output_stream_write_finish;
  ostream_class->flush_async  = g_filter_output_stream_flush_async;
  ostream_class->flush_finish = g_filter_output_stream_flush_finish;
  ostream_class->close_async  = g_filter_output_stream_close_async;
  ostream_class->close_finish = g_filter_output_stream_close_finish;

  g_object_class_install_property (object_class,
                                   PROP_BASE_STREAM,
                                   g_param_spec_object ("base-stream",
                                                         P_("The Filter Base Stream"),
                                                         P_("The underlying base stream the io ops will be done on"),
                                                         G_TYPE_OUTPUT_STREAM,
                                                         G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | 
                                                         G_PARAM_STATIC_NAME|G_PARAM_STATIC_NICK|G_PARAM_STATIC_BLURB));

}

static void
g_filter_output_stream_set_property (GObject         *object,
                                     guint            prop_id,
                                     const GValue    *value,
                                     GParamSpec      *pspec)
{
  GFilterOutputStream *filter_stream;
  GObject *obj;

  filter_stream = G_FILTER_OUTPUT_STREAM (object);

  switch (prop_id) 
    {
    case PROP_BASE_STREAM:
      obj = g_value_dup_object (value);
      filter_stream->base_stream = G_OUTPUT_STREAM (obj);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }

}

static void
g_filter_output_stream_get_property (GObject    *object,
                                     guint       prop_id,
                                     GValue     *value,
                                     GParamSpec *pspec)
{
  GFilterOutputStream *filter_stream;

  filter_stream = G_FILTER_OUTPUT_STREAM (object);

  switch (prop_id)
    {
    case PROP_BASE_STREAM:
      g_value_set_object (value, filter_stream->base_stream);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }

}

static void
g_filter_output_stream_dispose (GObject *object)
{
  GFilterOutputStream        *stream;
  GFilterOutputStreamPrivate *priv;

  stream = G_FILTER_OUTPUT_STREAM (object);
  priv = stream->priv;

  if (stream->base_stream)
    {
      g_object_unref (stream->base_stream);
      stream->base_stream = NULL;
    }

  G_OBJECT_CLASS (g_filter_output_stream_parent_class)->dispose (object);
}


static void
g_filter_output_stream_init (GFilterOutputStream *stream)
{
  stream->priv = G_TYPE_INSTANCE_GET_PRIVATE (stream,
                                              G_TYPE_FILTER_OUTPUT_STREAM,
                                              GFilterOutputStreamPrivate);
}


GOutputStream *
g_filter_output_stream_get_base_stream (GFilterOutputStream *stream)
{
  return stream->base_stream;
}

static gssize
g_filter_output_stream_write (GOutputStream *stream,
                              void          *buffer,
                              gsize          count,
                              GCancellable  *cancellable,
                              GError       **error)
{
  GFilterOutputStream *filter_stream;
  gssize nwritten;

  filter_stream = G_FILTER_OUTPUT_STREAM (stream);

  nwritten = g_output_stream_write (filter_stream->base_stream,
                                    buffer,
                                    count,
                                    cancellable,
                                    error);

  return nwritten;
}

static gboolean
g_filter_output_stream_flush (GOutputStream    *stream,
                              GCancellable  *cancellable,
                              GError          **error)
{
  GFilterOutputStream *filter_stream;
  gboolean res;

  filter_stream = G_FILTER_OUTPUT_STREAM (stream);

  res = g_output_stream_flush (filter_stream->base_stream,
                               cancellable,
                               error);

  return res;
}

static gboolean
g_filter_output_stream_close (GOutputStream  *stream,
                              GCancellable   *cancellable,
                              GError        **error)
{
  GFilterOutputStream *filter_stream;
  gboolean res;

  filter_stream = G_FILTER_OUTPUT_STREAM (stream);

  res = g_output_stream_close (filter_stream->base_stream,
                               cancellable,
                               error);

  return res;
}

static void
g_filter_output_stream_write_async (GOutputStream        *stream,
                                    void                 *buffer,
                                    gsize                 count,
                                    int                   io_priority,
                                    GCancellable         *cancellable,
                                    GAsyncReadyCallback   callback,
                                    gpointer              data)
{
  GFilterOutputStream *filter_stream;

  filter_stream = G_FILTER_OUTPUT_STREAM (stream);

  g_output_stream_write_async (filter_stream->base_stream,
                               buffer,
                               count,
                               io_priority,
                               cancellable,
                               callback,
                               data);

}

static gssize
g_filter_output_stream_write_finish (GOutputStream        *stream,
                                     GAsyncResult         *result,
                                     GError              **error)
{
  GFilterOutputStream *filter_stream;
  gssize nwritten;

  filter_stream = G_FILTER_OUTPUT_STREAM (stream);

  nwritten = g_output_stream_write_finish (filter_stream->base_stream,
                                           result,
                                           error);

  return nwritten;
}

static void
g_filter_output_stream_flush_async (GOutputStream        *stream,
                                    int                   io_priority,
                                    GCancellable         *cancellable,
                                    GAsyncReadyCallback   callback,
                                    gpointer              data)
{
  GFilterOutputStream *filter_stream;

  filter_stream = G_FILTER_OUTPUT_STREAM (stream);

  g_output_stream_flush_async (filter_stream->base_stream,
                               io_priority,
                               cancellable,
                               callback,
                               data);
}

static gboolean
g_filter_output_stream_flush_finish (GOutputStream        *stream,
                                     GAsyncResult         *result,
                                     GError              **error)
{
  GFilterOutputStream *filter_stream;
  gboolean res;

  filter_stream = G_FILTER_OUTPUT_STREAM (stream);

  res = g_output_stream_flush_finish (filter_stream->base_stream,
                                      result,
                                      error);

  return res;
}

static void
g_filter_output_stream_close_async (GOutputStream        *stream,
                                    int                   io_priority,
                                    GCancellable         *cancellable,
                                    GAsyncReadyCallback   callback,
                                    gpointer              data)
{
  GFilterOutputStream *filter_stream;

  filter_stream = G_FILTER_OUTPUT_STREAM (stream);

  g_output_stream_close_async (filter_stream->base_stream,
                               io_priority,
                               cancellable,
                               callback,
                               data);
}

static gboolean
g_filter_output_stream_close_finish (GOutputStream        *stream,
                                     GAsyncResult         *result,
                                     GError              **error)
{
  GFilterOutputStream *filter_stream;
  gboolean res;

  filter_stream = G_FILTER_OUTPUT_STREAM (stream);

  res = g_output_stream_close_finish (filter_stream->base_stream,
                                      result,
                                      error);

  return res;
}


/* vim: ts=2 sw=2 et */
