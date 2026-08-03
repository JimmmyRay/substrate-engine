#pragma once

#include "core/Json.h"

#include <cstddef>
#include <string>
#include <vector>

/**
 * @file tests/GltfExtras.h
 * @brief Parse a glTF document and hand its nodes to one extras reader.
 *
 * C14 stopped `parseSceneEmitters`, `parseSceneColliders` and `parseSceneAudioSources`
 * each parsing the whole file, so they now take a `nodes` array the loader parsed once.
 * A test that wants one of them has to do that parse -- and three suites want one, which
 * is the Rule of Threes and why this is a header rather than a copy in each.
 *
 * It also draws the line C14 drew in the engine: **"are these bytes a glTF document" is
 * now one question with one implementation**, so the malformed-input cases belong to
 * `GltfDocument::parse` and the suites below test extraction rather than parsing.
 */
namespace testing_extras {

/// Parse `data` and run `reader` over its nodes.
/// @return false when the bytes are not a glTF document. A valid document with no `nodes`
///         array succeeds and leaves `out` untouched, which is what the loader does.
template <typename Out, typename Fn>
bool parseNodes(const void* data, size_t length, Out& out, Fn reader) {
    core::json::GltfDocument document;
    if (!document.parse(data, length)) return false;
    if (document.nodes == nullptr) return true;
    return reader(*document.nodes, out);
}

template <typename Out, typename Fn>
bool parseNodes(const std::string& json, Out& out, Fn reader) {
    return parseNodes(json.data(), json.size(), out, reader);
}

template <typename Out, typename Fn>
bool parseNodes(const std::vector<unsigned char>& bytes, Out& out, Fn reader) {
    return parseNodes(bytes.data(), bytes.size(), out, reader);
}

} // namespace testing_extras
