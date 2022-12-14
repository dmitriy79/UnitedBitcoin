#ifndef _FJSON_PRIORITY_HPP_
#define  _FJSON_PRIORITY_HPP_

namespace fjson {
  /**
   *  An integer value used to sort asynchronous tasks.  The higher the
   *  prioirty the sooner it will be run.
   */
  class priority {
    public:
    explicit priority( int v = 0):value(v){}
    priority( const priority& p ):value(p.value){}
    bool operator < ( const priority& p )const {
       return value < p.value;
    }
    static priority max() { return priority(10000); }
    static priority min() { return priority(-10000); }
    static priority _internal__priority_for_short_sleeps() { return priority(-100000); }
    int value;
  };
}
#endif //  _FJSON_PRIORITY_HPP_
