#include <fjson/crypto/hex.hpp>
#include <fjson/fwd_impl.hpp>
#include <openssl/sha.h>
#include <openssl/ripemd.h>
#include <string.h>
#include <fcrypto/ripemd160.hpp>
#include <fcrypto/sha512.hpp>
#include <fcrypto/sha256.hpp>
#include <fjson/variant.hpp>
#include <vector>
#include "fcrypto/_digest_common.hpp"

namespace fcrypto
{
  
ripemd160::ripemd160() { memset( _hash, 0, sizeof(_hash) ); }
ripemd160::ripemd160( const fjson::string& hex_str ) {
  fjson::from_hex( hex_str, (char*)_hash, sizeof(_hash) );  
}

fjson::string ripemd160::str()const {
  return fjson::to_hex( (char*)_hash, sizeof(_hash) );
}
ripemd160::operator string()const { return  str(); }

char* ripemd160::data()const { return (char*)&_hash[0]; }


struct ripemd160::encoder::impl {
   impl()
   {
        memset( (char*)&ctx, 0, sizeof(ctx) );
   }
   RIPEMD160_CTX ctx;
};

ripemd160::encoder::~encoder() {}
ripemd160::encoder::encoder() {
  reset();
}

ripemd160 ripemd160::hash( const fcrypto::sha512& h )
{
  return hash( (const char*)&h, sizeof(h) );
}
ripemd160 ripemd160::hash( const fcrypto::sha256& h )
{
  return hash( (const char*)&h, sizeof(h) );
}
ripemd160 ripemd160::hash( const char* d, uint32_t dlen ) {
  encoder e;
  e.write(d,dlen);
  return e.result();
}
ripemd160 ripemd160::hash( const fjson::string& s ) {
  return hash( s.c_str(), s.size() );
}

void ripemd160::encoder::write( const char* d, uint32_t dlen ) {
  RIPEMD160_Update( &my->ctx, d, dlen); 
}
ripemd160 ripemd160::encoder::result() {
  ripemd160 h;
  RIPEMD160_Final((uint8_t*)h.data(), &my->ctx );
  return h;
}
void ripemd160::encoder::reset() {
  RIPEMD160_Init( &my->ctx);  
}

ripemd160 operator << ( const ripemd160& h1, uint32_t i ) {
  ripemd160 result;
  fcrypto::detail::shift_l( h1.data(), result.data(), result.data_size(), i );
  return result;
}
ripemd160 operator ^ ( const ripemd160& h1, const ripemd160& h2 ) {
  ripemd160 result;
  result._hash[0] = h1._hash[0] ^ h2._hash[0];
  result._hash[1] = h1._hash[1] ^ h2._hash[1];
  result._hash[2] = h1._hash[2] ^ h2._hash[2];
  result._hash[3] = h1._hash[3] ^ h2._hash[3];
  result._hash[4] = h1._hash[4] ^ h2._hash[4];
  return result;
}
bool operator >= ( const ripemd160& h1, const ripemd160& h2 ) {
  return memcmp( h1._hash, h2._hash, sizeof(h1._hash) ) >= 0;
}
bool operator > ( const ripemd160& h1, const ripemd160& h2 ) {
  return memcmp( h1._hash, h2._hash, sizeof(h1._hash) ) > 0;
}
bool operator < ( const ripemd160& h1, const ripemd160& h2 ) {
  return memcmp( h1._hash, h2._hash, sizeof(h1._hash) ) < 0;
}
bool operator != ( const ripemd160& h1, const ripemd160& h2 ) {
  return memcmp( h1._hash, h2._hash, sizeof(h1._hash) ) != 0;
}
bool operator == ( const ripemd160& h1, const ripemd160& h2 ) {
  return memcmp( h1._hash, h2._hash, sizeof(h1._hash) ) == 0;
}
  
  void to_variant( const ripemd160& bi, fjson::variant& v )
  {
     v = std::vector<char>( (const char*)&bi, ((const char*)&bi) + sizeof(bi) );
  }
  void from_variant( const fjson::variant& v, ripemd160& bi )
  {
    std::vector<char> ve = v.as< std::vector<char> >();
    if( ve.size() )
    {
        memcpy(&bi, ve.data(), fjson::min<size_t>(ve.size(),sizeof(bi)) );
    }
    else
        memset( &bi, char(0), sizeof(bi) );
  }
  
} // fcrypto
