#pragma once
#include <fjson/variant.hpp>
#include <fjson/container/flat_fwd.hpp>
#include <boost/container/flat_map.hpp>
#include <boost/container/flat_set.hpp>
#include <fjson/io/raw_fwd.hpp>

namespace fjson {
   namespace raw {
       template<typename Stream, typename T>
       inline void pack( Stream& s, const flat_set<T>& value ) {
         pack( s, unsigned_int((uint32_t)value.size()) );
         auto itr = value.begin();
         auto end = value.end();
         while( itr != end ) {
           fjson::raw::pack( s, *itr );
           ++itr;
         }
       }
       template<typename Stream, typename T>
       inline void unpack( Stream& s, flat_set<T>& value ) {
         unsigned_int size; unpack( s, size );
         value.clear();
         FJSON_ASSERT( size.value*sizeof(T) < MAX_ARRAY_ALLOC_SIZE );
         value.reserve(size.value);
         for( uint32_t i = 0; i < size.value; ++i )
         {
             T tmp;
             fjson::raw::unpack( s, tmp );
             value.insert( std::move(tmp) );
         }
       }
       template<typename Stream, typename K, typename V>
       inline void pack( Stream& s, const flat_map<K,V>& value ) {
         pack( s, unsigned_int((uint32_t)value.size()) );
         auto itr = value.begin();
         auto end = value.end();
         while( itr != end ) {
           fjson::raw::pack( s, *itr );
           ++itr;
         }
       }
       template<typename Stream, typename K, typename V>
       inline void unpack( Stream& s, flat_map<K,V>& value ) 
       {
         unsigned_int size; unpack( s, size );
         value.clear();
         FJSON_ASSERT( size.value*(sizeof(K)+sizeof(V)) < MAX_ARRAY_ALLOC_SIZE );
         value.reserve(size.value);
         for( uint32_t i = 0; i < size.value; ++i )
         {
             std::pair<K,V> tmp;
             fjson::raw::unpack( s, tmp );
             value.insert( std::move(tmp) );
         }
       }
   } // namespace raw


   template<typename T>
   void to_variant( const flat_set<T>& var,  variant& vo )
   {
       std::vector<variant> vars(var.size());
       size_t i = 0;
       for( auto itr = var.begin(); itr != var.end(); ++itr, ++i )
          vars[i] = variant(*itr);
       vo = vars;
   }
   template<typename T>
   void from_variant( const variant& var,  flat_set<T>& vo )
   {
      const variants& vars = var.get_array();
      vo.clear();
      vo.reserve( vars.size() );
      for( auto itr = vars.begin(); itr != vars.end(); ++itr )
         vo.insert( itr->as<T>() );
   }

   template<typename K, typename T>
   void to_variant( const flat_map<K, T>& var,  variant& vo )
   {
       std::vector< variant > vars(var.size());
       size_t i = 0;
       for( auto itr = var.begin(); itr != var.end(); ++itr, ++i )
          vars[i] = fjson::variant(*itr);
       vo = vars;
   }
   template<typename K, typename T>
   void from_variant( const variant& var,  flat_map<K, T>& vo )
   {
      const variants& vars = var.get_array();
      vo.clear();
      for( auto itr = vars.begin(); itr != vars.end(); ++itr )
         vo.insert( itr->as< std::pair<K,T> >() );

   }

}
