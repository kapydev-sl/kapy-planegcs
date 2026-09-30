// services/planegcs/build/capi/gcs_capi.cpp
//
// The C API of Kapy's planegcs: lifecycle, configuration, parameters, geometry,
// solving and diagnosis, as `extern "C"` functions that take and return only
// i32 and f64 (§2.2 of docs/history/v3/40-v3.3-solver-y-workers.md). It does
// the job of embind's `GcsSystem` without embind, without a JS glue and
// without the npm `GcsWrapper`: whoever calls it (the Node shim today, Rust
// next) keeps its own index bookkeeping and hands planegcs plain numbers.
//
// The contract, for every export: nothing is thrown and nothing is aborted
// for a bad argument. Handles and indices are checked before they are used;
// a failure returns -1 (or NaN for `gcs_get_param`) and leaves a code in
// `gcs_last_error`. A call that works resets that code to 0.
//
// `gcs_constraint` lives in gcs_capi_constraints.cpp.
//
// Who links it: the `planegcs-capi` target in ../planegcs/CMakeLists.txt.
// What does NOT belong here: the constraint dispatch, embind, or anything that
// knows what a Kapy sketch is.

#include <cmath>
#include <unordered_map>

#include "gcs_capi.h"

using namespace capi;

// Every export is `extern "C"` and, at its end, `noexcept`.
#define GCS_EXPORT extern "C"

void Sys::wipe() {
    // planegcs must forget its constraints first: they point into the doubles.
    clear();
    params.clear();
    chunks.clear();
    chunkUsed = 0;
    fixed.clear();
    arena.clear();
    for (auto& d : diag) {
        d.clear();
    }
}

double* Sys::newParam(double v) {
    if (chunks.empty() || chunkUsed == PARAM_CHUNK) {
        chunks.emplace_back(new double[PARAM_CHUNK]);
        chunkUsed = 0;
    }
    double* p = &chunks.back()[chunkUsed++];
    *p = v;
    return p;
}

// The solve embind's `solve_system` does: every non-fixed parameter, in the
// order they were pushed, is a variable (that order is the jacobian's column
// order, which the Kapy heap patch makes the only thing the result depends on).
int Sys::solveFree(int algorithm) {
    GCS::VEC_pD free;
    free.reserve(params.size());
    for (size_t i = 0; i < params.size(); ++i) {
        if (!fixed[i]) {
            free.push_back(params[i]);
        }
    }
    // The second argument (isFine) does not influence anything, as in embind.
    return solve(free, true, static_cast<GCS::Algorithm>(algorithm));
}

namespace {

// The geometry a `gcs_geom` call builds from parameter indices, in the order
// embind's `make_*` take them. `n` is how many indices the kind consumes.
struct GeomSpec {
    int kind;
    int n;
};

const GeomSpec GEOM_SPECS[] = {
    {GK_POINT, 2},    // x, y
    {GK_LINE, 4},     // p1x, p1y, p2x, p2y
    {GK_CIRCLE, 3},   // cx, cy, rad
    {GK_ARC, 9},      // cx, cy, sx, sy, ex, ey, startAngle, endAngle, rad
    {GK_ELLIPSE, 5},  // cx, cy, focus1x, focus1y, radmin
};

GCS::Point pointAt(const Sys& s, int x, int y) {
    GCS::Point p;
    p.x = s.params[x];
    p.y = s.params[y];
    return p;
}

int push(Sys& s, std::unique_ptr<Geom> g) {
    s.arena.push_back(std::move(g));
    return static_cast<int>(s.arena.size()) - 1;
}

}  // namespace

GCS_EXPORT int gcs_abi_version() noexcept {
    return ABI_VERSION;
}

GCS_EXPORT Sys* gcs_new() noexcept {
    return new Sys();
}

GCS_EXPORT void gcs_free(Sys* s) noexcept {
    if (s == nullptr) {
        return;
    }
    s->wipe();
    delete s;
}

GCS_EXPORT int gcs_clear(Sys* s) noexcept {
    if (s == nullptr) {
        return -1;
    }
    s->lastError = ERR_NONE;
    s->wipe();
    return 0;
}

GCS_EXPORT int gcs_last_error(Sys* s) noexcept {
    return s == nullptr ? ERR_NULL_SYSTEM : s->lastError;
}

GCS_EXPORT int gcs_set_convergence(Sys* s, double v) noexcept {
    if (s == nullptr) {
        return -1;
    }
    s->lastError = ERR_NONE;
    s->setConvergence(v);
    return 0;
}

GCS_EXPORT double gcs_get_convergence(Sys* s) noexcept {
    return s == nullptr ? std::nan("") : s->getConvergence();
}

GCS_EXPORT int gcs_set_max_iter(Sys* s, int n) noexcept {
    if (s == nullptr) {
        return -1;
    }
    s->lastError = ERR_NONE;
    s->setMaxIter(n);
    return 0;
}

GCS_EXPORT int gcs_get_max_iter(Sys* s) noexcept {
    return s == nullptr ? -1 : s->getMaxIter();
}

GCS_EXPORT int gcs_push_param(Sys* s, double v, int fixed) noexcept {
    if (s == nullptr) {
        return -1;
    }
    s->lastError = ERR_NONE;
    s->params.push_back(s->newParam(v));
    s->fixed.push_back(fixed != 0 ? 1 : 0);
    return static_cast<int>(s->params.size()) - 1;
}

GCS_EXPORT int gcs_set_param(Sys* s, int i, double v, int fixed) noexcept {
    if (s == nullptr) {
        return -1;
    }
    s->lastError = ERR_NONE;
    if (!s->paramOk(i)) {
        return s->fail(ERR_BAD_PARAM);
    }
    *s->params[i] = v;
    s->fixed[i] = fixed != 0 ? 1 : 0;
    return 0;
}

GCS_EXPORT double gcs_get_param(Sys* s, int i) noexcept {
    if (s == nullptr) {
        return std::nan("");
    }
    s->lastError = ERR_NONE;
    if (!s->paramOk(i)) {
        s->lastError = ERR_BAD_PARAM;
        return std::nan("");
    }
    return *s->params[i];
}

GCS_EXPORT int gcs_is_fixed(Sys* s, int i) noexcept {
    if (s == nullptr) {
        return -1;
    }
    s->lastError = ERR_NONE;
    if (!s->paramOk(i)) {
        return s->fail(ERR_BAD_PARAM);
    }
    return s->fixed[i];
}

GCS_EXPORT int gcs_param_count(Sys* s) noexcept {
    return s == nullptr ? -1 : static_cast<int>(s->params.size());
}

// One geometry from up to nine parameter indices; returns its handle. The
// handle is only meaningful to `gcs_constraint` on the same system, and lives
// until `gcs_clear` (the arena replaces the objects embind made per argument
// and the wrapper deleted after each constraint).
GCS_EXPORT int gcs_geom(Sys* s, int kind, int a0, int a1, int a2, int a3, int a4, int a5, int a6,
                        int a7, int a8) noexcept {
    if (s == nullptr) {
        return -1;
    }
    s->lastError = ERR_NONE;
    const GeomSpec* spec = nullptr;
    for (const GeomSpec& c : GEOM_SPECS) {
        if (c.kind == kind) {
            spec = &c;
        }
    }
    if (spec == nullptr) {
        return s->fail(ERR_BAD_KIND);
    }
    const int a[9] = {a0, a1, a2, a3, a4, a5, a6, a7, a8};
    for (int k = 0; k < spec->n; ++k) {
        if (!s->paramOk(a[k])) {
            return s->fail(ERR_BAD_PARAM);
        }
    }
    auto g = std::make_unique<Geom>();
    g->kind = kind;
    switch (kind) {
        case GK_POINT:
            g->point = pointAt(*s, a[0], a[1]);
            break;
        case GK_LINE: {
            auto l = std::make_unique<GCS::Line>();
            l->p1 = pointAt(*s, a[0], a[1]);
            l->p2 = pointAt(*s, a[2], a[3]);
            g->curve = std::move(l);
            break;
        }
        case GK_CIRCLE: {
            auto c = std::make_unique<GCS::Circle>();
            c->center = pointAt(*s, a[0], a[1]);
            c->rad = s->params[a[2]];
            g->curve = std::move(c);
            break;
        }
        case GK_ARC: {
            auto c = std::make_unique<GCS::Arc>();
            c->center = pointAt(*s, a[0], a[1]);
            c->start = pointAt(*s, a[2], a[3]);
            c->end = pointAt(*s, a[4], a[5]);
            c->startAngle = s->params[a[6]];
            c->endAngle = s->params[a[7]];
            c->rad = s->params[a[8]];
            g->curve = std::move(c);
            break;
        }
        case GK_ELLIPSE: {
            auto e = std::make_unique<GCS::Ellipse>();
            e->center = pointAt(*s, a[0], a[1]);
            e->focus1 = pointAt(*s, a[2], a[3]);
            e->radmin = s->params[a[4]];
            g->curve = std::move(e);
            break;
        }
        default:
            return s->fail(ERR_BAD_KIND);
    }
    return push(*s, std::move(g));
}

GCS_EXPORT int gcs_solve(Sys* s, int algorithm) noexcept {
    if (s == nullptr) {
        return -1;
    }
    s->lastError = ERR_NONE;
    if (algorithm < GCS::BFGS || algorithm > GCS::DogLeg) {
        return s->fail(ERR_BAD_ARG);
    }
    return s->solveFree(algorithm);
}

GCS_EXPORT int gcs_apply(Sys* s) noexcept {
    if (s == nullptr) {
        return -1;
    }
    s->lastError = ERR_NONE;
    s->applySolution();
    return 0;
}

// Degrees of freedom; -1 while planegcs has no diagnosis (it also answers -1
// on its own then, as embind's `dof` did, so -1 is not an error here).
GCS_EXPORT int gcs_dof(Sys* s) noexcept {
    if (s == nullptr) {
        return -1;
    }
    s->lastError = ERR_NONE;
    return s->dofsNumber();
}

// Whether `list` has anything to report, with planegcs' own convention (a
// system that has not been diagnosed reports true). 0 conflicting, 1
// redundant, 2 partially redundant.
GCS_EXPORT int gcs_diag_has(Sys* s, int list) noexcept {
    if (s == nullptr) {
        return -1;
    }
    s->lastError = ERR_NONE;
    switch (list) {
        case DL_CONFLICTING:
            return s->hasConflicting() ? 1 : 0;
        case DL_REDUNDANT:
            return s->hasRedundant() ? 1 : 0;
        case DL_PARTIAL:
            return s->hasPartiallyRedundant() ? 1 : 0;
        default:
            return s->fail(ERR_BAD_ARG);
    }
}

// Takes a snapshot of `list` and returns its length; `gcs_diag_at` walks that
// snapshot. Lists 0..2 are constraint tags, list 3 is the indices (into the
// pushed parameters) of the parameters planegcs found dependent.
GCS_EXPORT int gcs_diag_count(Sys* s, int list) noexcept {
    if (s == nullptr) {
        return -1;
    }
    s->lastError = ERR_NONE;
    if (list < 0 || list >= DL_COUNT) {
        return s->fail(ERR_BAD_ARG);
    }
    std::vector<int>& out = s->diag[list];
    out.clear();
    if (list == DL_DEPENDENT) {
        GCS::VEC_pD dependent;
        s->getDependentParams(dependent);
        std::unordered_map<const double*, int> index;
        index.reserve(s->params.size());
        for (size_t i = 0; i < s->params.size(); ++i) {
            index.emplace(s->params[i], static_cast<int>(i));
        }
        for (const double* p : dependent) {
            auto it = index.find(p);
            if (it != index.end()) {
                out.push_back(it->second);
            }
        }
    } else if (list == DL_CONFLICTING) {
        s->getConflicting(out);
    } else if (list == DL_REDUNDANT) {
        s->getRedundant(out);
    } else {
        s->getPartiallyRedundant(out);
    }
    return static_cast<int>(out.size());
}

GCS_EXPORT int gcs_diag_at(Sys* s, int list, int i) noexcept {
    if (s == nullptr) {
        return -1;
    }
    s->lastError = ERR_NONE;
    if (list < 0 || list >= DL_COUNT) {
        return s->fail(ERR_BAD_ARG);
    }
    const std::vector<int>& v = s->diag[list];
    if (i < 0 || i >= static_cast<int>(v.size())) {
        return s->fail(ERR_BAD_ARG);
    }
    return v[i];
}
