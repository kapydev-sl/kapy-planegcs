// services/planegcs/build/capi/gcs_capi_constraints.cpp
//
// `gcs_constraint`: one export that adds any constraint in
// constraint_kinds.def. It checks the kind, the geometry handles (and that
// each is the kind its slot wants) and the parameter indices, then calls the
// same `addConstraint*` planegcs method embind's `add_constraint_*` calls,
// and rescales the constraint when `scale` is not 1, exactly as the embind
// template does.
//
// Who links it: the `planegcs-capi` target. What does NOT belong here: the
// other exports (gcs_capi.cpp) or the kind table itself (the .def).

#include <cmath>

#include "gcs_capi.h"

using namespace capi;

namespace {

#define GCS_KIND(id, name, sig, g0, g1, g2, params) {id, {g0, g1, g2}, params},
const KindSpec KINDS[] = {
#include "constraint_kinds.def"
};
#undef GCS_KIND

bool holds(const Geom* g, Slot want) {
    switch (want) {
        case NO:
            return g == nullptr;
        case PT:
            return g->kind == GK_POINT;
        case LN:
            return g->kind == GK_LINE;
        case CI:
            // A `Circle&` parameter takes an arc too: Arc derives from Circle.
            return g->kind == GK_CIRCLE || g->kind == GK_ARC;
        case AR:
            return g->kind == GK_ARC;
        case EL:
            return g->kind == GK_ELLIPSE;
        case CV:
            return g->kind != GK_POINT;
    }
    return false;
}

// The typed views the `addConstraint*` overloads need. Circle and Arc are
// different overloads in planegcs, so a slot is cast to exactly its kind.
GCS::Point& P(Geom* g) { return g->point; }
GCS::Line& L(Geom* g) { return static_cast<GCS::Line&>(*g->curve); }
GCS::Circle& C(Geom* g) { return static_cast<GCS::Circle&>(*g->curve); }
GCS::Arc& A(Geom* g) { return static_cast<GCS::Arc&>(*g->curve); }
GCS::Ellipse& E(Geom* g) { return static_cast<GCS::Ellipse&>(*g->curve); }
GCS::Curve& V(Geom* g) { return *g->curve; }

}  // namespace

const KindSpec* capi::kindSpec(int kind) {
    for (const KindSpec& k : KINDS) {
        if (k.id == kind) {
            return &k;
        }
    }
    return nullptr;
}

// Returns the constraint's index in planegcs, or -1 with the reason in
// `gcs_last_error`. `flag` is a bool or an enum for the kinds that take one
// (see the .def); for `difference` it is the third parameter index.
extern "C" int gcs_constraint(Sys* s, int kind, int tag, int driving, double scale, int g0, int g1,
                              int g2, int p0, int p1, int flag) noexcept {
    if (s == nullptr) {
        return -1;
    }
    s->lastError = ERR_NONE;
    const KindSpec* spec = kindSpec(kind);
    if (spec == nullptr) {
        return s->fail(ERR_BAD_KIND);
    }
    const int handles[3] = {g0, g1, g2};
    Geom* g[3] = {nullptr, nullptr, nullptr};
    for (int k = 0; k < 3; ++k) {
        if (spec->g[k] == NO) {
            continue;
        }
        g[k] = s->geom(handles[k]);
        if (g[k] == nullptr) {
            return s->fail(ERR_BAD_GEOM);
        }
        if (!holds(g[k], spec->g[k])) {
            return s->fail(ERR_BAD_SLOT);
        }
    }
    const int indices[2] = {p0, p1};
    for (int k = 0; k < spec->params; ++k) {
        if (!s->paramOk(indices[k])) {
            return s->fail(ERR_BAD_PARAM);
        }
    }
    if (kind == 2 && !s->paramOk(flag)) {
        return s->fail(ERR_BAD_PARAM);
    }
    // The ellipse alignments are 0..9; the hyperbola ones need a hyperbola.
    if (kind == 54 && (flag < 0 || flag > GCS::EllipseFocus2Y)) {
        return s->fail(ERR_BAD_ARG);
    }
    if (std::isnan(scale)) {
        return s->fail(ERR_BAD_ARG);
    }

    const bool d = driving != 0;
    double* q0 = spec->params > 0 ? s->params[p0] : nullptr;
    double* q1 = spec->params > 1 ? s->params[p1] : nullptr;
    int i = -1;
    switch (kind) {
        case 1: {
            const auto align = (flag & 1) != 0 ? GCS::Constraint::Alignment::InternalAlignment
                                               : GCS::Constraint::Alignment::NoInternalAlignment;
            i = s->addConstraintEqual(q0, q1, tag, d, align);
            // Bit 1 asks for what the npm wrapper's `equal_optimization` did:
            // when exactly one side is fixed, the free one takes its value
            // now, so the solver starts from it. Off unless the caller asks.
            if ((flag & 2) != 0) {
                if (s->fixed[p0] != 0 && s->fixed[p1] == 0) {
                    *q1 = *q0;
                } else if (s->fixed[p1] != 0 && s->fixed[p0] == 0) {
                    *q0 = *q1;
                }
            }
            break;
        }
        case 2: i = s->addConstraintDifference(q0, q1, s->params[flag], tag, d); break;
        case 3: i = s->addConstraintP2PDistance(P(g[0]), P(g[1]), q0, tag, d); break;
        case 4: i = s->addConstraintP2PAngle(P(g[0]), P(g[1]), q0, tag, d); break;
        case 5: i = s->addConstraintP2LDistance(P(g[0]), L(g[1]), q0, tag, d); break;
        case 6: i = s->addConstraintPointOnLine(P(g[0]), L(g[1]), tag, d); break;
        case 7: i = s->addConstraintPointOnLine(P(g[0]), P(g[1]), P(g[2]), tag, d); break;
        case 8: i = s->addConstraintPointOnPerpBisector(P(g[0]), L(g[1]), tag, d); break;
        case 9:
            i = s->addConstraintPointOnPerpBisector(P(g[0]), P(g[1]), P(g[2]), tag, d);
            break;
        case 10: i = s->addConstraintParallel(L(g[0]), L(g[1]), tag, d); break;
        case 11: i = s->addConstraintPerpendicular(L(g[0]), L(g[1]), tag, d); break;
        case 12: i = s->addConstraintL2LAngle(L(g[0]), L(g[1]), q0, tag, d); break;
        case 13: i = s->addConstraintAngleViaPoint(V(g[0]), V(g[1]), P(g[2]), q0, tag, d); break;
        case 14: i = s->addConstraintMidpointOnLine(L(g[0]), L(g[1]), tag, d); break;
        case 15:
            i = s->addConstraintTangentCircumf(P(g[0]), P(g[1]), q0, q1, flag != 0, tag, d);
            break;
        case 16: i = s->addConstraintP2PCoincident(P(g[0]), P(g[1]), tag, d); break;
        case 17: i = s->addConstraintHorizontal(L(g[0]), tag, d); break;
        case 18: i = s->addConstraintHorizontal(P(g[0]), P(g[1]), tag, d); break;
        case 19: i = s->addConstraintVertical(L(g[0]), tag, d); break;
        case 20: i = s->addConstraintVertical(P(g[0]), P(g[1]), tag, d); break;
        case 21: i = s->addConstraintCoordinateX(P(g[0]), q0, tag, d); break;
        case 22: i = s->addConstraintCoordinateY(P(g[0]), q0, tag, d); break;
        case 23: i = s->addConstraintArcRules(A(g[0]), tag, d); break;
        case 24: i = s->addConstraintPointOnCircle(P(g[0]), C(g[1]), tag, d); break;
        case 25: i = s->addConstraintPointOnEllipse(P(g[0]), E(g[1]), tag, d); break;
        case 26: i = s->addConstraintPointOnArc(P(g[0]), A(g[1]), tag, d); break;
        case 27: i = s->addConstraintCurveValue(P(g[0]), V(g[1]), q0, tag, d); break;
        case 28:
            i = s->addConstraintPerpendicularLine2Arc(P(g[0]), P(g[1]), A(g[2]), tag, d);
            break;
        case 29:
            i = s->addConstraintPerpendicularArc2Line(A(g[0]), P(g[1]), P(g[2]), tag, d);
            break;
        case 30: i = s->addConstraintPerpendicularCircle2Arc(P(g[0]), q0, A(g[1]), tag, d); break;
        case 31: i = s->addConstraintPerpendicularArc2Circle(A(g[0]), P(g[1]), q0, tag, d); break;
        case 32:
            i = s->addConstraintPerpendicularArc2Arc(A(g[0]), (flag & 1) != 0, A(g[1]),
                                                     (flag & 2) != 0, tag, d);
            break;
        case 33: i = s->addConstraintTangent(L(g[0]), C(g[1]), tag, d); break;
        case 34: i = s->addConstraintTangent(L(g[0]), E(g[1]), tag, d); break;
        case 35: i = s->addConstraintTangent(L(g[0]), A(g[1]), tag, d); break;
        case 36: i = s->addConstraintTangent(C(g[0]), C(g[1]), tag, d); break;
        case 37: i = s->addConstraintTangent(A(g[0]), A(g[1]), tag, d); break;
        case 38: i = s->addConstraintTangent(C(g[0]), A(g[1]), tag, d); break;
        case 39: i = s->addConstraintCircleRadius(C(g[0]), q0, tag, d); break;
        case 40: i = s->addConstraintArcRadius(A(g[0]), q0, tag, d); break;
        case 41: i = s->addConstraintCircleDiameter(C(g[0]), q0, tag, d); break;
        case 42: i = s->addConstraintArcDiameter(A(g[0]), q0, tag, d); break;
        case 43: i = s->addConstraintEqualLength(L(g[0]), L(g[1]), tag, d); break;
        case 44: i = s->addConstraintEqualRadius(C(g[0]), C(g[1]), tag, d); break;
        case 45: i = s->addConstraintEqualRadius(C(g[0]), A(g[1]), tag, d); break;
        case 46: i = s->addConstraintEqualRadius(A(g[0]), A(g[1]), tag, d); break;
        case 47: i = s->addConstraintEqualRadii(E(g[0]), E(g[1]), tag, d); break;
        case 48: i = s->addConstraintP2PSymmetric(P(g[0]), P(g[1]), L(g[2]), tag, d); break;
        case 49: i = s->addConstraintP2PSymmetric(P(g[0]), P(g[1]), P(g[2]), tag, d); break;
        case 50: i = s->addConstraintC2CDistance(C(g[0]), C(g[1]), q0, tag, d); break;
        case 51: i = s->addConstraintC2LDistance(C(g[0]), L(g[1]), q0, tag, d); break;
        case 52: i = s->addConstraintP2CDistance(P(g[0]), C(g[1]), q0, tag, d); break;
        case 53: i = s->addConstraintArcLength(A(g[0]), q0, tag, d); break;
        case 54:
            i = s->addConstraintInternalAlignmentPoint2Ellipse(
                E(g[0]), P(g[1]), static_cast<GCS::InternalAlignmentType>(flag), tag, d);
            break;
        case 55:
            i = s->addConstraintInternalAlignmentEllipseMajorDiameter(E(g[0]), P(g[1]), P(g[2]),
                                                                      tag, d);
            break;
        case 56:
            i = s->addConstraintInternalAlignmentEllipseMinorDiameter(E(g[0]), P(g[1]), P(g[2]),
                                                                      tag, d);
            break;
        case 57: i = s->addConstraintInternalAlignmentEllipseFocus1(E(g[0]), P(g[1]), tag, d); break;
        case 58: i = s->addConstraintInternalAlignmentEllipseFocus2(E(g[0]), P(g[1]), tag, d); break;
        default: return s->fail(ERR_BAD_KIND);
    }
    if (scale != 1) {
        s->rescaleConstraint(i, scale);
    }
    return i;
}
