#include "ShelterMap.hpp"

#include "PCH.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

using namespace XPMF;

namespace {

/**
 * @brief Floor division; C++ division truncates toward zero, and half the world has negative coordinates
 */
auto floorDiv(int value,
              int divisor) -> int
{
    const int quotient = value / divisor;
    return (value % divisor != 0 && (value < 0) != (divisor < 0)) ? quotient - 1 : quotient;
}

auto nodeIndex(int localX,
               int localY) -> std::size_t
{
    return (static_cast<std::size_t>(localY) * static_cast<std::size_t>(ShelterMap::K_NODES))
        + static_cast<std::size_t>(localX);
}

} // namespace

void ShelterMap::rasterize(std::array<Layer,
                                      K_BLOCK_CELLS>& layers,
                           const RE::NiPoint3& first,
                           const RE::NiPoint3& second,
                           const RE::NiPoint3& third)
{
    // Twice the signed area of the triangle's footprint; a wall has none and covers no column
    const float denominator
        = ((second.y - third.y) * (first.x - third.x)) + ((third.x - second.x) * (first.y - third.y));
    constexpr float MIN_FOOTPRINT = 1.0F;
    if (std::abs(denominator) < MIN_FOOTPRINT) {
        return;
    }

    // Lattice nodes inside the footprint's bounding box, clipped to the block of layers
    const Layer& center = layers[K_BLOCK_CENTER];
    const int blockWest = (center.cellX - 1) * K_CELLS;
    const int blockSouth = (center.cellY - 1) * K_CELLS;
    const int nodeWest
        = std::max(static_cast<int>(std::ceil(std::min({first.x, second.x, third.x}) / K_SPACING)), blockWest);
    const int nodeEast = std::min(static_cast<int>(std::floor(std::max({first.x, second.x, third.x}) / K_SPACING)),
                                  blockWest + (K_BLOCK * K_CELLS));
    const int nodeSouth
        = std::max(static_cast<int>(std::ceil(std::min({first.y, second.y, third.y}) / K_SPACING)), blockSouth);
    const int nodeNorth = std::min(static_cast<int>(std::floor(std::max({first.y, second.y, third.y}) / K_SPACING)),
                                   blockSouth + (K_BLOCK * K_CELLS));

    const float inverse = 1.0F / denominator;
    constexpr float EDGE_TOLERANCE = -1.0e-4F; // nodes exactly on a shared edge belong to both triangles
    for (int nodeY = nodeSouth; nodeY <= nodeNorth; ++nodeY) {
        const float worldY = static_cast<float>(nodeY) * K_SPACING;
        for (int nodeX = nodeWest; nodeX <= nodeEast; ++nodeX) {
            const float worldX = static_cast<float>(nodeX) * K_SPACING;
            const float weightFirst
                = (((second.y - third.y) * (worldX - third.x)) + ((third.x - second.x) * (worldY - third.y))) * inverse;
            const float weightSecond
                = (((third.y - first.y) * (worldX - third.x)) + ((first.x - third.x) * (worldY - third.y))) * inverse;
            const float weightThird = 1.0F - weightFirst - weightSecond;
            if (weightFirst < EDGE_TOLERANCE || weightSecond < EDGE_TOLERANCE || weightThird < EDGE_TOLERANCE) {
                continue;
            }
            const float height = (weightFirst * first.z) + (weightSecond * second.z) + (weightThird * third.z);

            // A node on a cell's west or south edge is also the last node of the cell before it
            const int cellX = floorDiv(nodeX, K_CELLS);
            const int cellY = floorDiv(nodeY, K_CELLS);
            const int localX = nodeX - (cellX * K_CELLS);
            const int localY = nodeY - (cellY * K_CELLS);
            for (int shareY = 0; shareY <= (localY == 0 ? 1 : 0); ++shareY) {
                for (int shareX = 0; shareX <= (localX == 0 ? 1 : 0); ++shareX) {
                    const int slotX = cellX - shareX - center.cellX + 1;
                    const int slotY = cellY - shareY - center.cellY + 1;
                    if (slotX < 0 || slotX >= K_BLOCK || slotY < 0 || slotY >= K_BLOCK) {
                        continue;
                    }
                    Layer& layer = layers.at(static_cast<std::size_t>((slotY * K_BLOCK) + slotX));
                    if (layer.top.empty()) {
                        layer.top.assign(static_cast<std::size_t>(K_NODES) * K_NODES, K_NOTHING);
                    }
                    float& top = layer.top[nodeIndex(shareX == 1 ? K_CELLS : localX, shareY == 1 ? K_CELLS : localY)];
                    top = std::max(top, height);
                }
            }
        }
    }
}

auto ShelterMap::Slope::of(const RE::NiPoint3& normal) -> Slope
{
    // Tilted past this a surface is a wall as far as snow is concerned (it holds none), and the
    // plane of one says nothing about the columns next to it
    constexpr float MIN_UP = 0.2F;
    if (normal.z < MIN_UP) {
        return {};
    }
    return {.dzdx = -normal.x / normal.z, .dzdy = -normal.y / normal.z};
}

auto ShelterMap::Slope::ofTriangle(const RE::NiPoint3& first,
                                   const RE::NiPoint3& second,
                                   const RE::NiPoint3& third) -> Slope
{
    const RE::NiPoint3 normal = (second - first).Cross(third - first);
    const float length = normal.Length();
    constexpr float MIN_LENGTH = 1.0e-6F; /**< Twice the area of a triangle that is no triangle */
    if (length <= MIN_LENGTH) {
        return {};
    }
    // Whichever way the triangle is wound, its slope is that of the side that faces up
    const RE::NiPoint3 unit = normal * (1.0F / length);
    return of(unit.z < 0.0F ? -unit : unit);
}

auto ShelterMap::Field::topAt(int nodeX,
                              int nodeY) const -> float
{
    const int cellX = floorDiv(nodeX, K_CELLS);
    const int cellY = floorDiv(nodeY, K_CELLS);
    const int slotX = cellX - centerX + 1;
    const int slotY = cellY - centerY + 1;
    if (slotX < 0 || slotX >= K_BLOCK || slotY < 0 || slotY >= K_BLOCK) {
        return K_NOTHING;
    }
    const auto& map = maps.at(static_cast<std::size_t>((slotY * K_BLOCK) + slotX));
    return map != nullptr ? (*map)[nodeIndex(nodeX - (cellX * K_CELLS), nodeY - (cellY * K_CELLS))] : K_NOTHING;
}

auto ShelterMap::cellOf(float coordinate) -> int { return static_cast<int>(std::floor(coordinate / K_CELL_SIZE)); }

auto ShelterMap::nearestNodeOf(float coordinate) -> int
{
    return static_cast<int>(std::lround(coordinate / K_SPACING));
}

auto ShelterMap::Field::ceilingAt(const RE::NiPoint3& point,
                                  const Slope& slope,
                                  int nodeX,
                                  int nodeY) -> float
{
    return point.z + K_CLEARANCE + (slope.dzdx * ((static_cast<float>(nodeX) * K_SPACING) - point.x))
        + (slope.dzdy * ((static_cast<float>(nodeY) * K_SPACING) - point.y));
}

auto ShelterMap::Field::isCovered(const RE::NiPoint3& point,
                                  const Slope& slope) const -> bool
{
    const auto covered
        = [&](int nodeX, int nodeY) -> bool { return topAt(nodeX, nodeY) > ceilingAt(point, slope, nodeX, nodeY); };
    const int nodeX = nearestNodeOf(point.x);
    const int nodeY = nearestNodeOf(point.y);
    if (!covered(nodeX, nodeY)) {
        return false;
    }
    // ...and in one of the four blocks of four nodes that node is a corner of
    for (int blockY = nodeY - 1; blockY <= nodeY; ++blockY) {
        for (int blockX = nodeX - 1; blockX <= nodeX; ++blockX) {
            if (covered(blockX, blockY) && covered(blockX + 1, blockY) && covered(blockX, blockY + 1)
                && covered(blockX + 1, blockY + 1)) {
                return true;
            }
        }
    }
    return false;
}

auto ShelterMap::Field::depthUnderCover(const RE::NiPoint3& point,
                                        const Slope& slope,
                                        float reach) const -> float
{
    if (!isCovered(point, slope)) {
        return 0.0F;
    }
    const int nodeX = nearestNodeOf(point.x);
    const int nodeY = nearestNodeOf(point.y);

    // Distance to the nearest column that is open at this height - the drip line, from inside.
    // The window is centered on the nearest node, up to half a spacing from the point, hence the
    // half spacing more of radius
    constexpr float HALF_SPACING = K_SPACING * 0.5F;
    const int radius = static_cast<int>(std::ceil((reach + HALF_SPACING) / K_SPACING));
    float nearestSq = reach * reach;
    for (int offsetY = -radius; offsetY <= radius; ++offsetY) {
        for (int offsetX = -radius; offsetX <= radius; ++offsetX) {
            const int columnX = nodeX + offsetX;
            const int columnY = nodeY + offsetY;
            if (topAt(columnX, columnY) > ceilingAt(point, slope, columnX, columnY)) {
                continue;
            }
            const float deltaX = (static_cast<float>(columnX) * K_SPACING) - point.x;
            const float deltaY = (static_cast<float>(columnY) * K_SPACING) - point.y;
            nearestSq = std::min(nearestSq, (deltaX * deltaX) + (deltaY * deltaY));
        }
    }
    // The drip line runs somewhere between that open column and the covered one before it; half a
    // spacing is the unbiased guess (measuring to the node itself reads 0 to K_SPACING too deep)
    return std::max(std::sqrt(nearestSq) - HALF_SPACING, 0.0F);
}

auto ShelterMap::Field::measureOpenness(std::span<const RE::NiPoint3> positions,
                                        std::span<const RE::NiPoint3> normals,
                                        std::span<const std::uint16_t> indices,
                                        float fade,
                                        float edgeOpenness,
                                        std::vector<float>& openness) const -> bool
{
    // The fade, plus the spacing depthUnderCover's estimate gives up: a point deep under cover has
    // to be able to reach 0, or nothing ever counts as covered
    const float reach = fade + K_SPACING;
    const auto opennessAt = [&](const RE::NiPoint3& point, const Slope& slope) -> float {
        const float depth = depthUnderCover(point, slope, reach);
        if (depth <= 0.0F) {
            return 1.0F;
        }
        const float t = fade > 0.0F ? std::clamp(depth / fade, 0.0F, 1.0F) : 1.0F;
        return 1.0F - (t * t * (3.0F - (2.0F * t)));
    };

    // First step: every vertex's own spot, read along its own surface
    openness.assign(positions.size(), 1.0F);
    bool anyCover = false;
    for (std::size_t index = 0; index < positions.size(); ++index) {
        openness[index] = opennessAt(positions[index], index < normals.size() ? Slope::of(normals[index]) : Slope {});
        anyCover = anyCover || openness[index] < 1.0F;
    }
    if (!anyCover || indices.empty()) {
        return anyCover;
    }

    // Which vertices are in the open is decided now: those are never lowered, and they are the ends
    // the third step measures from
    std::vector<bool> isOpen(positions.size());
    for (std::size_t index = 0; index < positions.size(); ++index) {
        isOpen[index] = openness[index] >= 1.0F;
    }
    const auto isTriangle = [&](const std::array<std::size_t, 3>& triangle) -> bool {
        return triangle[0] < positions.size() && triangle[1] < positions.size() && triangle[2] < positions.size();
    };

    // Second step: the inside of every triangle that has a covered corner. Where the corners
    // interpolate to more openness than a probe in the middle has, the covered corners are lowered
    // just enough to close the gap, the excess spread over them in proportion to their weight at
    // the probe; a corner ends at the lowest value any probe asks of it. Open corners are never
    // touched: a triangle that is mostly in the open keeps its snow there.
    constexpr std::array<std::array<float, 3>, 4> PROBES {
        {{1.0F / 3.0F, 1.0F / 3.0F, 1.0F / 3.0F}, {0.5F, 0.5F, 0.0F}, {0.0F, 0.5F, 0.5F}, {0.5F, 0.0F, 0.5F}}};
    std::vector<float> lowered(openness);
    for (std::size_t corner = 0; corner + 2 < indices.size(); corner += 3) {
        const std::array<std::size_t, 3> triangle {indices[corner], indices[corner + 1], indices[corner + 2]};
        if (!isTriangle(triangle) || (isOpen[triangle[0]] && isOpen[triangle[1]] && isOpen[triangle[2]])) {
            continue;
        }
        const RE::NiPoint3& first = positions[triangle[0]];
        const RE::NiPoint3& second = positions[triangle[1]];
        const RE::NiPoint3& third = positions[triangle[2]];
        const Slope slope = Slope::ofTriangle(first, second, third);
        for (const auto& weights : PROBES) {
            float interpolated = 0.0F;
            float coveredWeight = 0.0F;
            for (std::size_t k = 0; k < triangle.size(); ++k) {
                const std::size_t vertex = triangle.at(k);
                interpolated += weights.at(k) * openness[vertex];
                coveredWeight += isOpen[vertex] ? 0.0F : weights.at(k);
            }
            if (interpolated <= 0.0F || coveredWeight <= 0.0F) {
                continue; // nothing left to lower, or nothing here that may be
            }
            const RE::NiPoint3 probe = (first * weights[0]) + (second * weights[1]) + (third * weights[2]);
            const float excess = interpolated - opennessAt(probe, slope);
            if (excess <= 0.0F) {
                continue;
            }
            const float cut = excess / coveredWeight;
            for (const std::size_t vertex : triangle) {
                if (!isOpen[vertex]) {
                    lowered[vertex] = std::min(lowered[vertex], std::max(openness[vertex] - cut, 0.0F));
                }
            }
        }
    }
    openness.swap(lowered);

    // Third step: put the snow edge where cover begins on every edge that crosses a drip line
    const auto localize = [&](std::size_t covered, std::size_t open, const Slope& slope) -> void {
        const RE::NiPoint3& from = positions[open];
        const RE::NiPoint3& to = positions[covered];
        const float deltaX = to.x - from.x;
        const float deltaY = to.y - from.y;
        const float deltaZ = to.z - from.z;
        const float length = std::sqrt((deltaX * deltaX) + (deltaY * deltaY) + (deltaZ * deltaZ));
        constexpr float MIN_EDGE_LENGTH = 1.0F; /**< Shorter is a doubled vertex, not a direction */
        if (length <= MIN_EDGE_LENGTH) {
            return;
        }
        // Fraction of the edge, measured from the open end, that lies in the open
        float inTheOpen = 1.0F;
        for (int sample = 1; sample <= K_EDGE_SAMPLES; ++sample) {
            const float fraction = static_cast<float>(sample) / static_cast<float>(K_EDGE_SAMPLES);
            if (isCovered({from.x + (deltaX * fraction), from.y + (deltaY * fraction), from.z + (deltaZ * fraction)},
                          slope)) {
                inTheOpen = (static_cast<float>(sample) - 0.5F) / static_cast<float>(K_EDGE_SAMPLES);
                break;
            }
        }
        // Interpolated openness runs from 1 at the open end to the covered vertex's value; it has
        // to pass edgeOpenness at the target fraction, which fixes that value
        constexpr float MIN_TARGET = 0.05F;
        const float target = std::clamp(inTheOpen + (0.5F * fade / length), MIN_TARGET, 1.0F);
        const float needed = 1.0F - ((1.0F - edgeOpenness) / target);
        openness[covered] = std::max(openness[covered], std::clamp(needed, 0.0F, 1.0F));
    };

    for (std::size_t corner = 0; corner + 2 < indices.size(); corner += 3) {
        const std::array<std::size_t, 3> triangle {indices[corner], indices[corner + 1], indices[corner + 2]};
        if (!isTriangle(triangle)) {
            continue;
        }
        const Slope slope = Slope::ofTriangle(positions[triangle[0]], positions[triangle[1]], positions[triangle[2]]);
        for (const std::size_t covered : triangle) {
            for (const std::size_t open : triangle) {
                if (!isOpen[covered] && isOpen[open]) {
                    localize(covered, open, slope);
                }
            }
        }
    }
    return true;
}

auto ShelterMap::tally(std::span<const float> openness,
                       std::span<const float> facing,
                       float holdsSnowFrom,
                       float edgeOpenness) -> Tally
{
    Tally counts;
    for (std::size_t index = 0; index < openness.size() && index < facing.size(); ++index) {
        if (facing[index] < holdsSnowFrom) {
            continue;
        }
        ++counts.holders;
        counts.covered += openness[index] < 1.0F ? 1 : 0;
        counts.buried += openness[index] <= 0.0F ? 1 : 0;
        counts.bare += openness[index] < edgeOpenness ? 1 : 0;
    }
    return counts;
}

auto ShelterMap::judge(const Tally& counts,
                       bool maskable) -> Verdict
{
    if (counts.holders == 0) {
        return Verdict::OPEN; // nothing on it shows snow either way
    }
    if (!maskable) {
        return counts.bare * 2 >= counts.holders ? Verdict::SHELTERED : Verdict::OPEN;
    }
    if (counts.covered == 0) {
        return Verdict::OPEN;
    }
    return counts.buried == counts.holders ? Verdict::SHELTERED : Verdict::PARTIAL;
}

auto ShelterMap::combine(const std::vector<std::shared_ptr<const Heights>>& layers) -> std::shared_ptr<const Heights>
{
    std::shared_ptr<Heights> combined;
    for (const auto& layer : layers) {
        if (layer == nullptr || layer->empty()) {
            continue;
        }
        if (combined == nullptr) {
            combined = std::make_shared<Heights>(*layer);
            continue;
        }
        std::ranges::transform(
            *combined, *layer, combined->begin(), [](float lhs, float rhs) -> float { return std::max(lhs, rhs); });
    }
    return combined;
}
