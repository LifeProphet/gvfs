#include <config.h>

#include "gioscheduler.h"

struct _GIOJob {
  GSList *active_link;
  GIOJobFunc job_func;
  GIODataFunc cancel_func; /* Runs under job map lock */
  gpointer data;
  GDestroyNotify destroy_notify;
  
  GMainContext *callback_context;
  gint io_priority;
  GCancellable *cancellable;
};

G_LOCK_DEFINE_STATIC(active_jobs);
static GSList *active_jobs = NULL;

static GThreadPool *job_thread_pool = NULL;

static void io_job_thread (gpointer       data,
			   gpointer       user_data);

static gint
g_io_job_compare (gconstpointer  a,
		  gconstpointer  b,
		  gpointer       user_data)
{
  const GIOJob *aa = a;
  const GIOJob *bb = b;

  /* Cancelled jobs are set prio == -1, so that
     they are executed as quickly as possible */
  
  /* Lower value => higher priority */
  if (aa->io_priority < bb->io_priority)
    return -1;
  if (aa->io_priority == bb->io_priority)
    return 0;
  return 1;
}

static gpointer
init_scheduler (gpointer arg)
{
  if (job_thread_pool == NULL)
    {
      /* TODO: thread_pool_new can fail */
      job_thread_pool = g_thread_pool_new (io_job_thread,
					   NULL,
					   10,
					   FALSE,
					   NULL);
      g_thread_pool_set_sort_function (job_thread_pool,
				       g_io_job_compare,
				       NULL);
    }
  return NULL;
}

static void
io_job_thread (gpointer       data,
	       gpointer       user_data)
{
  GIOJob *job = data;
  GIOJob *other_job;
  GSList *l;
  gboolean resort_jobs;

  if (job->cancellable)
    g_push_current_cancellable (job->cancellable);
  job->job_func (job, job->cancellable, job->data);
  if (job->cancellable)
    g_pop_current_cancellable (job->cancellable);

  if (job->destroy_notify)
    job->destroy_notify (job->data);

  G_LOCK (active_jobs);
  active_jobs = g_slist_delete_link (active_jobs, job->active_link);
  
  resort_jobs = FALSE;
  for (l = active_jobs; l != NULL; l = l->next)
    {
      other_job = l->data;
      if (other_job->io_priority >= 0 &&
	  g_cancellable_is_cancelled (other_job->cancellable))
	{
	  other_job->io_priority = -1;
	  resort_jobs = TRUE;
	}
    }
  G_UNLOCK (active_jobs);

  if (job->cancellable)
    g_object_unref (job->cancellable);
  g_main_context_unref (job->callback_context);
  g_free (job);

  if (resort_jobs)
    g_thread_pool_set_sort_function (job_thread_pool,
				     g_io_job_compare,
				     NULL);
}

void
g_schedule_io_job (GIOJobFunc     job_func,
		   gpointer       data,
		   GDestroyNotify notify,
		   gint           io_priority,
		   GMainContext  *callback_context,
		   GCancellable  *cancellable)
{
  static GOnce once_init = G_ONCE_INIT;
  GIOJob *job;

  if (callback_context == NULL)
    callback_context = g_main_context_default ();
  
  job = g_new0 (GIOJob, 1);
  job->job_func = job_func;
  job->data = data;
  job->destroy_notify = notify;
  job->io_priority = io_priority;
  job->callback_context = g_main_context_ref (callback_context);
  /* TODO: Should we create this cancellation always?
   * If we do cancel_all_jobs works for all ops, but if we
   * don't we save some resources for non-cancellable jobs
   */
  if (cancellable)
    job->cancellable = g_object_ref (cancellable);
  else
    job->cancellable = g_cancellable_new ();

  G_LOCK (active_jobs);
  active_jobs = g_slist_prepend (active_jobs, job);
  job->active_link = active_jobs;
  G_UNLOCK (active_jobs);

  g_once (&once_init, init_scheduler, NULL);
  g_thread_pool_push (job_thread_pool, job, NULL);
}

void
g_cancel_all_io_jobs (void)
{
  GSList *cancellable_list, *l;
  
  G_LOCK (active_jobs);
  cancellable_list = NULL;
  for (l = active_jobs; l != NULL; l = l->next)
    {
      GIOJob *job = l->data;
      if (job->cancellable)
	cancellable_list = g_slist_prepend (cancellable_list,
					    g_object_ref (job->cancellable));
    }
  G_UNLOCK (active_jobs);

  for (l = cancellable_list; l != NULL; l = l->next)
    {
      GCancellable *c = l->data;
      g_cancellable_cancel (c);
      g_object_unref (c);
    }
  g_slist_free (cancellable_list);
}

typedef struct {
  GIODataFunc func;
  gpointer    data;
  GDestroyNotify notify;

  GMutex *ack_lock;
  GCond *ack_condition;
} MainLoopProxy;

static gboolean
mainloop_proxy_func (gpointer data)
{
  MainLoopProxy *proxy = data;

  proxy->func (proxy->data);

  if (proxy->ack_lock)
    {
      g_mutex_lock (proxy->ack_lock);
      g_cond_signal (proxy->ack_condition);
      g_mutex_unlock (proxy->ack_lock);
    }
  
  return FALSE;
}

static void
mainloop_proxy_free (MainLoopProxy *proxy)
{
  if (proxy->ack_lock)
    {
      g_mutex_free (proxy->ack_lock);
      g_cond_free (proxy->ack_condition);
    }
  
  g_free (proxy);
}

static void
mainloop_proxy_notify (gpointer data)
{
  MainLoopProxy *proxy = data;

  if (proxy->notify)
    proxy->notify (proxy->data);

  /* If nonblocking we free here, otherwise we free in io thread */
  if (proxy->ack_lock == NULL)
    mainloop_proxy_free (proxy);
}

void
g_io_job_send_to_mainloop (GIOJob        *job,
			   GIODataFunc    func,
			   gpointer       data,
			   GDestroyNotify notify,
			   gboolean       block)
{
  GSource *source;
  MainLoopProxy *proxy;
  guint id;

  proxy = g_new0 (MainLoopProxy, 1);
  proxy->func = func;
  proxy->data = data;
  proxy->notify = notify;
  if (block)
    {
      proxy->ack_lock = g_mutex_new ();
      proxy->ack_condition = g_cond_new ();
    }
  
  source = g_idle_source_new ();
  g_source_set_priority (source, G_PRIORITY_DEFAULT);

  g_source_set_callback (source, mainloop_proxy_func, proxy, mainloop_proxy_notify);

  if (block)
    g_mutex_lock (proxy->ack_lock);
		  
  id = g_source_attach (source, job->callback_context);
  g_source_unref (source);

  if (block) {
	g_cond_wait (proxy->ack_condition, proxy->ack_lock);
	g_mutex_unlock (proxy->ack_lock);

	/* destroy notify didn't free proxy */
	mainloop_proxy_free (proxy);
  }
}
