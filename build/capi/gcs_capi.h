// services/planegcs/build/capi/gcs_capi.h
//
// What the two translation units of the C API share: the system object behind
// a handle, the geometry arena, the error codes and the slot table generated
// from constraint_kinds.def.
//
// Who includes it: gcs_capi.cpp and gcs_capi_constraints.cpp.
// What does NOT belong here: an export (those are declared where they are
// defined) or any logic beyond the small validators.

#ifndef KAPY_GCS_CAPI_H
#define KAPY_GCS_CAPI_H

#include <memory>
#include <vector>

#include "GCS.h"

namespace capi {

// Bumped whenever an export's meaning or a `kind` number changes.
constexpr int ABI_VERSION = 1;

// What `gcs_last_error` answers. 0 is «the last call worked».
enum Err {
    ERR_NONE = 0,
    ERR_NULL_SYSTEM = 1,
    ERR_BAD_PARAM = 2,   // a parameter index outside the system's parameters
    ERR_BAD_GEOM = 3,    // a geometry handle that is not in the arena
    ERR_BAD_KIND = 4,    // a geometry or constraint kind this build does not know
    ERR_BAD_SLOT = 5,    // a geometry of the wrong kind for its slot
    ERR_BAD_ARG = 6      // a value out of range (algorithm, list, index)
};

// Geometry kinds, as `gcs_geom` takes them. The gaps are reserved for the
// conics and the B-spline embind has and Kapy does not use.
enum GeomKind { GK_POINT = 0, GK_LINE = 1, GK_CIRCLE = 2, GK_ARC = 3, GK_ELLIPSE = 4 };

// The geometry a constraint slot wants (`NO` = the slot is unused).
// Doubles per parameter chunk: 128 KiB, enough for any sketch the editor draws.
constexpr size_t PARAM_CHUNK = 16384;

enum Slot { NO, PT, LN, CI, AR, EL, CV };

// One entry of the arena: a point by value, or a curve that owns its points.
struct Geom {
    int kind = GK_POINT;
    GCS::Point point;
    std::unique_ptr<GCS::Curve> curve;
};

// The diagnosis lists `gcs_diag_count` and `gcs_diag_at` walk.
enum DiagList { DL_CONFLICTING = 0, DL_REDUNDANT = 1, DL_PARTIAL = 2, DL_DEPENDENT = 3, DL_COUNT = 4 };

// planegcs plus everything embind's GcsSystem kept next to it: the parameters
// (heap doubles the geometry points into, in insertion order), which of them
// are fixed, the arena, the last error and a snapshot of each diagnosis list.
struct Sys : GCS::System {
    std::vector<double*> params;
    // Where `params` point: chunks of PARAM_CHUNK doubles, handed out in push
    // order, so that comparing two parameters by address is comparing them by
    // index (see newParam).
    std::vector<std::unique_ptr<double[]>> chunks;
    size_t chunkUsed = 0;
    std::vector<char> fixed;
    std::vector<std::unique_ptr<Geom>> arena;
    std::vector<int> diag[DL_COUNT];
    int lastError = ERR_NONE;

    // Frees every parameter and geometry and empties planegcs.
    void wipe();
    // A new parameter, its address above every earlier one's. planegcs orders
    // some of its containers by parameter address; taken from the allocator one
    // `new double` at a time that order was whatever the heap looked like, and
    // the same sketch solved differently after another one had used the heap.
    // Within a chunk the order is the push order; a system with more than
    // PARAM_CHUNK parameters starts a second chunk, whose place relative to the
    // first is the allocator's (documented in services/planegcs/README.md).
    double* newParam(double v);
    // Records `code` as the last error and returns the failure value, -1.
    int fail(int code) {
        lastError = code;
        return -1;
    }
    bool paramOk(int i) const { return i >= 0 && i < static_cast<int>(params.size()); }
    Geom* geom(int h) const {
        return (h >= 0 && h < static_cast<int>(arena.size())) ? arena[h].get() : nullptr;
    }
    // Typed accessors so the exports never touch planegcs' members directly.
    void setConvergence(double v) { convergence = v; }
    double getConvergence() const { return convergence; }
    void setMaxIter(int n) { maxIter = n; }
    int getMaxIter() const { return static_cast<int>(maxIter); }
    int solveFree(int algorithm);
};

// `gcs_constraint`'s row for `kind`, or nullptr for a kind not in the table.
struct KindSpec {
    int id;
    Slot g[3];
    int params;
};
const KindSpec* kindSpec(int kind);

}  // namespace capi

#endif
