/* Copyright (C) 2005-2019 Shugo Maeda <shugo@ruby-lang.org> and Charlie Savage <cfis@savagexi.com>
   Please see the LICENSE file for copyright and distribution information */

/* ruby-prof tracks the time spent executing every method in ruby programming.
   The main players are:

     profile_t         - This represents 1 profile.
     thread_data_t     - Stores data about a single thread.
     prof_stack_t      - The method call stack in a particular thread
     prof_method_t     - Profiling information about each method
     prof_call_info_t  - Keeps track a method's callers and callees.

  The final result is an instance of a profile object which has a hash table of
  thread_data_t, keyed on the thread id.  Each thread in turn has a hash table
  of prof_method_t, keyed on the method id.  A hash table is used for quick 
  look up when doing a profile.  However, it is exposed to Ruby as an array.

  Each prof_method_t has two hash tables, parent and children, of prof_call_info_t.
  These objects keep track of a method's callers (who called the method) and its
  callees (who the method called).  These are keyed the method id, but once again,
  are exposed to Ruby as arrays.  Each prof_call_into_t maintains a pointer to the
  caller or callee method, thereby making it easy to navigate through the call
  hierarchy in ruby - which is very helpful for creating call graphs.
*/

#include "ruby_prof.h"
#include <ruby/debug.h>
#include <assert.h>

VALUE mProf;
VALUE cProfile;
VALUE cExcludeCommonMethods;

static prof_profile_t*
prof_get_profile(VALUE self)
{
    /* Can't use Data_Get_Struct because that triggers the event hook
       ending up in endless recursion. */
    return DATA_PTR(self);
}

/* support tracing ruby events from ruby-prof. useful for getting at
   what actually happens inside the ruby interpreter (and ruby-prof).
   set environment variable RUBY_PROF_TRACE to filename you want to
   find the trace in.
 */
static FILE* trace_file = NULL;

/* Copied from thread.c (1.9.3) */
static const char *
get_event_name(rb_event_flag_t event)
{
  switch (event) {
  case RUBY_EVENT_LINE:
    return "line";
  case RUBY_EVENT_CLASS:
    return "class";
  case RUBY_EVENT_END:
    return "end";
  case RUBY_EVENT_CALL:
    return "call";
  case RUBY_EVENT_RETURN:
    return "return";
  case RUBY_EVENT_B_CALL:
      return "b-call";
  case RUBY_EVENT_B_RETURN:
      return "b-return";
  case RUBY_EVENT_C_CALL:
    return "c-call";
  case RUBY_EVENT_C_RETURN:
    return "c-return";
  case RUBY_EVENT_THREAD_BEGIN:
      return "thread-begin";
  case RUBY_EVENT_THREAD_END:
      return "thread-end";
  case RUBY_EVENT_FIBER_SWITCH:
      return "fiber-switch";
  case RUBY_EVENT_RAISE:
    return "raise";
  default:
    return "unknown";
  }
}

static int
excludes_method(st_data_t key, prof_profile_t *profile)
{
  return (profile->exclude_methods_tbl &&
    method_table_lookup(profile->exclude_methods_tbl, key) != NULL);
}

static void
prof_exclude_common_methods(VALUE profile)
{
  rb_funcall(cExcludeCommonMethods, rb_intern("apply!"), 1, profile);
}

static prof_method_t*
create_method(rb_event_flag_t event, st_data_t key, VALUE klass, ID mid, thread_data_t* thread_data, prof_profile_t* profile, int line)
{
    prof_method_t* method = NULL;

    if (excludes_method(key, profile))
    {
        /* We found a exclusion sentinel so propagate it into the thread's local hash table. */
        /* TODO(nelgau): Is there a way to avoid this allocation completely so that all these
           tables share the same exclusion method struct? The first attempt failed due to my
           ignorance of the whims of the GC. */
        method = prof_method_create_excluded(klass, mid);
    }
    else
    {
  	    method = prof_method_create(event, klass, mid, line);
    }

    /* Insert the newly created method, or the exlcusion sentinel. */
    method_table_insert(thread_data->method_table, method->key, method);
    
    return method;
}

static int
pop_frames(VALUE key, st_data_t value, st_data_t data)
{
    thread_data_t* thread_data = (thread_data_t *) value;
    prof_profile_t* profile = (prof_profile_t*) data;
    double measurement = prof_measure(profile->measurer);

    if (profile->last_thread_data->fiber != thread_data->fiber)
        switch_thread(profile, thread_data, measurement);

    while (prof_stack_pop(thread_data->stack, measurement));

    return ST_CONTINUE;
}

static void
prof_stop_threads(prof_profile_t* profile)
{
    st_foreach(profile->threads_tbl, pop_frames, (st_data_t) profile);
}

/* ===========  Profiling ================= */
static void
prof_trace(prof_profile_t* profile, rb_event_flag_t event, ID mid, VALUE klass, double measurement)
{
    static VALUE last_fiber = Qnil;

    VALUE thread = rb_thread_current();
    VALUE fiber = rb_fiber_current();

    if (mid == 0)
    {
        rb_frame_method_id_and_class(&mid, &klass);
    }

    const char* class_name = NULL;
    const char* method_name = rb_id2name(mid);
    const char* source_file = rb_sourcefile();
    unsigned int source_line = rb_sourceline();

    const char* event_name = get_event_name(event);

    if (klass != 0)
        klass = (BUILTIN_TYPE(klass) == T_ICLASS ? RBASIC(klass)->klass : klass);

    class_name = rb_class2name(klass);

    if (last_fiber != fiber)
    {
        fprintf(trace_file, "\n");
    }

    fprintf(trace_file, "%2lu:%2lu:%2f %-8s %s#%s    %s:%2d\n",
        FIX2ULONG(thread), FIX2ULONG(fiber), (double) measurement,
            event_name, class_name, method_name, source_file, source_line);
    fflush(trace_file);
    last_fiber = fiber;
}

static void
prof_event_hook(rb_event_flag_t event, VALUE data, VALUE self, ID mid, VALUE klass)
{
    prof_profile_t* profile = prof_get_profile(data);
    VALUE thread = Qnil;
    VALUE fiber = Qnil;
    thread_data_t* thread_data = NULL;
    prof_frame_t *frame = NULL;
    double measurement;

    /* Special case - skip any methods from the mProf
       module or cProfile class since they clutter
       the results but aren't important to them results. */
    if (self == mProf || klass == cProfile)
        return;

    /* Get current measurement */
    measurement = prof_measure(profile->measurer);

    if (trace_file != NULL)
    {
        prof_trace(profile, event, mid, klass, measurement);
    }

    /* Get the current thread and fiber information. */
    thread = rb_thread_current();
    fiber = rb_fiber_current();

    /* We need to switch the profiling context if we either had none before,
       we don't merge fibers and the fiber ids differ, or the thread ids differ. */
    if (profile->last_thread_data->fiber != fiber)
    {
        thread_data = threads_table_lookup(profile, fiber);
        if (!thread_data)
        {
            thread_data = threads_table_insert(profile, thread, fiber);
        }
        switch_thread(profile, thread_data, measurement);
    }
    else
    {
        thread_data = profile->last_thread_data;
    }

    if (!thread_data->trace)
        return;

    /* Get the current frame for the current thread. */
    frame = prof_stack_peek(thread_data->stack);

    switch (event) {
    case RUBY_EVENT_LINE:
    {
        /* Keep track of the current line number in this method.  When
           a new method is called, we know what line number it was
           called from. */
        if (frame)
        {
            if (prof_frame_is_real(frame))
            {
                frame->line = rb_sourceline();
            }
            break;
        }

        /* If we get here there was no frame, which means this is
           the first method seen for this thread, so fall through
           to below to create it. */
    }
    case RUBY_EVENT_THREAD_BEGIN:
    case RUBY_EVENT_THREAD_END:
    case RUBY_EVENT_FIBER_SWITCH:
    {
        // do nothing, we just want to listen to thread/fiber switch events
    }
    case RUBY_EVENT_B_CALL:
    {
        int a = 1;
    }
    case RUBY_EVENT_CALL:
    case RUBY_EVENT_C_CALL:
    {
        prof_frame_t* next_frame;
        prof_call_info_t* call_info;
        prof_method_t* method;

        if (mid == 0)
        {
            rb_frame_method_id_and_class(&mid, &klass);
        }

        int line = rb_sourceline();
        st_data_t key = method_key(klass, mid);
        method = method_table_lookup(thread_data->method_table, key);

        if (!method)
        {
            method = create_method(event, key, klass, mid, thread_data, profile, line);
        }

        if (method->excluded)
        {
          prof_stack_pass(thread_data->stack);
          break;
        }

        if (!frame)
        {
            method->root = true;
            call_info = prof_call_info_create(method, NULL);
            st_insert(method->parent_call_infos, (st_data_t)&key, (st_data_t)call_info);
        }
        else
        {
            call_info = call_info_table_lookup(method->parent_call_infos, frame->call_info->method->key);

            if (!call_info)
            {
                /* This call info does not yet exist.  So create it, then add
                   it to previous callinfo's children and to the current method .*/
                call_info = prof_call_info_create(method, frame->call_info->method);
                call_info_table_insert(method->parent_call_infos, frame->call_info->method->key, call_info);
                call_info_table_insert(frame->call_info->method->child_call_infos, method->key, call_info);
          }
        }

        /* Push a new frame onto the stack for a new c-call or ruby call (into a method) */
        next_frame = prof_stack_push(thread_data->stack, call_info, measurement, RTEST(profile->paused));
        next_frame->line = line;
        break;
    } 
    case RUBY_EVENT_B_RETURN:
    {
        int a = 2;
    }
    case RUBY_EVENT_RETURN:
    case RUBY_EVENT_C_RETURN:
    {
        prof_stack_pop(thread_data->stack, measurement);
        break;
    }
  }
}

void
prof_install_hook(VALUE self)
{
    rb_add_event_hook(prof_event_hook,
          RUBY_EVENT_THREAD_BEGIN  | RUBY_EVENT_THREAD_END | RUBY_EVENT_FIBER_SWITCH |
          RUBY_EVENT_CALL | RUBY_EVENT_RETURN |
          RUBY_EVENT_C_CALL | RUBY_EVENT_C_RETURN |
          RUBY_EVENT_B_CALL | RUBY_EVENT_B_RETURN |
          RUBY_EVENT_LINE, self);
}

void
prof_remove_hook(VALUE self)
{
    rb_remove_event_hook_with_data(prof_event_hook, self);
}

static int
collect_threads(st_data_t key, st_data_t value, st_data_t result)
{
    thread_data_t* thread_data = (thread_data_t*) value;
    if (thread_data->trace)
    {
        VALUE threads_array = (VALUE)result;
        rb_ary_push(threads_array, prof_thread_wrap(thread_data));
    }
    return ST_CONTINUE;
}

/* ========  Profile Class ====== */
static int
mark_threads(st_data_t key, st_data_t value, st_data_t result)
{
    thread_data_t *thread = (thread_data_t *) value;
    prof_thread_mark(thread);
    return ST_CONTINUE;
}

static int
mark_methods(st_data_t key, st_data_t value, st_data_t result)
{
    prof_method_t *method = (prof_method_t *) value;
    prof_method_mark(method);
    return ST_CONTINUE;
}

static void
prof_mark(prof_profile_t *profile)
{
    st_foreach(profile->threads_tbl, mark_threads, 0);
    st_foreach(profile->exclude_methods_tbl, mark_methods, 0);
}

/* Freeing the profile creates a cascade of freeing.
   It fress the thread table, which frees its methods,
   which frees its call infos. */
static void
prof_free(prof_profile_t *profile)
{
    profile->last_thread_data = NULL;

    threads_table_free(profile->threads_tbl);
    profile->threads_tbl = NULL;

    if (profile->exclude_threads_tbl)
    {
        st_free_table(profile->exclude_threads_tbl);
        profile->exclude_threads_tbl = NULL;
    }

    if (profile->include_threads_tbl)
    {
        st_free_table(profile->include_threads_tbl);
        profile->include_threads_tbl = NULL;
    }

    /* This table owns the excluded sentinels for now. */
    method_table_free(profile->exclude_methods_tbl);
    profile->exclude_methods_tbl = NULL;

    xfree(profile->measurer);
    profile->measurer = NULL;

    xfree(profile);
}

static VALUE
prof_allocate(VALUE klass)
{
    VALUE result;
    prof_profile_t* profile;
    result = Data_Make_Struct(klass, prof_profile_t, prof_mark, prof_free, profile);
    profile->threads_tbl = threads_table_create();
    profile->exclude_threads_tbl = NULL;
    profile->include_threads_tbl = NULL;
    profile->running = Qfalse;
    profile->allow_exceptions = 0;
    profile->exclude_methods_tbl = method_table_create();
    profile->running = Qfalse;
    return result;
}

/* call-seq:
   new()
   new(options)

   Returns a new profiler. Possible options for the options hash are:

   measure_mode::     Measure mode. Specifies the profile measure mode.
                      If not specified, defaults to RubyProf::WALL_TIME.
   exclude_threads::  Threads to exclude from the profiling results.
   include_threads::  Focus profiling on only the given threads. This will ignore
                      all other threads.
   merge_fibers::     Whether to merge all fibers under a given thread. This should be
                      used when profiling for a callgrind printer.
   allow_exceptions:: Whether to raise exceptions encountered during profiling,
                      or to suppress all exceptions during profiling
*/
static VALUE
prof_initialize(int argc,  VALUE *argv, VALUE self)
{
    prof_profile_t* profile = prof_get_profile(self);
    VALUE mode_or_options;
    VALUE mode = Qnil;
    VALUE exclude_threads = Qnil;
    VALUE include_threads = Qnil;
    VALUE exclude_common = Qnil;
    VALUE allow_exceptions = Qnil;
    int i;

    switch (rb_scan_args(argc, argv, "02", &mode_or_options, &exclude_threads))
    {
    case 0:
        break;
    case 1:
        if (FIXNUM_P(mode_or_options))
        {
            mode = mode_or_options;
        }
        else
        {
            Check_Type(mode_or_options, T_HASH);
            mode = rb_hash_aref(mode_or_options, ID2SYM(rb_intern("measure_mode")));
            allow_exceptions = rb_hash_aref(mode_or_options, ID2SYM(rb_intern("allow_exceptions")));
            exclude_common = rb_hash_aref(mode_or_options, ID2SYM(rb_intern("exclude_common")));
            exclude_threads = rb_hash_aref(mode_or_options, ID2SYM(rb_intern("exclude_threads")));
            include_threads = rb_hash_aref(mode_or_options, ID2SYM(rb_intern("include_threads")));
        }
        break;
    case 2:
        Check_Type(exclude_threads, T_ARRAY);
        break;
    }

    if (mode == Qnil)
    {
        mode = INT2NUM(MEASURE_WALL_TIME);
    }
    else
    {
        Check_Type(mode, T_FIXNUM);
    }
    profile->measurer = prof_get_measurer(NUM2INT(mode));
    profile->allow_exceptions = allow_exceptions != Qnil && allow_exceptions != Qfalse;

    if (exclude_threads != Qnil)
    {
        Check_Type(exclude_threads, T_ARRAY);
        assert(profile->exclude_threads_tbl == NULL);
        profile->exclude_threads_tbl = threads_table_create();
        for (i = 0; i < RARRAY_LEN(exclude_threads); i++)
        {
            VALUE thread = rb_ary_entry(exclude_threads, i);
            st_insert(profile->exclude_threads_tbl, thread, Qtrue);
        }
    }

    if (include_threads != Qnil)
    {
        Check_Type(include_threads, T_ARRAY);
        assert(profile->include_threads_tbl == NULL);
        profile->include_threads_tbl = threads_table_create();
        for (i = 0; i < RARRAY_LEN(include_threads); i++)
        {
            VALUE thread = rb_ary_entry(include_threads, i);
            st_insert(profile->include_threads_tbl, thread, Qtrue);
        }
    }

    if (RTEST(exclude_common)) {
        prof_exclude_common_methods(self);
    }

    return self;
}

/* call-seq:
   paused? -> boolean

   Returns whether a profile is currently paused.*/
static VALUE
prof_paused(VALUE self)
{
    prof_profile_t* profile = prof_get_profile(self);
    return profile->paused;
}

/* call-seq:
   running? -> boolean

   Returns whether a profile is currently running.*/
static VALUE
prof_running(VALUE self)
{
    prof_profile_t* profile = prof_get_profile(self);
    return profile->running;
}

/* call-seq:
   start -> self

   Starts recording profile data.*/
static VALUE
prof_start(VALUE self)
{
    char* trace_file_name;

    prof_profile_t* profile = prof_get_profile(self);

    if (profile->running == Qtrue)
    {
        rb_raise(rb_eRuntimeError, "RubyProf.start was already called");
    }

    profile->running = Qtrue;
    profile->paused = Qfalse;
    profile->last_thread_data = threads_table_insert(profile, rb_thread_current(), rb_fiber_current());

    /* open trace file if environment wants it */
    trace_file_name = getenv("RUBY_PROF_TRACE");
    if (trace_file_name != NULL) 
    {
      if (strcmp(trace_file_name, "stdout") == 0) 
      {
        trace_file = stdout;
      } 
      else if (strcmp(trace_file_name, "stderr") == 0)
      {
        trace_file = stderr;
      }
      else 
      {
        trace_file = fopen(trace_file_name, "w");
      }
    }

    prof_install_hook(self);
    return self;
}

/* call-seq:
   pause -> self

   Pauses collecting profile data. */
static VALUE
prof_pause(VALUE self)
{
    prof_profile_t* profile = prof_get_profile(self);
    if (profile->running == Qfalse)
    {
        rb_raise(rb_eRuntimeError, "RubyProf is not running.");
    }

    if (profile->paused == Qfalse)
    {
        profile->paused = Qtrue;
        profile->measurement_at_pause_resume = prof_measure(profile->measurer);
        st_foreach(profile->threads_tbl, pause_thread, (st_data_t) profile);
    }

    return self;
}

/* call-seq:
   resume -> self
   resume(&block) -> self

   Resumes recording profile data.*/
static VALUE
prof_resume(VALUE self)
{
    prof_profile_t* profile = prof_get_profile(self);
    if (profile->running == Qfalse)
    {
        rb_raise(rb_eRuntimeError, "RubyProf is not running.");
    }

    if (profile->paused == Qtrue)
    {
        profile->paused = Qfalse;
        profile->measurement_at_pause_resume = prof_measure(profile->measurer);
        st_foreach(profile->threads_tbl, unpause_thread, (st_data_t) profile);
    }

    return rb_block_given_p() ? rb_ensure(rb_yield, self, prof_pause, self) : self;
}

/* call-seq:
   stop -> self

   Stops collecting profile data.*/
static VALUE
prof_stop(VALUE self)
{
    prof_profile_t* profile = prof_get_profile(self);

    if (profile->running == Qfalse)
    {
        rb_raise(rb_eRuntimeError, "RubyProf.start was not yet called");
    }

    prof_remove_hook(self);

    /* close trace file if open */
    if (trace_file != NULL)
    {
      if (trace_file !=stderr && trace_file != stdout)
      {
#ifdef _MSC_VER
          _fcloseall();
#else
        fclose(trace_file);
#endif
      }
      trace_file = NULL;
    }

    prof_stop_threads(profile);

    /* Unset the last_thread_data (very important!)
       and the threads table */
    profile->running = profile->paused = Qfalse;
    profile->last_thread_data = NULL;

    return self;
}

/* call-seq:
   threads -> Array of RubyProf::Thread

Returns an array of RubyProf::Thread instances that were executed
while the the program was being run. */
static VALUE
prof_threads(VALUE self)
{
    VALUE result = rb_ary_new();
    prof_profile_t* profile = prof_get_profile(self);
    st_foreach(profile->threads_tbl, collect_threads, result);
    return result;
}

/* call-seq:
   profile {block} -> RubyProf::Result

Profiles the specified block and returns a RubyProf::Result object. */
static VALUE
prof_profile_object(VALUE self)
{
    int result;
    prof_profile_t* profile = prof_get_profile(self);

    if (!rb_block_given_p())
    {
        rb_raise(rb_eArgError, "A block must be provided to the profile method.");
    }

    prof_start(self);
    rb_protect(rb_yield, self, &result);
    self = prof_stop(self);

    if (profile->allow_exceptions && result != 0)
    {
        rb_jump_tag(result);
    }

    return self;

}

/* call-seq:
   profile(&block) -> self
   profile(options, &block) -> self

Profiles the specified block and returns a RubyProf::Profile
object. Arguments are passed to Profile initialize method.
*/
static VALUE
prof_profile_class(int argc,  VALUE *argv, VALUE klass)
{
    return prof_profile_object(rb_class_new_instance(argc, argv, cProfile));
}

static VALUE
prof_exclude_method(VALUE self, VALUE klass, VALUE sym)
{
    prof_profile_t* profile = prof_get_profile(self);
    ID mid = SYM2ID(sym);

    st_data_t key = method_key(klass, mid);
    prof_method_t *method;

    if (profile->running == Qtrue)
    {
        rb_raise(rb_eRuntimeError, "RubyProf.start was already called");
    }

    method = method_table_lookup(profile->exclude_methods_tbl, key);

    if (!method)
    {
      method = prof_method_create_excluded(klass, mid);
      method_table_insert(profile->exclude_methods_tbl, method->key, method);
    }

    return self;
}

VALUE prof_profile_dump(VALUE self)
{
    VALUE result = rb_hash_new();
    rb_hash_aset(result, ID2SYM(rb_intern("threads")), prof_threads(self));
    return result;
}

VALUE prof_profile_load(VALUE self, VALUE data)
{
    prof_profile_t* profile = prof_get_profile(self);
    VALUE threads = rb_hash_aref(data, ID2SYM(rb_intern("threads")));
    for (int i = 0; i < rb_array_len(threads); i++)
    {
        VALUE thread = rb_ary_entry(threads, i);
        thread_data_t* thread_data = DATA_PTR(thread);
        st_insert(profile->threads_tbl, (st_data_t)thread_data->fiber_id, (st_data_t)thread_data);
    }

    return data;
}

void Init_ruby_prof()
{
    mProf = rb_define_module("RubyProf");

    rp_init_measure();
    rp_init_method_info();
    rp_init_call_info();
    rp_init_thread();

    cProfile = rb_define_class_under(mProf, "Profile", rb_cObject);
    rb_define_alloc_func (cProfile, prof_allocate);

    rb_define_singleton_method(cProfile, "profile", prof_profile_class, -1);
    rb_define_method(cProfile, "initialize", prof_initialize, -1);
    rb_define_method(cProfile, "start", prof_start, 0);
    rb_define_method(cProfile, "stop", prof_stop, 0);
    rb_define_method(cProfile, "resume", prof_resume, 0);
    rb_define_method(cProfile, "pause", prof_pause, 0);
    rb_define_method(cProfile, "running?", prof_running, 0);
    rb_define_method(cProfile, "paused?", prof_paused, 0);
    rb_define_method(cProfile, "threads", prof_threads, 0);
    rb_define_method(cProfile, "exclude_method!", prof_exclude_method, 2);
    rb_define_method(cProfile, "profile", prof_profile_object, 0);
    rb_define_method(cProfile, "_dump_data", prof_profile_dump, 0);
    rb_define_method(cProfile, "_load_data", prof_profile_load, 1);

    cExcludeCommonMethods = rb_define_class_under(cProfile, "ExcludeCommonMethods", rb_cObject);
}
