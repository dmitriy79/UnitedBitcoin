#pragma once
#include <fjson/crypto/base64.hpp>
#include <fjson/variant.hpp>
#include <fjson/reflect/reflect.hpp>

namespace fjson {

  /**
   *  Provides a fixed size array that is easier for templates to specialize 
   *  against or overload than T[N].  
   */
  template<typename T, size_t N>
  class array {
    public:
    /**
     *  Checked indexing (when in debug build) that also simplifies dereferencing
     *  when you have an array<T,N>*.    
     */
    ///@{
    T&       at( size_t pos )      { assert( pos < N); return data[pos]; }
    const T& at( size_t pos )const { assert( pos < N); return data[pos]; }
    ///@}
    
    T*           begin()       {  return &data[0]; }
    const T*     begin()const  {  return &data[0]; }
    const T*     end()const    {  return &data[N]; }

    size_t       size()const { return N; }
    
    T data[N];
  };

  /** provided for default 0 init */
  template<size_t N>
  class array<unsigned char,N>
  {
    public:
    typedef unsigned char T;
    array(){ memset( data, 0, sizeof(data) ); }
    /**
     *  Checked indexing (when in debug build) that also simplifies dereferencing
     *  when you have an array<T,N>*.    
     */
    ///@{
    T&       at( size_t pos )      { assert( pos < N); return data[pos]; }
    const T& at( size_t pos )const { assert( pos < N); return data[pos]; }
    ///@}
    
    T*           begin()       {  return &data[0]; }
    const T*     begin()const  {  return &data[0]; }
    const T*     end()const    {  return &data[N]; }

    size_t       size()const { return N; }
    
    T data[N];
  };

  /** provided for default 0 init */
  template<size_t N>
  class array<char,N>
  {
    public:
    typedef char T;
    array(){ memset( data, 0, sizeof(data) ); }
    /**
     *  Checked indexing (when in debug build) that also simplifies dereferencing
     *  when you have an array<T,N>*.    
     */
    ///@{
    T&       at( size_t pos )      { assert( pos < N); return data[pos]; }
    const T& at( size_t pos )const { assert( pos < N); return data[pos]; }
    ///@}
    
    T*           begin()       {  return &data[0]; }
    const T*     begin()const  {  return &data[0]; }
    const T*     end()const    {  return &data[N]; }

    size_t       size()const { return N; }
    
    T data[N];
  };

  template<typename T, size_t N>
  bool operator == ( const array<T,N>& a, const array<T,N>& b )
  { return 0 == memcmp( a.data, b.data, N*sizeof(T) ); }
  template<typename T, size_t N>
  bool operator < ( const array<T,N>& a, const array<T,N>& b )
  { return  memcmp( a.data, b.data, N*sizeof(T) ) < 0 ; }

  template<typename T, size_t N>
  bool operator > ( const array<T,N>& a, const array<T,N>& b )
  { return  memcmp( a.data, b.data, N*sizeof(T) ) > 0 ; }

  template<typename T, size_t N>
  bool operator != ( const array<T,N>& a, const array<T,N>& b )
  { return 0 != memcmp( a.data, b.data, N*sizeof(T) ); }

  template<typename T, size_t N>
  void to_variant( const array<T,N>& bi, variant& v )
  {
     v = std::vector<char>( (const char*)&bi, ((const char*)&bi) + sizeof(bi) );
  }
  template<typename T, size_t N>
  void from_variant( const variant& v, array<T,N>& bi )
  {
    std::vector<char> ve = v.as< std::vector<char> >();
    if( ve.size() )
    {
        memcpy(&bi, ve.data(), fjson::min<size_t>(ve.size(),sizeof(bi)) );
    }
    else
        memset( &bi, char(0), sizeof(bi) );
  }


  template<typename T,size_t N> struct get_typename< fjson::array<T,N> >  
  { 
     static const char* name()  
     { 
        static std::string _name = std::string("fjson::array<")+std::string(fjson::get_typename<T>::name())+","+ fjson::to_string(N) + ">";
        return _name.c_str();
     } 
  }; 
}

#include <unordered_map>
#include <fjson/crypto/city.hpp>
namespace std
{
    template<typename T, size_t N>
    struct hash<fjson::array<T,N> >
    {
       size_t operator()( const fjson::array<T,N>& e )const
       {
          return fjson::city_hash_size_t( (char*)&e, sizeof(e) );
       }
    };
}

