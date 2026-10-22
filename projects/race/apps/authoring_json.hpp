#pragma once
#include "race/assets/assets.hpp"
#include <limits>

// Private bounded JSON token reader shared by executable authoring formats.
namespace race::authoring_json {
struct Reader {
  std::string_view text;std::size_t offset{};
  void space(){while(offset<text.size()&&(text[offset]==' '||text[offset]=='\t'||text[offset]=='\r'||text[offset]=='\n'))++offset;}
  bool take(char c){space();if(offset==text.size()||text[offset]!=c)return false;++offset;return true;}
  Result<std::string_view> string(){
    if(!take('"'))return fail(ErrorCode::InvalidInput,"Authoring JSON requires a string");
    const auto first=offset;
    while(offset<text.size()&&text[offset]!='"'){
      const auto c=static_cast<unsigned char>(text[offset]);
      if(c<32||c>126||c=='\\')return fail(ErrorCode::InvalidInput,"Authoring JSON requires unescaped printable ASCII strings");
      if(offset-first==96)return fail(ErrorCode::Exhausted,"Authoring JSON string limit");++offset;
    }
    if(offset==text.size())return fail(ErrorCode::InvalidInput,"Unterminated recipe string");
    const auto value=text.substr(first,offset-first);++offset;return value;
  }
  Result<std::uint32_t> integer(){
    space();const auto first=offset;std::uint32_t value{};
    while(offset<text.size()&&text[offset]>='0'&&text[offset]<='9'){
      const auto digit=static_cast<std::uint32_t>(text[offset]-'0');
      if(value>(std::numeric_limits<std::uint32_t>::max()-digit)/10)return fail(ErrorCode::Exhausted,"Authoring JSON integer limit");
      value=value*10+digit;++offset;
    }
    if(first==offset||(offset-first>1&&text[first]=='0'))return fail(ErrorCode::InvalidInput,"Authoring JSON requires a strict unsigned integer");return value;
  }
};
inline Result<assets::Digest> hex(std::string_view text){
  if(text.size()!=64)return fail(ErrorCode::InvalidInput,"Authoring JSON fingerprint length");
  assets::Digest value{};
  for(std::size_t n=0;n<text.size();++n){
    const auto c=text[n];const int digit=c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:-1;
    if(digit<0)return fail(ErrorCode::InvalidInput,"Authoring JSON requires lowercase hexadecimal fingerprints");
    value[n/2]|=static_cast<std::byte>(digit<<(n%2==0?4:0));
  }
  return value;
}
}
