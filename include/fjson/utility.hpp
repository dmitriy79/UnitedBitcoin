#pragma once
#include <stdint.h>
#include <new>

#ifdef _MSC_VER
#pragma warning(disable: 4482) // nonstandard extension used enum Name::Val, standard in C++11
#define NO_RETURN __declspec(noreturn)
#else
#define NO_RETURN __attribute__((noreturn))
#endif


//namespace std {
//  typedef decltype(sizeof(int)) size_t;
//  typedef decltype(nullptr) nullptr_t;
//}

namespace fjson {
  using std::size_t;
  typedef decltype(nullptr) nullptr_t;

  template<typename T> struct remove_reference           { typedef T type;       };
  template<typename T> struct remove_reference<T&>       { typedef T type;       };
  template<typename T> struct remove_reference<T&&>      { typedef T type;       };

  template<typename T> struct deduce           { typedef T type; };
  template<typename T> struct deduce<T&>       { typedef T type; };
  template<typename T> struct deduce<const T&> { typedef T type; };
  template<typename T> struct deduce<T&&>      { typedef T type; };
  template<typename T> struct deduce<const T&&>{ typedef T type; };

  template<typename T>
  typename fjson::remove_reference<T>::type&& move( T&& t ) { return static_cast<typename fjson::remove_reference<T>::type&&>(t); }

  template<typename T, typename U>
  inline T&& forward( U&& u ) { return static_cast<T&&>(u); }

  struct true_type  { enum _value { value = 1 }; };
  struct false_type { enum _value { value = 0 }; };

  namespace detail {
    template<typename T> fjson::true_type is_class_helper(void(T::*)());
    template<typename T> fjson::false_type is_class_helper(...);
  }

  template<typename T>
  struct is_class { typedef decltype(detail::is_class_helper<T>(0)) type; enum value_enum { value = type::value }; };
#ifdef min 
#undef min
#endif
  template<typename T>
  const T& min( const T& a, const T& b ) { return a < b ? a: b; }

}
  // outside of namespace fjson becuase of VC++ conflict with std::swap
  template<typename T>
  void fjson_swap( T& a, T& b ) {     
    T tmp = fjson::move(a);
    a = fjson::move(b);
    b = fjson::move(tmp);
  }

#define LLCONST(constant)   static_cast<int64_t>(constant##ll)
#define ULLCONST(constant)  static_cast<uint64_t>(constant##ull)

