#ifdef NDEBUG
#error Position admission checks require assertions
#endif
#include <cmath>
#include "common/scope_position_program.hpp"
#include <cassert>
#include <fstream>
#include <vector>
using namespace ss2vr;
static std::vector<uint32_t> fixture(bool skin) {
    std::vector<uint32_t> p{0xfffe0101,31,0x80000005,0x900f0000,31,0x80030005,0x900f0003,
                          31,0x80050005,0x900f0005,31,0x80060005,0x900f0006};
    if (skin) p.insert(p.end(),{
        81,0xa00f00ff,0x3f800000,0x3f000000,0,0x443f40a4,
        5,0x80010000,0x90000005,0xa0ff00ff, // zero palette index times stride
        1,0xb0010000,0x80000000,
        1,0x800f0002,0xa0e42015,1,0x800f0003,0xa0e42016,1,0x800f0004,0xa0e42017,
        9,0x80010005,0x90e40000,0x80e40002,
        9,0x80020005,0x90e40000,0x80e40003,
        9,0x80040005,0x90e40000,0x80e40004,
        1,0x80080005,0xa00000ff});
    p.insert(p.end(),{20,0x800f0001,skin?0x80e40005u:0x90e40000u,0xa0e40001,
                     1,0xc00f0000,0x80e40001,
                     9,0xe0010003,0x90e40003,0xa0e40008,
                     9,0xe0020003,0x90e40003,0xa0e40009,0xffff});
    return p;
}
int main(int argc,char **argv) {
    ScopeCapGeometry cap; cap.valid=true;
    for (size_t i=0;i<ScopeCapVertices;++i) {
        const float a=float(i)*2*Pi/ScopeCapVertices;
        cap.positions[i]={.02f*std::cos(a),.3f+.02f*std::sin(a),.05f};
        cap.uv[i]={.5f+.2f*std::cos(a),.5f+.2f*std::sin(a)};
    }
    std::array<std::array<float,4>,256> constants{};
    const auto projectionMatrix=projection({-.7f,.8f,.6f,-.65f});
    const auto view=matrix(inverse(Pose{normalize({.1f,-.2f,.15f,.9f}),{3,2,1}}));
    const auto affine=matrix(Pose{normalize({.2f,.1f,-.3f,.85f}),{3,2,-2}});
    Matrix44 worldClip,expected;
    assert(scopeCapClip(projectionMatrix,view,matrix({}),worldClip));
    assert(scopeCapClip(projectionMatrix,view,affine,expected));
    for (unsigned i=0;i<4;++i) for (unsigned j=0;j<4;++j) constants[i+1][j]=expected.m[i*4+j];
    assert(scopeCapPosition(fixture(false),constants,cap,false,expected));
    const auto program=fixture(true);
    {
        // Explicit two-stage normalized-byte conversion yields integral palette
        // row addresses. The VM must use the copied index, not always palette0.
        auto indexed=program;
        const auto mul=std::find(indexed.begin(),indexed.end(),5u);
        assert(mul!=indexed.end());
        const size_t start=size_t(mul-indexed.begin());
        indexed[start+3]=0xa00000fe; // c254.x =255, first normalize-back stage.
        indexed.insert(indexed.begin()+std::ptrdiff_t(start+4),
                       {5,0x80010000,0x80000000,0xa05500fe}); // c254.y =3 rows.
        std::array<std::array<float,4>,256> uploaded{};
        uploaded[254]={255,3,0,0};
        for(unsigned r=0;r<4;++r)uploaded[1+r][r]=1;
        for(unsigned palette=0;palette<3;++palette) {
            for(unsigned r=0;r<3;++r)uploaded[21+palette*3+r][r]=1;
            uploaded[21+palette*3][3]=float(palette)*10;
        }
        const Vec3 vertex{1,2,3};
        std::array<float,4> actual{},legacy{};
        assert(scope_position::position(indexed,uploaded,vertex,{0,0},true,legacy));
        for(unsigned palette=0;palette<3;++palette) {
            scope_position::RigidPaletteInput copied{{uint8_t(palette),0,0,0},{255,0,0,0},3};
            assert(copied.valid() && scope_position::position(indexed,uploaded,vertex,{0,0},true,actual,
                   GeometryInputLayout::Legacy56,&copied));
            assert(actual[0]==1+float(palette)*10 && actual[1]==2 && actual[2]==3 && actual[3]==1);
        }
        assert(legacy[0]==1 && legacy[1]==2 && legacy[2]==3 && legacy[3]==1);
        scope_position::RigidPaletteInput copied{{2,0,0,0},{255,0,0,0},3};
        assert(!scope_position::position(indexed,uploaded,vertex,{0,0},false,actual,
                GeometryInputLayout::Legacy56,&copied));
        copied.paletteCount=2;
        assert(!scope_position::position(indexed,uploaded,vertex,{0,0},true,actual,
                GeometryInputLayout::Legacy56,&copied));
        copied.paletteCount=3;copied.indices[1]=1;
        assert(!scope_position::position(indexed,uploaded,vertex,{0,0},true,actual,
                GeometryInputLayout::Legacy56,&copied));
        copied.indices[1]=0;copied.weights={127,128,0,0};
        assert(!scope_position::position(indexed,uploaded,vertex,{0,0},true,actual,
                GeometryInputLayout::Legacy56,&copied));
        copied.weights={255,0,0,0};
        for(uint32_t count:{0u,4u}) {
            copied.paletteCount=count;
            assert(!scope_position::position(indexed,uploaded,vertex,{0,0},true,actual,
                    GeometryInputLayout::Legacy56,&copied));
        }
        copied.paletteCount=3;
        // The 7/8 family must seed the declared local-index register for its
        // actual layout, rather than accidentally continuing to use v5/v6.
        auto observed78=indexed;
        observed78[8]=0x80070005;observed78[9]=0x900f0007;
        observed78[11]=0x80080005;observed78[12]=0x900f0008;
        observed78[start+2]=0x90000007;
        // Consume v8.x as well: the rigid weight must be one in this layout.
        observed78.insert(observed78.end()-1,{5,0xc00f0000,0x80e40001,0x90000008});
        for(auto layout:{GeometryInputLayout::Observed78,GeometryInputLayout::ObservedMultiUV78,
                         GeometryInputLayout::NoUV78}) {
            assert(scope_position::position(observed78,uploaded,vertex,{0,0},true,actual,layout,&copied));
            assert(actual[0]==21 && actual[1]==2 && actual[2]==3 && actual[3]==1);
        }
        assert(!scope_position::position(observed78,uploaded,vertex,{0,0},true,actual,
                GeometryInputLayout::Legacy56,&copied)); // v7 stays unknown in the 5/6 layout.
        auto undeclared=indexed;
        undeclared.erase(undeclared.begin()+7,undeclared.begin()+10);
        assert(!scope_position::position(undeclared,uploaded,vertex,{0,0},true,actual,
                GeometryInputLayout::Legacy56,&copied));
        auto unavailable=indexed;
        unavailable[start+3]=0xa00000fe;
        assert(!scope_position::position(unavailable,std::span(uploaded).first(254),vertex,{0,0},true,actual,
                GeometryInputLayout::Legacy56,&copied)); // c254 has no copied upload or DEF.
        auto missingRows=indexed;
        missingRows.insert(missingRows.begin()+13,{81,0xa00f00fe,0x437f0000,0x40400000,0,0});
        assert(vertexPositionProgram(missingRows));
        assert(scope_position::position(missingRows,uploaded,vertex,{0,0},true,actual,
                GeometryInputLayout::Legacy56,&copied));
        assert(!scope_position::position(missingRows,std::span(uploaded).first(27),vertex,{0,0},true,actual,
                GeometryInputLayout::Legacy56,&copied)); // Address exists; selected palette rows do not.
        for(size_t words=0;words<indexed.size();++words)
            assert(!scope_position::position(std::span(indexed).first(words),uploaded,vertex,{0,0},true,
                                            actual,GeometryInputLayout::Legacy56,&copied));
        auto fractional=indexed;
        fractional[start+3]=0xa05500fe; // byte/255 *3 is not integral; no guessed rounding.
        copied.indices[0]=1;
        assert(!scope_position::position(fractional,uploaded,vertex,{0,0},true,actual,
                GeometryInputLayout::Legacy56,&copied));
        std::array<std::array<float,4>,257> oversized{};
        assert(!scope_position::position(indexed,oversized,vertex,{0,0},true,actual,
                GeometryInputLayout::Legacy56,&copied));
    }
    for (unsigned i=0;i<4;++i) for (unsigned j=0;j<4;++j) constants[i+1][j]=worldClip.m[i*4+j];
    for (unsigned i=0;i<3;++i) for (unsigned j=0;j<4;++j) constants[21+i][j]=affine.m[i*4+j];
    assert(scopeCapPosition(program,constants,cap,false,expected));
    assert(scopeCapPosition(program,constants,cap,true,expected));
    {auto unknown=program;
     unknown.insert(unknown.begin()+1,{31,0x80070005,0x900f0007,31,0x80080005,0x900f0008});
     unknown.insert(unknown.end()-1,{1,0xc00f0000,0x90e40007});
     std::array<float,4> clip{};
     assert(scopeUvProgram(unknown));
     assert(!scope_position::position(unknown,constants,{0,0,0},{0,0},true,clip));
     assert(!scopeCapPosition(unknown,constants,cap,true,expected));
     unknown[unknown.size()-2]=0x90e40008;
     assert(scopeUvProgram(unknown));
     assert(!scope_position::position(unknown,constants,{0,0,0},{0,0},true,clip));
     assert(!scopeCapPosition(unknown,constants,cap,true,expected));} // Declared but unproved 7/8 never receive invented seeds.
    for (size_t size=0;size<program.size();++size)
        assert(!scopeCapPosition(std::span(program).first(size),constants,cap,true,expected));
    {double worst=0; // Float rounding through native products stays admitted, with a measured margin.
     const float saved=constants[21][3]; constants[21][3]=std::nextafter(std::nextafter(saved,1.f),1.f);
     assert(scopeCapPosition(program,constants,cap,true,expected,&worst) && worst>=0 && worst<1e-4);
     constants[21][3]=saved;}
    constants[21][3]+=.01f;
    assert(!scopeCapPosition(program,constants,cap,true,expected)); constants[21][3]-=.01f;
    constants[21][3]+=.005f; // Five millimetres of cap displacement is a different pose.
    assert(!scopeCapPosition(program,constants,cap,true,expected)); constants[21][3]-=.005f;
    auto bad=program; bad.insert(bad.end()-1,{1,0xc0010000,0x90000003});
    assert(!scopeCapPosition(bad,constants,cap,true,expected)); // UV replacing clip X.
    bad=program; bad.insert(bad.end()-1,{1,0xc00f0000,0x80e4000b});
    assert(!scopeCapPosition(bad,constants,cap,true,expected)); // Undefined temp.
    // A trailing DEF overrides uploaded constants globally, not only code
    // appearing after it. Otherwise a late constant can counterfeit oPos proof.
    bad=program;
    bad.insert(bad.end()-1,{81,0xa00f0015,0,0,0,0});
    assert(!scopeCapPosition(bad,constants,cap,true,expected));
    auto color=program;
    color.insert(color.end()-1,{3,0x800f0000,0x90e40000,0xa0e40006});
    assert(scopeUvProgram(color) && scopeCapPosition(color,constants,cap,true,expected)); // SUB outside position.
    constants[21][0]=std::numeric_limits<float>::infinity();
    assert(!scopeCapPosition(program,constants,cap,true,expected));
    assert(!scopeCapPosition(program,std::span(constants).first(21),cap,true,expected));
    // Bounded malformed-token exercise through the production framing and
    // position gates. Some mutations are harmless; the obligation is safe,
    // deterministic rejection/acceptance without reading outside token spans.
    constants[21][0]=affine.m[0];
    assert(scopeCapPosition(program,constants,cap,true,expected));
    uint32_t random=0x517a9u;
    for (unsigned trial=0;trial<2000;++trial) {
        auto mutated=program;
        random=random*1664525u+1013904223u;
        const size_t index=random%mutated.size();
        random=random*1664525u+1013904223u;
        mutated[index]^=random;
        (void)scopeCapPosition(mutated,constants,cap,true,expected);
    }
    auto offline=fixture(false);
    for(uint32_t reg:{8u,9u}) {
        auto defined=offline;
        defined.insert(defined.end()-1,{81,0xa00f0000u|reg,0,0,0,0});
        assert(vertexPositionProgram(defined));assert(!scopeUvProgram(defined));
    }
    if (argc==3 && std::string(argv[1])=="--write-fixture") {
        const auto bytes=color;
        std::ofstream output(argv[2],std::ios::binary);
        output.write(reinterpret_cast<const char *>(bytes.data()),std::streamsize(bytes.size()*4));
        assert(output.good());
    }
}
