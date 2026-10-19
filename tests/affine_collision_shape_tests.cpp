// MAINTENANCE: regression-keep; affine support must preserve Bullet 2.75 margins.
#include "mikumikustudio/affine_collision_shape.hpp"
#include <btBulletDynamicsCommon.h>
#include <cmath>
#include <cstdio>
using namespace mikumikustudio;
int main() {
    int failures = 0;
    auto check = [&](bool pass, const char *text) {
        if (!pass) {
            ++failures;
            std::printf("FAIL %s\n", text);
        }
    };
    btSphereShape sphere(2);
    btBoxShape box(btVector3(1, 2, 3));
    btCapsuleShape capsule(1.5f, 4);
    const btMatrix3x3 transforms[] = {btMatrix3x3::getIdentity(), btMatrix3x3(2, 0, 0, 0, 2, 0, 0, 0, 2),
                                      btMatrix3x3(2, 0, 0, 0, .5f, 0, 0, 0, 1.2f),
                                      btMatrix3x3(1, .3f, .2f, 0, 2, .1f, 0, 0, .7f)};
    for (int kind = 0; kind < 3; ++kind) {
        const btConvexShape &base = kind == 0   ? static_cast<const btConvexShape &>(sphere)
                                    : kind == 1 ? static_cast<const btConvexShape &>(box)
                                                : static_cast<const btConvexShape &>(capsule);
        const auto margin = base.getMargin();
        const auto scaling = base.getLocalScaling();
        AffineConvexShape shape(base);
        for (const auto &a : transforms) {
            shape.SetAffine(a);
            btVector3 lo, hi, inertia;
            const btTransform world(btQuaternion(btVector3(1, 2, 3).normalized(), .6f),
                                    btVector3(2, -3, 5));
            shape.getAabb(world, lo, hi);
            shape.calculateLocalInertia(4, inertia);
            for (int i = 0; i < 3; ++i)
                check(std::isfinite(inertia[i]) && inertia[i] > 0, "positive finite inertia");
            for (int i = 1; i <= 180; ++i) {
                const btVector3 d =
                    btVector3(std::sin(i * 1.2f), std::cos(i * .7f), std::sin(i * .43f)).normalized();
                const btVector3 v = a.transpose() * d;
                btVector3 expected;
                if (kind == 0)
                    expected = v.normalized() * 2;
                else if (kind == 1)
                    expected = btVector3(v.x() >= 0 ? 1 : -1, v.y() >= 0 ? 2 : -2, v.z() >= 0 ? 3 : -3);
                else {
                    expected = v.normalized() * 1.5f;
                    expected[1] += v.y() >= 0 ? 2 : -2;
                }
                expected = a * expected;
                const auto actual = shape.localGetSupportingVertexWithoutMargin(d);
                check((actual - expected).length() < 2e-4f, "analytic affine support");
                btVector3 batched;
                shape.batchedUnitVectorGetSupportingVertexWithoutMargin(&d, &batched, 1);
                check((batched - actual).length() < 1e-6f, "batch support agrees");
                const auto point = world * actual;
                for (int k = 0; k < 3; ++k)
                    check(point[k] >= lo[k] - 1e-4f && point[k] <= hi[k] + 1e-4f,
                          "AABB contains transformed boundary");
            }
        }
        shape.SetAffine(btMatrix3x3::getIdentity());
        for (int i = 0; i < 3; ++i) {
            btVector3 d(0, 0, 0);
            d[i] = 1;
            check((shape.localGetSupportingVertex(d) - base.localGetSupportingVertex(d)).length() <
                      1e-6f,
                  "identity restoration");
        }
        check(base.getMargin() == margin && base.getLocalScaling() == scaling, "source shape unchanged");
        check(shape.getMargin() == 0, "base margin not counted twice");
    }
    std::printf("affine collision: %d failures\n", failures);
    return failures ? 1 : 0;
}
