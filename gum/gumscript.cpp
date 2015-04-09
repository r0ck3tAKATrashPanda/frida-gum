/*
 * Copyright (C) 2010-2015 Ole André Vadla Ravnås <ole.andre.ravnas@tillitech.com>
 * Copyright (C) 2013 Karl Trygve Kalleberg <karltk@boblycat.org>
 *
 * Licence: wxWindows Library Licence, Version 3.1
 */

#include "gumscript.h"

#include "gum-init.h"
#include "gumscriptcore.h"
#include "gumscriptfile.h"
#include "gumscriptinstruction.h"
#include "gumscriptinterceptor.h"
#include "gumscriptmemory.h"
#include "gumscriptmodule.h"
#include "gumscriptprocess.h"
#include "gumscriptscope.h"
#include "gumscriptsocket.h"
#include "gumscriptstalker.h"
#include "gumscriptsymbol.h"
#include "gumscriptthread.h"

#include <gio/gio.h>
#include <string.h>
#include <v8-debug.h>
#include <v8-platform.h>

#define GUM_SCRIPT_V8_FLAGS "--harmony --expose-gc"
#define GUM_SCRIPT_RUNTIME_SOURCE_LINE_COUNT 1

using namespace v8;

enum
{
  PROP_0,
  PROP_SOURCE
};

struct _GumScriptPrivate
{
  gchar * source;

  Isolate * isolate;
  GumScriptCore core;
  GumScriptMemory memory;
  GumScriptProcess process;
  GumScriptThread thread;
  GumScriptModule module;
  GumScriptFile file;
  GumScriptSocket socket;
  GumScriptInterceptor interceptor;
  GumScriptStalker stalker;
  GumScriptSymbol symbol;
  GumScriptInstruction instruction;
  GumPersistent<Context>::type * context;
  GumPersistent<Script>::type * raw_script;
  gboolean loaded;
};

static gpointer gum_script_runtime_do_init (gpointer data);
static void gum_script_runtime_do_deinit (void);

static void gum_script_listener_iface_init (gpointer g_iface,
    gpointer iface_data);

static void gum_script_dispose (GObject * object);
static void gum_script_finalize (GObject * object);
static void gum_script_get_property (GObject * object, guint property_id,
    GValue * value, GParamSpec * pspec);
static void gum_script_set_property (GObject * object, guint property_id,
    const GValue * value, GParamSpec * pspec);
static void gum_script_destroy_context (GumScript * self);

static void gum_script_on_debug_message (const Debug::Message & message);

static void gum_script_on_enter (GumInvocationListener * listener,
    GumInvocationContext * context);
static void gum_script_on_leave (GumInvocationListener * listener,
    GumInvocationContext * context);

G_DEFINE_TYPE_EXTENDED (GumScript,
                        gum_script,
                        G_TYPE_OBJECT,
                        0,
                        G_IMPLEMENT_INTERFACE (GUM_TYPE_INVOCATION_LISTENER,
                            gum_script_listener_iface_init));

class GumScriptPlatform : public Platform
{
public:
  GumScriptPlatform (GMainContext * main_context,
                     GumScriptScheduler * scheduler)
    : main_context (main_context),
      scheduler (scheduler),
      start_time (g_get_monotonic_time ())
  {
  }

  GMainContext * GetMainContext () const
  {
    return main_context;
  }

  virtual void CallOnBackgroundThread (Task * task,
      ExpectedRuntime expected_runtime)
  {
    gum_script_scheduler_push_job (scheduler,
        reinterpret_cast<GumScriptJobFunc> (PerformTask),
        task,
        reinterpret_cast<GDestroyNotify> (DisposeTask),
        NULL);
  }

  virtual void CallOnForegroundThread (Isolate * isolate, Task * task)
  {
    GSource * source = g_idle_source_new ();
    g_source_set_priority (source, G_PRIORITY_HIGH);
    g_source_set_callback (source,
        reinterpret_cast<GSourceFunc> (PerformTaskWhenIdle),
        task,
        reinterpret_cast<GDestroyNotify> (DisposeTask));
    g_source_attach (source, main_context);
    g_source_unref (source);
  }

  virtual double MonotonicallyIncreasingTime ()
  {
    gint64 delta = g_get_monotonic_time () - start_time;
    return ((double) (delta / G_GINT64_CONSTANT (1000))) / 1000.0;
  }

private:
  static void PerformTask (Task * task)
  {
    task->Run ();
  }

  static gboolean PerformTaskWhenIdle (Task * task)
  {
    PerformTask (task);
    return FALSE;
  }

  static void DisposeTask (Task * task)
  {
    delete task;
  }

  GMainContext * main_context;
  GumScriptScheduler * scheduler;
  const gint64 start_time;

  GumScriptPlatform (const GumScriptPlatform &) V8_DELETE;
  void operator= (const GumScriptPlatform &) V8_DELETE;
};

static GumScriptScheduler * gum_scheduler = NULL;
static GumScriptPlatform * gum_platform = nullptr;

static GumScriptDebugMessageHandler gum_debug_handler = NULL;
static gpointer gum_debug_data = NULL;
static GDestroyNotify gum_debug_notify = NULL;
static GumPersistent<Context>::type * gum_debug_context = nullptr;

static Isolate *
gum_script_runtime_get_isolate (void)
{
  static GOnce init_once = G_ONCE_INIT;

  g_once (&init_once, gum_script_runtime_do_init, NULL);

  return static_cast<Isolate *> (init_once.retval);
}

static gpointer
gum_script_runtime_do_init (gpointer data)
{
  (void) data;

  gum_scheduler = gum_script_scheduler_new ();

  gum_platform = new GumScriptPlatform (g_main_context_get_thread_default (),
      gum_scheduler);

  V8::SetFlagsFromString (GUM_SCRIPT_V8_FLAGS,
                          static_cast<int> (strlen (GUM_SCRIPT_V8_FLAGS)));
  V8::InitializePlatform (gum_platform);
  V8::Initialize ();

  Isolate * isolate = Isolate::New ();
  isolate->Enter ();

  _gum_register_destructor (gum_script_runtime_do_deinit);

  return isolate;
}

static void
gum_script_runtime_do_deinit (void)
{
  gum_script_set_debug_message_handler (NULL, NULL, NULL);

  Isolate * isolate = Isolate::GetCurrent ();
  isolate->Exit ();
  isolate->Dispose ();

  V8::Dispose ();
  V8::ShutdownPlatform ();

  delete gum_platform;
  gum_platform = nullptr;

  gum_script_scheduler_free (gum_scheduler);
  gum_scheduler = NULL;
}

static void
gum_script_class_init (GumScriptClass * klass)
{
  GObjectClass * object_class = G_OBJECT_CLASS (klass);

  g_type_class_add_private (klass, sizeof (GumScriptPrivate));

  object_class->dispose = gum_script_dispose;
  object_class->finalize = gum_script_finalize;
  object_class->get_property = gum_script_get_property;
  object_class->set_property = gum_script_set_property;

  g_object_class_install_property (object_class, PROP_SOURCE,
      g_param_spec_string ("source", "Source", "Source code", NULL,
      static_cast<GParamFlags> (G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY |
      G_PARAM_STATIC_STRINGS)));
}

static void
gum_script_listener_iface_init (gpointer g_iface,
                                gpointer iface_data)
{
  GumInvocationListenerIface * iface = (GumInvocationListenerIface *) g_iface;

  (void) iface_data;

  iface->on_enter = gum_script_on_enter;
  iface->on_leave = gum_script_on_leave;
}

static void
gum_script_init (GumScript * self)
{
  GumScriptPrivate * priv;

  priv = self->priv = G_TYPE_INSTANCE_GET_PRIVATE (self,
      GUM_TYPE_SCRIPT, GumScriptPrivate);

  priv->isolate = gum_script_runtime_get_isolate ();
  priv->loaded = FALSE;
}

static void
gum_script_dispose (GObject * object)
{
  GumScript * self = GUM_SCRIPT (object);
  GumScriptPrivate * priv = self->priv;

  gum_script_unload (self);

  priv->isolate = NULL;

  G_OBJECT_CLASS (gum_script_parent_class)->dispose (object);
}

static void
gum_script_finalize (GObject * object)
{
  GumScript * self = GUM_SCRIPT (object);

  g_free (self->priv->source);

  G_OBJECT_CLASS (gum_script_parent_class)->finalize (object);
}

static void
gum_script_get_property (GObject * object,
                         guint property_id,
                         GValue * value,
                         GParamSpec * pspec)
{
  GumScript * self = GUM_SCRIPT (object);
  GumScriptPrivate * priv = self->priv;

  switch (property_id)
  {
    case PROP_SOURCE:
      g_value_set_string (value, priv->source);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
  }
}

static void
gum_script_set_property (GObject * object,
                         guint property_id,
                         const GValue * value,
                         GParamSpec * pspec)
{
  GumScript * self = GUM_SCRIPT (object);
  GumScriptPrivate * priv = self->priv;

  switch (property_id)
  {
    case PROP_SOURCE:
      g_free (priv->source);
      priv->source = g_value_dup_string (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
  }
}

static gboolean
gum_script_create_context (GumScript * self,
                           GError ** error)
{
  GumScriptPrivate * priv = self->priv;

  g_assert (priv->context == NULL);

  {
    Locker locker (priv->isolate);
    Isolate::Scope isolate_scope (priv->isolate);
    HandleScope handle_scope (priv->isolate);

    Handle<ObjectTemplate> global_templ = ObjectTemplate::New ();
    _gum_script_core_init (&priv->core, self, gum_scheduler,
        gum_platform->GetMainContext (), priv->isolate, global_templ);
    _gum_script_memory_init (&priv->memory, &priv->core, global_templ);
    _gum_script_process_init (&priv->process, &priv->core, global_templ);
    _gum_script_thread_init (&priv->thread, &priv->core, global_templ);
    _gum_script_module_init (&priv->module, &priv->core, global_templ);
    _gum_script_file_init (&priv->file, &priv->core, global_templ);
    _gum_script_socket_init (&priv->socket, &priv->core, global_templ);
    _gum_script_interceptor_init (&priv->interceptor, &priv->core,
        global_templ);
    _gum_script_stalker_init (&priv->stalker, &priv->core, global_templ);
    _gum_script_symbol_init (&priv->symbol, &priv->core, global_templ);
    _gum_script_instruction_init (&priv->instruction, &priv->core,
        global_templ);

    Local<Context> context (Context::New (priv->isolate, NULL, global_templ));
    priv->context = new GumPersistent<Context>::type (priv->isolate, context);
    Context::Scope context_scope (context);
    _gum_script_core_realize (&priv->core);
    _gum_script_memory_realize (&priv->memory);
    _gum_script_process_realize (&priv->process);
    _gum_script_thread_realize (&priv->thread);
    _gum_script_module_realize (&priv->module);
    _gum_script_file_realize (&priv->file);
    _gum_script_socket_realize (&priv->socket);
    _gum_script_interceptor_realize (&priv->interceptor);
    _gum_script_stalker_realize (&priv->stalker);
    _gum_script_symbol_realize (&priv->symbol);
    _gum_script_instruction_realize (&priv->instruction);

    gchar * combined_source = g_strconcat (
#include "gumscript-runtime.h"
        "\n",
        priv->source,
        static_cast<void *> (NULL));
    Local<String> source_value (String::NewFromUtf8 (priv->isolate, combined_source));
    g_free (combined_source);
    TryCatch trycatch;
    Handle<Script> raw_script = Script::Compile (source_value);
    if (raw_script.IsEmpty ())
    {
      Handle<Message> message = trycatch.Message ();
      Handle<Value> exception = trycatch.Exception ();
      String::Utf8Value exception_str (exception);
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED, "Script(line %d): %s",
          message->GetLineNumber () - GUM_SCRIPT_RUNTIME_SOURCE_LINE_COUNT,
          *exception_str);
    }
    else
    {
      priv->raw_script =
          new GumPersistent<Script>::type (priv->isolate, raw_script);
    }
  }

  if (priv->raw_script == NULL)
  {
    gum_script_destroy_context (self);
    return FALSE;
  }

  return TRUE;
}

static void
gum_script_destroy_context (GumScript * self)
{
  GumScriptPrivate * priv = self->priv;

  g_assert (priv->context != NULL);

  Locker locker (priv->isolate);
  Isolate::Scope isolate_scope (priv->isolate);
  HandleScope handle_scope (priv->isolate);

  {
    Local<Context> context (Local<Context>::New (priv->isolate, *priv->context));
    Context::Scope context_scope (context);

    _gum_script_core_flush (&priv->core);

    _gum_script_instruction_dispose (&priv->instruction);
    _gum_script_symbol_dispose (&priv->symbol);
    _gum_script_stalker_dispose (&priv->stalker);
    _gum_script_interceptor_dispose (&priv->interceptor);
    _gum_script_socket_dispose (&priv->socket);
    _gum_script_file_dispose (&priv->file);
    _gum_script_module_dispose (&priv->module);
    _gum_script_thread_dispose (&priv->thread);
    _gum_script_process_dispose (&priv->process);
    _gum_script_memory_dispose (&priv->memory);
    _gum_script_core_dispose (&priv->core);
  }

  delete priv->raw_script;
  priv->raw_script = NULL;
  delete priv->context;
  priv->context = NULL;

  _gum_script_instruction_finalize (&priv->instruction);
  _gum_script_symbol_finalize (&priv->symbol);
  _gum_script_stalker_finalize (&priv->stalker);
  _gum_script_interceptor_finalize (&priv->interceptor);
  _gum_script_socket_finalize (&priv->socket);
  _gum_script_file_finalize (&priv->file);
  _gum_script_module_finalize (&priv->module);
  _gum_script_thread_finalize (&priv->thread);
  _gum_script_process_finalize (&priv->process);
  _gum_script_memory_finalize (&priv->memory);
  _gum_script_core_finalize (&priv->core);

  priv->loaded = FALSE;
}

GumScript *
gum_script_from_string (const gchar * source,
                        GError ** error)
{
  GumScript * script = GUM_SCRIPT (g_object_new (GUM_TYPE_SCRIPT,
      "source", source,
      NULL));

  if (!gum_script_create_context (script, error))
  {
    g_object_unref (script);
    script = NULL;
  }

  return script;
}

GumStalker *
gum_script_get_stalker (GumScript * self)
{
  return _gum_script_stalker_get (&self->priv->stalker);
}

void
gum_script_set_message_handler (GumScript * self,
                                GumScriptMessageHandler func,
                                gpointer data,
                                GDestroyNotify notify)
{
  _gum_script_core_set_message_handler (&self->priv->core, func, data, notify);
}

void
gum_script_load (GumScript * self)
{
  GumScriptPrivate * priv = self->priv;

  if (priv->raw_script == NULL)
    gum_script_create_context (self, NULL);

  if (priv->raw_script != NULL && !priv->loaded)
  {
    priv->loaded = TRUE;

    ScriptScope scope (self);
    Local<Script> raw_script (Local<Script>::New (priv->isolate, *priv->raw_script));
    raw_script->Run ();
  }
}

void
gum_script_unload (GumScript * self)
{
  GumScriptPrivate * priv = self->priv;

  if (priv->loaded)
  {
    priv->loaded = FALSE;

    gum_script_destroy_context (self);
  }
}

void
gum_script_post_message (GumScript * self,
                         const gchar * message)
{
  _gum_script_core_post_message (&self->priv->core, message);
}

void
gum_script_set_debug_message_handler (GumScriptDebugMessageHandler func,
                                      gpointer data,
                                      GDestroyNotify notify)
{
  Isolate * isolate = gum_script_runtime_get_isolate ();
  Locker locker (isolate);
  Isolate::Scope isolate_scope (isolate);
  HandleScope handle_scope (isolate);

  if (gum_debug_handler != NULL)
  {
    delete gum_debug_context;
    gum_debug_context = nullptr;

    Debug::SetMessageHandler (nullptr);

    if (gum_debug_notify != NULL)
      gum_debug_notify (gum_debug_data);
    gum_debug_handler = NULL;
    gum_debug_data = NULL;
    gum_debug_notify = NULL;
  }

  if (func != NULL)
  {
    gum_debug_handler = func;
    gum_debug_data = data;
    gum_debug_notify = notify;

    Debug::SetMessageHandler (gum_script_on_debug_message);

    gum_debug_context = new GumPersistent<Context>::type (isolate,
        Debug::GetDebugContext ());
  }
}

static void
gum_script_on_debug_message (const Debug::Message & message)
{
  Isolate * isolate = message.GetIsolate ();
  HandleScope scope (isolate);

  Local<String> json = message.GetJSON ();
  String::Utf8Value json_str (json);
  gum_debug_handler (*json_str, gum_debug_data);
}

void
gum_script_post_debug_message (const gchar * message)
{
  g_assert (gum_debug_handler != NULL);

  Isolate * isolate = gum_script_runtime_get_isolate ();
  Locker locker (isolate);
  Isolate::Scope isolate_scope (isolate);
  HandleScope handle_scope (isolate);
  Local<Context> context (Local<Context>::New (isolate, *gum_debug_context));
  Context::Scope context_scope (context);

  glong command_length;
  uint16_t * command = g_utf8_to_utf16 (message, strlen (message), NULL,
      &command_length, NULL);
  g_assert (command != NULL);

  Debug::SendCommand (isolate, command, command_length);
  Debug::ProcessDebugMessages ();

  g_free (command);
}

static void
gum_script_on_enter (GumInvocationListener * listener,
                     GumInvocationContext * context)
{
  GumScript * self = GUM_SCRIPT_CAST (listener);

  _gum_script_interceptor_on_enter (&self->priv->interceptor, context);
}

static void
gum_script_on_leave (GumInvocationListener * listener,
                     GumInvocationContext * context)
{
  GumScript * self = GUM_SCRIPT_CAST (listener);

  _gum_script_interceptor_on_leave (&self->priv->interceptor, context);
}

class ScriptScopeImpl
{
public:
  ScriptScopeImpl (GumScript * parent)
    : parent (parent),
      locker (parent->priv->isolate),
      isolate_scope (parent->priv->isolate),
      handle_scope (parent->priv->isolate),
      context (Local<Context>::New (parent->priv->isolate, *parent->priv->context)),
      context_scope (context)
  {
  }

  ~ScriptScopeImpl ()
  {
    GumScriptPrivate * priv = parent->priv;

    if (trycatch.HasCaught ())
    {
      Handle<Message> message = trycatch.Message ();
      Handle<Value> exception = trycatch.Exception ();
      String::Utf8Value exception_str (exception);
      gchar * exception_str_escaped = g_strescape (*exception_str, "");
      gchar * error = g_strdup_printf (
          "{\"type\":\"error\",\"lineNumber\":%d,\"description\":\"%s\"}",
          message->GetLineNumber () - GUM_SCRIPT_RUNTIME_SOURCE_LINE_COUNT,
          exception_str_escaped);
      _gum_script_core_emit_message (&priv->core, error, NULL, 0);
      g_free (exception_str_escaped);
      g_free (error);
    }
  }

private:
  GumScript * parent;
  Locker locker;
  Isolate::Scope isolate_scope;
  HandleScope handle_scope;
  Local<Context> context;
  Context::Scope context_scope;
  TryCatch trycatch;
};

ScriptScope::ScriptScope (GumScript * parent)
  : parent (parent),
    impl (new ScriptScopeImpl (parent))
{
}

ScriptScope::~ScriptScope ()
{
  delete impl;
  impl = NULL;

  _gum_script_stalker_process_pending (&parent->priv->stalker);
}

