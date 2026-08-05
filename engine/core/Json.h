#pragma once

#include <rapidjson/document.h>

#include <cstdint>
#include <cstring>
#include <string>

namespace core {

/**
 * @file Json.h
 * @brief The "absent or the wrong type keeps the default" readers, shared.
 *
 * None of these carries a policy: they leave `out` untouched and say nothing. Adding a
 * refusal here couples callers that disagree -- a config that fails to parse is refused
 * outright, a scene declaring no emitter is Sponza.
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

/// N floats from an array of exactly N. A shorter or longer array keeps the default rather
/// than filling what it can: half a colour is not a colour.
template <int N> void readVec(const rapidjson::Value& parent, const char* name, float* out) {
    const rapidjson::Value* v = member(parent, name);
    if (v == nullptr || !v->IsArray() || v->Size() != static_cast<rapidjson::SizeType>(N)) return;
    for (rapidjson::SizeType i = 0; i < static_cast<rapidjson::SizeType>(N); ++i) {
        if ((*v)[i].IsNumber()) out[i] = (*v)[i].GetFloat();
    }
}

/// Degrees in the file, radians in the struct. Reading one with `readFloat` instead lands
/// degrees in a field the maths treats as radians.
inline void readAngleDegrees(const rapidjson::Value& parent, const char* name, float& outRadians) {
    if (const rapidjson::Value* v = member(parent, name); v != nullptr && v->IsNumber()) {
        outRadians = v->GetFloat() * 0.01745329252f;
    }
}

/// @brief Point `json` at the JSON of a glTF document, unwrapping a `.glb` container.
///
/// A GLB is a 12-byte header followed by chunks of (length, type, payload), of which chunk
/// 0 is required to be the JSON.
///
/// Returns a verdict where the `readX` readers above return nothing, because bytes that are
/// not a glTF document at all are a failure every caller agrees about.
inline bool gltfJsonSpan(const void* data, size_t length, const char*& json, size_t& jsonLength) {
    const auto* bytes = static_cast<const unsigned char*>(data);
    if (length >= 4 && std::memcmp(bytes, "glTF", 4) == 0) {
        if (length < 20) return false;
        uint32_t chunkLength = 0;
        uint32_t chunkType = 0;
        std::memcpy(&chunkLength, bytes + 12, 4);
        std::memcpy(&chunkType, bytes + 16, 4);
        // 0x4E4F534A is 'JSON' little-endian. The spec requires chunk 0 to be it, so this
        // is a check rather than a search.
        if (chunkType != 0x4E4F534Au || static_cast<size_t>(chunkLength) + 20 > length) return false;
        json = reinterpret_cast<const char*>(bytes) + 20;
        jsonLength = chunkLength;
        return true;
    }
    json = static_cast<const char*>(data);
    jsonLength = length;
    return length > 0;
}

/// @brief One glTF document, parsed once, with its `nodes` array to hand.
///
/// Every extras reader has to share one of these. A reader that parses the bytes itself
/// costs a whole extra copying pass -- on an 8000-node scene that was 57 ms of a 79 ms
/// load, per reader.
struct GltfDocument {
    rapidjson::Document doc;
    /// The document's `nodes` array, or nullptr when it has none -- which is a valid glTF,
    /// so a reader must treat it as "no nodes" rather than as a failure.
    const rapidjson::Value* nodes = nullptr;

    /// @return false only when the bytes are not a glTF document at all. A document
    ///         without a `nodes` array parses successfully and leaves `nodes` null.
    [[nodiscard]] bool parse(const void* data, size_t length) {
        const char* json = nullptr;
        size_t jsonLength = 0;
        if (data == nullptr || !gltfJsonSpan(data, length, json, jsonLength)) return false;

        // The copying parser, never `ParseInsitu`: the caller's buffer is a memory-mapped
        // file, and writing terminators into it is a segmentation fault.
        doc.Parse(json, jsonLength);
        if (doc.HasParseError() || !doc.IsObject()) return false;

        const rapidjson::Value* found = member(doc, "nodes");
        nodes = (found != nullptr && found->IsArray()) ? found : nullptr;
        return true;
    }
};

} // namespace json

} // namespace core
