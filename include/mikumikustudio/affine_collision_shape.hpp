#pragma once
#include <BulletCollision/CollisionShapes/btConvexShape.h>

namespace mikumikustudio {
// Non-owning affine image of a convex shape. Bullet's rigid body transform
// remains orthonormal; the complete deformation belongs to this support map.
class AffineConvexShape final : public btConvexShape {
    const btConvexShape &base_;
    btMatrix3x3 affine_ = btMatrix3x3::getIdentity();
    btMatrix3x3 effective_ = btMatrix3x3::getIdentity();
    btVector3 scaling_{1, 1, 1};
    void Update() {
        effective_ = affine_;
        for (int row = 0; row < 3; ++row)
            effective_[row] *= scaling_[row];
    }

  public:
    explicit AffineConvexShape(const btConvexShape &base) : base_(base) {
        m_shapeType = CUSTOM_CONVEX_SHAPE_TYPE;
    }
    void SetAffine(const btMatrix3x3 &affine) {
        affine_ = affine;
        Update();
    }
    const btConvexShape &BaseShape() const { return base_; }
    btVector3 localGetSupportingVertex(const btVector3 &direction) const override {
        return effective_ * base_.localGetSupportingVertex(effective_.transpose() * direction);
    }
    btVector3 localGetSupportingVertexWithoutMargin(const btVector3 &direction) const override {
        // Include the base margin here exactly once. In 2.75 the sphere's
        // entire radius is its margin; excluding it would erase the sphere.
        return localGetSupportingVertex(direction);
    }
    void batchedUnitVectorGetSupportingVertexWithoutMargin(const btVector3 *directions,
                                                           btVector3 *vertices,
                                                           int count) const override {
        for (int i = 0; i < count; ++i)
            vertices[i] = localGetSupportingVertex(directions[i]);
    }
    void getAabb(const btTransform &transform, btVector3 &minimum, btVector3 &maximum) const override {
        for (int axis = 0; axis < 3; ++axis) {
            const btVector3 direction = transform.getBasis().getRow(axis);
            maximum[axis] = (transform * localGetSupportingVertex(direction))[axis];
            minimum[axis] = (transform * localGetSupportingVertex(-direction))[axis];
        }
    }
    void getAabbSlow(const btTransform &t, btVector3 &lo, btVector3 &hi) const override {
        getAabb(t, lo, hi);
    }
    void calculateLocalInertia(btScalar mass, btVector3 &inertia) const override {
        // Diagonal inertia of the enclosing box. The collision support is
        // exact; this deliberately approximates mass distribution under shear.
        btVector3 lo, hi;
        getAabb(btTransform::getIdentity(), lo, hi);
        const btVector3 size = hi - lo;
        for (int i = 0; i < 3; ++i)
            inertia[i] = mass / 12 *
                         (size[(i + 1) % 3] * size[(i + 1) % 3] + size[(i + 2) % 3] * size[(i + 2) % 3]);
    }
    void setLocalScaling(const btVector3 &value) override {
        scaling_ = value;
        Update();
    }
    const btVector3 &getLocalScaling() const override { return scaling_; }
    void setMargin(btScalar) override {}
    btScalar getMargin() const override { return 0; }
    int getNumPreferredPenetrationDirections() const override { return 0; }
    void getPreferredPenetrationDirection(int, btVector3 &out) const override { out.setValue(0, 0, 0); }
    const char *getName() const override { return "AffineConvex"; }
};
} // namespace mikumikustudio
