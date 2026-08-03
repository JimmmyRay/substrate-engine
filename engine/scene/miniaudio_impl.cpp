// Single translation unit that instantiates miniaudio (S5.1).
//
// The same arrangement `vma_impl.cpp` and `stb_impl.cpp` use, and for the same reason:
// miniaudio ships as one header that is a declaration in every translation unit and a
// definition in exactly one. It is submoduled rather than vendored because it has a
// repository and tags to pin -- unlike RenderDoc's header, which upstream means to be
// copied.
//
// Encoding is switched off. Nothing in this engine writes an audio file, and leaving it
// on compiles a WAV encoder into every build to be called by nobody.
#define MA_NO_ENCODING

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wtype-limits"
#endif

#define MA_IMPLEMENTATION
#include <miniaudio.h>

#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
