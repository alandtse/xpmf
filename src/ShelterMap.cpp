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

constexpr int SPAN = 3; /**< Cells per side of a layer block / field */

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
                                      9>& layers,
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
    const Layer& center = layers[4];
    const int blockWest = (center.cellX - 1) * K_CELLS;
    const int blockSouth = (center.cellY - 1) * K_CELLS;
    const int nodeWest
        = std::max(static_cast<int>(std::ceil(std::min({first.x, second.x, third.x}) / K_SPACING)), blockWest);
    const int nodeEast = std::min(static_cast<int>(std::floor(std::max({first.x, second.x, third.x}) / K_SPACING)),
                                  blockWest + (SPAN * K_CELLS));
    const int nodeSouth
        = std::max(static_cast<int>(std::ceil(std::min({first.y, second.y, third.y}) / K_SPACING)), blockSouth);
    const int nodeNorth = std::min(static_cast<int>(std::floor(std::max({first.y, second.y, third.y}) / K_SPACING)),
                                   blockSouth + (SPAN * K_CELLS));

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
                    if (slotX < 0 || slotX >= SPAN || slotY < 0 || slotY >= SPAN) {
                        continue;
                    }
                    Layer& layer = layers.at(static_cast<std::size_t>((slotY * SPAN) + slotX));
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

auto ShelterMap::Field::topAt(int nodeX,
                              int nodeY) const -> float
{
    const int cellX = floorDiv(nodeX, K_CELLS);
    const int cellY = floorDiv(nodeY, K_CELLS);
    const int slotX = cellX - centerX + 1;
    const int slotY = cellY - centerY + 1;
    if (slotX < 0 || slotX >= SPAN || slotY < 0 || slotY >= SPAN) {
        return K_NOTHING;
    }
    const auto& map = maps.at(static_cast<std::size_t>((slotY * SPAN) + slotX));
    return map != nullptr ? (*map)[nodeIndex(nodeX - (cellX * K_CELLS), nodeY - (cellY * K_CELLS))] : K_NOTHING;
}

auto ShelterMap::Field::isCovered(const RE::NiPoint3& point) const -> bool
{
    const int nodeX = static_cast<int>(std::floor(point.x / K_SPACING));
    const int nodeY = static_cast<int>(std::floor(point.y / K_SPACING));
    const float ceiling = point.z + K_CLEARANCE;
    return topAt(nodeX, nodeY) > ceiling && topAt(nodeX + 1, nodeY) > ceiling && topAt(nodeX, nodeY + 1) > ceiling
        && topAt(nodeX + 1, nodeY + 1) > ceiling;
}

auto ShelterMap::Field::depthUnderCover(const RE::NiPoint3& point,
                                        float reach) const -> float
{
    if (!isCovered(point)) {
        return 0.0F;
    }
    const int nodeX = static_cast<int>(std::floor(point.x / K_SPACING));
    const int nodeY = static_cast<int>(std::floor(point.y / K_SPACING));
    const float ceiling = point.z + K_CLEARANCE;

    // Distance to the nearest column that is open at this height - the drip line, from inside
    const int radius = static_cast<int>(std::ceil(reach / K_SPACING));
    float nearestSq = reach * reach;
    for (int offsetY = -radius; offsetY <= radius + 1; ++offsetY) {
        for (int offsetX = -radius; offsetX <= radius + 1; ++offsetX) {
            if (topAt(nodeX + offsetX, nodeY + offsetY) > ceiling) {
                continue;
            }
            const float deltaX = (static_cast<float>(nodeX + offsetX) * K_SPACING) - point.x;
            const float deltaY = (static_cast<float>(nodeY + offsetY) * K_SPACING) - point.y;
            nearestSq = std::min(nearestSq, (deltaX * deltaX) + (deltaY * deltaY));
        }
    }
    // The drip line runs somewhere between that open column and the covered one before it; half a
    // spacing is the unbiased guess (measuring to the node itself reads 0 to K_SPACING too deep)
    constexpr float HALF_SPACING = K_SPACING * 0.5F;
    return std::max(std::sqrt(nearestSq) - HALF_SPACING, 0.0F);
}

auto ShelterMap::Field::measureOpenness(std::span<const RE::NiPoint3> positions,
                                        std::span<const std::uint16_t> indices,
                                        float fade,
                                        float edgeOpenness,
                                        std::vector<float>& openness) const -> bool
{
    // The fade, plus the spacing depthUnderCover's estimate gives up: a vertex deep under cover has
    // to be able to reach 0, or nothing ever counts as covered
    const float reach = fade + K_SPACING;

    openness.assign(positions.size(), 1.0F);
    bool anyCover = false;
    for (std::size_t index = 0; index < positions.size(); ++index) {
        const float depth = depthUnderCover(positions[index], reach);
        if (depth <= 0.0F) {
            continue;
        }
        anyCover = true;
        const float t = fade > 0.0F ? std::clamp(depth / fade, 0.0F, 1.0F) : 1.0F;
        openness[index] = 1.0F - (t * t * (3.0F - (2.0F * t)));
    }
    if (!anyCover || indices.empty()) {
        return anyCover;
    }

    // Second step: put the snow edge where cover begins on every edge that crosses a drip line
    const auto localize = [&](std::size_t covered, std::size_t open) -> void {
        const RE::NiPoint3& from = positions[open];
        const RE::NiPoint3& to = positions[covered];
        const float deltaX = to.x - from.x;
        const float deltaY = to.y - from.y;
        const float deltaZ = to.z - from.z;
        const float length = std::sqrt((deltaX * deltaX) + (deltaY * deltaY) + (deltaZ * deltaZ));
        if (length <= 1.0F) {
            return;
        }
        // Fraction of the edge, measured from the open end, that lies in the open
        float inTheOpen = 1.0F;
        for (int sample = 1; sample <= K_EDGE_SAMPLES; ++sample) {
            const float fraction = static_cast<float>(sample) / static_cast<float>(K_EDGE_SAMPLES);
            if (isCovered({from.x + (deltaX * fraction), from.y + (deltaY * fraction), from.z + (deltaZ * fraction)})) {
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

    // Which vertices are in the open is decided before any value is raised
    std::vector<bool> isOpen(positions.size());
    for (std::size_t index = 0; index < positions.size(); ++index) {
        isOpen[index] = openness[index] >= 1.0F;
    }
    for (std::size_t corner = 0; corner + 2 < indices.size(); corner += 3) {
        const std::array<std::size_t, 3> triangle {indices[corner], indices[corner + 1], indices[corner + 2]};
        if (triangle[0] >= positions.size() || triangle[1] >= positions.size() || triangle[2] >= positions.size()) {
            continue;
        }
        for (const std::size_t covered : triangle) {
            for (const std::size_t open : triangle) {
                if (!isOpen[covered] && isOpen[open]) {
                    localize(covered, open);
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
