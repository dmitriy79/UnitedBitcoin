#include <fjson/thread/thread.hpp>
#include <fjson/string.hpp>
#include <fjson/time.hpp>
#include <boost/thread.hpp>
#include "context.hpp"
#include <boost/thread/condition_variable.hpp>
#include <boost/thread.hpp>
#include <boost/atomic.hpp>
#include <vector>
//#include <fjson/logger.hpp>

namespace fjson {
    struct sleep_priority_less {
        bool operator()( const context::ptr& a, const context::ptr& b ) {
            return a->resume_time > b->resume_time;
        }
    };
    class thread_d {

        public:
           thread_d(fjson::thread& s)
            :self(s), boost_thread(0),
             task_in_queue(0),
             next_posted_num(1),
             done(false),
             current(0),
             pt_head(0),
             blocked(0),
             next_unused_task_storage_slot(0)
#ifndef NDEBUG
             ,non_preemptable_scope_count(0)
#endif
            { 
              static boost::atomic<int> cnt(0);
              name = fjson::string("th_") + char('a'+cnt++); 
//              printf("thread=%p\n",this);
            }

            ~thread_d()
            {
              delete current;
              fjson::context* temp;
              for (fjson::context* ready_context : ready_heap)
                delete ready_context;
              ready_heap.clear();
              while (blocked)
              {
                temp = blocked->next;
                delete blocked;
                blocked = temp;
              }
              /*
              while (pt_head)
              {
                temp = pt_head->next;
                delete pt_head;
                pt_head = temp;
              }
              */
              //ilog("");
             if (boost_thread)
             {
               boost_thread->detach();
               delete boost_thread;
             }
           }

           fjson::thread&             self;
           boost::thread* boost_thread;
           stack_allocator                  stack_alloc;
           boost::condition_variable        task_ready;
           boost::mutex                     task_ready_mutex;

           boost::atomic<task_base*>       task_in_queue;
           std::vector<task_base*>         task_pqueue;    // heap of tasks that have never started, ordered by proirity & scheduling time
           uint64_t                        next_posted_num; // each task or context gets assigned a number in the order it is ready to execute, tracked here
           std::vector<task_base*>         task_sch_queue; // heap of tasks that have never started but are scheduled for a time in the future, ordered by the time they should be run
           std::vector<fjson::context*>       sleep_pqueue;   // heap of running tasks that have sleeped, ordered by the time they should resume
           std::vector<fjson::context*>       free_list;      // list of unused contexts that are ready for deletion

           bool                     done;
           fjson::string               name;
           fjson::context*             current;     // the currently-executing task in this thread

           fjson::context*             pt_head;     // list of contexts that can be reused for new tasks

           std::vector<fjson::context*> ready_heap; // priority heap of contexts that are ready to run

           fjson::context*             blocked;     // linked list of contexts (using 'next_blocked') blocked on promises via wait()

           // values for thread specific data objects for this thread
           std::vector<detail::specific_data_info> thread_specific_data;
           // values for task_specific data for code executing on a thread that's
           // not a task launched by async (usually the default task on the main 
           // thread in a process)
           std::vector<detail::specific_data_info> non_task_specific_data;
           unsigned next_unused_task_storage_slot;

#ifndef NDEBUG
           unsigned                 non_preemptable_scope_count;
#endif

#if 0
           void debug( const fjson::string& s ) {
          return;
              //boost::unique_lock<boost::mutex> lock(log_mutex());

              fjson::cerr<<"--------------------- "<<s.c_str()<<" - "<<current;
              if( current && current->cur_task ) fjson::cerr<<'('<<current->cur_task->get_desc()<<')';
              fjson::cerr<<" ---------------------------\n";
              fjson::cerr<<"  Ready\n";
              fjson::context* c = ready_head;
              while( c ) {
                fjson::cerr<<"    "<<c;
                if( c->cur_task ) fjson::cerr<<'('<<c->cur_task->get_desc()<<')'; 
                fjson::context* p = c->caller_context;
                while( p ) {
                  fjson::cerr<<"  ->  "<<p;
                  p = p->caller_context;
                }
                fjson::cerr<<"\n";
                c = c->next;
              }
              fjson::cerr<<"  Blocked\n";
              c = blocked;
              while( c ) {
                fjson::cerr<<"   ctx: "<< c; 
                if( c->cur_task ) fjson::cerr<<'('<<c->cur_task->get_desc()<<')'; 
                fjson::cerr << " blocked on prom: ";
                for( uint32_t i = 0; i < c->blocking_prom.size(); ++i ) {
                  fjson::cerr<<c->blocking_prom[i].prom<<'('<<c->blocking_prom[i].prom->get_desc()<<')';
                  if( i + 1 < c->blocking_prom.size() ) { 
                    fjson::cerr<<",";
                  }
                }

                fjson::context* p = c->caller_context;
                while( p ) {
                  fjson::cerr<<"  ->  "<<p;
                  p = p->caller_context;
                }
                fjson::cerr<<"\n";
                c = c->next_blocked;
              }
              fjson::cerr<<"-------------------------------------------------\n";
           }
#endif
            // insert at from of blocked linked list
           inline void add_to_blocked( fjson::context* c ) 
           {
              c->next_blocked = blocked;
              blocked = c;
           }

           void pt_push_back(fjson::context* c) 
           {
              c->next = pt_head;
              pt_head = c;
              /* 
              fjson::context* n = pt_head;
              int i = 0;
              while( n ) {
                ++i;
                n = n->next;
              }
              wlog( "idle context...%2%  %1%", c, i );
              */
           }

          fjson::context::ptr ready_pop_front() 
          {
            fjson::context* highest_priority_context = ready_heap.front();
            std::pop_heap(ready_heap.begin(), ready_heap.end(), task_priority_less());
            ready_heap.pop_back();
            return highest_priority_context;
          }
           
           void add_context_to_ready_list(context* context_to_add, bool at_end = false)
           {

             context_to_add->context_posted_num = next_posted_num++;
             ready_heap.push_back(context_to_add);
             std::push_heap(ready_heap.begin(), ready_heap.end(), task_priority_less());
           }

          struct task_priority_less 
          {
            bool operator()(const task_base* a, const task_base* b) const
            {
              return a->_prio.value < b->_prio.value ? true : 
                                                       (a->_prio.value > b->_prio.value ? false : 
                                                                                          a->_posted_num > b->_posted_num);
            }
            bool operator()(const task_base* a, const context* b) const
            {
              return a->_prio.value < b->prio.value ? true : 
                                                      (a->_prio.value > b->prio.value ? false : 
                                                                                        a->_posted_num > b->context_posted_num);
            }
            bool operator()(const context* a, const task_base* b) const
            {
              return a->prio.value < b->_prio.value ? true : 
                                                      (a->prio.value > b->_prio.value ? false : 
                                                                                        a->context_posted_num > b->_posted_num);
            }
            bool operator()(const context* a, const context* b) const
            {
              return a->prio.value < b->prio.value ? true : 
                                                     (a->prio.value > b->prio.value ? false : 
                                                                                      a->context_posted_num > b->context_posted_num);
            }
          };

          struct task_when_less 
          {
            bool operator()( task_base* a, task_base* b ) 
            {
              return a->_when > b->_when;
            }
          };

           void enqueue( task_base* t ) 
           {
              time_point now = time_point::now();
              task_base* cur = t;

              // the linked list of tasks passed to enqueue is in the reverse order of
              // what you'd expect -- the first task to be scheduled is at the end of
              // the list.  We'll rectify the ordering by assigning the _posted_num
              // in reverse order
              unsigned num_ready_tasks = 0;
              while (cur)
              {
                if (cur->_when <= now)
                  ++num_ready_tasks;
                cur = cur->_next;
              }

              cur = t;
              next_posted_num += num_ready_tasks;
              unsigned tasks_posted = 0;
              while (cur)
              {
                if (cur->_when > now)
                {
                  task_sch_queue.push_back(cur);
                  std::push_heap(task_sch_queue.begin(),
                                 task_sch_queue.end(), task_when_less());
                }
                else
                {
                  cur->_posted_num = next_posted_num - (++tasks_posted);
                  task_pqueue.push_back(cur);
                  std::push_heap(task_pqueue.begin(),
                                 task_pqueue.end(), task_priority_less());
                  BOOST_ASSERT(this == thread::current().my);
                }
                cur = cur->_next;
              }
           }

          void move_newly_scheduled_tasks_to_task_pqueue()
          {
            BOOST_ASSERT(this == thread::current().my);

            // first, if there are any new tasks on 'task_in_queue', which is tasks that 
            // have been just been async or scheduled, but we haven't processed them.
            // move them into the task_sch_queue or task_pqueue, as appropriate

            //DLN: changed from memory_order_consume for boost 1.55.
            //This appears to be safest replacement for now, maybe
            //can be changed to relaxed later, but needs analysis.
            task_base* pending_list = task_in_queue.exchange(0, boost::memory_order_seq_cst);
            if (pending_list)
              enqueue(pending_list);

            // second, walk through task_sch_queue and move any scheduled tasks that are now
            // able to run (because their scheduled time has arrived) to task_pqueue

            while (!task_sch_queue.empty() &&
                   task_sch_queue.front()->_when <= time_point::now())
            {
              task_base* ready_task = task_sch_queue.front();
              std::pop_heap(task_sch_queue.begin(), task_sch_queue.end(), task_when_less());
              task_sch_queue.pop_back();

              ready_task->_posted_num = next_posted_num++;
              task_pqueue.push_back(ready_task);
              std::push_heap(task_pqueue.begin(), task_pqueue.end(), task_priority_less());
            }
          }

           task_base* dequeue() 
           {
                // get a new task
                BOOST_ASSERT( this == thread::current().my );

                assert(!task_pqueue.empty());
                task_base* p = task_pqueue.front();
                std::pop_heap(task_pqueue.begin(), task_pqueue.end(), task_priority_less() );
                task_pqueue.pop_back();
                return p;
           }

           bool process_canceled_tasks()
           {
              bool canceled_task = false;
              for( auto task_itr = task_sch_queue.begin();
                   task_itr != task_sch_queue.end();
                   )
              {
                 if( (*task_itr)->canceled() )
                 {
                    (*task_itr)->run();
                    (*task_itr)->release();
                    task_itr = task_sch_queue.erase(task_itr);
                    canceled_task = true;
                    continue;
                 }
                 ++task_itr;
              }

              if( canceled_task )  
                 std::make_heap( task_sch_queue.begin(), task_sch_queue.end(), task_when_less() );

              return canceled_task;
           }
           
           /**
            * This should be before or after a context switch to
            * detect quit/cancel operations and throw an exception.
            */
           void check_fiber_exceptions() 
           {
              if( current && current->canceled ) 
              {
#ifdef NDEBUG
                FJSON_THROW_EXCEPTION( canceled_exception, "" );
#else
                FJSON_THROW_EXCEPTION( canceled_exception, "cancellation reason: ${reason}", ("reason", current->cancellation_reason ? current->cancellation_reason : "[none given]"));
#endif
              } 
              else if( done )  
              {
                ilog( "throwing canceled exception" );
                FJSON_THROW_EXCEPTION( canceled_exception, "cancellation reason: thread quitting" ); 
                // BOOST_THROW_EXCEPTION( thread_quit() );
              }
           }
           
           /**
            *   Find the next available context and switch to it.
            *   If none are available then create a new context and
            *   have it wait for something to do.
            */
           bool start_next_fiber( bool reschedule = false ) 
           {
              /* If this assert fires, it means you are executing an operation that is causing
               * the current task to yield, but there is a ASSERT_TASK_NOT_PREEMPTED() in effect
               * (somewhere up the stack) */
              assert(non_preemptable_scope_count == 0);

              /* If this assert fires, it means you are causing the current task to yield while
               * in the middle of handling an exception.  The boost::context library's behavior
               * is not well-defined in this case, and this has the potential to corrupt the
               * exception stack, often resulting in a crash very soon after this */
              /* NB: At least on Win64, this only catches a yield while in the body of 
               * a catch block; it fails to catch a yield while unwinding the stack, which 
               * is probably just as likely to cause crashes */
              assert(std::current_exception() == std::exception_ptr());

              check_for_timeouts();
              if( !current ) 
                current = new fjson::context( &fjson::thread::current() );

              priority original_priority = current->prio;

              // check to see if any other contexts are ready
              if (!ready_heap.empty())
              {
                fjson::context* next = ready_pop_front();
                if (next == current)
                {
                  // elog( "next == current... something went wrong" );
                  assert(next != current);
                  return false;
                }
                BOOST_ASSERT(next != current);

                // jump to next context, saving current context
                fjson::context* prev = current;
                current = next;
                if (reschedule)
                {
                  current->prio = priority::_internal__priority_for_short_sleeps();
                  add_context_to_ready_list(prev, true);
                }
                // slog( "jump to %p from %p", next, prev );
                // fc_dlog( logger::get("fc_context"), "from ${from} to ${to}", ( "from", int64_t(prev) )( "to", int64_t(next) ) ); 
#if BOOST_VERSION >= 105600
                bc::jump_fcontext( &prev->my_context, next->my_context, 0 );
#elif BOOST_VERSION >= 105300
                bc::jump_fcontext( prev->my_context, next->my_context, 0 );
#else
                bc::jump_fcontext( &prev->my_context, &next->my_context, 0 );
#endif
                BOOST_ASSERT( current );
                BOOST_ASSERT( current == prev );
                //current = prev;
              } 
              else 
              { 
                // all contexts are blocked, create a new context 
                // that will process posted tasks...
                fjson::context* prev = current;

                fjson::context* next = nullptr;
                if( pt_head ) 
                { 
                  // grab cached context
                  next = pt_head;
                  pt_head = pt_head->next;
                  next->next = 0;
                  next->reinitialize();
                } 
                else 
                { 
                  // create new context.
                  next = new fjson::context( &thread_d::start_process_tasks, stack_alloc,
                                          &fjson::thread::current() );
                }

                current = next;
                if( reschedule )  
                {
                  current->prio = priority::_internal__priority_for_short_sleeps();
                  add_context_to_ready_list(prev, true);
                }

                // slog( "jump to %p from %p", next, prev );
                // fc_dlog( logger::get("fc_context"), "from ${from} to ${to}", ( "from", int64_t(prev) )( "to", int64_t(next) ) );
#if BOOST_VERSION >= 105600
                bc::jump_fcontext( &prev->my_context, next->my_context, (intptr_t)this );
#elif BOOST_VERSION >= 105300
                bc::jump_fcontext( prev->my_context, next->my_context, (intptr_t)this );
#else
                bc::jump_fcontext( &prev->my_context, &next->my_context, (intptr_t)this );
#endif
                BOOST_ASSERT( current );
                BOOST_ASSERT( current == prev );
                //current = prev;
              }

              if (reschedule)
                current->prio = original_priority;

              if( current->canceled ) 
              {
                //current->canceled = false;
#ifdef NDEBUG
                FJSON_THROW_EXCEPTION( canceled_exception, "" );
#else
                FJSON_THROW_EXCEPTION( canceled_exception, "cancellation reason: ${reason}", ("reason", current->cancellation_reason ? current->cancellation_reason : "[none given]"));
#endif
              }

              return true;
           }

           static void start_process_tasks( intptr_t my ) 
           {
              thread_d* self = (thread_d*)my;
              try 
              {
                self->process_tasks();
              } 
              catch ( canceled_exception& ) { /* allowed exception */  }
              catch ( ... ) 
              {
                elog( "fiber ${name} exited with uncaught exception: ${e}", ("e",fjson::except_str())("name", self->name) );
                // assert( !"fiber exited with uncaught exception" );
                //TODO replace errror           fjson::cerr<<"fiber exited with uncaught exception:\n "<< 
                // boost::current_exception_diagnostic_information() <<std::endl;
              }
              self->free_list.push_back(self->current);
              self->start_next_fiber( false );
           }

           void run_next_task() 
           {
              task_base* next = dequeue();

              next->_set_active_context( current );
              current->cur_task = next;
              next->run();
              current->cur_task = 0;
              next->_set_active_context(0);
              next->release();
              current->reinitialize();
           }

           bool has_next_task() 
           {
             if( task_pqueue.size() ||
                 (task_sch_queue.size() && task_sch_queue.front()->_when <= time_point::now()) ||
                 task_in_queue.load( boost::memory_order_relaxed ) )
               return true;
             return false;
           }

           void clear_free_list() 
           {
              for( uint32_t i = 0; i < free_list.size(); ++i ) 
                delete free_list[i];
              free_list.clear();
           }

           void process_tasks() 
           {
              while( !done || blocked ) 
              {
                // move all new tasks to the task_pqueue
                move_newly_scheduled_tasks_to_task_pqueue();

                // move all now-ready sleeping tasks to the ready list
                check_for_timeouts();

                if (!task_pqueue.empty())
                {
                  if (!ready_heap.empty())
                  {
                    // a new task and an existing task are both ready to go
                    if (task_priority_less()(task_pqueue.front(), ready_heap.front()))
                    {
                      // run the existing task first
                      pt_push_back(current);
                      start_next_fiber(false);
                      continue;
                    }
                  }

                  // if we made it here, either there's no ready context, or the ready context is
                  // scheduled after the ready task, so we should run the task first
                  run_next_task();
                  continue;
                }

                // if I have something else to do other than
                // process tasks... do it.
                if (!ready_heap.empty())
                { 
                   pt_push_back( current ); 
                   start_next_fiber(false);  
                   continue;
                }

                if( process_canceled_tasks() ) 
                  continue;

                clear_free_list();

                { // lock scope
                  boost::unique_lock<boost::mutex> lock(task_ready_mutex);
                  if( has_next_task() ) 
                    continue;
                  time_point timeout_time = check_for_timeouts();
                  
                  if( done ) 
                    return;
                  if( timeout_time == time_point::maximum() ) 
                    task_ready.wait( lock );
                  else if( timeout_time != time_point::min() ) 
                  {
                     // there may be tasks that have been canceled we should filter them out now
                     // rather than waiting... 


                    /* This bit is kind of sloppy -- this wait was originally implemented as a wait
                     * with respect to boost::chrono::system_clock.  This behaved rather comically
                     * if you were to do a:
                     *   fjson::usleep(fjson::seconds(60));
                     * and then set your system's clock back a month, it would sleep for a month
                     * plus a minute before waking back up (this happened on Linux, it seems 
                     * Windows' behavior in this case was less unexpected).
                     *
                     * Boost Chrono's steady_clock will always increase monotonically so it will 
                     * avoid this behavior.
                     *
                     * Right now we don't really have a way to distinguish when a timeout_time is coming
                     * from a function that takes a relative time like fjson::usleep() vs something
                     * that takes an absolute time like fjson::promise::wait_until(), so we can't always
                     * do the right thing here.
                     */ 
                    task_ready.wait_until( lock, boost::chrono::steady_clock::now() + 
                                                 boost::chrono::microseconds(timeout_time.time_since_epoch().count() - time_point::now().time_since_epoch().count()) );
                  }
                }
              }
           }
    /**
     *    Return system_clock::time_point::min() if tasks have timed out
     *    Retunn system_clock::time_point::max() if there are no scheduled tasks
     *    Return the time the next task needs to be run if there is anything scheduled.
     */
    time_point check_for_timeouts() 
    {
        if( !sleep_pqueue.size() && !task_sch_queue.size() ) 
        {
          // ilog( "no timeouts ready" );
          return time_point::maximum();
        }

        time_point next = time_point::maximum();
        if( !sleep_pqueue.empty() && next > sleep_pqueue.front()->resume_time )
          next = sleep_pqueue.front()->resume_time;
        if( !task_sch_queue.empty() && next > task_sch_queue.front()->_when )
          next = task_sch_queue.front()->_when;

        time_point now = time_point::now();
        if( now < next )
          return next;

        // move all expired sleeping tasks to the ready queue
        while( sleep_pqueue.size() && sleep_pqueue.front()->resume_time < now ) 
        {
          fjson::context::ptr c = sleep_pqueue.front();
          std::pop_heap(sleep_pqueue.begin(), sleep_pqueue.end(), sleep_priority_less() );
          // ilog( "sleep pop back..." );
          sleep_pqueue.pop_back();

          if( c->blocking_prom.size() ) 
          {
            // ilog( "timeout blocking prom" );
            c->timeout_blocking_promises();
          }
          else 
          { 
            // ilog( "..." );
            // ilog( "ready_push_front" );
            if (c != current)
              add_context_to_ready_list(c);
          }
        }
        return time_point::min();
    }

        void unblock( fjson::context* c ) 
        {
          if( fjson::thread::current().my != this ) 
          {
            self.async( [=](){ unblock(c); }, "thread_d::unblock" );
            return;
          }

          if (c != current)
            add_context_to_ready_list(c);
        }

        void yield_until( const time_point& tp, bool reschedule ) {
          check_fiber_exceptions();

          if( tp <= (time_point::now()+fjson::microseconds(10000)) ) 
            return;

          FJSON_ASSERT(std::current_exception() == std::exception_ptr(), 
                    "Attempting to yield while processing an exception");

          if( !current ) 
            current = new fjson::context(&fjson::thread::current());

          current->resume_time = tp;
          current->clear_blocking_promises();

          sleep_pqueue.push_back(current);
          std::push_heap( sleep_pqueue.begin(),
                          sleep_pqueue.end(), sleep_priority_less()   );
          
          start_next_fiber(reschedule);

          // clear current context from sleep queue...
          for( uint32_t i = 0; i < sleep_pqueue.size(); ++i ) 
          {
            if( sleep_pqueue[i] == current ) 
            {
              sleep_pqueue[i] = sleep_pqueue.back();
              sleep_pqueue.pop_back();
              std::make_heap( sleep_pqueue.begin(),
                              sleep_pqueue.end(), sleep_priority_less() );
              break;
            }
          }

          current->resume_time = time_point::maximum();
          check_fiber_exceptions();
        }

        void wait( const promise_base::ptr& p, const time_point& timeout ) {
          if( p->ready() ) 
            return;

          FJSON_ASSERT(std::current_exception() == std::exception_ptr(), 
                    "Attempting to yield while processing an exception");

          if( timeout < time_point::now() ) 
            FJSON_THROW_EXCEPTION( timeout_exception, "" );
          
          if( !current ) 
            current = new fjson::context(&fjson::thread::current()); 
          
          // slog( "                                 %1% blocking on %2%", current, p.get() );
          current->add_blocking_promise(p.get(),true);

          // if not max timeout, added to sleep pqueue
          if( timeout != time_point::maximum() ) 
          {
            current->resume_time = timeout;
            sleep_pqueue.push_back(current);
            std::push_heap( sleep_pqueue.begin(),
                            sleep_pqueue.end(), 
                            sleep_priority_less()   );
          }

          // elog( "blocking %1%", current );
          add_to_blocked( current );
          // debug("swtiching fibers..." );


          start_next_fiber();
          // slog( "resuming %1%", current );

          // slog( "                                 %1% unblocking blocking on %2%", current, p.get() );
          current->remove_blocking_promise(p.get());

          check_fiber_exceptions();
        }

        void cleanup_thread_specific_data()
        {
          for (auto iter = non_task_specific_data.begin(); iter != non_task_specific_data.end(); ++iter)
            if (iter->cleanup)
              iter->cleanup(iter->value);

          for (auto iter = thread_specific_data.begin(); iter != thread_specific_data.end(); ++iter)
            if (iter->cleanup)
              iter->cleanup(iter->value);
        }

        void notify_task_has_been_canceled()
        {
          for (fjson::context** iter = &blocked; *iter;)
          {
            if ((*iter)->canceled)
            {
              fjson::context* next_blocked = (*iter)->next_blocked;
              (*iter)->next_blocked = nullptr;
              add_context_to_ready_list(*iter);
              *iter = next_blocked;
              continue;
            }
            iter = &(*iter)->next_blocked;
          }

          bool task_removed_from_sleep_pqueue = false;
          for (auto sleep_iter = sleep_pqueue.begin(); sleep_iter != sleep_pqueue.end();)
          {
            if ((*sleep_iter)->canceled)
            {
              bool already_on_ready_list = std::find(ready_heap.begin(), ready_heap.end(), 
                                                     *sleep_iter) != ready_heap.end();
              if (!already_on_ready_list)
                add_context_to_ready_list(*sleep_iter);
              sleep_iter = sleep_pqueue.erase(sleep_iter);
              task_removed_from_sleep_pqueue = true;
            }
            else
              ++sleep_iter;
          }
          if (task_removed_from_sleep_pqueue)
            std::make_heap(sleep_pqueue.begin(), sleep_pqueue.end(), sleep_priority_less());
        }
    };
} // namespace fjson
