#pragma once
#include "NativeUi.h"
#include <cstdint>
#include <string>

namespace hudlocale {
enum class Language { Unknown, English, Russian, French, Chinese };
using Read = bool(*)(const void*,void*,size_t);
inline const char* label(Language language) {
    switch(language) {
        case Language::Russian:return "КРУИЗ КОНТРОЛЬ";
        case Language::French:return "RÉGULATEUR DE VITESSE";
        case Language::Chinese:return "巡航";
        default:return "CRUISE CONTROL";
    }
}
// True when the UTF-8 caption contains a CJK unified ideograph. The exact
// Chinese handbrake wording varies ("手刹"/"手刹车"/"手制动器"), so any Han
// glyph identifies a Simplified/Traditional Chinese UI without hardcoding it.
inline bool containsHan(const std::string& text) {
    for(size_t i=0;i<text.size();) {
        const unsigned char c=static_cast<unsigned char>(text[i]);
        if((c&0xe0)==0xc0&&i+1<text.size()){i+=2;continue;}
        if((c&0xf0)==0xe0&&i+2<text.size()){
            const uint32_t cp=((c&0x0f)<<12)|((static_cast<unsigned char>(text[i+1])&0x3f)<<6)|(static_cast<unsigned char>(text[i+2])&0x3f);
            if(cp>=0x4e00&&cp<=0x9fff)return true;
            i+=3;continue;
        }
        ++i;
    }
    return false;
}
inline Language detect(const std::string& text) {
    if(text=="РУЧНОЙ ТОРМОЗ"||text=="Ручной тормоз"||text=="ручной тормоз")return Language::Russian;
    if(text=="FREIN À MAIN"||text=="Frein à main"||text=="frein à main")return Language::French;
    if(text=="HANDBRAKE"||text=="Handbrake"||text=="handbrake")return Language::English;
    if(containsHan(text))return Language::Chinese;
    return Language::Unknown;
}
// Copy while the owned GFx string is alive. No game pointer is retained.
// Managed strings contain a pointer to the character pointer; type 7 uses
// Windows UTF-16. The process reader rejects inaccessible memory.
inline std::string copyString(const nativeui::Value& value,Read read) {
    if(!read||(nativeui::type(value)!=6&&nativeui::type(value)!=7))return {};
    uintptr_t address=value.object;
    if(value.type&0x40) {
        if(!read(reinterpret_cast<const void*>(address),&address,sizeof(address)))return {};
    }
    if(!address)return {};
    std::string result;
    for(size_t i=0;i<128;++i) {
        uint16_t ch=0;
        const auto width=nativeui::type(value)==7?2u:1u;
        if(address>UINTPTR_MAX-i*width||!read(reinterpret_cast<const void*>(address+i*width),&ch,width))return {};
        if(!ch)return result;
        if(width==1)result+=static_cast<char>(ch);
        else if(ch<0x80)result+=static_cast<char>(ch);
        else if(ch<0x800) {result+=static_cast<char>(0xc0|(ch>>6));result+=static_cast<char>(0x80|(ch&0x3f));}
        else if(ch>=0xd800&&ch<=0xdfff)return {};
        else {result+=static_cast<char>(0xe0|(ch>>12));result+=static_cast<char>(0x80|((ch>>6)&0x3f));result+=static_cast<char>(0x80|(ch&0x3f));}
    }
    return {}; // Unterminated or unexpectedly long text is not a language.
}
inline bool fromBrake(const nativeui::Api& api,const nativeui::Value& brake,Read read,Language& language) {
    nativeui::Owned container(api),field(api),text(api);
    if(!nativeui::get(api,brake,"tfCont",container)||!nativeui::get(api,container.value,"tf",field)||
       !nativeui::get(api,field.value,"text",text))return false;
    auto caption=copyString(text.value,read);
    const auto first=caption.find_first_not_of(" \t\r\n");
    if(first==std::string::npos)return false;
    caption=caption.substr(first,caption.find_last_not_of(" \t\r\n")-first+1);
    if(caption.rfind("UI_",0)==0)return false; // Localisation has not run yet.
    language=detect(caption);
    if(language==Language::Unknown)language=Language::English;
    return true;
}
inline bool fromPanel(const nativeui::Api& api,const nativeui::Value& panel,Read read,Language& language) {
    nativeui::Owned brake(api);
    return nativeui::get(api,panel,"brakeBtn",brake)&&fromBrake(api,brake.value,read,language);
}
}
