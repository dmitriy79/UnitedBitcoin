#pragma once
#include <fjson/time.hpp>
#include <fjson/thread/spin_yield_lock.hpp>

// This file defines a macro:
//   ASSERT_TASK_NOT_PREEMPTED()
// This macro is used to declare that the current scope is not expected to yield.
// If the task does yield, it will generate an assertion.
//
// Use this when you you're coding somethign that must not yield, and you think you
// have done it (just by virtue of the fact that you don't think you've called any
// functions that could yield).  This will help detect when your assumptions are
// wrong and you accidentally call something that yields.
//
// This has no cost in release mode, and is extremely cheap in debug mode.

#define FJSON_NON_PREEMPTABLE_SCOPE_COMBINE_HELPER(x,y) x ## y
#define FJSON_NON_PREEMPTABLE_SCOPE_COMBINE(x,y) FJSON_NON_PREEMPTABLE_SCOPE_COMBINE_HELPER(x,y)

#ifdef NDEBUG
# define ASSERT_TASK_NOT_PREEMPTED() do {} while (0)
#else
# define ASSERT_TASK_NOT_PREEMPTED() fjson::non_preemptable_scope_check FJSON_NON_PREEMPTABLE_SCOPE_COMBINE(scope_checker_, __LINE__)

namespace fjson 
{ 
  class non_preemptable_scope_check
  {
  public:
    non_preemptable_scope_check();
    ~non_preemptable_scope_check();
  };
} // namespace fjson
#endif

