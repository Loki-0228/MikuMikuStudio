#include "mikumikustudio/transparent_material_order.hpp"
#include <cstdio>
#include <limits>
#include <random>

using namespace mikumikustudio;
namespace {
int failures = 0;
AlphaMaterialGeometry Quad(float x, float z, bool reverse = false) {
    AlphaMaterialGeometry g{{{{{x,0,z},{x+1,0,z},{x,1,z}}},
                             {{{x+1,0,z},{x+1,1,z},{x,1,z}}}}};
    if (reverse) for (auto& t : g.triangles) std::swap(t.p[0], t.p[1]);
    return g;
}
void Append(AlphaMaterialGeometry& a, const AlphaMaterialGeometry& b) {
    a.triangles.insert(a.triangles.end(), b.triangles.begin(), b.triangles.end());
}
void Expect(const char* name, const std::vector<AlphaMaterialGeometry>& geometry,
            std::vector<std::size_t> input, const std::vector<std::size_t>& expected,
            AlphaOrderResult expectedResult, std::size_t budget = 262144) {
    const auto result = OrderAlphaMaterials(geometry, input, budget);
    if (input != expected || result != expectedResult) {
        std::fprintf(stderr, "FAIL %s: result=%d order=", name, static_cast<int>(result));
        for (auto i : input) std::fprintf(stderr, "%zu,", i);
        std::fputc('\n', stderr); ++failures;
    }
}
}
int main() {
    using R = AlphaOrderResult;
    for (bool reversed : {false, true}) {
        auto near = Quad(0, .2f, reversed), far = Quad(0, .8f, !reversed);
        Expect("far before near", {near, far}, {0,1}, {1,0}, R::Reordered);
        Expect("correct order stays", {near, far}, {1,0}, {1,0}, R::Unchanged);
        Expect("no overlap", {near, Quad(2,.8f)}, {0,1}, {0,1}, R::Unchanged);
        Expect("budget preserves input", {near,far}, {0,1}, {0,1}, R::Unsupported, 0);
    }
    AlphaMaterialGeometry plane{{{{{0,0,0},{1,0,.8f},{0,1,.8f}}}}};
    AlphaMaterialGeometry inset{{{{{.55f,.15f,.4f},{.8f,.15f,.4f},{.55f,.35f,.4f}}}}};
    Expect("overlapping depth ranges with ordered intersection", {inset,plane},
           {0,1}, {1,0}, R::Reordered);
    AlphaMaterialGeometry cornerA{{{{{0,0,.2f},{2,0,.2f},{0,2,.2f}}}}};
    AlphaMaterialGeometry cornerB{{{{{2,2,.8f},{2,.5f,.8f},{.5f,2,.8f}}}}};
    Expect("bbox overlap without triangle overlap", {cornerA,cornerB},
           {0,1}, {0,1}, R::Unchanged);
    AlphaMaterialGeometry crossing{{{{{0,0,.2f},{1,0,.8f},{0,1,.2f}}}}};
    AlphaMaterialGeometry flat{{{{{0,0,.5f},{1,0,.5f},{0,1,.5f}}}}};
    Expect("crossing planes", {crossing,flat}, {0,1}, {0,1}, R::Unsupported);
    auto mixed = Quad(0,.2f); Append(mixed,Quad(2,.8f));
    auto middle = Quad(0,.5f); Append(middle,Quad(2,.5f));
    Expect("contradictory regions", {mixed,middle}, {0,1}, {0,1}, R::Unsupported);
    auto both = Quad(0,.5f); Append(both,Quad(2,.5f));
    Expect("coplanar author priority survives dependencies", {both,Quad(0,.5f),Quad(2,.8f)},
           {0,1,2}, {2,0,1}, R::Reordered);
    auto a = Quad(0,.8f); Append(a,Quad(4,.2f));
    auto b = Quad(0,.2f); Append(b,Quad(2,.8f));
    auto c = Quad(2,.2f); Append(c,Quad(4,.8f));
    Expect("three-material cycle", {a,b,c}, {0,1,2}, {0,1,2}, R::Unsupported);
    auto invalid = Quad(0,.2f);
    invalid.triangles[0].p[0].z = std::numeric_limits<float>::quiet_NaN();
    Expect("NaN", {invalid,Quad(0,.8f)}, {0,1}, {0,1}, R::Unsupported);
    invalid.triangles[0].p[0].z = -1;
    Expect("clipped depth", {invalid,Quad(0,.8f)}, {0,1}, {0,1}, R::Unsupported);
    invalid = Quad(0,.2f); invalid.triangles[0].p[0] = invalid.triangles[0].p[1];
    Expect("degenerate", {invalid,Quad(0,.8f)}, {0,1}, {0,1}, R::Unsupported);
    Expect("duplicate permutation", {a,b}, {1,1}, {1,1}, R::Unsupported);
    Expect("invalid singleton", {a}, {1}, {1}, R::Unsupported);
    Expect("empty is not opaque", {{},Quad(0,.2f),Quad(0,.8f)}, {0,1,2}, {0,2,1}, R::Reordered);
    std::mt19937 random(932);
    for (int trial = 0; trial < 100; ++trial) {
        const std::size_t n = 2 + random() % 7;
        std::vector<AlphaMaterialGeometry> g;
        std::vector<std::size_t> input(n), expected(n);
        std::iota(input.begin(),input.end(),0);
        std::shuffle(input.begin(),input.end(),random);
        for (std::size_t i = 0; i < n; ++i) {
            g.push_back(Quad(0, float(i+1)/float(n+1), random()%2 != 0));
            expected[i] = n-1-i;
        }
        Expect("random ordered planes", g, input, expected,
               input == expected ? R::Unchanged : R::Reordered);
    }
    // Disjoint surfaces may merge into two material runs even when their depths alternate.
    std::vector<AlphaMaterialGeometry> disjoint(2);
    for (int i=0;i<12;++i) Append(disjoint[i%2],Quad(float(i*2),float(i+1)/14));
    std::vector<AlphaTriangleReference> batches;
    if (!BatchAlphaTriangles(disjoint,batches)) ++failures;
    int runs=0; std::size_t last=2;
    for (auto ref:batches) { if(ref.material!=last) ++runs; last=ref.material; }
    if(runs!=2 || batches.size()!=24) ++failures;
    const auto saved=batches;
    if(BatchAlphaTriangles(disjoint,batches,0) || batches.size()!=saved.size()) ++failures;
    // Independent sample compositor: batching must match centroid order wherever
    // triangles overlap, including geometrically interleaved materials.
    for(int trial=0;trial<30;++trial) {
        std::vector<AlphaMaterialGeometry> g(4);
        for(int i=0;i<16;++i) {
            auto q=Quad(float(random()%80)/40,float(i+1)/18,random()%2!=0);
            for(auto& t:q.triangles) for(auto& p:t.p) p.y+=float(i%3)*.23f;
            Append(g[i%4],q);
        }
        std::vector<AlphaTriangleReference> reference,batched;
        for(std::size_t m=0;m<g.size();++m)
            for(std::size_t t=0;t<g[m].triangles.size();++t) reference.push_back({m,t});
        std::stable_sort(reference.begin(),reference.end(),[&](auto a,auto b) {
            return g[a.material].triangles[a.triangle].p[0].z > g[b.material].triangles[b.triangle].p[0].z;
        });
        if(!BatchAlphaTriangles(g,batched) || batched.size()!=reference.size()) {++failures;continue;}
        const auto sample=[&](const auto& refs,double x,double y) {
            double colour=1;
            for(auto ref:refs) {
                const auto& p=g[ref.material].triangles[ref.triangle].p;
                const auto edge=[&](int a,int b) {return (double(p[b].x)-p[a].x)*(y-p[a].y)-(double(p[b].y)-p[a].y)*(x-p[a].x);};
                const double e0=edge(0,1),e1=edge(1,2),e2=edge(2,0);
                if((e0>1e-9&&e1>1e-9&&e2>1e-9)||(e0< -1e-9&&e1< -1e-9&&e2< -1e-9))
                    colour=.4*(ref.material+1)/5+.6*colour;
            }
            return colour;
        };
        for(int y=0;y<15;++y) for(int x=0;x<30;++x)
            if(std::abs(sample(reference,(x+.37)/10,(y+.41)/10)-sample(batched,(x+.37)/10,(y+.41)/10))>1e-10) ++failures;
    }
    // Double-winding broad phase: fixed-seed ordinary, thin and sub-pixel
    // triangles in three materials, one colour per triangle, compared sample by
    // sample against the stable centroid order, plus nested, same-material
    // sandwiched and point-touching cases pinned down separately.
    {
        auto shade=[&](const std::vector<AlphaMaterialGeometry>& g,
                       const std::vector<AlphaTriangleReference>& o,double x,double y) {
            double c=0;
            for(auto r:o) {
                const auto&p=g[r.material].triangles[r.triangle].p;
                const double d=(double(p[1].x)-p[0].x)*(y-p[0].y)-(double(p[1].y)-p[0].y)*(x-p[0].x);
                const double e=(double(p[2].x)-p[1].x)*(y-p[1].y)-(double(p[2].y)-p[1].y)*(x-p[1].x);
                const double f=(double(p[0].x)-p[2].x)*(y-p[2].y)-(double(p[0].y)-p[2].y)*(x-p[2].x);
                if((d>0&&e>0&&f>0)||(d<0&&e<0&&f<0)) {
                    const double colour=.37*double(r.triangle+1)+.11*double(r.material+1);
                    c=.4*colour+.6*c;   // blend, so a dropped back layer discolours
                }
            }
            return c;
        };
        auto check=[&](const std::vector<AlphaMaterialGeometry>& g,const char* what,
                       AlphaBatchOverlapCache* cache=nullptr) {
            std::vector<AlphaTriangleReference> all,b;
            for(std::size_t m=0;m<g.size();++m)
                for(std::size_t t=0;t<g[m].triangles.size();++t) all.push_back({m,t});
            std::vector<AlphaTriangleReference> ref=all;
            const auto zsum=[&](AlphaTriangleReference r) {
                const auto&p=g[r.material].triangles[r.triangle].p;
                return double(p[0].z)+double(p[1].z)+double(p[2].z);};
            std::stable_sort(ref.begin(),ref.end(),[&](auto a,auto c) {
                return zsum(a)>zsum(c);});
            if(!BatchAlphaTriangles(g,b,262144,cache)||b.size()!=all.size()) {++failures;
                std::fprintf(stderr,"FAIL %s: batch failed or lost a triangle\n",what); return;}
            double x0=1e30,x1=-1e30,y0=1e30,y1=-1e30;
            for(auto r:all) for(int v=0;v<3;++v) {
                const auto&p=g[r.material].triangles[r.triangle].p[v];
                x0=std::min<double>(x0,p.x); x1=std::max<double>(x1,p.x);
                y0=std::min<double>(y0,p.y); y1=std::max<double>(y1,p.y);}
            const double pad=.05*std::max(x1-x0,y1-y0)+1e-9;
            x0-=pad; x1+=pad; y0-=pad; y1+=pad; const double w=x1-x0,h=y1-y0;
            for(int sy=0;sy<32;++sy) for(int sx=0;sx<32;++sx) {
                const double x=x0+w*(sx+.37)/32, y=y0+h*(sy+.41)/32;
                if(std::abs(shade(g,ref,x,y)-shade(g,b,x,y))>1e-10) {++failures;
                    std::fprintf(stderr,"FAIL %s: sample %d,%d differs\n",what,sx,sy); return;}
            }
            // Barycentric samples strictly inside every triangle, so a tiny or
            // thin triangle is still hit even when the grid above misses it.
            for(auto r:all) {
                const auto&p=g[r.material].triangles[r.triangle].p;
                const double bx[4]={.6,.2,.2,.34},by[4]={.2,.6,.2,.34};
                for(int k=0;k<4;++k) {
                    const double u=bx[k],v=by[k],x=u*p[0].x+v*p[1].x+(1-u-v)*p[2].x;
                    const double y=u*p[0].y+v*p[1].y+(1-u-v)*p[2].y;
                    if(std::abs(shade(g,ref,x,y)-shade(g,b,x,y))>1e-10) {++failures;
                        std::fprintf(stderr,"FAIL %s: interior sample %d differs\n",what,k); return;}
                }
            }
        };
        std::mt19937 shape(20260924u);
        for(int pass=0;pass<3;++pass) {
            const float scale=pass==0?1.f:pass==1?.02f:1e-3f;
            const float thin=pass==1?.04f:1.f;
            std::vector<AlphaMaterialGeometry> g(3);
            for(int k=0;k<7;++k) {
                const float cx=(float(shape()%200)/100.f-1.f)*scale;
                const float cy=(float(shape()%200)/100.f-1.f)*scale;
                const float w=(.3f+float(shape()%70)/100.f)*scale*thin;
                const float h=(.3f+float(shape()%70)/100.f)*scale;
                const float z=.05f+.9f*(float(shape()%128)+.5f)/128.f;
                AlphaScreenTriangle t{{{cx-w,cy-h,z},{cx+w,cy-h,z},{cx,cy+h,z}}};
                if(shape()%2) std::swap(t.p[1],t.p[2]);
                t.p[2].x+=1e-6f*scale; t.p[2].y+=1e-6f*scale;
                g[k%3].triangles.push_back(t);
            }
            check(g,pass==0?"seeded ordinary":pass==1?"seeded thin":"seeded sub-pixel");
        }
        // Nested pair, every triangle reversed, both submission orders. The
        // inner outline is strictly inside the outer one, so only the depth can
        // order them, and the reverse winding must not change the answer.
        for(int order=0;order<2;++order) {
            std::vector<AlphaMaterialGeometry> nest(2);
            AlphaScreenTriangle outer{{{-1,-1,.80f},{1,-1,.80f},{0,1,.80f}}};
            AlphaScreenTriangle inner{{{-.4f,-.4f,.20f},{.4f,-.4f,.20f},{0,.4f,.20f}}};
            outer.p[2].x+=1e-6f; inner.p[2].x+=1e-6f;
            std::swap(outer.p[1],outer.p[2]); std::swap(inner.p[1],inner.p[2]);
            nest[order].triangles.push_back(outer);
            nest[1-order].triangles.push_back(inner);
            check(nest,"nested reversed winding");
        }
        // Same-material overlap: material 0 holds two triangles on one shared
        // projection at different depths, and material 1 holds a triangle on a
        // strictly inner footprint pinched between them, so the outer pair
        // cannot both precede it and only one order is correct.
        {
            std::vector<AlphaMaterialGeometry> inter(2);
            AlphaScreenTriangle a{{{0,0,.70f},{1.2f,0,.70f},{0,1.2f,.70f}}};  // m0 far
            AlphaScreenTriangle c{{{0,0,.40f},{1.f,0,.40f},{0,1.f,.40f}}};    // m1 middle
            AlphaScreenTriangle e{{{0,0,.10f},{.6f,0,.10f},{0,.6f,.10f}}};    // m0 near
            a.p[2].x+=1e-6f; c.p[2].x+=1e-6f; e.p[2].x+=1e-6f;
            std::swap(a.p[1],a.p[2]); std::swap(c.p[1],c.p[2]); std::swap(e.p[1],e.p[2]);
            inter[0].triangles.push_back(a); inter[0].triangles.push_back(e);
            inter[1].triangles.push_back(c);
            check(inter,"same-material sandwiched overlap");
        }
        // Touching pair: two coplanar triangles of one material meeting at the
        // single point (1, 0), so the broad phase reports point contact.
        std::vector<AlphaMaterialGeometry> touch(2);
        for(int side=0;side<2;++side) {
            AlphaScreenTriangle t{{{-1.f+2.f*side,0,.40f},{1.f+2.f*side,0,.40f},{2.f*side,1,.40f}}};
            t.p[2].x+=1e-6f; touch[0].triangles.push_back(t);
        }
        check(touch,"touching at one point");
        // Reused overlap cache across consecutive frames. Every check below runs
        // through check(g,label,&cache), so the order is compared sample by
        // sample against the stable centroid order, and the cache is exercised
        // as the renderer uses it: shared across frames, never inspected here.
        {
            std::mt19937 move(771444u);
            // The seed is reset on every call, so the layout is identical from
            // one frame to the next and only dx and dz move it. Each step stays
            // small, and a running total that leaves the anchor padding forces
            // the helper to rebuild its pairs instead of reusing them.
            const auto build=[&](float dx,float dz) {
                move.seed(771444u);
                std::vector<AlphaMaterialGeometry> g(2);
                const auto emit=[&](float x,float z) {
                    // Tapered triangle: the narrow tip makes the boundary sweep
                    // sideways as dx moves it.
                    AlphaScreenTriangle t{{{x-.7f,0,z},{x+.7f,0,z},{x,.8f,z+.04f}}};
                    t.p[2].x+=1e-6f;  // keep the cross product away from zero
                    return t;
                };
                for(int f=0;f<8;++f) {   // eight triangles per material per frame
                    const float x=float(move()%200)/1000.f-.1f;
                    g[0].triangles.push_back(emit(x,.25f));
                    g[1].triangles.push_back(emit(x+dx,.25f+dz));
                }
                return g;
            };
            AlphaBatchOverlapCache cache;
            // Small steps: every frame stays inside the anchor padding, so the
            // helper reuses the cached pairs instead of rebuilding them.
            for(int f=0;f<8;++f) {
                const float dx=.0002f*float(f);
                check(build(dx,.02f*float(f)),"cached small translation",&cache);
            }
            // Depth flip: the two triangles swap which one is nearer while their
            // projections stay put, so the reused pairs must not pin a stale
            // order. Anchor check keeps the hit path.
            for(int f=0;f<8;++f) {
                const float dz=.12f-.04f*float(f);
                check(build(0.f,dz),"cached depth flip",&cache);
            }
            // Two triangles of one material share a projection, far at z=.8 and
            // near at z=.2, while a second material's middle layer at z=.5
            // slides in between them. With only one triangle per material a
            // missing edge would still fall back to centroid order and stay
            // invisible, but the outer pair cannot both precede the middle one,
            // so a stale pair set from an early frame shows up as a wrong order
            // and a wrong colour. check runs on every frame, so the drift has to
            // stay absent at every step. The middle layer starts clear of the
            // outer projection and closes in by 0.001 a frame, small enough to
            // stay inside the anchor padding, until 320 frames bring the two
            // overlapping by an area the sampler can actually see.
            for(int f=0;f<320;++f) {
                std::vector<AlphaMaterialGeometry> g(2);
                AlphaScreenTriangle far{{{0,0,.8f},{1.2f,0,.8f},{0,1.2f,.8f}}};
                AlphaScreenTriangle near{{{0,0,.2f},{1.2f,0,.2f},{0,1.2f,.2f}}};
                far.p[2].x+=1e-6f; near.p[2].x+=1e-6f;
                const float gap=1.21f-.001f*float(f);
                AlphaScreenTriangle mid{{{gap,0,.5f},{gap+1.2f,0,.5f},{gap+.6f,.8f,.5f}}};
                mid.p[2].x+=1e-6f;
                g[0].triangles.push_back(far); g[0].triangles.push_back(near);
                g[1].triangles.push_back(mid);
                check(g,"separated triangles closing in",&cache);
            }
            // Triangle counts change between frames: reuse must be refused and
            // the rebuilt pairs must still produce the reference order.
            for(int step=0;step<6;++step) {
                std::vector<AlphaMaterialGeometry> g(2);
                const int count=step%2?1:3;
                for(int k=0;k<count;++k) {
                    AlphaScreenTriangle a{{{-.6f+float(k),.0f,.2f+float(k)*.1f},
                                           {.4f+float(k),.0f,.2f+float(k)*.1f},
                                           {-.1f+float(k),.7f,.24f+float(k)*.1f}}};
                    a.p[2].x+=1e-6f; g[0].triangles.push_back(a);
                }
                AlphaScreenTriangle b{{{-.3f,0,.5f},{.9f,0,.5f},{.3f,.7f,.54f}}};
                b.p[2].x+=1e-6f; g[1].triangles.push_back(b);
                check(g,"triangle count changes",&cache);
            }
            // A zero budget must fail on the reused-pair path exactly as it does
            // on the rebuilding path: false, and the caller's output intact.
            std::vector<AlphaMaterialGeometry> warm(2);
            AlphaScreenTriangle wa{{{-1,0,.5f},{1,0,.5f},{0,1,.54f}}};
            AlphaScreenTriangle wb{{{0,0,.2f},{1,0,.2f},{.5f,1,.24f}}};
            wa.p[2].x+=1e-6f; wb.p[2].x+=1e-6f;
            warm[0].triangles.push_back(wa); warm[1].triangles.push_back(wb);
            std::vector<AlphaTriangleReference> out;
            if(!BatchAlphaTriangles(warm,out,262144,&cache)) {++failures;
                std::fprintf(stderr,"FAIL cached warm-up rejected valid geometry\n");}
            else {
                out={{7,9},{8,10}};
                const AlphaTriangleReference sentinel[2]={{7,9},{8,10}};
                if(BatchAlphaTriangles(warm,out,0,&cache)) {++failures;
                    std::fprintf(stderr,"FAIL cached zero budget reported success\n");}
                else if(out.size()!=2||out[0].material!=sentinel[0].material||
                        out[0].triangle!=sentinel[0].triangle||
                        out[1].material!=sentinel[1].material||
                        out[1].triangle!=sentinel[1].triangle) {++failures;
                    std::fprintf(stderr,"FAIL cached zero budget clobbered output\n");}
            }
        }
    }
    if (failures) {std::fprintf(stderr,"%d failures\n",failures);return 1;}
    std::puts("Transparent material geometry regressions passed.");
    return 0;
}
