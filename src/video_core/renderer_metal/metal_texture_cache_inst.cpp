// SPDX-License-Identifier: GPL-3.0-or-later
#include "video_core/renderer_metal/metal_texture_cache.h"
#include "video_core/texture_cache/texture_cache.h"
template class VideoCommon::TextureCache<NativeMetal::TextureCacheParams>;
// Member templates are not instantiated by the class instantiation above.
template void VideoCommon::TextureCache<NativeMetal::TextureCacheParams>::FillGraphicsImageViews<
    true>(std::span<VideoCommon::ImageViewInOut>);
template void VideoCommon::TextureCache<NativeMetal::TextureCacheParams>::FillGraphicsImageViews<
    false>(std::span<VideoCommon::ImageViewInOut>);
