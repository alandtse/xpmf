#pragma once

#include "PCH.h"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>
#include <unordered_map>

namespace XPMF {

/**
 * @brief Builds and shares the replacement vertex data that carries this plugin's vertex colors
 *
 * The lighting shader hands a shape's vertex color to the pixel shader whole: rgb tints the
 * finished pixel - the projected snow or ash included, which is what keeps a matched material
 * from matching on the three quarters of vanilla snow shapes whose colors darken snow-facing
 * vertices - and alpha scales the projection weight (dot(normal, up) * alpha against the
 * material's threshold), which is the mask one in five of those shapes already uses and the
 * channel the roof shelter writes. Both live in the shape's one interleaved vertex buffer,
 * shared by every clone of the model, so changing either for projection-carrying clones only
 * means giving those clones a buffer of their own: a "variant" of the source renderer data with
 * identical positions and topology (the index buffer is shared outright) and different colors.
 *
 * Variants are built through the engine's own CreateTriShape and freed by its own release,
 * so they are ordinary renderer data to everything else - the decal builder included, which
 * is why each gets a copy of the source's CPU index list as well.
 *
 * Most shapes with a projection on them share one variant per model and set of settings: white
 * colors (a profile's neutralizeVertexColors), alpha reset to 1 (its roofShelter). Only a shape
 * partly under cover gets a private variant, because only there does the alpha depend on where
 * the instance stands; one entirely under cover gets no variant at all - its projection is
 * switched off and it goes back to the model's own data (see ProjectedGeometry).
 *
 * Whitening is all or nothing per shape. Which vertices end up under snow depends on the
 * instance's orientation, the static's angle, the material and the noise, and guessing at that
 * per vertex buys little: a shape with a projection on it has white colors, everywhere.
 *
 * The registry keeps one reference on every variant and one on its source. The second pins
 * the source's address, which is the cache key; the first means a variant whose count is back
 * to 1 has no users left, which is how collectGarbage finds what to free.
 */
class ProjectedVertexData {
public:
    ProjectedVertexData() = delete;

    using Data = RE::BSGraphics::TriShape; /**< The engine's renderer data of a shape: its vertex and index
                                              buffers, with the CPU copies it keeps of them */

    /**
     * @brief What is known about the shape the data belongs to
     */
    struct Shape {
        Data* source {}; /**< The model's original renderer data (see sourceOf) */
        std::uint32_t vertexCount {}; /**< Vertices in the shape */
        std::uint32_t triangleCount {}; /**< Triangles in the shape */
        bool colorsEnabled {}; /**< Whether the shader's Vertex_Colors flag is set. When it is not, the colors
                                  were never shown and may hold anything, so a variant (which only makes sense
                                  with the flag turned on) has all of them white */
        bool keepAlpha {}; /**< Whether the mesh's vertex alpha is transparency: an alpha property that blends
                              (snow drifts fading into the ground, ...) or tests an alpha the mesh paints. Such
                              a shape keeps its alpha whatever roofShelter says; resetting it would give it hard
                              edges. A shape that only alpha tests an unpainted alpha is not one of these (its
                              test threshold follows its alpha instead, see
                              ProjectedGeometry::scaledAlphaThreshold), and neither is one with the Vertex_Alpha
                              shader flag and no alpha property: the flag alone shows nothing through */
        bool neutralize {}; /**< The profile's neutralizeVertexColors: colors the shader shows become white */
        bool shelter {}; /**< The profile's roofShelter: vertex alpha is this plugin's to write */
    };

    /**
     * @brief Returns the shared variant of a source, building it on first use
     *
     * Thread safe; called from the loader threads (Clone3D) and the worker.
     *
     * @return Data* Renderer data carrying one reference for the caller - the source itself when
     *         the variant would be identical to it - or nullptr when the layout is not
     *         supported or the engine failed to create the buffer (keep what the shape has)
     */
    [[nodiscard]] static auto shared(const Shape& shape) -> Data*;

    /**
     * @brief Builds a private variant from per vertex alpha (colors as for the shared one)
     *
     * @param alpha Per vertex alpha
     * @return Data* As for shared(); also nullptr once K_BUDGET is spent
     */
    [[nodiscard]] static auto custom(const Shape& shape,
                                     std::span<const std::uint8_t> alpha) -> Data*;

    /**
     * @brief Resolves renderer data to the model's original: itself, unless it is a variant
     */
    [[nodiscard]] static auto sourceOf(Data* data) -> Data*;

    /**
     * @brief Whether renderer data is a variant built for a shape whose Vertex_Colors flag was off
     *
     * Such a shape only shows colors because this plugin switched them on for the variant's sake,
     * and has to stop showing them when it goes back to the model's own data.
     */
    [[nodiscard]] static auto isForColorlessShape(const Data* data) -> bool;

    /**
     * @brief Fingerprint of the per vertex alpha a private variant was built from; 0 for anything else
     */
    [[nodiscard]] static auto fingerprintOf(const Data* data) -> std::uint64_t;

    /**
     * @brief Fingerprint custom() would store for these values
     */
    [[nodiscard]] static auto fingerprint(std::span<const std::uint8_t> alpha) -> std::uint64_t;

    /**
     * @brief One more reference on renderer data (the engine's own count)
     */
    static void addRef(Data* data);

    /**
     * @brief One reference less; at zero the engine frees the data, D3D buffers and CPU copies alike
     */
    static void release(Data* data);

    /**
     * @brief Puts renderer data into a shape, releasing what it held; consumes the caller's reference
     *
     * Only where nothing else touches the shape: on a clone not yet handed to the scene, or on
     * the main thread.
     */
    static void install(RE::BSTriShape& shape,
                        Data* data);

    /**
     * @brief Frees every variant nothing uses any more; main thread
     */
    static void collectGarbage();

private:
    constexpr static std::size_t K_BUDGET = std::size_t {256} * 1024 * 1024; /**< Bytes of private variant vertex
                                                                                data (held twice: once by the
                                                                                GPU, once as the engine's CPU
                                                                                copy) before custom() declines */

    /**
     * @brief How a variant's colors are derived from the source's
     */
    struct Recipe {
        bool whiten {}; /**< rgb = white; otherwise the mesh's */
        bool keepAlpha {}; /**< alpha = the mesh's; otherwise 1 everywhere... */
        std::span<const std::uint8_t> alpha; /**< ...or these, one per vertex, whatever keepAlpha says */
    };

    struct Entry {
        Data* source {}; /**< Pinned by a reference of its own */
        std::size_t bytes {}; /**< Vertex data size */
        std::uint64_t fingerprint {}; /**< Private variants only */
        bool isShared {};
        bool colorless {}; /**< Built for a shape whose Vertex_Colors flag was off */
    };

    struct SharedKey {
        const Data* source {};
        bool colorsEnabled {};
        bool keepAlpha {};
        bool neutralize {}; /**< One model can stand under materials of two profiles */
        bool shelter {};
        auto operator==(const SharedKey&) const -> bool = default;
    };

    struct SharedKeyHash {
        auto operator()(const SharedKey& key) const noexcept -> std::size_t;
    };

    /**
     * @brief Builds renderer data for a recipe
     *
     * @return Data* New data with a reference count of 1, the source (count untouched) when
     *         nothing would differ, or nullptr on failure
     */
    [[nodiscard]] static auto build(const Shape& shape,
                                    const Recipe& recipe) -> Data*;

    static inline std::mutex s_lock; /**< Guards everything below */
    static inline std::unordered_map<const Data*, Entry> s_variants; /**< Every live variant */
    static inline std::unordered_map<SharedKey, Data*, SharedKeyHash> s_shared; /**< Shared ones by what they are of */
    static inline std::size_t s_privateBytes = 0; /**< Against K_BUDGET */
};

} // namespace XPMF
