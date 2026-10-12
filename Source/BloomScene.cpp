#include "BloomScene.h"
#include "BloomSceneData.h"
#include "BloomGoblinData.h"
#include "BloomPerf.h"

#include <array>
#include <map>
#include <string>
#include <vector>
#include <cmath>
#include <cstring>

using namespace juce::gl;

//==============================================================================
// Petits outils de maths (matrices 4x4, rangees par colonnes comme dans le prototype)
//==============================================================================
namespace
{
    using Mat = std::array<float, 16>;
    using V3  = std::array<float, 3>;

    Mat mId() { return Mat{ 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 }; }

    Mat mMul(const Mat& a, const Mat& b)
    {
        Mat o{};
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r)
            {
                float s = 0.0f;
                for (int k = 0; k < 4; ++k)
                    s += a[(size_t) (k * 4 + r)] * b[(size_t) (c * 4 + k)];
                o[(size_t) (c * 4 + r)] = s;
            }
        return o;
    }

    Mat mT(float x, float y, float z) { return Mat{ 1,0,0,0, 0,1,0,0, 0,0,1,0, x,y,z,1 }; }
    Mat mS(float x, float y, float z) { return Mat{ x,0,0,0, 0,y,0,0, 0,0,z,0, 0,0,0,1 }; }
    Mat mS(float s)                   { return mS(s, s, s); }
    Mat mRX(float a) { const float c = std::cos(a), s = std::sin(a); return Mat{ 1,0,0,0, 0,c,s,0, 0,-s,c,0, 0,0,0,1 }; }
    Mat mRY(float a) { const float c = std::cos(a), s = std::sin(a); return Mat{ c,0,-s,0, 0,1,0,0, s,0,c,0, 0,0,0,1 }; }
    Mat mRZ(float a) { const float c = std::cos(a), s = std::sin(a); return Mat{ c,s,0,0, -s,c,0,0, 0,0,1,0, 0,0,0,1 }; }

    Mat mPersp(float f, float aspect, float zNear, float zFar)
    {
        const float t = 1.0f / std::tan(f / 2.0f);
        return Mat{ t / aspect,0,0,0, 0,t,0,0, 0,0,(zFar + zNear) / (zNear - zFar),-1, 0,0,2 * zFar * zNear / (zNear - zFar),0 };
    }

    Mat mLook(V3 e, V3 c, V3 u)
    {
        auto sub = [](V3 a, V3 b) { return V3{ a[0] - b[0], a[1] - b[1], a[2] - b[2] }; };
        auto cr  = [](V3 a, V3 b) { return V3{ a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0] }; };
        auto nz  = [](V3 v) { const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); return V3{ v[0] / l, v[1] / l, v[2] / l }; };
        auto dt  = [](V3 a, V3 b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };

        const V3 z = nz(sub(e, c));
        const V3 x = nz(cr(u, z));
        const V3 y = cr(z, x);
        return Mat{ x[0],y[0],z[0],0, x[1],y[1],z[1],0, x[2],y[2],z[2],0, -dt(x, e),-dt(y, e),-dt(z, e),1 };
    }

    Mat mCh(std::initializer_list<Mat> ms)
    {
        Mat r = mId();
        bool first = true;
        for (const auto& m : ms)
        {
            r = first ? m : mMul(r, m);
            first = false;
        }
        return r;
    }

    V3 mAp(const Mat& m, V3 p)
    {
        return V3{ m[0] * p[0] + m[4] * p[1] + m[8]  * p[2] + m[12],
                   m[1] * p[0] + m[5] * p[1] + m[9]  * p[2] + m[13],
                   m[2] * p[0] + m[6] * p[1] + m[10] * p[2] + m[14] };
    }

    V3 vMix(V3 a, V3 b, float t) { return V3{ a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t }; }
    V3 vMul(V3 a, float s)       { return V3{ a[0] * s, a[1] * s, a[2] * s }; }

    float clamp01(float v) { return juce::jlimit(0.0f, 1.0f, v); }

    //==========================================================================
    // Shaders (les memes que dans le prototype, ecrits pour OpenGL 3.2)
    //==========================================================================
    const char* const kHead = "#version 150\n";

    const char* const VS_LIT = R"GLSL(
in vec3 aPos;in vec3 aNor;in vec3 aCol;in vec3 aEmi;
uniform mat4 uVP,uModel;uniform vec3 uTint,uEmis;uniform vec3 uAmb,uMoonDir,uMoonCol;
uniform vec3 uPLp[4];uniform vec3 uPLc[4];uniform float uPLk[4];
uniform float uFogD,uTwo;uniform vec2 uSnap;uniform vec3 uEye;
out vec3 vCol;out float vFog;
void main(){
  vec4 wp=uModel*vec4(aPos,1.);vec3 N=normalize(mat3(uModel)*aNor);
  vec3 V=normalize(uEye-wp.xyz);if(uTwo>.5&&dot(N,V)<0.)N=-N;
  vec3 L=uAmb+uMoonCol*max(dot(N,uMoonDir),0.);
  for(int i=0;i<4;i++){vec3 d=uPLp[i]-wp.xyz;float r=length(d);L+=uPLc[i]*max(dot(N,d/r),0.)/(1.+r*r*uPLk[i]);}
  L+=vec3(.05,.035,.08)*pow(1.-max(dot(N,V),0.),2.);
  vCol=aCol*uTint*L+aEmi*uEmis;
  vec4 p=uVP*wp;float z=p.w;vFog=clamp(exp(-uFogD*uFogD*z*z),0.,1.);
  p.xy=floor(p.xy/p.w*uSnap+.5)/uSnap*p.w;gl_Position=p;})GLSL";

    const char* const FS_LIT = R"GLSL(
in vec3 vCol;in float vFog;uniform vec3 uFogCol;out vec4 o;
void main(){o=vec4(mix(uFogCol,vCol,vFog),1.);})GLSL";

    const char* const VS_FULL = R"GLSL(
in vec2 aP;out vec2 vUv;void main(){vUv=aP*.5+.5;gl_Position=vec4(aP,0.,1.);})GLSL";

    const char* const FS_SKY = R"GLSL(
in vec2 vUv;uniform float uT,uAir,uWarm;out vec4 o;
float h(vec2 p){return fract(sin(dot(p,vec2(127.1,311.7)))*43758.5453);}
float n(vec2 p){vec2 i=floor(p),f=fract(p);f=f*f*(3.-2.*f);return mix(mix(h(i),h(i+vec2(1,0)),f.x),mix(h(i+vec2(0,1)),h(i+vec2(1,1)),f.x),f.y);}
void main(){float y=vUv.y;
 vec3 top=vec3(.035,.028,.06),mid=vec3(.08,.06,.11),hor=mix(vec3(.2,.09,.06),vec3(.26,.07,.04),uWarm);
 vec3 c=mix(mid,top,smoothstep(.45,1.,y));c=mix(c,vec3(.02,.015,.03),smoothstep(.5,.0,y));c+=hor*.35*exp(-pow((y-.47)*9.,2.));
 float cl=n(vec2(vUv.x*3.+uT*.01,y*6.))*n(vec2(vUv.x*7.-uT*.02,y*11.));
 c+=vec3(.09,.07,.13)*cl*smoothstep(.3,.9,y);
 float band=smoothstep(.1,0.,abs(y-.72-sin(vUv.x*5.+uT*.05)*.05))*n(vec2(vUv.x*12.+uT*.08,1.));
 c+=vec3(.25,.18,.5)*band*.35*uAir;
 o=vec4(c,1.);})GLSL";

    const char* const VS_SPR = R"GLSL(
in vec2 aP;uniform mat4 uV,uPr;uniform vec3 uC;uniform vec2 uSz;out vec2 vUv;
void main(){vUv=aP;vec4 v=uV*vec4(uC,1.);v.xy+=aP*uSz;gl_Position=uPr*v;})GLSL";

    const char* const FS_SPR = R"GLSL(
in vec2 vUv;uniform vec3 uCol;uniform float uA,uMode,uPow;out vec4 o;
void main(){float d=length(vUv);float a;
 if(uMode<.5){a=pow(max(1.-d,0.),uPow);}
 else if(uMode<1.5){a=smoothstep(.36,.34,d)*.95+pow(max(1.-d,0.),3.)*.5;}
 else{a=pow(max(1.-abs(vUv.x),0.),3.)*smoothstep(1.,-.6,vUv.y)*smoothstep(-1.,-.7,vUv.y);}
 o=vec4(uCol*a*uA,1.);})GLSL";

    const char* const VS_PT = R"GLSL(
in vec3 aPos;in vec4 aC;uniform mat4 uVP;uniform float uScale;out vec4 vC;
void main(){vec4 p=uVP*vec4(aPos,1.);gl_Position=p;gl_PointSize=clamp(uScale*aC.a/p.w,1.,uScale*1.68);vC=aC;})GLSL";

    const char* const FS_PT = R"GLSL(
in vec4 vC;out vec4 o;
void main(){float d=length(gl_PointCoord-.5)*2.;float a=smoothstep(1.,.2,d);o=vec4(vC.rgb*a,1.);})GLSL";

    const char* const VS_FOG = R"GLSL(
in vec2 aP;uniform mat4 uVP;uniform vec3 uC;uniform float uR;out vec2 vW;out vec2 vL;
void main(){vL=aP;vec3 w=uC+vec3(aP.x*uR,0.,aP.y*uR);vW=w.xz;gl_Position=uVP*vec4(w,1.);})GLSL";

    const char* const FS_FOG = R"GLSL(
in vec2 vW;in vec2 vL;uniform float uT,uA,uSeed;uniform vec3 uCol;out vec4 o;
float h(vec2 p){return fract(sin(dot(p,vec2(127.1,311.7)))*43758.5453);}
float n(vec2 p){vec2 i=floor(p),f=fract(p);f=f*f*(3.-2.*f);return mix(mix(h(i),h(i+vec2(1,0)),f.x),mix(h(i+vec2(0,1)),h(i+vec2(1,1)),f.x),f.y);}
void main(){vec2 p=vW*.28+vec2(uSeed,uSeed*.7);float f=n(p+vec2(uT*.03,0.))*.55+n(p*2.1-vec2(0.,uT*.04))*.3+n(p*4.3+uT*.05)*.15;
 float e=smoothstep(1.,.35,length(vL));o=vec4(uCol,smoothstep(.35,.85,f)*e*uA);})GLSL";

    const char* const FS_POST = R"GLSL(
in vec2 vUv;uniform sampler2D uTex;uniform vec2 uRes;uniform float uT,uGlow,uThr,uDelta,uFade;out vec4 o;
float b2(vec2 a){a=floor(a);return fract(dot(a,vec2(.5,a.y*.75)));}
float b4(vec2 a){return b2(.5*a)*.25+b2(a);}
void main(){vec3 c=texture(uTex,vUv).rgb;
 vec2 px=1./uRes;float k=uRes.x/250.;vec3 gl=vec3(0.);
 for(int i=-2;i<=2;i++)for(int j=-2;j<=2;j++){vec3 s=texture(uTex,vUv+vec2(i,j)*px*2.*k).rgb;gl+=max(s-uThr,0.);}
 vec3 glo=gl/25.*uGlow;c+=glo;
 c=mix(c,glo*1.7+c*.1,uDelta);
 float v=smoothstep(1.25,.35,length((vUv-.5)*vec2(1.25,1.)));c*=mix(.35,1.,v);
 c=pow(c,vec3(.95));
 float lv=26.;c+= (b4(floor(vUv*uRes))-.5)/lv;c=floor(c*lv+.5)/lv;
 o=vec4(mix(vec3(.05,.04,.08),c,uFade),1.);})GLSL";

    const char* const VS_WTR = R"GLSL(
in vec3 aPos;in vec2 aUV;uniform mat4 uVP,uModel;uniform vec2 uSnap;uniform float uLen,uFogD;
out vec2 vUv;out float vS,vFog;
void main(){vec4 p=uVP*(uModel*vec4(aPos,1.));vUv=aUV;vS=aUV.y/uLen;vFog=clamp(exp(-uFogD*uFogD*p.w*p.w),0.,1.);
  p.xy=floor(p.xy/p.w*uSnap+.5)/uSnap*p.w;gl_Position=p;})GLSL";

    const char* const FS_WTR = R"GLSL(
in vec2 vUv;in float vS,vFog;uniform float uT,uA,uMode;out vec4 o;
float h(vec2 p){return fract(sin(dot(p,vec2(127.1,311.7)))*43758.5453);}
float n(vec2 p){vec2 i=floor(p),f=fract(p);f=f*f*(3.-2.*f);return mix(mix(h(i),h(i+vec2(1,0)),f.x),mix(h(i+vec2(0,1)),h(i+vec2(1,1)),f.x),f.y);}
void main(){float u=vUv.x,v=vUv.y;float edge=smoothstep(0.,.16,u)*smoothstep(1.,.84,u);float a,st;vec3 col;
 if(uMode<.5){st=n(vec2(u*15.,v*.7-uT*3.))*.65+n(vec2(u*33.+7.,v*1.3-uT*5.2))*.35;
  a=(.22+st*.95)*edge*smoothstep(0.,.03,vS)*(1.-smoothstep(.42,1.,vS));a+=.55*(1.-smoothstep(0.,.07,vS))*edge;
  col=mix(vec3(.18,.32,.62),vec3(.80,.92,1.),st);col=mix(col,vec3(.5,.42,.9),smoothstep(.35,1.,vS)*.55);}
 else{st=n(vec2(u*9.+3.,v*2.2-uT*1.7))*.6+n(vec2(u*21.,v*4.-uT*2.8))*.4;
  a=(.28+st*.7)*edge*smoothstep(0.,.18,vS);col=mix(vec3(.2,.36,.66),vec3(.75,.9,1.),st);}
 a*=uA*(.35+.65*vFog);o=vec4(col*a,1.);})GLSL";

    //==========================================================================
    // Decodage des donnees embarquees (texte base64 standard -> gzip -> octets)
    //==========================================================================
    juce::MemoryBlock decodeChunks(const char* const* chunks, int count)
    {
        signed char tbl[256];
        for (auto& t : tbl) t = -1;
        const char* alpha = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int i = 0; i < 64; ++i)
            tbl[(unsigned char) alpha[i]] = (signed char) i;

        std::vector<juce::uint8> raw;
        juce::uint32 acc = 0;
        int bits = 0;
        for (int c = 0; c < count; ++c)
        {
            for (const char* s = chunks[c]; *s != 0; ++s)
            {
                const int d = tbl[(unsigned char) *s];
                if (d < 0)
                    continue;
                acc = (acc << 6) | (juce::uint32) d;
                bits += 6;
                if (bits >= 8)
                {
                    bits -= 8;
                    raw.push_back((juce::uint8) ((acc >> bits) & 0xFF));
                }
            }
        }

        juce::MemoryInputStream in(raw.data(), raw.size(), false);
        juce::GZIPDecompressorInputStream gz(&in, false, juce::GZIPDecompressorInputStream::gzipFormat);
        juce::MemoryOutputStream out;
        out.writeFromInputStream(gz, -1);
        return out.getMemoryBlock();
    }
}

//==============================================================================
// Donnees de la scene decodees UNE fois par session, sur un fil a part (pendant que la fenetre s'ouvre).
// Volontairement jamais detruites : pas d'objet "static" detruit au dechargement du plugin.
namespace
{
    struct DecodedScene
    {
        juce::MemoryBlock blob, meta, goblin;
        std::atomic<bool> ready { false };
        std::atomic<bool> started { false };
    };

    DecodedScene& decodedScene()
    {
        static auto* d = new DecodedScene();
        return *d;
    }

    void startBackgroundDecode()
    {
        auto& d = decodedScene();
        bool expected = false;
        if (!d.started.compare_exchange_strong(expected, true))
            return;
        juce::Thread::launch([]
        {
            auto& dd = decodedScene();
            dd.blob   = decodeChunks(kSceneBlob, kSceneBlobCount);
            dd.meta   = decodeChunks(kSceneMeta, kSceneMetaCount);
            dd.goblin = decodeChunks(kGoblinBlob, kGoblinBlobCount);
            dd.ready.store(true);
        });
    }

    // ---- l'affiche (derniere image du monde) ----
    struct PosterStore
    {
        juce::CriticalSection lock;
        juce::Image image;
        bool dirty = false;           // nouvelle image pas encore enregistree
        bool triedDisk = false;
        double lastSave = -1.0e9;
    };

    PosterStore& posterStore()
    {
        static auto* p = new PosterStore();   // jamais detruit volontairement
        return *p;
    }

    juce::File posterFile()
    {
        return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                   .getChildFile("BLOOM").getChildFile("poster.png");
    }

    void loadPosterFromDisk()
    {
        auto& ps = posterStore();
        {
            const juce::ScopedLock sl(ps.lock);
            if (ps.triedDisk || ps.image.isValid())
                return;
            ps.triedDisk = true;
        }
        const auto f = posterFile();
        if (!f.existsAsFile())
            return;
        auto img = juce::ImageFileFormat::loadFrom(f);
        if (img.isValid())
        {
            const juce::ScopedLock sl(ps.lock);
            if (!ps.image.isValid())
                ps.image = img;
        }
    }
}

bool bloomSceneDataReady()
{
    return decodedScene().ready.load();
}

juce::Image bloomGetPoster()
{
    loadPosterFromDisk();
    auto& ps = posterStore();
    const juce::ScopedLock sl(ps.lock);
    return ps.image;
}

void bloomSavePosterIfNeeded()
{
    auto& ps = posterStore();
    juce::Image img;
    {
        const juce::ScopedLock sl(ps.lock);
        const double now = juce::Time::getMillisecondCounterHiRes();
        if (!ps.dirty || !ps.image.isValid() || now - ps.lastSave < 30000.0)
            return;
        ps.dirty = false;
        ps.lastSave = now;
        img = ps.image;
    }
    const auto f = posterFile();
    f.getParentDirectory().createDirectory();
    juce::FileOutputStream out(f);
    if (out.openedOk())
    {
        out.setPosition(0);
        out.truncate();
        juce::PNGImageFormat png;
        png.writeImageToStream(img, out);
    }
}

//==============================================================================
struct BloomScene::Impl
{
    struct Prog
    {
        GLuint id = 0;
        std::map<std::string, GLint> cache;

        GLint u(const char* name)
        {
            auto it = cache.find(name);
            if (it != cache.end())
                return it->second;
            const GLint l = glGetUniformLocation(id, name);
            cache[name] = l;
            return l;
        }
    };

    struct Mesh
    {
        GLuint vao = 0, vbo = 0;
        GLsizei n = 0;
    };

    struct Islet  { float x, y, z, sc, ph; bool bell; int mesh, bellMesh; };
    struct Raven  { float a, r, y, s, ph, f, sc; };
    struct Rock   { float r, a, y, s, rx, ry, sp; int mesh; };

    Impl(juce::AudioProcessorValueTreeState& apvts, juce::OpenGLContext& c, juce::Component& o, std::atomic<int>& h)
        : ctx(c), owner(o), hover(h), rnd(7)
    {
        const char* ids[7] = { "weight", "warmth", "space", "presence", "air", "enhance", "mix" };
        for (int i = 0; i < 7; ++i)
        {
            pk[i] = apvts.getParameter(ids[i]);
            S[i] = P(i);
            boost[i] = 0.0f;
            anchX[i].store(-1.0e9f);
            anchY[i].store(0.0f);
        }
        pByp = apvts.getParameter("bypass");
        pDel = apvts.getParameter("delta");
        lastTime = juce::Time::getMillisecondCounterHiRes();
        statStart = lastTime;
    }

    float P(int i) const { return pk[i] != nullptr ? pk[i]->getValue() : 0.0f; }
    float R(float a, float b) { return a + (b - a) * rnd.nextFloat(); }

    //--------------------------------------------------------------------------
    GLuint compileShader(GLenum type, const char* body)
    {
        const char* srcs[2] = { kHead, body };
        const GLuint s = glCreateShader(type);
        glShaderSource(s, 2, srcs, nullptr);
        glCompileShader(s);
        GLint ok = 0;
        glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
        if (ok == 0)
        {
            char log[2048] = {};
            glGetShaderInfoLog(s, 2047, nullptr, log);
            DBG("BLOOM shader error: " << log);
            failed = true;
        }
        return s;
    }

    void makeProg(Prog& p, const char* vs, const char* fs)
    {
        p.id = glCreateProgram();
        const GLuint v = compileShader(GL_VERTEX_SHADER, vs);
        const GLuint f = compileShader(GL_FRAGMENT_SHADER, fs);
        glAttachShader(p.id, v);
        glAttachShader(p.id, f);
        // les attributs ont toujours les memes numeros
        glBindAttribLocation(p.id, 0, "aPos");
        glBindAttribLocation(p.id, 0, "aP");
        glBindAttribLocation(p.id, 1, "aNor");
        glBindAttribLocation(p.id, 1, "aUV");
        glBindAttribLocation(p.id, 1, "aC");
        glBindAttribLocation(p.id, 2, "aCol");
        glBindAttribLocation(p.id, 3, "aEmi");
        glLinkProgram(p.id);
        GLint ok = 0;
        glGetProgramiv(p.id, GL_LINK_STATUS, &ok);
        if (ok == 0)
        {
            char log[2048] = {};
            glGetProgramInfoLog(p.id, 2047, nullptr, log);
            DBG("BLOOM link error: " << log);
            failed = true;
        }
        glDeleteShader(v);
        glDeleteShader(f);
    }

    //--------------------------------------------------------------------------
    Mesh makeLit(const std::vector<float>& pos, const std::vector<juce::uint8>& col, const std::vector<juce::uint8>& emi, int n)
    {
        std::vector<float> data((size_t) n * 12);
        for (int t = 0; t + 2 < n; t += 3)
        {
            const float* a = &pos[(size_t) t * 3];
            const float* b = &pos[(size_t) (t + 1) * 3];
            const float* c = &pos[(size_t) (t + 2) * 3];
            const float u[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
            const float v[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
            float nn[3] = { u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0] };
            float l = std::sqrt(nn[0] * nn[0] + nn[1] * nn[1] + nn[2] * nn[2]);
            if (l < 1.0e-12f) l = 1.0f;
            for (float& x : nn) x /= l;

            for (int k = 0; k < 3; ++k)
            {
                const size_t vi = (size_t) (t + k);
                float* d = &data[vi * 12];
                d[0] = pos[vi * 3]; d[1] = pos[vi * 3 + 1]; d[2] = pos[vi * 3 + 2];
                d[3] = nn[0]; d[4] = nn[1]; d[5] = nn[2];
                for (int q = 0; q < 3; ++q)
                {
                    d[6 + q] = (float) col[vi * 3 + (size_t) q] / 255.0f * 1.5f;
                    d[9 + q] = (float) emi[vi * 3 + (size_t) q] / 255.0f * 1.5f;
                }
            }
        }

        Mesh m;
        m.n = n;
        glGenVertexArrays(1, &m.vao);
        glBindVertexArray(m.vao);
        glGenBuffers(1, &m.vbo);
        glBindBuffer(GL_ARRAY_BUFFER, m.vbo);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr) (data.size() * sizeof(float)), data.data(), GL_STATIC_DRAW);
        for (int a = 0; a < 4; ++a)
        {
            glEnableVertexAttribArray((GLuint) a);
            glVertexAttribPointer((GLuint) a, 3, GL_FLOAT, GL_FALSE, 48, (const void*) (uintptr_t) (a * 12));
        }
        glBindVertexArray(0);
        return m;
    }

    Mesh makeWater(const std::vector<float>& pos, const std::vector<float>& uv, int n)
    {
        Mesh m;
        m.n = n;
        glGenVertexArrays(1, &m.vao);
        glBindVertexArray(m.vao);
        GLuint b[2];
        glGenBuffers(2, b);
        glBindBuffer(GL_ARRAY_BUFFER, b[0]);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr) (pos.size() * sizeof(float)), pos.data(), GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, nullptr);
        glBindBuffer(GL_ARRAY_BUFFER, b[1]);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr) (uv.size() * sizeof(float)), uv.data(), GL_STATIC_DRAW);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
        glBindVertexArray(0);
        m.vbo = b[0];
        waterExtraVbo.push_back(b[1]);
        return m;
    }

    GLuint makeVao2D(const std::vector<float>& d)
    {
        GLuint vao = 0, vb = 0;
        glGenVertexArrays(1, &vao);
        glBindVertexArray(vao);
        glGenBuffers(1, &vb);
        glBindBuffer(GL_ARRAY_BUFFER, vb);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr) (d.size() * sizeof(float)), d.data(), GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
        glBindVertexArray(0);
        extraVbo.push_back(vb);
        return vao;
    }

    //--------------------------------------------------------------------------
    // La creation est decoupee en petites etapes, une par image : la fenetre s'affiche tout de suite
    // (cadre, logo, arcs sur fond sombre) et le monde 3D apparait en fondu des qu'il est pret.
    void beginCreate()
    {
        failed = false;
        loaded = false;
        initStage = 0;
        firstFrame = true;
        firstCall = true;
        startBackgroundDecode();
        worldFrames.store(0);
        usePosterHandover = bloomGetPoster().isValid();   // une affiche est montree : pas de fondu
        lastPosterGrab = juce::Time::getMillisecondCounterHiRes();
        bloomperf::mark("scene : contexte OpenGL cree");
    }

    void stepCreate()
    {
        const double t = juce::Time::getMillisecondCounterHiRes();
        switch (initStage)
        {
            case 0: makeProg(pPost, VS_FULL, FS_POST); makeProg(pSky, VS_FULL, FS_SKY); break;
            case 1: makeProg(pLit, VS_LIT, FS_LIT); break;
            case 2: makeProg(pSpr, VS_SPR, FS_SPR); makeProg(pPt, VS_PT, FS_PT); break;
            case 3: makeProg(pFog, VS_FOG, FS_FOG); makeProg(pWtr, VS_WTR, FS_WTR); break;
            case 4:
                if (!decodedScene().ready.load() && juce::Time::getMillisecondCounterHiRes() - createT0 < 3000.0)
                    return; // decodage pas encore fini : on attend l'image suivante
                buildWorld();
                loaded = !failed;
                revealT0 = usePosterHandover ? -1.0e9 : juce::Time::getMillisecondCounterHiRes();
                break;
            default: break;
        }
        bloomperf::mark("scene : etape " + juce::String(initStage) + " (" + juce::String(juce::Time::getMillisecondCounterHiRes() - t, 1) + " ms)"
                        + (loaded ? "  -> monde pret" : ""));
        ++initStage;
    }

    void buildWorld()
    {
        // --- les maillages embarques (deja decodes en arriere-plan si possible) ---
        auto& dec = decodedScene();
        const bool pre = dec.ready.load();
        const juce::MemoryBlock blob = pre ? dec.blob : decodeChunks(kSceneBlob, kSceneBlobCount);
        const juce::MemoryBlock metaBlock = pre ? dec.meta : decodeChunks(kSceneMeta, kSceneMetaCount);
        const juce::var meta = juce::JSON::parse(metaBlock.toString());

        if (blob.getSize() < 8 || !meta.isObject())
        {
            DBG("BLOOM : donnees de scene illisibles");
            failed = true;
            return;
        }

        const auto* p = static_cast<const juce::uint8*>(blob.getData());
        const size_t total = blob.getSize();
        size_t off = 0;
        auto rd32 = [&]() { juce::uint32 v = 0; if (off + 4 <= total) std::memcpy(&v, p + off, 4); off += 4; return v; };

        const int litCount = (int) rd32();
        meshes.clear();
        for (int i = 0; i < litCount; ++i)
        {
            const int n = (int) rd32();
            std::vector<float> pos((size_t) n * 3);
            std::vector<juce::uint8> col((size_t) n * 3), emi((size_t) n * 3);
            if (off + (size_t) n * 18 > total) { failed = true; return; }
            std::memcpy(pos.data(), p + off, (size_t) n * 12); off += (size_t) n * 12;
            std::memcpy(col.data(), p + off, (size_t) n * 3);  off += (size_t) n * 3;
            std::memcpy(emi.data(), p + off, (size_t) n * 3);  off += (size_t) n * 3;
            meshes.push_back(makeLit(pos, col, emi, n));
        }

        for (int w = 0; w < 2; ++w)
        {
            const int n = (int) rd32();
            float len = 0.0f;
            if (off + 4 <= total) std::memcpy(&len, p + off, 4);
            off += 4;
            std::vector<float> pos((size_t) n * 3), uv((size_t) n * 2);
            if (off + (size_t) n * 20 > total) { failed = true; return; }
            std::memcpy(pos.data(), p + off, (size_t) n * 12); off += (size_t) n * 12;
            std::memcpy(uv.data(), p + off, (size_t) n * 8);   off += (size_t) n * 8;
            water[w] = makeWater(pos, uv, n);
            waterLen[w] = len;
        }

        // --- le gobelin et sa bourse d'or (donnees separees : BloomGoblinData.h) ---
        iGob = iPurse = -1;
        {
            const juce::MemoryBlock gb = pre ? dec.goblin : decodeChunks(kGoblinBlob, kGoblinBlobCount);
            const auto* gp = static_cast<const juce::uint8*>(gb.getData());
            const size_t gt = gb.getSize();
            size_t go = 0;
            auto g32 = [&]() { juce::uint32 v = 0; if (go + 4 <= gt) std::memcpy(&v, gp + go, 4); go += 4; return v; };
            bool ok = gt >= 8;
            const int gCount = ok ? (int) g32() : 0;
            for (int i = 0; ok && i < gCount && i < 2; ++i)
            {
                const int n = (int) g32();
                if (n <= 0 || go + (size_t) n * 18 > gt) { ok = false; break; }
                std::vector<float> pos((size_t) n * 3);
                std::vector<juce::uint8> col((size_t) n * 3), emi((size_t) n * 3);
                std::memcpy(pos.data(), gp + go, (size_t) n * 12); go += (size_t) n * 12;
                std::memcpy(col.data(), gp + go, (size_t) n * 3);  go += (size_t) n * 3;
                std::memcpy(emi.data(), gp + go, (size_t) n * 3);  go += (size_t) n * 3;
                meshes.push_back(makeLit(pos, col, emi, n));
                (i == 0 ? iGob : iPurse) = (int) meshes.size() - 1;
            }
            if (!ok) { iGob = iPurse = -1; DBG("BLOOM : donnees du gobelin illisibles"); }

            // placement du gobelin sur l'ile (comme GM / GHAND du prototype)
            const float ga = 1.2f + juce::MathConstants<float>::pi;
            GM = mCh({ mT(std::cos(ga) * 3.4f, 0.05f, std::sin(ga) * 3.4f), mRY(juce::MathConstants<float>::pi * 0.5f - ga), mS(0.95f) });
            GHAND = { 0.28f, 0.64f, -0.17f + 0.26f };
        }

        // --- les reglages de placement (positions des ilots, corbeaux, rochers...) ---
        auto num = [](const juce::var& v) { return (float) (double) v; };
        auto meshId = [&](const juce::String& name) { return (int) meta["meshes"][name.toRawUTF8()]; };

        iFar = meshId("mFar"); iIsland = meshId("mIsland"); iGrass = meshId("mGrass"); iRoots = meshId("mRoots");
        iKnight = meshId("mKnight"); iFire = meshId("mFire"); iDeco = meshId("mDeco"); iStatue = meshId("mStatue");
        iRB = meshId("mRB"); iRW = meshId("mRW"); iPetal = meshId("mPetal"); iCore = meshId("mCore"); iInner = meshId("mInner");

        islets.clear();
        if (auto* a = meta["islets"].getArray())
        {
            int k = 0;
            for (auto& e : *a)
            {
                Islet is{};
                is.x = num(e["x"]); is.y = num(e["y"]); is.z = num(e["z"]); is.sc = num(e["sc"]); is.ph = num(e["ph"]);
                is.bell = (bool) e["bell"];
                is.mesh = meshId("islet" + juce::String(k));
                is.bellMesh = is.bell ? meshId("isletB" + juce::String(k)) : -1;
                islets.push_back(is);
                ++k;
            }
        }

        ravens.clear();
        if (auto* a = meta["ravens"].getArray())
            for (auto& e : *a)
                ravens.push_back({ num(e["a"]), num(e["r"]), num(e["y"]), num(e["s"]), num(e["ph"]), num(e["f"]), num(e["sc"]) });

        rocks.clear();
        if (auto* a = meta["rocks"].getArray())
        {
            int k = 0;
            for (auto& e : *a)
            {
                rocks.push_back({ num(e["r"]), num(e["a"]), num(e["y"]), num(e["s"]), num(e["rx"]), num(e["ry"]), num(e["sp"]),
                                  meshId("rock" + juce::String(k)) });
                ++k;
            }
        }

        wPh.assign((size_t) NW * 4, 0.0f);
        if (auto* a = meta["wPh"].getArray())
            for (int i = 0; i < a->size() && i < NW * 4; ++i)
                wPh[(size_t) i] = num((*a)[i]);

        KPOS = { num(meta["KPOS"][0]), num(meta["KPOS"][1]), num(meta["KPOS"][2]) };
        KROT = num(meta["KROT"]);
        FIRE = { num(meta["FIRE"][0]), num(meta["FIRE"][1]), num(meta["FIRE"][2]) };
        WPHI = num(meta["WPHI"]);
        WR = num(meta["WR"]);
        WFT = num(meta["WFT"]);

        // --- particules ---
        const int NT = NE + NW + NS + NO;
        ePos.assign((size_t) NE * 3, 0.0f);
        eVel.assign((size_t) NE * 3, 0.0f);
        eLife.assign((size_t) NE, 0.0f);
        ptPos.assign((size_t) NT * 3, 0.0f);
        ptCol.assign((size_t) NT * 4, 0.0f);
        ISL = mId();
        for (int i = 0; i < NE; ++i)
            spawnEmber(i, true);

        if (auto* sp = meta["stars"]["pos"].getArray())
            for (int i = 0; i < sp->size() && i < NS * 3; ++i)
                ptPos[(size_t) (NE + NW) * 3 + (size_t) i] = num((*sp)[i]);
        if (auto* sc = meta["stars"]["col"].getArray())
            for (int i = 0; i < sc->size() && i < NS * 4; ++i)
                ptCol[(size_t) (NE + NW) * 4 + (size_t) i] = num((*sc)[i]);

        glGenVertexArrays(1, &vaoPts);
        glBindVertexArray(vaoPts);
        glGenBuffers(1, &bPP);
        glGenBuffers(1, &bPC);
        glBindBuffer(GL_ARRAY_BUFFER, bPP);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr) (ptPos.size() * sizeof(float)), ptPos.data(), GL_DYNAMIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, nullptr);
        glBindBuffer(GL_ARRAY_BUFFER, bPC);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr) (ptCol.size() * sizeof(float)), ptCol.data(), GL_DYNAMIC_DRAW);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 0, nullptr);
        glBindVertexArray(0);

        vaoFull = makeVao2D({ -1, -1, 3, -1, -1, 3 });
        vaoQuad = makeVao2D({ -1, -1, 1, -1, 1, 1, -1, -1, 1, 1, -1, 1 });
    }

    void destroy()
    {
        for (auto& m : meshes)
        {
            if (m.vao) glDeleteVertexArrays(1, &m.vao);
            if (m.vbo) glDeleteBuffers(1, &m.vbo);
        }
        meshes.clear();
        for (auto& m : water)
        {
            if (m.vao) glDeleteVertexArrays(1, &m.vao);
            if (m.vbo) glDeleteBuffers(1, &m.vbo);
            m = Mesh();
        }
        for (auto b : waterExtraVbo) glDeleteBuffers(1, &b);
        waterExtraVbo.clear();
        for (auto b : extraVbo) glDeleteBuffers(1, &b);
        extraVbo.clear();
        if (vaoFull) glDeleteVertexArrays(1, &vaoFull);
        if (vaoQuad) glDeleteVertexArrays(1, &vaoQuad);
        if (vaoPts)  glDeleteVertexArrays(1, &vaoPts);
        if (bPP) glDeleteBuffers(1, &bPP);
        if (bPC) glDeleteBuffers(1, &bPC);
        vaoFull = vaoQuad = vaoPts = bPP = bPC = 0;
        releaseFbo();
        for (Prog* p : { &pLit, &pSky, &pSpr, &pPt, &pFog, &pPost, &pWtr })
        {
            if (p->id) glDeleteProgram(p->id);
            p->id = 0;
            p->cache.clear();
        }
        loaded = false;
    }

    void releaseFbo()
    {
        if (fbo) glDeleteFramebuffers(1, &fbo);
        if (tex) glDeleteTextures(1, &tex);
        if (rbo) glDeleteRenderbuffers(1, &rbo);
        fbo = tex = rbo = 0;
        fw = fh = 0;
    }

    void ensureFbo(int cw, int ch)
    {
        const int tw = juce::jlimit(200, 720, juce::roundToInt((float) cw / 1.6f)); // resolution de la 3D (avant : / 2.4)
        const int w = tw;
        const int h = juce::jmax(1, juce::roundToInt((float) tw * (float) ch / (float) cw));
        if (w == fw && h == fh && fbo != 0)
            return;

        releaseFbo();
        fw = w;
        fh = h;

        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, fw, fh, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        glGenRenderbuffers(1, &rbo);
        glBindRenderbuffer(GL_RENDERBUFFER, rbo);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, fw, fh);

        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, rbo);
        glBindFramebuffer(GL_FRAMEBUFFER, (GLuint) ctx.getFrameBufferID());
    }

    //--------------------------------------------------------------------------
    void spawnEmber(int i, bool init)
    {
        const size_t j = (size_t) i * 3;
        if (i % 5 == 0)
        {
            const V3 fw3 = mAp(ISL, { FIRE[0], 0.2f, FIRE[2] });
            ePos[j] = fw3[0] + R(-0.1f, 0.1f);
            ePos[j + 1] = init ? R(0.1f, 2.5f) : fw3[1];
            ePos[j + 2] = fw3[2] + R(-0.1f, 0.1f);
            eVel[j] = R(-0.06f, 0.06f);
            eVel[j + 1] = R(0.45f, 0.9f);
            eVel[j + 2] = R(-0.06f, 0.06f);
            eLife[(size_t) i] = R(0.3f, 0.7f);
            return;
        }
        const float a = R(0.0f, 6.28f);
        const float r = std::pow(rnd.nextFloat(), 0.6f) * 3.4f;
        ePos[j] = std::cos(a) * r;
        ePos[j + 1] = init ? R(-3.0f, 7.0f) : R(-3.5f, 0.6f);
        ePos[j + 2] = std::sin(a) * r;
        eVel[j] = R(-0.08f, 0.08f);
        eVel[j + 1] = R(0.25f, 0.7f);
        eVel[j + 2] = R(-0.08f, 0.08f);
        eLife[(size_t) i] = R(0.6f, 1.4f);
    }

    //--------------------------------------------------------------------------
    void drawLit(int mi, const Mat& model, V3 tint = { 1, 1, 1 }, V3 emis = { 1, 1, 1 }, float fog = -1.0f, bool two = false)
    {
        if (mi < 0 || mi >= (int) meshes.size())
            return;
        glUniformMatrix4fv(pLit.u("uModel"), 1, GL_FALSE, model.data());
        glUniform3fv(pLit.u("uTint"), 1, tint.data());
        glUniform3fv(pLit.u("uEmis"), 1, emis.data());
        glUniform1f(pLit.u("uFogD"), fog >= 0.0f ? fog : fogD);
        glUniform1f(pLit.u("uTwo"), two ? 1.0f : 0.0f);
        glBindVertexArray(meshes[(size_t) mi].vao);
        glDrawArrays(GL_TRIANGLES, 0, meshes[(size_t) mi].n);
    }

    void spr(V3 c, float sx, float sy, V3 col, float a, float mode = 0.0f, float pw = 2.0f)
    {
        glUniform3fv(pSpr.u("uC"), 1, c.data());
        glUniform2f(pSpr.u("uSz"), sx, sy);
        glUniform3fv(pSpr.u("uCol"), 1, col.data());
        glUniform1f(pSpr.u("uA"), a);
        glUniform1f(pSpr.u("uMode"), mode);
        glUniform1f(pSpr.u("uPow"), pw);
        glDrawArrays(GL_TRIANGLES, 0, 6);
    }

    V3 wfPos(float t) const
    {
        const float dx = std::cos(WPHI), dz = std::sin(WPHI);
        return V3{ dx * (WR + 0.7f * t), 0.06f - 4.5f * t * t, dz * (WR + 0.7f * t) };
    }

    //--------------------------------------------------------------------------
    // copie l'image du monde (avant que l'interface soit posee dessus) toutes les 2 secondes
    void grabPosterIfDue(int pw, int ph)
    {
        const double now = juce::Time::getMillisecondCounterHiRes();
        if (now - lastPosterGrab < 2000.0 || now - revealT0 < 400.0 || pw <= 0 || ph <= 0)
            return;
        lastPosterGrab = now;
        BLOOM_PERF_SCOPE("scene : copie de l'affiche", 8.0);
        posterPixels.resize((size_t) pw * (size_t) ph * 4);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, pw, ph, GL_RGBA, GL_UNSIGNED_BYTE, posterPixels.data());
        juce::Image img(juce::Image::RGB, pw, ph, false);
        {
            juce::Image::BitmapData bd(img, juce::Image::BitmapData::writeOnly);
            for (int y = 0; y < ph; ++y)
            {
                const uint8_t* src = posterPixels.data() + (size_t) (ph - 1 - y) * (size_t) pw * 4;
                for (int x = 0; x < pw; ++x, src += 4)
                    bd.setPixelColour(x, y, juce::Colour(src[0], src[1], src[2]));
            }
        }
        auto& ps = posterStore();
        const juce::ScopedLock sl(ps.lock);
        ps.image = img;
        ps.dirty = true;
    }

    void render()
    {
        if (!loaded)
        {
            // tout est prepare d'un coup (quelques ms) : la premiere image affichee est deja le monde complet
            for (int guard = 0; guard < 8 && !loaded && !failed; ++guard)
            {
                const int before = initStage;
                stepCreate();
                if (initStage == before)
                    break; // decodage pas encore fini
            }
            glClearColor(0.05f, 0.04f, 0.08f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            if (!loaded)
            {
                if (failed)
                    worldFrames.fetch_add(1); // pas de 3D possible : l'interface s'affiche quand meme
                return;
            }
        }

        const int cw = owner.getWidth();
        const int ch = owner.getHeight();
        if (cw <= 0 || ch <= 0)
            return;

        const float scale = (float) ctx.getRenderingScale();
        const int pw = juce::roundToInt((float) cw * scale);
        const int ph = juce::roundToInt((float) ch * scale);
        const GLuint screenFbo = (GLuint) ctx.getFrameBufferID();

        ensureFbo(cw, ch);
        const float W = (float) fw, H = (float) fh;

        // --- temps et reglages lisses ---
        const double now = juce::Time::getMillisecondCounterHiRes();
        const double gapMs = now - lastTime;
        if (firstFrame)
        {
            firstFrame = false;
            bloomperf::mark("scene : premiere image du monde");
        }
        else if (gapMs > 45.0)
            bloomperf::mark("scene : image en retard (" + juce::String(gapMs, 0) + " ms depuis la precedente)");
        const float dt = juce::jmin(0.05f, (float) (gapMs / 1000.0));
        lastTime = now;

        // mesures pour l'affichage de diagnostic (images par seconde, plus long intervalle)
        ++statFrames;
        statWorstGap = juce::jmax(statWorstGap, gapMs);
        if (now - statStart >= 1000.0)
        {
            statFps.store((float) ((double) statFrames * 1000.0 / (now - statStart)));
            statWorst.store((float) statWorstGap);
            statFrames = 0;
            statWorstGap = 0.0;
            statStart = now;
        }
        T += dt;

        const float ks = juce::jmin(1.0f, dt * 6.0f);
        const int hv = hover.load();
        for (int i = 0; i < 7; ++i)
        {
            S[i] += (P(i) - S[i]) * ks;
            boost[i] = juce::jmax(hv == i ? 1.0f : 0.0f, boost[i] - dt * 1.5f);
        }
        const bool byp = pByp != nullptr && pByp->getValue() > 0.5f;
        const bool del = pDel != nullptr && pDel->getValue() > 0.5f;
        sByp += ((byp ? 1.0f : 0.0f) - sByp) * ks;
        sDel += ((del ? 1.0f : 0.0f) - sDel) * ks;

        enum { WEIGHT = 0, WARMTH, SPACE, PRESENCE, AIR, ENHANCE, MIX };
        const float mx = S[MIX] * (1.0f - sByp);
        const float wB = S[WEIGHT] * mx + boost[WEIGHT] * 0.35f;
        const float pB = S[PRESENCE] * mx + boost[PRESENCE] * 0.35f;
        const float aB = S[AIR] * mx + boost[AIR] * 0.35f;
        const float sB = S[SPACE] * mx;
        const float warm = S[WARMTH] * mx + boost[WARMTH] * 0.3f;
        const float en = S[ENHANCE] * mx + boost[ENHANCE] * 0.35f;
        fogD = 0.085f - sB * 0.055f;

        // --- camera (format vertical) ---
        const float fov = 44.0f * juce::MathConstants<float>::pi / 180.0f;
        const float sway = std::sin(T * 0.07f);
        const V3 eye{ sway * 2.0f, 4.1f + std::sin(T * 0.05f) * 0.2f, 12.6f };
        const V3 tgt{ 0.0f, 1.75f, 0.0f };
        const Mat V = mLook(eye, tgt, { 0, 1, 0 });
        const Mat Pr = mPersp(fov, W / H, 0.1f, 200.0f);
        const Mat VP = mMul(Pr, V);

        const float bob = std::sin(T * 0.55f) * 0.07f * (1.0f + wB * 0.6f);
        const float shake = wB > 0.85f ? std::sin(T * 47.0f) * 0.006f * (wB - 0.85f) * 8.0f : 0.0f;
        const Mat isl = mCh({ mT(shake, bob, 0), mRY(T * 0.17f) });
        ISL = isl;

        const V3 coreCol = vMix({ 1, 0.72f, 0.38f }, { 1, 0.36f, 0.12f }, warm);
        const V3 hot = vMix({ 1, 0.95f, 0.8f }, { 1, 0.75f, 0.45f }, warm);
        const V3 cp = mAp(isl, { 0, 2.68f, 0 });
        const float coreY = cp[1];
        const float coreI = 1.6f + pB * 2.6f + en * 0.8f;
        const float rootGlow = 0.25f + wB * 1.35f;

        // ================= dessin dans la petite image =================
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glViewport(0, 0, fw, fh);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_BLEND);
        glDisable(GL_CULL_FACE);

        // ciel
        glUseProgram(pSky.id);
        glUniform1f(pSky.u("uT"), T);
        glUniform1f(pSky.u("uAir"), 0.4f + aB * 0.8f);
        glUniform1f(pSky.u("uWarm"), warm);
        glBindVertexArray(vaoFull);
        glDrawArrays(GL_TRIANGLES, 0, 3);

        // lune
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE);
        glUseProgram(pSpr.id);
        glUniformMatrix4fv(pSpr.u("uV"), 1, GL_FALSE, V.data());
        glUniformMatrix4fv(pSpr.u("uPr"), 1, GL_FALSE, Pr.data());
        glBindVertexArray(vaoQuad);
        spr({ -26, 24, -70 }, 11, 11, { 0.62f, 0.6f, 0.72f }, 0.85f, 1);
        spr({ -26, 24, -70 }, 30, 30, { 0.28f, 0.25f, 0.4f }, 0.35f, 0, 2.5f);
        glDisable(GL_BLEND);

        // monde en 3D
        glEnable(GL_DEPTH_TEST);
        glDepthMask(GL_TRUE);
        glClear(GL_DEPTH_BUFFER_BIT);

        glUseProgram(pLit.id);
        glUniformMatrix4fv(pLit.u("uVP"), 1, GL_FALSE, VP.data());
        glUniform3fv(pLit.u("uEye"), 1, eye.data());
        const float amb[3] = { 0.07f, 0.062f, 0.095f };
        glUniform3fv(pLit.u("uAmb"), 1, amb);
        {
            float md[3] = { -0.6f, 0.55f, 0.35f };
            const float ml = std::sqrt(md[0] * md[0] + md[1] * md[1] + md[2] * md[2]);
            for (float& x : md) x /= ml;
            const float mc[3] = { 0.2f, 0.22f, 0.4f };
            glUniform3fv(pLit.u("uMoonDir"), 1, md);
            glUniform3fv(pLit.u("uMoonCol"), 1, mc);
        }
        const float fl = 0.75f + 0.25f * std::sin(T * 13.0f) * std::sin(T * 7.7f + 1.0f) + 0.1f * std::sin(T * 23.0f);
        const V3 fpL = mAp(isl, { FIRE[0], 0.5f, FIRE[2] });
        {
            const float lp[12] = { cp[0], cp[1], cp[2] + 0.15f,  0.0f, -3.6f + bob, 0.0f,  0.0f, 6.0f, 1.0f,  fpL[0], fpL[1], fpL[2] };
            const V3 c0 = vMul(coreCol, coreI), c1 = vMul({ 1, 0.42f, 0.15f }, wB * 2.4f);
            const V3 c2 = vMul({ 0.5f, 0.42f, 0.9f }, aB * 0.9f), c3 = vMul({ 1, 0.48f, 0.16f }, 1.5f * fl);
            const float lc[12] = { c0[0], c0[1], c0[2], c1[0], c1[1], c1[2], c2[0], c2[1], c2[2], c3[0], c3[1], c3[2] };
            const float lk[4] = { 0.11f, 0.3f, 0.18f, 1.1f };
            glUniform3fv(pLit.u("uPLp"), 4, lp);
            glUniform3fv(pLit.u("uPLc"), 4, lc);
            glUniform1fv(pLit.u("uPLk"), 4, lk);
        }
        glUniform2f(pLit.u("uSnap"), W / 2.0f, H / 2.0f);
        const float fogCol[3] = { 0.095f, 0.075f, 0.125f };
        glUniform3fv(pLit.u("uFogCol"), 1, fogCol);

        auto grey = [](float v) { return V3{ v, v, v }; };

        drawLit(iFar, mId(), { 1, 1, 1 }, { 0.6f, 0.6f, 0.6f }, 0.016f);
        drawLit(iIsland, isl, { 1, 1, 1 }, grey(0.35f + wB * 0.9f + en * 0.3f), -1.0f, true);
        {
            const float shear = std::sin(T * 1.1f) * 0.07f + std::sin(T * 2.3f) * 0.03f + wB * std::sin(T * 9.0f) * 0.02f;
            const Mat sh{ 1,0,0,0, shear,1,0,0, 0,0,1,0, 0,0,0,1 };
            drawLit(iGrass, mCh({ isl, sh }), { 2.3f, 2.4f, 2.1f }, grey(0.5f + wB * 0.5f + en * 0.2f), -1.0f, true);
        }
        drawLit(iRoots, isl, { 1, 1, 1 }, vMul(vMix({ 1, 0.8f, 0.7f }, { 1, 0.6f, 0.45f }, warm), rootGlow));
        drawLit(iKnight, mCh({ isl, mT(KPOS[0], KPOS[1], KPOS[2]), mRY(KROT), mS(1, 1 + std::sin(T * 1.3f) * 0.01f, 1) }), { 1, 1, 1 }, { 1, 1, 1 }, -1.0f, true);
        drawLit(iFire, isl, { 1, 1, 1 }, grey(0.6f + fl * 0.6f), -1.0f, true);
        // gobelin assis au bord de l'ile, et sa bourse d'or qu'il lance puis rattrape (cycle de 4 s, lancer sur 55 %)
        drawLit(iGob, mCh({ isl, GM }), { 2.6f, 2.5f, 2.6f }, { 1, 1, 1 }, -1.0f, true);
        {
            const float tp = juce::jmin(1.0f, (std::fmod(T, 4.0f) / 4.0f) / 0.55f);
            const float arc = std::sin(tp * juce::MathConstants<float>::pi);
            const float hy = arc * 0.45f, spin = tp * juce::MathConstants<float>::pi * 2.0f;
            drawLit(iPurse, mCh({ isl, GM, mT(GHAND[0] + arc * 0.04f, GHAND[1] + 0.04f + hy, GHAND[2]), mRX(spin), mRZ(arc * 0.4f) }),
                    { 2.6f, 2.5f, 2.6f }, { 1, 1, 1 }, -1.0f, true);
        }
        drawLit(iDeco, mCh({ isl, mT(std::sin(T * 33.0f) * 0.012f * wB, std::sin(T * 27.0f) * 0.01f * wB, 0) }), { 1, 1, 1 }, grey(0.35f + wB * 0.9f), -1.0f, true);
        drawLit(iStatue, mCh({ mT(12, -9.5f, -48), mRY(-0.3f), mS(1.6f) }), { 2.6f, 2.4f, 3 }, { 1, 1, 1 }, 0.011f, true);

        // ilots flottants
        V3 is0{ 0, 0, 0 }, is2{ 0, 0, 0 }, rv0{ 0, 0, 0 };
        for (size_t ix = 0; ix < islets.size(); ++ix)
        {
            const auto& o = islets[ix];
            const float k = 1.0f + sB * 0.4f, px = 0.62f;
            const float x = o.x * k * px + std::sin(T * 0.12f + o.ph) * 0.5f;
            const float y = o.y + std::sin(T * 0.4f + o.ph) * 0.3f + sB * (o.y > 0.0f ? 1.0f : -1.0f) * 0.8f;
            const float z = o.z - sB * 2.0f;
            if (ix == 0) is0 = { x, y, z };
            if (ix == 2) is2 = { x, y + 1.0f, z };
            const Mat m = mCh({ mT(x, y, z), mRY(T * 0.05f + o.ph), mS(o.sc) });
            drawLit(o.mesh, m, { 2.4f, 2.2f, 2.8f }, grey(0.5f + wB * 0.5f), -1.0f, true);
            if (o.bell)
                drawLit(o.bellMesh, mCh({ m, mT(0, 1.58f, 0), mRZ(std::sin(T * 0.9f) * 0.22f), mT(0, -1.58f, 0) }),
                        { 2.4f, 2.2f, 2.8f }, grey(0.5f), -1.0f, true);
        }

        // corbeaux
        for (size_t ri = 0; ri < ravens.size(); ++ri)
        {
            const auto& r = ravens[ri];
            const float sg = r.s < 0.0f ? -1.0f : 1.0f;
            const float a = r.a + T * r.s;
            const float y = r.y + aB * 1.6f + std::sin(T * 0.7f + r.ph) * 0.4f;
            const float px = std::cos(a) * r.r, pz = std::sin(a) * r.r * 0.75f - 1.0f;
            const float dx = -std::sin(a) * sg * r.r, dz = std::cos(a) * sg * r.r * 0.75f;
            if (ri == 0) rv0 = { px, y, pz };
            const Mat base = mCh({ mT(px, y, pz), mRY(std::atan2(dx, dz)), mRZ(sg * -0.18f), mS(r.sc) });
            const float fw2 = std::sin(T * r.f + r.ph) * 0.7f;
            const V3 em{ 0.5f, 0.42f, 0.7f };
            drawLit(iRB, base, { 1, 1, 1 }, em, 0.03f, true);
            drawLit(iRW, mCh({ base, mRZ(fw2) }), { 1, 1, 1 }, em, 0.03f, true);
            drawLit(iRW, mCh({ base, mS(-1, 1, 1), mRZ(fw2) }), { 1, 1, 1 }, em, 0.03f, true);
        }

        // rochers en orbite
        for (const auto& r : rocks)
        {
            const float a = r.a + T * r.s;
            const Mat m = mCh({ mT(std::cos(a) * r.r, r.y + std::sin(T * r.sp + r.a) * 0.25f + bob * 0.5f, std::sin(a) * r.r),
                                mRY(T * 0.2f + r.ry), mRX(T * 0.13f + r.rx) });
            drawLit(r.mesh, m, { 1, 1, 1 }, grey(0.3f + wB * 0.7f));
        }

        // petales de la fleur
        const float open = 0.55f + aB * 0.55f;
        const Mat head = mCh({ isl, mT(0, 2.5f, 0), mS(1.8f) });
        for (int i = 0; i < 7; ++i)
        {
            const Mat m = mCh({ head, mRY((float) i / 7.0f * 6.283f + 0.2f), mRX(open + 0.25f + std::sin(T * 0.8f + (float) i) * 0.03f), mS(1, 1.25f, 1) });
            drawLit(iPetal, m, { 0.5f, 0.42f, 0.85f }, vMul({ 0.5f, 0.34f, 1 }, 0.45f + pB * 0.6f + aB * 0.3f), -1.0f, true);
        }
        for (int i = 0; i < 5; ++i)
        {
            const Mat m = mCh({ head, mRY((float) i / 5.0f * 6.283f + 0.9f), mRX(open * 0.45f + 0.05f + std::sin(T * 1.1f + (float) i) * 0.04f), mS(0.85f, 0.9f, 0.85f) });
            drawLit(iPetal, m, { 0.85f, 0.78f, 1 }, vMul({ 0.85f, 0.72f, 1 }, 0.55f + pB * 0.7f), -1.0f, true);
        }
        const float cs = 0.85f + pB * 0.45f + std::sin(T * 2.2f) * 0.04f * pB;
        drawLit(iCore, mCh({ mT(cp[0], cp[1], cp[2]), mRY(T * 0.6f), mRX(T * 0.4f), mS(cs * 1.4f) }), { 0, 0, 0 }, vMul(coreCol, 1.25f), 0.0f);
        drawLit(iInner, mCh({ mT(cp[0], cp[1], cp[2] + 0.3f), mRY(-T), mS(cs * 1.4f) }), { 0, 0, 0 }, hot, 0.0f);

        // brume
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glDepthMask(GL_FALSE);
        glUseProgram(pFog.id);
        glUniformMatrix4fv(pFog.u("uVP"), 1, GL_FALSE, VP.data());
        glUniform1f(pFog.u("uT"), T);
        glBindVertexArray(vaoQuad);
        {
            const float fogA = 0.62f - sB * 0.42f;
            const V3 clr = vMul({ 0.3f, 0.13f, 0.09f }, 0.4f + wB * 1.2f);
            struct FL { float y, r, a, sd; bool w; };
            const FL layers[6] = { { -4.6f, 22, 0.8f, 0, false }, { -2.2f, 16, 0.6f, 3, false }, { 0.15f, 6, 0.35f, 7, false },
                                   { -7.5f, 34, 0.9f, 11, false }, { -6, 30, 0.8f, 17, true }, { -9.5f, 44, 0.9f, 23, true } };
            for (const auto& l : layers)
            {
                const float c3[3] = { 0.0f, l.y, 0.0f };
                glUniform3fv(pFog.u("uC"), 1, c3);
                glUniform1f(pFog.u("uR"), l.r);
                glUniform1f(pFog.u("uA"), l.a * (l.w ? 0.8f : fogA));
                glUniform1f(pFog.u("uSeed"), l.sd);
                const V3 col = l.w ? clr : V3{ 0.13f, 0.11f, 0.17f };
                glUniform3fv(pFog.u("uCol"), 1, col.data());
                glDrawArrays(GL_TRIANGLES, 0, 6);
            }
        }

        // lumieres additives
        glBlendFunc(GL_ONE, GL_ONE);
        glUseProgram(pSpr.id);
        glUniformMatrix4fv(pSpr.u("uV"), 1, GL_FALSE, V.data());
        glUniformMatrix4fv(pSpr.u("uPr"), 1, GL_FALSE, Pr.data());
        glBindVertexArray(vaoQuad);
        glDisable(GL_DEPTH_TEST);
        const V3 cpos{ cp[0], cp[1], cp[2] + 0.1f };
        spr({ cp[0], coreY + 6.0f, cp[2] }, 0.7f + aB * 0.5f, 7, vMul({ 0.55f, 0.45f, 1 }, 1.0f), 0.22f + aB * 0.35f, 2);
        spr(cpos, 7 + pB * 2.5f, 7 + pB * 2.5f, { 0.3f, 0.18f, 0.58f }, 0.3f + pB * 0.18f, 0, 2.4f);
        spr(cpos, 1.5f + pB * 0.9f, 1.5f + pB * 0.9f, coreCol, 0.3f + pB * 0.25f, 0, 2.6f);
        spr(cpos, 0.45f + pB * 0.3f, 0.45f + pB * 0.3f, hot, 0.6f, 0, 1.8f);
        glEnable(GL_DEPTH_TEST);
        const V3 fp = mAp(isl, { FIRE[0], 0.22f, FIRE[2] });
        spr(fp, 1.3f, 1.3f, { 1, 0.38f, 0.1f }, 0.16f + fl * 0.08f, 0, 2.2f);
        spr({ fp[0], fp[1] + 0.14f, fp[2] }, 0.17f, 0.36f + fl * 0.08f, { 1, 0.5f, 0.15f }, 0.95f, 2);
        spr({ fp[0] + 0.03f, fp[1] + 0.06f, fp[2] + 0.02f }, 0.1f, 0.22f + fl * 0.05f, { 1, 0.85f, 0.45f }, 0.9f, 2);
        spr({ 0, -3.4f + bob, 0 }, 4.5f, 3.2f, { 1, 0.4f, 0.12f }, 0.06f + wB * 0.2f, 0, 2.4f);
        spr({ 0, -5.2f + bob, 0 }, 2.6f, 2.6f, { 1, 0.5f, 0.18f }, 0.08f + wB * 0.3f, 0, 2);

        // cascade
        {
            const float wa = 0.78f + sB * 0.5f + boost[SPACE] * 0.3f;
            glUseProgram(pWtr.id);
            glUniformMatrix4fv(pWtr.u("uVP"), 1, GL_FALSE, VP.data());
            glUniformMatrix4fv(pWtr.u("uModel"), 1, GL_FALSE, isl.data());
            glUniform2f(pWtr.u("uSnap"), W / 2.0f, H / 2.0f);
            glUniform1f(pWtr.u("uFogD"), fogD);
            glUniform1f(pWtr.u("uT"), T);
            glUniform1f(pWtr.u("uA"), wa);
            for (int w = 0; w < 2; ++w)
            {
                glUniform1f(pWtr.u("uMode"), (float) w);
                glUniform1f(pWtr.u("uLen"), waterLen[w]);
                glBindVertexArray(water[w].vao);
                glDrawArrays(GL_TRIANGLES, 0, water[w].n);
            }

            glUseProgram(pSpr.id);
            glBindVertexArray(vaoQuad);
            const float wt0 = -std::sin(WPHI), wt2 = std::cos(WPHI);
            spr(mAp(isl, wfPos(0.02f)), 0.55f, 0.4f, { 0.55f, 0.75f, 1 }, 0.4f * wa, 0, 2);
            const float mist[4][3] = { { 0.3f, 1.1f, 0.1f }, { 0.55f, 1.5f, 0.09f }, { 0.9f, 2.6f, 0.12f }, { 1.15f, 3.6f, 0.12f } };
            for (int k = 0; k < 4; ++k)
            {
                const V3 p = wfPos(juce::jmin(WFT, mist[k][0] * WFT * 0.95f));
                spr(mAp(isl, p), mist[k][1] * (1.0f + 0.1f * std::sin(T * 1.3f + (float) k)),
                    mist[k][1] * (1.2f + 0.1f * std::sin(T * 1.1f + (float) k * 2.0f)), { 0.22f, 0.34f, 0.6f }, mist[k][2] * wa, 0, 2.2f);
            }
            for (int i = 0; i < 16; ++i)
            {
                const float ss = std::fmod(T * 0.55f + (float) i * 0.371f, 1.0f);
                const float t = ss * WFT;
                const V3 p = wfPos(t);
                const float lat = (std::sin((float) i * 12.9f) * 0.5f) * (0.5f + 0.7f * ss);
                const float sw = 0.04f + 0.03f * (float) (i % 3);
                spr(mAp(isl, { p[0] + wt0 * lat, p[1], p[2] + wt2 * lat }), sw, sw, { 0.7f, 0.85f, 1 }, (0.8f - 0.55f * ss) * wa, 1);
            }
        }

        // particules (braises, lucioles, etoiles, orbes)
        {
            const float sp = (0.35f + aB * 1.25f) * (1.0f + en * 0.5f);
            const V3 ec = vMix({ 1, 0.75f, 0.4f }, { 1, 0.42f, 0.15f }, warm);
            for (int i = 0; i < NE; ++i)
            {
                const size_t j = (size_t) i * 3;
                ePos[j] += (eVel[j] + std::sin(T * 0.7f + (float) i) * 0.06f) * dt * sp;
                ePos[j + 1] += eVel[j + 1] * dt * sp;
                ePos[j + 2] += (eVel[j + 2] + std::cos(T * 0.6f + (float) i) * 0.06f) * dt * sp;
                eLife[(size_t) i] -= dt * 0.12f * sp;
                if (eLife[(size_t) i] <= 0.0f || ePos[j + 1] > 8.0f)
                    spawnEmber(i, false);
                const float f = juce::jmin(1.0f, eLife[(size_t) i] * 1.4f) * juce::jmin(1.0f, (ePos[j + 1] + 4.0f) * 0.5f);
                const float fk = 0.6f + 0.4f * std::sin(T * 9.0f + (float) i * 1.7f);
                ptPos[j] = ePos[j];
                ptPos[j + 1] = ePos[j + 1] + bob;
                ptPos[j + 2] = ePos[j + 2];
                ptCol[(size_t) i * 4] = ec[0] * f * fk;
                ptCol[(size_t) i * 4 + 1] = ec[1] * f * fk;
                ptCol[(size_t) i * 4 + 2] = ec[2] * f * fk;
                ptCol[(size_t) i * 4 + 3] = (i % 9 == 0 ? 60.0f : 30.0f) * (0.6f + aB * 0.6f);
            }
            for (int i = 0; i < NW; ++i)
            {
                const size_t k = (size_t) (NE + i);
                const float a = wPh[(size_t) i * 4] + T * wPh[(size_t) i * 4 + 3];
                const float r = wPh[(size_t) i * 4 + 1] + std::sin(T * 0.5f + (float) i) * 0.2f;
                ptPos[k * 3] = cp[0] + std::cos(a) * r;
                ptPos[k * 3 + 1] = coreY - 0.8f + wPh[(size_t) i * 4 + 2] * 0.9f + std::sin(T * 0.9f + (float) i) * 0.3f;
                ptPos[k * 3 + 2] = cp[2] + std::sin(a) * r;
                const float b = (0.35f + 0.65f * juce::jmax(0.0f, std::sin(T * 1.7f + (float) i * 2.3f))) * (0.4f + aB * 0.8f);
                ptCol[k * 4] = 0.6f * b; ptCol[k * 4 + 1] = 0.5f * b; ptCol[k * 4 + 2] = 1.0f * b; ptCol[k * 4 + 3] = 36.0f;
            }
            for (int i = 0; i < NO; ++i)
            {
                const size_t k = (size_t) (NE + NW + NS + i);
                const float f1 = 0.6f + (float) (i % 5) * 0.12f, ph2 = (float) i * 2.399f;
                ptPos[k * 3] = std::sin(T * 0.11f * f1 + ph2) * 8.5f * 0.65f;
                ptPos[k * 3 + 1] = 1.0f + std::sin(T * 0.19f * f1 + ph2 * 2.0f) * 3.6f;
                ptPos[k * 3 + 2] = std::cos(T * 0.09f * f1 + ph2 * 1.3f) * 6.0f - 2.0f;
                const float vis = juce::jlimit(0.0f, 1.0f, (0.3f + pB * 0.75f) * (float) NO - (float) i);
                const float b = vis * (0.45f + 0.55f * juce::jmax(0.0f, std::sin(T * 1.3f + (float) i * 1.9f)));
                ptCol[k * 4] = 0.3f * b; ptCol[k * 4 + 1] = 0.62f * b; ptCol[k * 4 + 2] = 1.0f * b; ptCol[k * 4 + 3] = 52.0f;
            }

            glUseProgram(pPt.id);
            glUniformMatrix4fv(pPt.u("uVP"), 1, GL_FALSE, VP.data());
            glUniform1f(pPt.u("uScale"), H / 150.0f);
            glEnable(GL_PROGRAM_POINT_SIZE);
            glBindBuffer(GL_ARRAY_BUFFER, bPP);
            glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr) (ptPos.size() * sizeof(float)), ptPos.data());
            glBindBuffer(GL_ARRAY_BUFFER, bPC);
            glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr) (ptCol.size() * sizeof(float)), ptCol.data());
            glBindVertexArray(vaoPts);
            glDrawArrays(GL_POINTS, 0, NE + NW + NS + NO);
            glDisable(GL_PROGRAM_POINT_SIZE);
        }
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);

        // ================= passe finale "PS1" vers l'ecran =================
        glBindFramebuffer(GL_FRAMEBUFFER, screenFbo);
        glViewport(0, 0, pw, ph);
        glDisable(GL_DEPTH_TEST);
        glUseProgram(pPost.id);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, tex);
        glUniform1i(pPost.u("uTex"), 0);
        glUniform2f(pPost.u("uRes"), W, H);
        glUniform1f(pPost.u("uT"), T);
        glUniform1f(pPost.u("uGlow"), 1.0f + en * 1.4f);
        glUniform1f(pPost.u("uThr"), 0.72f - en * 0.14f);
        glUniform1f(pPost.u("uDelta"), sDel);
        {
            const float rv = (float) juce::jlimit(0.0, 1.0, (juce::Time::getMillisecondCounterHiRes() - revealT0) / 350.0);
            glUniform1f(pPost.u("uFade"), rv * rv * (3.0f - 2.0f * rv)); // apparition en fondu (0.35 s)
        }
        glBindVertexArray(vaoFull);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        worldFrames.fetch_add(1);
        grabPosterIfDue(pw, ph);

        // --- ancres des reglages dans le monde (pour le point de vue 2) ---
        {
            const auto proj = [&](V3 p, int slot)
            {
                const float x = VP[0] * p[0] + VP[4] * p[1] + VP[8] * p[2] + VP[12];
                const float y = VP[1] * p[0] + VP[5] * p[1] + VP[9] * p[2] + VP[13];
                const float w = VP[3] * p[0] + VP[7] * p[1] + VP[11] * p[2] + VP[15];
                if (w <= 0.1f)
                {
                    anchX[slot].store(-1.0e9f);
                    return;
                }
                anchX[slot].store((x / w * 0.5f + 0.5f) * (float) cw);
                anchY[slot].store((1.0f - (y / w * 0.5f + 0.5f)) * (float) ch);
            };

            static const float CHS[4][3] = { { 0.7f, 5.2f, 9.0f }, { 2.5f, 4.8f, 8.5f }, { 3.9f, 5.4f, 9.5f }, { 5.4f, 5.0f, 9.0f } };
            V3 chainPos[4];
            for (int i = 0; i < 4; ++i)
            {
                const float a = CHS[i][0], r = CHS[i][1], h = CHS[i][2];
                const V3 rim{ std::cos(a) * 3.15f, -0.45f, std::sin(a) * 3.15f };
                const V3 top{ std::cos(a + 0.15f) * r, h, std::sin(a + 0.15f) * r };
                chainPos[i] = mAp(isl, { rim[0] + (top[0] - rim[0]) * 0.28f, rim[1] + (top[1] - rim[1]) * 0.28f, rim[2] + (top[2] - rim[2]) * 0.28f });
            }
            int bi = 0;
            for (int i = 1; i < 4; ++i)
                if (chainPos[i][2] > chainPos[bi][2]) bi = i;
            if (chainPos[bi][2] > chainPos[chainSel][2] + 1.2f)
                chainSel = bi;

            proj(chainPos[chainSel], 0);                                   // WEIGHT : la chaine
            proj({ fp[0], fp[1] + 0.5f, fp[2] }, 1);                       // WARMTH : le feu
            proj(is0, 2);                                                  // SPACE : l'ilot
            proj(mAp(mCh({ isl, GM }), { 0.0f, 0.62f, -0.17f }), 3);      // PRESENCE : le torse du gobelin
            proj(rv0, 4);                                                  // AIR : le corbeau
            proj({ 12.0f, -9.5f + 2.0f, -46.0f }, 5);                      // ENHANCE : la statue au loin
            proj(is2, 6);                                                  // MIX : l'autre ilot
        }

        // on rend un etat propre a JUCE (qui dessine ensuite les arcs, le cadre et les boutons par-dessus)
        glBindVertexArray(0);
        glUseProgram(0);
        glBindTexture(GL_TEXTURE_2D, 0);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_BLEND);
        glDepthMask(GL_TRUE);
    }

    //--------------------------------------------------------------------------
    juce::OpenGLContext& ctx;
    juce::Component& owner;
    std::atomic<int>& hover;

    juce::RangedAudioParameter* pk[7] = {};
    juce::RangedAudioParameter* pByp = nullptr;
    juce::RangedAudioParameter* pDel = nullptr;
    float S[7] = {}, boost[7] = {};
    float sByp = 0.0f, sDel = 0.0f;

    Prog pLit, pSky, pSpr, pPt, pFog, pPost, pWtr;
    std::vector<Mesh> meshes;
    Mesh water[2];
    float waterLen[2] = { 1.0f, 1.0f };
    std::vector<GLuint> waterExtraVbo, extraVbo;
    GLuint vaoFull = 0, vaoQuad = 0, vaoPts = 0, bPP = 0, bPC = 0;
    GLuint fbo = 0, tex = 0, rbo = 0;
    int fw = 0, fh = 0;

    int iFar = -1, iIsland = -1, iGrass = -1, iRoots = -1, iKnight = -1, iFire = -1, iDeco = -1, iStatue = -1,
        iRB = -1, iRW = -1, iPetal = -1, iCore = -1, iInner = -1, iGob = -1, iPurse = -1;
    Mat GM = mId();
    V3 GHAND{ 0.28f, 0.64f, 0.09f };
    std::vector<Islet> islets;
    std::vector<Raven> ravens;
    std::vector<Rock> rocks;
    std::vector<float> wPh;
    V3 KPOS{ 0, 0, 0 }, FIRE{ 0, 0, 0 };
    float KROT = 0.0f, WPHI = 0.0f, WR = 3.38f, WFT = 1.56f;

    static constexpr int NE = 200, NW = 70, NS = 160, NO = 26;
    std::vector<float> ePos, eVel, eLife, ptPos, ptCol;
    Mat ISL = mId();
    int chainSel = 0;
    std::atomic<float> anchX[7], anchY[7];

    std::atomic<float> statFps{ 0.0f }, statWorst{ 0.0f };
    int statFrames = 0;
    double statWorstGap = 0.0, statStart = 0.0;

    juce::Random rnd;
    double lastTime = 0.0;
    float T = 0.0f;
    float fogD = 0.06f;
    bool failed = false, loaded = false, firstFrame = true, firstCall = true;
    int initStage = 0;
    double revealT0 = 0.0, createT0 = 0.0;
    std::atomic<int> worldFrames { 0 };
    bool usePosterHandover = false;
    double lastPosterGrab = 0.0;
    std::vector<uint8_t> posterPixels;
};

//==============================================================================
BloomScene::BloomScene(juce::AudioProcessorValueTreeState& apvts, juce::OpenGLContext& context,
                       juce::Component& owner, std::atomic<int>& hoverIndex)
    : impl(new Impl(apvts, context, owner, hoverIndex))
{
    startBackgroundDecode();
}

BloomScene::~BloomScene() = default;

void BloomScene::newOpenGLContextCreated()
{
    juce::gl::loadFunctions();
    impl->createT0 = juce::Time::getMillisecondCounterHiRes();
    impl->beginCreate();
}

void BloomScene::renderOpenGL()
{
    BLOOM_PERF_SCOPE("scene : rendu d'une image", 30.0);
    if (impl->firstCall)
    {
        impl->firstCall = false;
        bloomperf::mark("scene : premier appel du rendu");
    }
    impl->render();
}

bool BloomScene::getAnchor(int index, float& x, float& y) const
{
    if (index < 0 || index >= 7)
        return false;
    const float ax = impl->anchX[index].load();
    if (ax < -1.0e8f)
        return false;
    x = ax;
    y = impl->anchY[index].load();
    return true;
}

bool BloomScene::hasPresentedWorld() const
{
    return impl->worldFrames.load() >= 3;
}

void BloomScene::getStats(float& fps, float& worstMs) const
{
    fps = impl->statFps.load();
    worstMs = impl->statWorst.load();
}

void BloomScene::openGLContextClosing()
{
    impl->destroy();
}


// ============================================================================
//  Reveil du pilote graphique (Windows) : on cree une petite fenetre invisible
//  avec un contexte OpenGL dans un fil a part, puis on detruit tout. Le pilote
//  reste charge en memoire : la vraie fenetre du plugin s'ouvre ensuite tout de suite.
// ============================================================================
#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #include <windows.h>
 #pragma comment(lib, "opengl32.lib")
 #pragma comment(lib, "gdi32.lib")
 #pragma comment(lib, "user32.lib")
#endif

namespace
{
    struct GraphicsWarmup
    {
        std::atomic<int> state { 0 };            // 0 pas lance, 1 en cours, 2 fini
        juce::WaitableEvent done { true };
    };

    GraphicsWarmup& graphicsWarmup()
    {
        static auto* w = new GraphicsWarmup();   // jamais detruit volontairement
        return *w;
    }

   #if JUCE_WINDOWS
    void warmupBody()
    {
        const double t0 = juce::Time::getMillisecondCounterHiRes();
        HINSTANCE inst = GetModuleHandleW(nullptr);
        const wchar_t* cls = L"BloomGLWarmupWindow";

        WNDCLASSW wc {};
        wc.style = CS_OWNDC;
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = inst;
        wc.lpszClassName = cls;
        const bool registered = RegisterClassW(&wc) != 0;

        if (HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, cls, L"", WS_POPUP, -100, -100, 4, 4,
                                        nullptr, nullptr, inst, nullptr))
        {
            if (HDC dc = GetDC(hwnd))
            {
                PIXELFORMATDESCRIPTOR pfd {};
                pfd.nSize = sizeof(pfd);
                pfd.nVersion = 1;
                pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
                pfd.iPixelType = PFD_TYPE_RGBA;
                pfd.cColorBits = 32;
                pfd.cAlphaBits = 8;
                pfd.cDepthBits = 24;
                pfd.iLayerType = PFD_MAIN_PLANE;

                const int pf = ChoosePixelFormat(dc, &pfd);
                if (pf != 0 && SetPixelFormat(dc, pf, &pfd))
                {
                    if (HGLRC rc = wglCreateContext(dc))
                    {
                        if (wglMakeCurrent(dc, rc))
                            wglMakeCurrent(nullptr, nullptr);
                        wglDeleteContext(rc);
                    }
                }
                ReleaseDC(hwnd, dc);
            }
            DestroyWindow(hwnd);
        }

        if (registered)
            UnregisterClassW(cls, inst);

        bloomperf::mark("prechauffage graphique termine en "
                        + juce::String(juce::Time::getMillisecondCounterHiRes() - t0, 0) + " ms");
    }
   #endif
}

void bloomPrewarmGraphics()
{
    startBackgroundDecode();   // prepare aussi le decor 3D a l'avance
   #if JUCE_WINDOWS
    auto& w = graphicsWarmup();
    int expected = 0;
    if (!w.state.compare_exchange_strong(expected, 1))
        return;   // deja fait (ou en cours) pour cette session

    juce::Thread::launch([]
    {
        loadPosterFromDisk();
        warmupBody();
        auto& ww = graphicsWarmup();
        ww.state.store(2);
        ww.done.signal();
    });
   #endif
}

void bloomWaitGraphicsWarmup()
{
    auto& w = graphicsWarmup();
    if (w.state.load() == 1)
        w.done.wait(15000);

    auto& d = decodedScene();
    for (int i = 0; i < 500 && d.started.load() && !d.ready.load(); ++i)
        juce::Thread::sleep(10);
}
