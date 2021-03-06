/***********************************************************
 *
 * $A2 150704 thinkhy  priority scheduler
 * $A3 150720 thinkhy  advance  scheduler
 *
 ***********************************************************/
#include "threads/thread.h"
#include <debug.h>
#include <stddef.h>
#include <random.h>
#include <stdio.h>
#include <string.h>
#include "threads/flags.h"
#include "threads/interrupt.h"
#include "threads/intr-stubs.h"
#include "threads/palloc.h"
#include "threads/switch.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#include "threads/fixed-point.h"
#include "devices/timer.h"
#ifdef USERPROG
#include "userprog/process.h"
#endif

/* Random value for struct thread's `magic' member.
   Used to detect stack overflow.  See the big comment at the top
   of thread.h for details. */
#define THREAD_MAGIC 0xcd6abf4b


/* List of processes in THREAD_READY state, that is, processes
   that are ready to run but not actually running. */
static struct list ready_list;

/* List of all processes.  Processes are added to this list
   when they are first scheduled and removed when they exit. */
static struct list all_list;


/* Idle thread. */
static struct thread *idle_thread;

/* Initial thread, the thread running init.c:main(). */
static struct thread *initial_thread;

/* Lock used by allocate_tid(). */
static struct lock tid_lock;

/* Stack frame for kernel_thread(). */
struct kernel_thread_frame 
  {
    void *eip;                  /* Return address. */
    thread_func *function;      /* Function to call. */
    void *aux;                  /* Auxiliary data for function. */
  };

/* Statistics. */
static long long idle_ticks;    /* # of timer ticks spent idle. */
static long long kernel_ticks;  /* # of timer ticks in kernel threads. */
static long long user_ticks;    /* # of timer ticks in user programs. */

/* Scheduling. */
#define TIME_SLICE 4            /* # of timer ticks to give each thread. */
static unsigned thread_ticks;   /* # of timer ticks since last yield. */

/* If false (default), use priority scheduler.
   If true, use multi-level feedback queue scheduler.
   Controlled by kernel command-line option "-o mlfqs". */
bool thread_mlfqs;

/* 4.4BSD Scheduler has 64 priorities and thus 64 ready queue numbered 0 (PRI_MIN) through 63 (PRI_MAX).
 * Lower numbers correspond to lower priorities, so that priority 0 is the lowest priority and priority 63
 * is the highest. Thread priority is calculated initially at thread initialization. It is also recalculated
 * once every fourth clock tick, for every thread.
   @A3A */
static struct list ready_queues[ PRI_MAX + 1 ];                                               /* @A3A */

/* The number of threads that are either running or ready to run at time of 
 * update(not including idle thread @A3A */
static int ready_threads;                                                                     /* @A3A */

/*
   System load average estimates the average number of threads ready to run over the past minute.
   It's an exponentially weighted moving average. Unlink priority and recent_cpu, load average is system-wide,
   not thread-specific. At system boot, it is initialized to 0. Once per second thereafter, it is updated according
   to the following formula:
       load_avg = (59/60) * load_avg + (1/60)*ready_threads
   @A3A 
 */
static int load_avg;                                     /* Pintos doesn't support floating point @A3A */

static void kernel_thread (thread_func *, void *aux);

static void idle (void *aux UNUSED);
static struct thread *running_thread (void);
static struct thread *next_thread_to_run (void);
static void init_thread (struct thread *, const char *name, int priority);
static bool is_thread (struct thread *) UNUSED;
static void *alloc_frame (struct thread *, size_t size);
static void schedule (void);
void thread_schedule_tail (struct thread *prev);
static tid_t allocate_tid (void);
static int thread_adjust_priority(int priority);                                  /* @A3A */

/* Initializes the threading system by transforming the code
   that's currently running into a thread.  This can't work in
   general and it is possible in this case only because loader.S
   was careful to put the bottom of the stack at a page boundary.

   Also initializes the run queue and the tid lock.

   After calling this function, be sure to initialize the page
   allocator before trying to create any threads with
   thread_create().

   It is not safe to call thread_current() until this function
   finishes. */
void
thread_init (void) 
{
  ASSERT (intr_get_level () == INTR_OFF);

  lock_init (&tid_lock);
  list_init (&ready_list);
  list_init (&all_list);

  if (thread_mlfqs)                                                  /* @A3A */
   {
    load_avg      = 0;                                               /* @A3A */
    ready_threads = 0;                                               /* @A3A */

    int i;                                                           /* @A3A */
    for(i = PRI_MIN; i < PRI_MAX + 1; i++)                           /* @A3A */
      list_init (&ready_queues[i]);                                  /* @A3A */
   }

  /* Set up a thread structure for the running thread. */
  initial_thread = running_thread ();
  init_thread (initial_thread, "main", PRI_DEFAULT);
  initial_thread->status = THREAD_RUNNING;
  initial_thread->tid = allocate_tid ();

}

/* Starts preemptive thread scheduling by enabling interrupts.
   Also creates the idle thread. */
void
thread_start (void) 
{
  /* Create the idle thread. */
  struct semaphore idle_started;
  sema_init (&idle_started, 0);
  thread_create ("idle", PRI_MIN, idle, &idle_started);

  /* Start preemptive thread scheduling. */
  intr_enable ();

  /* Wait for the idle thread to initialize idle_thread. */
  sema_down (&idle_started);
}

static void thread_calc_recent_cpu (struct thread *t) 
 {
     ASSERT (is_thread(t));
     fixed_point_t recent_cpu_fix = __mk_fix (t->recent_cpu);  

   /* Note from handout: You may need to think about the order of calculations in this formula.
    * We recommend computing the coecient of recent_cpu first,  then  multiplying.   
    * Some  students  have  reported  that  multiplying load_avg by recent_cpu directly 
    * can cause overflow. 
    * Formular: recent_cpu = (2 * load_avg)/(2 * load_avg + 1) * recent_cpu + nice   */
     recent_cpu_fix =  fix_add (fix_mul (fix_div (fix_scale (__mk_fix (load_avg), 2), 	
			                          fix_add (fix_scale (__mk_fix (load_avg), 2), 
                                                           fix_int (1))),
	                                 recent_cpu_fix),
                                fix_int(t->nice));

     t->recent_cpu = recent_cpu_fix.f;

     return; 
 }

static void thread_calc_load_avg (void) 
 {
    /* load_avg estimates the average number of threads ready to run over the past minute.  
     * It is initialized to 0 at boot and recalculated once per second as follows   
     * load_avg = (59/60) * load_avg + (1/60) * ready_threads  @A3A */
     fixed_point_t load_avg_fix = __mk_fix (load_avg);                             /* @A3A */
     load_avg_fix = fix_add (fix_mul (fix_frac (59, 60), load_avg_fix),            /* @A3A */
			     fix_scale (fix_frac (1, 60), ready_threads) );        /* @A3A */
     load_avg = load_avg_fix.f;                                                    /* @A3A */

     return;                                                                       /* @A3A */
 }

static int thread_calc_priority (struct thread *t) 
 {
     /* Formular: priority = PRI_MAX - (recent_cpu/4) - (nice * 2)                    @A3A */
     fixed_point_t priority_fix;                                                   /* @A3A */
     priority_fix = fix_sub ( fix_sub (fix_int (PRI_MAX),                          /* @A3A */
				       fix_div (__mk_fix (t->recent_cpu),          /* @A3A */
                                                fix_int (4))),                     /* @A3A */
                              fix_int (t->nice * 2));                              /* @A3A */
     t->priority = thread_adjust_priority (fix_trunc (priority_fix));              /* @A3A */

     return t->priority;                                                           /* @A3A */
 }

/* Called by the timer interrupt handler at each timer tick.
   Thus, this function runs in an external interrupt context. */
void
thread_tick (void) 
{
  struct list_elem *e;                                                           /* @A3A */
  struct thread *t = thread_current ();

  /* Update statistics. */
  if (t == idle_thread)
    idle_ticks++;
#ifdef USERPROG
  else if (t->pagedir != NULL)
    user_ticks++;
#endif
  else
    kernel_ticks++;

  /* Enforce preemption. */
  if (++thread_ticks >= TIME_SLICE)
    intr_yield_on_return ();

  if (thread_mlfqs)                                                              /* @A3A */
   {
   int ticks = timer_ticks();                                                    /* @A3A */
  
   /* Each time a timer interrupt occurs, recent_cpu is incremented by 1 for 
      the running thread only, unless the idle thread is running. @A3A */
   if (t != idle_thread)                                                         /* @A3A */
    {
      fixed_point_t sum = fix_add (__mk_fix (t->recent_cpu), fix_int (1));       /* @A3A */
      t->recent_cpu = sum.f;                                                     /* @A3A */
    }

   /* Note: priority, nice and ready_threads are integers, but recent_cpu and load_avg
    * are real numbers.  Unfortunately, Pintos does not support oating-point arithmetic 
    * in the kernel, because it would complicate and slow the kernel.  Real kernels often 
    * have the same limitation, for the same reason. This means that calculations on real 
    * quantities must be simulated using integers. @A3A */

   /* Update load_avg and recnet_cpu per second                                     @A3A */
   if (ticks % TIMER_FREQ == 0)                                                  /* @A3A */
    {
     thread_calc_load_avg ();  

    /* Once per second(timer_ticks() % TIMER_FREQ == 0) the value of recent_cpu is 
     * recalculated for every thread(whether running, ready, or blocked).           
     * Formular: recent_cpu = (2 * load_avg)/(2 * load_avg + 1) * recent_cpu + nice
     * @A3A */
     for (e = list_begin (&all_list); e != list_end (&all_list);                 /* @A3A */
          e = list_next (e))                                                     /* @A3A */
     {
         struct thread *t = list_entry (e, struct thread, allelem);              /* @A3A */
         thread_calc_recent_cpu(t);                                              /* @A3A */
     }
    }

   /* Each thread's priority is recalculated once every fourth clock tick           @A3A */
   if (ticks % TIME_SLICE == 0)                                                  /* @A3A */
    {
     for (e = list_begin (&all_list); e != list_end (&all_list);                 /* @A3A */
          e = list_next (e))                                                     /* @A3A */
     {
         struct thread *t = list_entry (e, struct thread, allelem);              /* @A3A */
         thread_calc_priority(t);                                                /* @A3A */
     }
    }
   }
}

/* Prints thread statistics. */
void
thread_print_stats (void) 
{
  printf ("Thread: %lld idle ticks, %lld kernel ticks, %lld user ticks\n",
          idle_ticks, kernel_ticks, user_ticks);
}

/* Creates a new kernel thread named NAME with the given initial
   PRIORITY, which executes FUNCTION passing AUX as the argument,
   and adds it to the ready queue.  Returns the thread identifier
   for the new thread, or TID_ERROR if creation fails.

   If thread_start() has been called, then the new thread may be
   scheduled before thread_create() returns.  It could even exit
   before thread_create() returns.  Contrariwise, the original
   thread may run for any amount of time before the new thread is
   scheduled.  Use a semaphore or some other form of
   synchronization if you need to ensure ordering.

   The code provided sets the new thread's `priority' member to
   PRIORITY, but no actual priority scheduling is implemented.
   Priority scheduling is the goal of Problem 1-3. */
tid_t
thread_create (const char *name, int priority,
               thread_func *function, void *aux) 
{
  struct thread *t;
  struct kernel_thread_frame *kf;
  struct switch_entry_frame *ef;
  struct switch_threads_frame *sf;
  tid_t tid;

  ASSERT (function != NULL);

  /* Allocate thread. */
  t = palloc_get_page (PAL_ZERO);
  if (t == NULL)
    return TID_ERROR;

  /* Initialize thread. */
  init_thread (t, name, priority);
  tid = t->tid = allocate_tid ();

  /* Stack frame for kernel_thread(). */
  kf = alloc_frame (t, sizeof *kf);
  kf->eip = NULL;
  kf->function = function;
  kf->aux = aux;

  /* Stack frame for switch_entry(). */
  ef = alloc_frame (t, sizeof *ef);
  ef->eip = (void (*) (void)) kernel_thread;

  /* Stack frame for switch_threads(). */
  sf = alloc_frame (t, sizeof *sf);
  sf->eip = switch_entry;
  sf->ebp = 0;

  /* Add to run queue. */
  thread_unblock (t);

  if (t->priority > thread_current()->priority)                 /* @A2A */
       thread_yield();                                          /* @A2A */

  return tid;
}

/* Puts the current thread to sleep.  It will not be scheduled
   again until awoken by thread_unblock().

   This function must be called with interrupts turned off.  It
   is usually a better idea to use one of the synchronization
   primitives in synch.h. */
void
thread_block (void) 
{
  ASSERT (!intr_context ());
  ASSERT (intr_get_level () == INTR_OFF);

  if (thread_mlfqs)                                                  /* @A3A */
    if (thread_current () != idle_thread)                            /* @A3A */
	   ready_threads--;                                          /* @A3A */
  thread_current ()->status = THREAD_BLOCKED;
  schedule ();
}

/* Transitions a blocked thread T to the ready-to-run state.
   This is an error if T is not blocked.  (Use thread_yield() to
   make the running thread ready.)

   This function does not preempt the running thread.  This can
   be important: if the caller had disabled interrupts itself,
   it may expect that it can atomically unblock a thread and
   update other data. */
void
thread_unblock (struct thread *t) 
{
  enum intr_level old_level;

  ASSERT (is_thread (t));

  old_level = intr_disable ();
  ASSERT (t != idle_thread);                                         /* @A3A */
  ASSERT (t->status == THREAD_BLOCKED);
  t->status = THREAD_READY;
  if (thread_mlfqs == false)                                         /* @A3A */
     list_push_back (&ready_list, &t->elem);
  else                                                               /* @A3A */
   {
    // printf ("priority: %d\n", t->priority);
    list_push_back (&ready_queues[t->priority],                      /* @A3A */
			&t->elem);                                   /* @A3A */
   }
  ready_threads++;                                                   /* @A3A */
  intr_set_level (old_level);
}

/* Returns the name of the running thread. */
const char *
thread_name (void) 
{
  return thread_current ()->name;
}

/* Returns the running thread.
   This is running_thread() plus a couple of sanity checks.
   See the big comment at the top of thread.h for details. */
struct thread *
thread_current (void) 
{
  struct thread *t = running_thread ();
  
  /* Make sure T is really a thread.
     If either of these assertions fire, then your thread may
     have overflowed its stack.  Each thread has less than 4 kB
     of stack, so a few big automatic arrays or moderate
     recursion can cause stack overflow. */
  ASSERT (is_thread (t));
  ASSERT (t->status == THREAD_RUNNING);

  return t;
}

/* Returns the running thread's tid. */
tid_t
thread_tid (void) 
{
  return thread_current ()->tid;
}

/* Deschedules the current thread and destroys it.  Never
   returns to the caller. */
void
thread_exit (void) 
{
  ASSERT (!intr_context ());

#ifdef USERPROG
  process_exit ();
#endif

  /* Remove thread from all threads list, set our status to dying,
     and schedule another process.  That process will destroy us
     when it calls thread_schedule_tail(). */
  intr_disable ();
  list_remove (&thread_current()->allelem);
  thread_current ()->status = THREAD_DYING;
  schedule ();
  NOT_REACHED ();
}

/* Yields the CPU.  The current thread is not put to sleep and
   may be scheduled again immediately at the scheduler's whim. */
void
thread_yield (void) 
{
  struct thread *cur = thread_current ();
  enum intr_level old_level;
  
  ASSERT (!intr_context ());

  old_level = intr_disable ();
  if (cur != idle_thread) 
   {
    if (thread_mlfqs == false)                                       /* @A3A */
	list_push_back (&ready_list, &cur->elem);
    else                                                             /* @A3A */
        list_push_back (&ready_queues[cur->priority],                /* @A3A */
			&cur->elem);                                 /* @A3A */
   }

  cur->status = THREAD_READY;
  schedule ();
  intr_set_level (old_level);
}

/* Invoke function 'func' on all threads, passing along 'aux'.
   This function must be called with interrupts off. */
void
thread_foreach (thread_action_func *func, void *aux)
{
  struct list_elem *e;

  ASSERT (intr_get_level () == INTR_OFF);

  for (e = list_begin (&all_list); e != list_end (&all_list);
       e = list_next (e))
    {
      struct thread *t = list_entry (e, struct thread, allelem);
      func (t, aux);
    }
}

/* Sets the current thread's priority to NEW_PRIORITY. */
void
thread_set_priority (int new_priority) 
{
  /* When 4.4BSD scheduler is enabled, threads no longer directly
   * control their own priorities.                                     @A3A */
  if (thread_mlfqs == false)                                        /* @A3A */
   {
  struct thread *t = thread_current();                              /* @A2A */
  t->priority = new_priority;                                       /* @A2C */
  int old_effective_priority = t->effective_priority;               /* @A2A */
  t->is_dirty = true;                                               /* @A2A */

  /* If the current thread no longer has the highest priority, yields. @A2A */
  /* If updated effective priority is less than old priority, then yield */
  if (old_effective_priority > thread_get_priority())               /* @A2A */
     thread_yield();                                                /* @A2A */
   }
}

/* Set flag that determins if effective_priorities needs to be updated @A2A */
void thread_set_dirty(struct thread *t, bool is_dirty)              /* @A2A */
{ 
  /* The advanced scheduler does not do priority donation              @A3A */
  if (thread_mlfqs == false)                                        /* @A3A */
   {
   ASSERT(is_thread(t));                                            /* @A2A */
   t->is_dirty = is_dirty;                                          /* @A2A */
   }
}

/* Returns the current thread's priority.                              @A2A */
int
thread_get_effective_priority(struct thread *t)                     /* @A2A */
{
  /* The advanced scheduler does not do priority donation              @A3A */
  if (thread_mlfqs == false)                                        /* @A3A */
   {
  int priority;                                                     /* @A2A */ 
  struct list_elem *e;                                              /* @A2A */
  enum intr_level old_level;                                        /* @A2A */  


  /* return thread_current ()->priority;                               @A2D */
  /* if current thread is dirty, update effective priority value with     
   * max(priority, effective_priorities in waiting threads)
   */
  if (t->is_dirty)                                                  /* @A2A */
   {
     old_level = intr_disable ();
     t->effective_priority = t->priority;                                        /* @A2A */
     for (e = list_begin (&(t->waiting_list));                                   /* @A2A */ 
            e != list_end (&(t->waiting_list)); e = list_next (e))               /* @A2A */        
      {				                                                 
        struct thread *waiting_thread = list_entry (e, struct thread, waitelem); /* @A2A */
	ASSERT(waiting_thread->waited_thread == t);
        priority = thread_get_effective_priority(waiting_thread);                /* @A2A */

        /* Donate waiting thread's priority if current thread's priority is 
         * lower                                                                         */
	if ( t->effective_priority < priority )                                  /* @A2A */
		t->effective_priority = priority;                                /* @A2A */
      }

      /* TODO: shoud lock below line?? 150709                                           */
      thread_set_dirty(t, false);                                                /* @A2A */
      intr_set_level (old_level);                                                /* @A2A */
   }
   } // for if (!thread_mlfqs)                                                   /* @A3A */
  

  return t->effective_priority;                                                  /* @A2A */
}

/* The calculated priority is always adjusted to lie 
* in the valid range PRI_MIN to PRI_MAX .       @A3A */
static int thread_adjust_priority(int priority)                                  /* @A3A */
{ 
  if (priority > PRI_MAX)                                                        /* @A3A */            
   return PRI_MAX;                                                               /* @A3A */
  else if (priority < PRI_MIN)                                                   /* @A3A */
   return PRI_MIN;                                                               /* @A3A */
  else                                                                           /* @A3A */
   return priority;                                                              /* @A3A */
} 

/* Returns the current thread's priority. */
int
thread_get_priority (void) 
{
  struct thread *t = thread_current();
  if (thread_mlfqs)                                                              /* @A3A */
   {
      return thread_adjust_priority (t->priority);                               /* @A3A */
   }
  else {                                                                         /* @A3A */
  /* int priority;                                                                  @A2D */

  /* return thread_current ()->priority;                                            @A2D */
  /* if current thread is dirty, update effective priority value with     
   * max(priority, effective_priorities in waiting threads)
   */
  if (t->is_dirty)                                                               /* @A2A */
   {
       thread_get_effective_priority(t);                                         /* @A2A */
   }
  return t->effective_priority;                                                  /* @A2A */
  }
}

/* Sets the current thread's nice value to NICE. */
void
thread_set_nice (int nice UNUSED) 
{
  /* A positive nice , to the maximum of 20, decreases the
   * priority of a thread and causes it to give up some CPU time 
   * it would otherwise receive.  On the other hand, a negative
   * nice , to the minimum of -20, tends to take away CPU time from other threads.       */
  ASSERT(nice >= -20 && nice <= 20);                                             /* @A3A */ 

  /* Sets the current thread's nice value to new nice                               @A3A */
  struct thread *t = thread_current();                                           /* @A3A */
  ASSERT(is_thread(t));
  t->nice = nice;

  /* Recalculate the thread's priority based on the new value.                      @A3A */ 
  int old_priority = t->priority;                                                /* @A3A */
  thread_calc_priority(t);                                                       /* @A3A */

  /* If the running thread no longer has the highest priority, yields.              @A3A */
  if (old_priority > t->priority)                                                /* @A3A */
      thread_yield();                                                            /* @A3A */
}

/* Returns the current thread's nice value. */
int thread_get_nice (void) 
{
  /* Returns the current thread's nice value.                                       @A3A */
  struct thread *t = thread_current();                                           /* @A3A */ 
  ASSERT(is_thread(t));                                                          /* @A3A */

  return t->nice;                                                                /* @A3A */
}

/* Returns 100 times the system load average. */
int
thread_get_load_avg (void) 
{
  /* Round to nearest integer                                                            */
  return fix_round (fix_scale (__mk_fix (load_avg), 100));                       /* @A3A */
}

/* Returns 100 times the current thread's recent_cpu value. */
int
thread_get_recent_cpu (void) 
{
  /* Returns the current thread's nice value.                                       @A3A */
  struct thread *t = thread_current();                                           /* @A3A */ 
  ASSERT(is_thread(t));                                                          /* @A3A */

  /* Round to nearest integer                                                            */
  return fix_round (fix_scale (__mk_fix (t->recent_cpu), 100));                  /* @A3A */
}

/* Idle thread.  Executes when no other thread is ready to run.

   The idle thread is initially put on the ready list by
   thread_start().  It will be scheduled once initially, at which
   point it initializes idle_thread, "up"s the semaphore passed
   to it to enable thread_start() to continue, and immediately
   blocks.  After that, the idle thread never appears in the
   ready list.  It is returned by next_thread_to_run() as a
   special case when the ready list is empty. */
static void
idle (void *idle_started_ UNUSED) 
{
  struct semaphore *idle_started = idle_started_;
  idle_thread = thread_current ();
  sema_up (idle_started);

  for (;;) 
    {
      /* Let someone else run. */
      intr_disable ();
      thread_block ();

      /* Re-enable interrupts and wait for the next one.

         The `sti' instruction disables interrupts until the
         completion of the next instruction, so these two
         instructions are executed atomically.  This atomicity is
         important; otherwise, an interrupt could be handled
         between re-enabling interrupts and waiting for the next
         one to occur, wasting as much as one clock tick worth of
         time.

         See [IA32-v2a] "HLT", [IA32-v2b] "STI", and [IA32-v3a]
         7.11.1 "HLT Instruction". */
      asm volatile ("sti; hlt" : : : "memory");
    }
}

/* Function used as the basis for a kernel thread. */
static void
kernel_thread (thread_func *function, void *aux) 
{
  ASSERT (function != NULL);

  intr_enable ();       /* The scheduler runs with interrupts off. */
  function (aux);       /* Execute the thread function. */
  thread_exit ();       /* If function() returns, kill the thread. */
}

/* Returns the running thread. */
struct thread *
running_thread (void) 
{
  uint32_t *esp;

  /* Copy the CPU's stack pointer into `esp', and then round that
     down to the start of a page.  Because `struct thread' is
     always at the beginning of a page and the stack pointer is
     somewhere in the middle, this locates the curent thread. */
  asm ("mov %%esp, %0" : "=g" (esp));
  return pg_round_down (esp);
}

/* Returns true if T appears to point to a valid thread. */
static bool
is_thread (struct thread *t)
{
  return t != NULL && t->magic == THREAD_MAGIC;
}

/* Does basic initialization of T as a blocked thread named
   NAME. */
static void
init_thread (struct thread *t, const char *name, int priority)
{
  enum intr_level old_level;

  ASSERT (t != NULL);
  ASSERT (PRI_MIN <= priority && priority <= PRI_MAX);
  ASSERT (name != NULL);

  memset (t, 0, sizeof *t);
  t->status = THREAD_BLOCKED;
  strlcpy (t->name, name, sizeof t->name);
  t->stack = (uint8_t *) t + PGSIZE;
  t->magic = THREAD_MAGIC;
 
  if (thread_mlfqs)                                                 /* @A3A */
   {
  t->recent_cpu = 0;                                                /* @A3A */
  
  /* The initial thread starts with a nice value of zero.              @A3A */
  if (list_empty(&all_list))                                        /* @A3A */
     t->nice = 0;                                                   /* @A3A */
  /* Other threads start with a nice value inherited from their parent thread. */
  /* @A3A */
  else                                                              /* @A3A */
     t->nice = thread_current()->nice;                              /* @A3A */

  thread_calc_priority (t);                                         /* @A3A */
   }                                                                  
  else                                                              /* @A3A */
   {
  t->priority = priority;
  t->effective_priority = priority;                                 /* @A2A */
  t->waited_thread = NULL;                                          /* @A2A */
  t->is_dirty = false;                                              /* @A2A */
  list_init (&(t->waiting_list));                                   /* @A2A */
   }

  old_level = intr_disable ();
  list_push_back (&all_list, &t->allelem);
  intr_set_level (old_level);
}

/* Allocates a SIZE-byte frame at the top of thread T's stack and
   returns a pointer to the frame's base. */
static void *
alloc_frame (struct thread *t, size_t size) 
{
  /* Stack data is always allocated in word-size units. */
  ASSERT (is_thread (t));
  ASSERT (size % sizeof (uint32_t) == 0);

  t->stack -= size;
  return t->stack;
}

/* Chooses and returns the next thread to be scheduled.  Should
   return a thread from the run queue, unless the run queue is
   empty.  (If the running thread can continue running, then it
   will be in the run queue.)  If the run queue is empty, return
   idle_thread. */
static struct thread *
next_thread_to_run (void) 
{
  if (thread_mlfqs == false)                                            /* @A3A */
   {
  if (list_empty (&ready_list))
    return idle_thread;

/*  else                                                                    @A2D */
/*  return list_entry (list_pop_front (&ready_list), struct thread, elem);  @A2D */

  int max_priority = -1;                                                 /* @A2A */
  struct thread *next_thread = NULL;                                     /* @A2A */
  struct list_elem *e;                                    		 /* @A2A */        
  for ( e = list_begin (&ready_list);                                    /* @A2A */
		e != list_end(&ready_list); e = list_next(e) )           /* @A2A */ 
   {
      struct thread *t = list_entry(e, struct thread, elem);             /* @A2A */
      ASSERT(is_thread(t));                                              /* @A2A */
      int effective_priority = thread_get_effective_priority(t);         /* @A2A */
      // msg("ready list: %s %u\n", t->name, effective_priority);        /* @A2A */
      if (effective_priority > max_priority)                             /* @A2A */
       {
            max_priority = effective_priority;                           /* @A2A */
            next_thread = t;                                             /* @A2A */
       }
   }
  
   list_remove(&(next_thread->elem));                                    /* @A2A */
   return next_thread;                                                   /* @A2A */
   }
   else                                                                  /* @A3A */
    {
    /* This type of scheduler maintains several queues of 
     * ready-to-run threads, where each queue holds threads with a
     * diㄦent priority.  At any given time, the scheduler chooses 
     * a thread from the highest-priority non-empty queue.  
     * If the highest-priority queue contains multiple threads, 
     * then they run in round robin" order
     * @A3A */
    struct thread *next_thread = NULL;                                   /* @A3A */
    int priority;                                                        /* @A3A */
    for (priority = PRI_MAX; priority >= PRI_MIN; priority--)            /* @A3A */
     {
        if (!list_empty(&ready_queues[priority]))                        /* @A3A */
         {
	    struct list_elem *e =                                        /* @A3A */
                           list_pop_front (&ready_queues[priority]);     /* @A3A */
            next_thread = list_entry (e, struct thread, elem);           /* @A3A */
            break;                                                       /* @A3A */
         }
     }

    if (next_thread == NULL)
       return idle_thread;
    else {
       ASSERT(is_thread(next_thread));                                   /* @A3A */
       ASSERT(next_thread->status == THREAD_READY);                      /* @A3A */
       return next_thread;                                               /* @A2A */
    }
    }

}

/* Completes a thread switch by activating the new thread's page
   tables, and, if the previous thread is dying, destroying it.

   At this function's invocation, we just switched from thread
   PREV, the new thread is already running, and interrupts are
   still disabled.  This function is normally invoked by
   thread_schedule() as its final action before returning, but
   the first time a thread is scheduled it is called by
   switch_entry() (see switch.S).

   It's not safe to call printf() until the thread switch is
   complete.  In practice that means that printf()s should be
   added at the end of the function.

   After this function and its caller returns, the thread switch
   is complete. */
void
thread_schedule_tail (struct thread *prev)
{
  struct thread *cur = running_thread ();
  
  ASSERT (intr_get_level () == INTR_OFF);

  /* Mark us as running. */
  cur->status = THREAD_RUNNING;

  /* Start new time slice. */
  thread_ticks = 0;

#ifdef USERPROG
  /* Activate the new address space. */
  process_activate ();
#endif

  /* If the thread we switched from is dying, destroy its struct
     thread.  This must happen late so that thread_exit() doesn't
     pull out the rug under itself.  (We don't free
     initial_thread because its memory was not obtained via
     palloc().) */
  if (prev != NULL && prev->status == THREAD_DYING && prev != initial_thread) 
    {
      ASSERT (prev != cur);
      palloc_free_page (prev);
      ready_threads--;                                               /* @A3A */
    }
}

/* Schedules a new process.  At entry, interrupts must be off and
   the running process's state must have been changed from
   running to some other state.  This function finds another
   thread to run and switches to it.

   It's not safe to call printf() until thread_schedule_tail()
   has completed. */
static void
schedule (void) 
{
  struct thread *cur = running_thread ();
  struct thread *next = next_thread_to_run ();
  struct thread *prev = NULL;

  ASSERT (intr_get_level () == INTR_OFF);
  ASSERT (cur->status != THREAD_RUNNING);
  ASSERT (is_thread (next));

  if (cur != next)
    prev = switch_threads (cur, next);
  thread_schedule_tail (prev);
}

/* Returns a tid to use for a new thread. */
static tid_t
allocate_tid (void) 
{
  static tid_t next_tid = 1;
  tid_t tid;

  lock_acquire (&tid_lock);
  tid = next_tid++;
  lock_release (&tid_lock);

  return tid;
}

//void print_waiting_list(struct list *waiting_list) {                     /* @A2A */
//  int current_priority = -1;                                             /* @A2A */
//  struct thread *next_thread = NULL;                                     /* @A2A */
//  struct list_elem *e;                                    		 /* @A2A */        
//  msg("Entry of print_waiting_list\n");
//  for ( e = list_begin (waiting_list);                                   /* @A2A */
//		e != list_end(waiting_list); e = list_next(e) )          /* @A2A */ 
//   {
//      struct thread *t = list_entry(e, struct thread, waitelem);         /* @A2A */
//      ASSERT(is_thread(t));                                              /* @A2A */
//      msg("waiting list: %s %s\n", t->name, t->waited_thread->name);     /* @A2A */
//   }
//  msg("End of print_waiting_list\n");
//}

/* Offset of `stack' member within `struct thread'.
   Used by switch.S, which can't figure it out on its own. */
uint32_t thread_stack_ofs = offsetof (struct thread, stack);
