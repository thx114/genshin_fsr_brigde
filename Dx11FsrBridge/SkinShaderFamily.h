#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <algorithm>
namespace skinfamily {
enum class Kind:uint32_t {Unknown=0,Pending=1,Rejected=2,Material2=3,ConstantFace=4};
struct Result {Kind kind=Kind::Rejected;uint32_t sampleLine=0,bucketLine=0,classLine=0,outputLine=0;const char* reason="unmatched-game-material-family";};
inline bool accepted(Kind k){return k==Kind::Material2||k==Kind::ConstantFace;}
namespace detail {
inline std::string trim(std::string_view v){const auto a=v.find_first_not_of(" \t\r\n");if(a==v.npos)return {};const auto b=v.find_last_not_of(" \t\r\n");return std::string(v.substr(a,b-a+1));}
inline std::string compact(std::string_view v){std::string out;for(char c:v)if(c!=' '&&c!='\t'&&c!='\r')out+=c;return out;}
struct Instruction {std::string op;std::vector<std::string> args;uint32_t line=0;};
inline std::vector<Instruction> instructions(std::string_view text) {
    std::vector<Instruction> out;uint32_t line=0;
    while(!text.empty()) {
        const auto end=text.find('\n');auto row=trim(text.substr(0,end));text=end==text.npos?std::string_view{}:text.substr(end+1);++line;
        if(row.empty()||row.starts_with("//"))continue;
        if(const auto comment=row.find("//");comment!=row.npos)row.resize(comment);
        const auto split=row.find_first_of(" \t");Instruction ins;ins.op=row.substr(0,split);ins.line=line;
        if(split!=row.npos) {
            const std::string args=row.substr(split+1);int depth=0;size_t start=0;
            for(size_t i=0;i<=args.size();++i) {
                if(i<args.size()&&args[i]=='(')++depth;else if(i<args.size()&&args[i]==')')--depth;
                if(i==args.size()||(args[i]==','&&depth==0)){ins.args.push_back(compact(std::string_view(args).substr(start,i-start)));start=i+1;}
            }
        }
        out.push_back(std::move(ins));if(out.size()>8192)return {};
    }
    return out;
}
inline std::string base(std::string_view reg){const auto p=reg.find('.');return std::string(reg.substr(0,p));}
inline std::string scalar(std::string_view reg) {
    const auto p=reg.find('.');if(p==reg.npos||p+1==reg.size())return {};
    const char c=reg[p+1];if(c!='x'&&c!='y'&&c!='z'&&c!='w')return {};
    for(size_t i=p+1;i<reg.size();++i)if(reg[i]!=c)return {};
    return std::string(reg.substr(0,p+2));
}
inline bool writes(const Instruction& ins,std::string_view component) {
    if(ins.args.empty()||ins.op.starts_with("dcl_")||ins.op=="if_nz"||ins.op=="if_z"||ins.op.starts_with("discard")||ins.op=="case")return false;
    const auto p=component.find('.');if(p==component.npos||p+1>=component.size())return false;
    const auto& dest=ins.args[0];const auto d=dest.find('.');
    return d!=dest.npos&&dest.substr(0,d)==component.substr(0,p)&&dest.find(component[p+1],d+1)!=dest.npos;
}
inline size_t last_write(const std::vector<Instruction>& ins,size_t before,std::string_view component) {
    while(before){--before;if(writes(ins[before],component))return before;}return ins.size();
}
inline bool immediate333(const std::string& value){return value=="l(0.333000)"||value=="l(0.333333)"||value=="l(0x3eaa7efa)";}
}
// Strict, bounded STRUCTURAL candidate admission from original Microsoft DXBC
// disassembly. No opcode execution/replacement and no claim of universal skin.
// Require six-MRT PS5 family, sampled texture ALPHA, canonical material buckets
// and the matching class2 flag that reaches MRT0.a without being overwritten.
inline Result classify(std::string_view text) {
    Result result;if(text.empty()||text.size()>1024*1024){result.reason="assembly-size-refused";return result;}
    const auto ins=detail::instructions(text);if(ins.empty())return result;
    bool ps5=false;uint32_t outputs=0;
    for(const auto& i:ins) {
        if(i.op=="ps_5_0")ps5=true;
        if(i.op=="dcl_output"&&i.args.size()==1)for(uint32_t slot=0;slot<6;++slot)
            if(detail::base(i.args[0])=="o"+std::to_string(slot))outputs|=1u<<slot;
    }
    if(!ps5||outputs!=63){result.reason="not-observed-six-MRT-PS5-family";return result;}
    uint32_t bucketAttempts=0;
    for(size_t g=0;g<ins.size();++g) {
        const auto& bucket=ins[g];
        if(bucket.op!="ge"||bucket.args.size()!=3||bucket.args[2]!="l(0.800000,0.400000,0.200000,0.600000)")continue;
        if(++bucketAttempts>32){result.reason="shader-family-search-budget-exhausted";return result;}
        if(bucket.args[0]!=detail::base(bucket.args[0])+".xyzw")continue;
        const auto alpha=detail::scalar(bucket.args[1]);if(alpha.empty())continue;
        const auto sample=detail::last_write(ins,g,alpha);if(sample==ins.size())continue;
        const auto& s=ins[sample];if(!s.op.starts_with("sample")||s.args.size()<4)continue;
        const auto tex=s.args[2];const auto dot=tex.find('.');const auto pos=std::string("xyzw").find(alpha.back());
        if(dot==tex.npos||pos==std::string::npos||dot+1+pos>=tex.size()||tex[dot+1+pos]!='w'||tex.front()!='t')continue;
        const auto firstCondition=detail::base(bucket.args[0])+".x";
        for(size_t m=g+1;m<std::min(ins.size(),g+9);++m) {
            const auto& choice=ins[m];if(choice.op!="movc"||choice.args.size()!=4||choice.args[2]!="l(2.000000)"||choice.args[3]!="l(1.000000)")continue;
            const auto material=detail::scalar(choice.args[0]),condition=detail::scalar(choice.args[1]);if(material.empty()||condition.empty())continue;
            const auto gate=detail::last_write(ins,m,condition);if(gate<=g||gate>=m)continue;
            const auto& andGate=ins[gate];if(andGate.op!="and"||andGate.args.size()!=3||(detail::scalar(andGate.args[1])!=firstCondition&&detail::scalar(andGate.args[2])!=firstCondition))continue;
            bool liveCondition=true;for(size_t j=g+1;j<gate;++j)if(detail::writes(ins[j],firstCondition))liveCondition=false;
            if(!liveCondition)continue;
            for(size_t e=m+1;e<ins.size();++e) {
                const auto& eq=ins[e];
                if(eq.op!="eq"||eq.args.size()!=3||eq.args[2]!="l(2.000000,3.000000,4.000000,5.000000)"||detail::scalar(eq.args[1])!=material)continue;
                if(eq.args[0]!=detail::base(eq.args[0])+".xyzw")continue;
                bool materialPreserved=true;
                for(size_t j=m+1;j<e;++j)if(detail::writes(ins[j],material)) {
                    const auto& replace=ins[j];
                    if(replace.op!="movc"||replace.args.size()!=4||detail::scalar(replace.args[3])!=material||
                       (replace.args[2]!="l(3.000000)"&&replace.args[2]!="l(4.000000)"&&replace.args[2]!="l(5.000000)"))materialPreserved=false;
                }
                if(!materialPreserved)continue;
                const auto class2=detail::base(eq.args[0])+".x";
                for(size_t o=e+1;o<ins.size();++o) {
                    const auto& output=ins[o];if(output.args.empty()||output.args[0]!="o0.w")continue;
                    Kind kind=Kind::Rejected;
                    if(output.op=="mov"&&output.args.size()==2&&detail::immediate333(output.args[1]))kind=Kind::ConstantFace;
                    if(output.op=="and"&&output.args.size()==3&&detail::scalar(output.args[1])==class2&&detail::immediate333(output.args[2])) {
                        const auto writer=detail::last_write(ins,o,class2);if(writer==e)kind=Kind::Material2;
                    }
                    if(!accepted(kind))continue;
                    bool finalAlpha=true;for(size_t j=o+1;j<ins.size();++j)if(detail::writes(ins[j],"o0.w"))finalAlpha=false;
                    if(!finalAlpha)continue;
                    return {kind,s.line,bucket.line,eq.line,output.line,kind==Kind::Material2?"sampled-alpha-material2-MRT0-marker":"six-MRT-face-constant-alpha-marker"};
                }
            }
        }
    }
    return result;
}
}
