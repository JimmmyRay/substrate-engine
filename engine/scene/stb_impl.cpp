// Single translation unit that instantiates the header-only stb libraries.
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wcast-qual"
#endif

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

// stb_image_write backs frame capture (5.2) and the difference image the golden-image
// comparison (5.3) writes. PNG only -- the other encoders it offers are dead weight in
// a binary that captures screenshots.
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

// stb_truetype rasterises the optional debug font (render.debugFont). The embedded
// bitmap font needs none of this, so a build with no TTF configured still links it
// and simply never calls it.
#define STB_TRUETYPE_IMPLEMENTATION
#include <stb_truetype.h>

#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
