#pragma once

#include <rapidjson/document.h>

#include <cstdint>
#include <cstring>
#include <string>

namespace core {

/**
 * @file Json.h
 * @brief The "absent or the wrong type keeps the default" readers, shared (S4.2).
 *
 * Three callers, which is what put them here rather than in any one of them:
 * `Config.cpp` reads a settings file, `ParticleSystem.cpp` reads
 * `extras.substrate_emitter` and `Collider.cpp` reads `extras.substrate_collider`.
 * Two of those were a coincidence and the duplication was deliberate; the third is the
 * Rule of Threes met rather than anticipated.
 *
 * `core/` rather than `scene/` because that is the narrowest scope all three callers
 * reach -- one of them is a core file, and promoting only far enough to reach it is the
 * whole of the scope rule.
 *
 * **What is deliberately not here is a policy.** Every function below leaves `out`
 * untouched when the key is absent or holds the wrong type, and says nothing about it.
 * What a *document* missing something means is the caller's decision and the three
 * disagree: a config that fails to parse is refused outright, and a scene that declares
 * no emitter is Sponza. Sharing five readers does not couple those two answers, which
 * is what the note this replaced was actually protecting.
 *
 * Readers with one caller stay with that caller. `readPath`, `readStringArray`,
 * `readInt` and `readUint64` are all Config's alone and are still in Config.cpp.
 */
namespace json {

/// The member named `name`, or null when `parent` is not an object or lacks it.
inline const rapidjson::Value* member(const rapidjson::Value& parent, const char* name) {
    if (!parent.IsObject()) return nullptr;
    auto it = parent.FindMember(name);
    return it != parent.MemberEnd() ? &it->value : nullptr;
}

inline void readBool(const rapidjson::Value& parent, const char* name, bool& out) {
    if (const rapidjson::Value* v = member(parent, name); v != nullptr && v->IsBool()) out = v->GetBool();
}

inline void readUint(const rapidjson::Value& parent, const char* name, uint32_t& out) {
    if (const rapidjson::Value* v = member(parent, name); v != nullptr && v->IsUint()) out = v->GetUint();
}

inline void readFloat(const rapidjson::Value& parent, const char* name, float& out) {
    if (const rapidjson::Value* v = member(parent, name); v != nullptr && v->IsNumber()) out = v->GetFloat();
}

inline void readString(const rapidjson::Value& parent, const char* name, std::string& out) {
    if (const rapidjson::Value* v = member(parent, name); v != nullptr && v->IsString()) out = v->GetString();
}

/// N floats from an array of exactly N. A shorter or longer array is a different
/// quantity than the one asked for, so it keeps the default rather than filling what
/// it can -- half a colour is not a colour.
template <int N> void readVec(const rapidjson::Value& parent, const char* name, float* out) {
    const rapidjson::Value* v = member(parent, name);
    if (v == nullptr || !v->IsArray() || v->Size() != static_cast<rapidjson::SizeType>(N)) return;
    for (rapidjson::SizeType i = 0; i < static_cast<rapidjson::SizeType>(N); ++i) {
        if ((*v)[i].IsNumber()) out[i] = (*v)[i].GetFloat();
    }
}

/// Degrees in the file, radians in the struct. Angles are the one quantity in these
/// schemas an author writes by eye, and nobody writes 0.3490658.
inline void readAngleDegrees(const rapidjson::Value& parent, const char* name, float& outRadians) {
    if (const rapidjson::Value* v = member(parent, name); v != nullptr && v->IsNumber()) {
        outRadians = v->GetFloat() * 0.01745329252f;
    }
}

/**
 * @brief Point `json` at the JSON of a glTF document, unwrapping a `.glb` container.
 *
 * A GLB is a 12-byte header -- magic, version, total length -- followed by chunks of
 * (length, type, payload), of which chunk 0 is required to be the JSON. Fifteen lines to
 * handle it, against the alternative of a stated limitation that would read as "extras
 * work except in the container half the world ships".
 *
 * **The Rule of Threes, met rather than anticipated.** S3.1 wrote this in
 * `ParticleSystem.cpp` and S4.2 copied it into `Collider.cpp` with a comment saying the
 * duplication was deliberate, because two occurrences are a coincidence. S5.2's
 * `AudioSource.cpp` is the third, and the third is what moves it. It lands here rather
 * than in a header of its own for the reason the readers above landed here: this is the
 * file all three callers already include, and a fourth header holding one function would
 * be a rung of scope nobody asked for.
 *
 * Not `readX`-shaped and deliberately not: those five leave `out` alone and say nothing,
 * because what an absent key means is the caller's decision. Bytes that are not a glTF
 * document at all are a failure every caller agrees about, so this one returns a verdict.
 */
inline bool gltfJsonSpan(const void* data, size_t length, const char*& json, size_t& jsonLength) {
    const auto* bytes = static_cast<const unsigned char*>(data);
    if (length >= 4 && std::memcmp(bytes, "glTF", 4) == 0) {
        if (length < 20) return false;
        uint32_t chunkLength = 0;
        uint32_t chunkType = 0;
        std::memcpy(&chunkLength, bytes + 12, 4);
        std::memcpy(&chunkType, bytes + 16, 4);
        // 0x4E4F534A is 'JSON' little-endian. A GLB whose first chunk is anything else
        // is malformed by the spec, so this is a check rather than a search.
        if (chunkType != 0x4E4F534Au || static_cast<size_t>(chunkLength) + 20 > length) return false;
        json = reinterpret_cast<const char*>(bytes) + 20;
        jsonLength = chunkLength;
        return true;
    }
    json = static_cast<const char*>(data);
    jsonLength = length;
    return length > 0;
}

/**
 * @brief One glTF document, parsed once, with its `nodes` array to hand (C14).
 *
 * **This exists because three extras readers used to parse the whole file each.**
 * `parseSceneEmitters`, `parseSceneColliders` and `parseSceneAudioSources` each built a
 * complete copying `rapidjson::Document` over the same bytes to read one key out of
 * `nodes[].extras` -- three full passes, and C13 measured what that cost: about 57 ms of
 * a 79 ms load on an 8000-node scene, which is 73% of the whole thing and more than
 * everything fastgltf does put together.
 *
 * It lands beside `gltfJsonSpan` rather than under `scene/`, where all three callers now
 * live, and that is a deliberate exception to the narrowest-scope rule: `gltfJsonSpan` is
 * the first half of this same operation, and splitting a two-step glTF-JSON entry point
 * across two directories to satisfy a rule about reach is the rule misapplied.
 *
 * Move-only, because `rapidjson::Document` is. Nobody needs a copy: it is built once per
 * load, on the stack, and handed to three readers by reference.
 */
struct GltfDocument {
    rapidjson::Document doc;
    /// The document's `nodes` array, or nullptr when it has none -- which is a valid
    /// glTF, so the readers treat it as "no nodes" rather than as a failure.
    const rapidjson::Value* nodes = nullptr;

    /// @return false only when the bytes are not a glTF document at all. A document
    ///         without a `nodes` array parses successfully and leaves `nodes` null.
    [[nodiscard]] bool parse(const void* data, size_t length) {
        const char* json = nullptr;
        size_t jsonLength = 0;
        if (data == nullptr || !gltfJsonSpan(data, length, json, jsonLength)) return false;

        // The copying parser, not `ParseInsitu`: the caller's buffer is a memory-mapped
        // file in the only caller there is, and writing terminators into it would be a
        // segmentation fault rather than a wrong answer.
        doc.Parse(json, jsonLength);
        if (doc.HasParseError() || !doc.IsObject()) return false;

        const rapidjson::Value* found = member(doc, "nodes");
        nodes = (found != nullptr && found->IsArray()) ? found : nullptr;
        return true;
    }
};

} // namespace json

} // namespace core
