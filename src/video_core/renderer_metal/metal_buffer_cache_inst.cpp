// SPDX-License-Identifier: GPL-3.0-or-later
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/renderer_metal/metal_buffer_cache.h"
template class VideoCommon::BufferCache<NativeMetal::BufferCacheParams>;
