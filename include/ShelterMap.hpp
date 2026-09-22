#pragma once

#include "PCH.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <vector>

namespace XPMF {

/**
 * @brief Top-down height fields that answer "how far under cover is this point?"
 *
 * Snow falls straight down, and the single pass projection is straight down too, so whether
 * a point is sheltered is a question about the column above it: is any surface higher up? That
 * makes the whole scene reducible to one number per column - the height of its topmost
 * surface - which is what these maps hold, sampled on a lattice of K_SPACING units. A point
 * at height z is under cover where the top surface is more than K_CLEARANCE above it, and in
 * the open where it is not (which includes a roof's own top side, and a wall right next to a
 * point: a taller neighbor column blocks nothing above).
 *
 * The maps are rasterized from the triangles the game renders (the CPU copies it keeps for its
 * decal builder), not from collision: what shelters is exactly what can be seen sheltering, a
 * roof without collision counts, an invisible collision box does not.
 *
 * Cells are the unit of bookkeeping. A Layer is what the triangles gathered from one source
 * cell put over one target cell (a building on a cell border covers both), a target's map is
 * the maximum over its layers, and a Field is the 3x3 block of maps a query may reach into.
 */
class ShelterMap {
public:
    ShelterMap() = delete;

    constexpr static float K_CELL_SIZE = 4096.0F; /**< World units per exterior cell side */
    constexpr static float K_SPACING = 32.0F; /**< Lattice spacing; a roof edge is located to about half of it */
    constexpr static int K_CELLS = static_cast<int>(K_CELL_SIZE / K_SPACING); /**< Lattice cells per cell side */
    constexpr static int K_NODES = K_CELLS + 1; /**< Lattice nodes per side; the last row is the neighbor's first */
    constexpr static int K_BLOCK = 3; /**< Cells per side of the block of cells a source cell's triangles reach and a
                                         query may read: the cell and its neighbors, since a building on a border
                                         overhangs the next cell and never the one beyond */
    constexpr static int K_BLOCK_CELLS = K_BLOCK * K_BLOCK; /**< Cells in a block, row major */
    constexpr static std::size_t K_BLOCK_CENTER = K_BLOCK_CELLS / 2; /**< The block's middle cell */
    constexpr static float K_CLEARANCE = 24.0F; /**< How far above a point a surface must be to shelter it: more
                                                   than the lattice can misjudge a surface the point itself
                                                   lies on, less than any roof a snowed-on thing fits under */
    constexpr static float K_NOTHING = std::numeric_limits<float>::lowest(); /**< Node no triangle covers */
    constexpr static int K_EDGE_SAMPLES = 12; /**< Points tested along a triangle edge that crosses a drip line */

    using Heights = std::vector<float>; /**< K_NODES x K_NODES top surface heights, row major (y, then x) */

    /**
     * @brief Height field under construction for one target cell
     */
    struct Layer {
        int cellX {}; /**< Target cell X */
        int cellY {}; /**< Target cell Y */
        Heights top; /**< Empty until the first triangle lands */
    };

    /**
     * @brief Rasterizes one world space triangle into the layers of the 3x3 cells around a source cell
     *
     * A node takes the triangle's height where the node's column passes through it, keeping
     * the maximum. Near-vertical triangles cover no columns and cost a bounding box test.
     *
     * @param layers The block of layers around the source cell, row major, [K_BLOCK_CENTER] being
     *        the source itself
     */
    static void rasterize(std::array<Layer,
                                     K_BLOCK_CELLS>& layers,
                          const RE::NiPoint3& first,
                          const RE::NiPoint3& second,
                          const RE::NiPoint3& third);

    /**
     * @brief The 3x3 block of finished maps around a cell, immutable and safe to read on the worker
     */
    struct Field {
        int centerX {}; /**< Cell X of the middle map */
        int centerY {}; /**< Cell Y of the middle map */
        std::array<std::shared_ptr<const Heights>, K_BLOCK_CELLS> maps; /**< Row major; nullptr where no map exists
                                                                          yet */

        /**
         * @brief Per vertex openness (1 in the open .. 0 deep under cover) of one mesh
         *
         * Two steps. Every vertex first gets the openness of its own spot: 1 in the open,
         * falling to 0 over the fade distance under cover. That alone is only
         * right where the mesh is fine enough to follow the fade, and game meshes are not - a
         * stair flight is two rows of vertices, a porch plank has one at either end - so a
         * covered vertex would drag the interpolated value down along the whole triangle and
         * strip snow that lies in the open. The second step therefore walks every triangle
         * edge that joins an open and a covered vertex, finds where along it cover actually
         * begins, and raises the covered vertex's openness just enough that the interpolated
         * value crosses edgeOpenness there (half a fade past the drip line) rather than
         * somewhere out in the open.
         *
         * @param positions World space vertex positions
         * @param indices The mesh's triangle list; empty skips the second step
         * @param fade World units under cover over which openness falls to 0
         * @param edgeOpenness Openness at which snow visibly ends on the mesh's material
         * @param openness Out: one value per position
         * @return bool Whether any vertex is under cover
         */
        [[nodiscard]] auto measureOpenness(std::span<const RE::NiPoint3> positions,
                                           std::span<const std::uint16_t> indices,
                                           float fade,
                                           float edgeOpenness,
                                           std::vector<float>& openness) const -> bool;

    private:
        /**
         * @brief Top surface height at a global lattice node; K_NOTHING where unknown
         */
        [[nodiscard]] auto topAt(int nodeX,
                                 int nodeY) const -> float;

        /**
         * @brief Whether a world space point has something overhead
         *
         * Under cover only when all four lattice columns around the point top out more than
         * K_CLEARANCE above it: interpolating across the foot of a wall would otherwise put a
         * snow-free band along every vertical surface.
         */
        [[nodiscard]] auto isCovered(const RE::NiPoint3& point) const -> bool;

        /**
         * @brief How far inside cover a world space point is
         *
         * @param point The point
         * @param reach The farthest distance worth reporting (the fade distance plus a spacing)
         * @return float 0 in the open; otherwise the distance to the nearest lattice column that
         *         is open at the point's height, less half a spacing (the drip line runs
         *         somewhere between that column and the covered one before it), never above
         *         reach less that half spacing and never below 0
         */
        [[nodiscard]] auto depthUnderCover(const RE::NiPoint3& point,
                                           float reach) const -> float;
    };

    /**
     * @brief The exterior cell a world coordinate falls in, on one axis
     */
    [[nodiscard]] static auto cellOf(float coordinate) -> int;

    /**
     * @brief What cover means for a whole shape
     */
    enum class Verdict : std::uint8_t {
        OPEN, /**< Snow stays as it is */
        PARTIAL, /**< Snow has to be masked vertex by vertex */
        SHELTERED /**< No snow belongs on this shape at all */
    };

    /**
     * @brief What cover does to the vertices of a shape that can hold snow
     */
    struct Tally {
        std::size_t holders {}; /**< Vertices facing up enough to hold snow, covered or not */
        std::size_t covered {}; /**< ...of those, with anything overhead */
        std::size_t buried {}; /**< ...entirely under cover */
        std::size_t bare {}; /**< ...far enough under cover for their snow to be gone */
    };

    /**
     * @brief Counts a shape's snow-holding vertices by what cover does to them
     *
     * @param openness Per vertex openness, from Field::measureOpenness
     * @param facing Per vertex dot(normal, up)
     * @param holdsSnowFrom The facing from which a vertex can hold snow at all
     * @param edgeOpenness Openness at which snow visibly ends
     */
    [[nodiscard]] static auto tally(std::span<const float> openness,
                                    std::span<const float> facing,
                                    float holdsSnowFrom,
                                    float edgeOpenness) -> Tally;

    /**
     * @brief Judges a shape from the openness of the vertices that can hold snow
     *
     * Only those count. Every rock overhangs its own flanks and every house its own walls, but
     * a flank or a wall never carries snow, covered or not - judged on all vertices, half the
     * snowed-on shapes in a town came out "partly covered" and got vertex data of their own
     * for nothing.
     *
     * A shape whose vertex alpha can serve as a mask is OPEN when no such vertex is covered,
     * SHELTERED when all of them are entirely, PARTIAL otherwise. A shape whose vertex alpha is
     * taken (it blends or alpha tests with it) cannot be masked, so it is all or nothing:
     * SHELTERED once snow is gone from at least half of those vertices, OPEN below that.
     *
     * @param counts The shape's snow-holding vertices, from tally
     * @param maskable Whether the shape's vertex alpha is free to be used as a mask
     */
    [[nodiscard]] static auto judge(const Tally& counts,
                                    bool maskable) -> Verdict;

    /**
     * @brief Element-wise maximum of a cell's layers
     *
     * @return std::shared_ptr<const Heights> nullptr when no layer holds anything
     */
    [[nodiscard]] static auto combine(const std::vector<std::shared_ptr<const Heights>>& layers)
        -> std::shared_ptr<const Heights>;

private:
    /**
     * @brief The lattice node a world coordinate falls in, on one axis
     */
    [[nodiscard]] static auto nodeOf(float coordinate) -> int;
};

} // namespace XPMF
