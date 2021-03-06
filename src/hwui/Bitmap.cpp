/*
 * Copyright (C) 2015 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "Bitmap.h"

#include "Caches.h"
#include "utils/Color.h"

#include <sys/mman.h>

#include <log/log.h>
#include <cutils/ashmem.h>

#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <ui/PixelFormat.h>

#include <SkCanvas.h>

namespace android {

static bool computeAllocationSize(size_t rowBytes, int height, size_t* size) {
    int32_t rowBytes32 = SkToS32(rowBytes);
    int64_t bigSize = (int64_t) height * rowBytes32;
    if (rowBytes32 < 0 || !sk_64_isS32(bigSize)) {
        return false; // allocation will be too large
    }

    *size = sk_64_asS32(bigSize);
    return true;
}

typedef sk_sp<Bitmap> (*AllocPixeRef)(size_t allocSize, const SkImageInfo& info, size_t rowBytes,
        SkColorTable* ctable);

static sk_sp<Bitmap> allocateBitmap(SkBitmap* bitmap, SkColorTable* ctable, AllocPixeRef alloc) {
    const SkImageInfo& info = bitmap->info();
    if (info.colorType() == kUnknown_SkColorType) {
        LOG_ALWAYS_FATAL("unknown bitmap configuration");
        return nullptr;
    }

    size_t size;

    // we must respect the rowBytes value already set on the bitmap instead of
    // attempting to compute our own.
    const size_t rowBytes = bitmap->rowBytes();
    if (!computeAllocationSize(rowBytes, bitmap->height(), &size)) {
        return nullptr;
    }

    auto wrapper = alloc(size, info, rowBytes, ctable);
    if (wrapper) {
        wrapper->getSkBitmap(bitmap);
        // since we're already allocated, we lockPixels right away
        // HeapAllocator behaves this way too
        bitmap->lockPixels();
    }
    return wrapper;
}

sk_sp<Bitmap> Bitmap::allocateAshmemBitmap(SkBitmap* bitmap, SkColorTable* ctable) {
   return allocateBitmap(bitmap, ctable, &Bitmap::allocateAshmemBitmap);
}

static sk_sp<Bitmap> allocateHeapBitmap(size_t size, const SkImageInfo& info, size_t rowBytes,
        SkColorTable* ctable) {
    void* addr = calloc(size, 1);
    if (!addr) {
        return nullptr;
    }
    return sk_sp<Bitmap>(new Bitmap(addr, size, info, rowBytes, ctable));
}

#define FENCE_TIMEOUT 2000000000

// TODO: handle SRGB sanely
static PixelFormat internalFormatToPixelFormat(GLint internalFormat) {
    switch (internalFormat) {
    case GL_LUMINANCE:
        return PIXEL_FORMAT_RGBA_8888;
    case GL_SRGB8_ALPHA8:
        return PIXEL_FORMAT_RGBA_8888;
    case GL_RGBA:
        return PIXEL_FORMAT_RGBA_8888;
    case GL_RGB:
        return PIXEL_FORMAT_RGB_565;
    case GL_RGBA16F:
        return PIXEL_FORMAT_RGBA_FP16;
    default:
        LOG_ALWAYS_FATAL("Unsupported bitmap colorType: %d", internalFormat);
        return PIXEL_FORMAT_UNKNOWN;
    }
}

// class AutoEglFence {
// public:
//     AutoEglFence(EGLDisplay display)
//             : mDisplay(display) {
//         fence = eglCreateSyncKHR(mDisplay, EGL_SYNC_FENCE_KHR, NULL);
//     }
//
//     ~AutoEglFence() {
//         if (fence != EGL_NO_SYNC_KHR) {
//             eglDestroySyncKHR(mDisplay, fence);
//         }
//     }
//
//     EGLSyncKHR fence = EGL_NO_SYNC_KHR;
// private:
//     EGLDisplay mDisplay = EGL_NO_DISPLAY;
// };
//
// class AutoEglImage {
// public:
//     AutoEglImage(EGLDisplay display, EGLClientBuffer clientBuffer)
//             : mDisplay(display) {
//         EGLint imageAttrs[] = { EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE };
//         image = eglCreateImageKHR(display, EGL_NO_CONTEXT,
//                 EGL_NATIVE_BUFFER_ANDROID, clientBuffer, imageAttrs);
//     }
//
//     ~AutoEglImage() {
//         if (image != EGL_NO_IMAGE_KHR) {
//             eglDestroyImageKHR(mDisplay, image);
//         }
//     }
//
//     EGLImageKHR image = EGL_NO_IMAGE_KHR;
// private:
//     EGLDisplay mDisplay = EGL_NO_DISPLAY;
// };

class AutoGlTexture {
public:
    AutoGlTexture(uirenderer::Caches& caches)
            : mCaches(caches) {
        glGenTextures(1, &mTexture);
        caches.textureState().bindTexture(mTexture);
    }

    ~AutoGlTexture() {
        mCaches.textureState().deleteTexture(mTexture);
    }

private:
    uirenderer::Caches& mCaches;
    GLuint mTexture = 0;
};

sk_sp<Bitmap> Bitmap::allocateHeapBitmap(const SkImageInfo& info) {
    size_t size;
    if (!computeAllocationSize(info.minRowBytes(), info.height(), &size)) {
        LOG_ALWAYS_FATAL("trying to allocate too large bitmap");
        return nullptr;
    }
    return android::allocateHeapBitmap(size, info, info.minRowBytes(), nullptr);
}

sk_sp<Bitmap> Bitmap::allocateAshmemBitmap(size_t size, const SkImageInfo& info,
        size_t rowBytes, SkColorTable* ctable) {
    // Create new ashmem region with read/write priv
    int fd = ashmem_create_region("bitmap", size);
    if (fd < 0) {
        return nullptr;
    }

    void* addr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (addr == MAP_FAILED) {
        close(fd);
        return nullptr;
    }

    if (ashmem_set_prot_region(fd, PROT_READ) < 0) {
        munmap(addr, size);
        close(fd);
        return nullptr;
    }
    return sk_sp<Bitmap>(new Bitmap(addr, fd, size, info, rowBytes, ctable));
}

void FreePixelRef(void* addr, void* context) {
    auto pixelRef = (SkPixelRef*) context;
    pixelRef->unlockPixels();
    pixelRef->unref();
}

sk_sp<Bitmap> Bitmap::createFrom(const SkImageInfo& info, SkPixelRef& pixelRef) {
    pixelRef.ref();
    pixelRef.lockPixels();
    return sk_sp<Bitmap>(new Bitmap((void*) pixelRef.pixels(), (void*) &pixelRef, FreePixelRef,
            info, pixelRef.rowBytes(), pixelRef.colorTable()));
}

// sk_sp<Bitmap> Bitmap::createFrom(sp<GraphicBuffer> graphicBuffer) {
//     PixelFormat format = graphicBuffer->getPixelFormat();
//     if (!graphicBuffer.get() ||
//             (format != PIXEL_FORMAT_RGBA_8888 && format != PIXEL_FORMAT_RGBA_FP16)) {
//         return nullptr;
//     }
//     SkImageInfo info = SkImageInfo::Make(graphicBuffer->getWidth(), graphicBuffer->getHeight(),
//             kRGBA_8888_SkColorType, kPremul_SkAlphaType,
//             SkColorSpace::MakeSRGB());
//     return sk_sp<Bitmap>(new Bitmap(graphicBuffer.get(), info));
// }

void Bitmap::setColorSpace(sk_sp<SkColorSpace> colorSpace) {
    // TODO: See todo in reconfigure() below
    SkImageInfo* myInfo = const_cast<SkImageInfo*>(&this->info());
    *myInfo = info().makeColorSpace(std::move(colorSpace));
}

void Bitmap::reconfigure(const SkImageInfo& newInfo, size_t rowBytes, SkColorTable* ctable) {
    if (kIndex_8_SkColorType != newInfo.colorType()) {
        ctable = nullptr;
    }
    mRowBytes = rowBytes;
    if (mColorTable.get() != ctable) {
        mColorTable.reset(SkSafeRef(ctable));
    }

    // Need to validate the alpha type to filter against the color type
    // to prevent things like a non-opaque RGB565 bitmap
    SkAlphaType alphaType;
    LOG_ALWAYS_FATAL_IF(!SkColorTypeValidateAlphaType(
            newInfo.colorType(), newInfo.alphaType(), &alphaType),
            "Failed to validate alpha type!");

    // Dirty hack is dirty
    // TODO: Figure something out here, Skia's current design makes this
    // really hard to work with. Skia really, really wants immutable objects,
    // but with the nested-ref-count hackery going on that's just not
    // feasible without going insane trying to figure it out
    SkImageInfo* myInfo = const_cast<SkImageInfo*>(&this->info());
    *myInfo = newInfo;
    changeAlphaType(alphaType);

    // Docs say to only call this in the ctor, but we're going to call
    // it anyway even if this isn't always the ctor.
    // TODO: Fix this too as part of the above TODO
    setPreLocked(getStorage(), mRowBytes, mColorTable.get());
}

Bitmap::Bitmap(void* address, size_t size, const SkImageInfo& info, size_t rowBytes, SkColorTable* ctable)
            : SkPixelRef(info)
            , mPixelStorageType(PixelStorageType::Heap) {
    mPixelStorage.heap.address = address;
    mPixelStorage.heap.size = size;
    reconfigure(info, rowBytes, ctable);
}

Bitmap::Bitmap(void* address, void* context, FreeFunc freeFunc,
                const SkImageInfo& info, size_t rowBytes, SkColorTable* ctable)
            : SkPixelRef(info)
            , mPixelStorageType(PixelStorageType::External) {
    mPixelStorage.external.address = address;
    mPixelStorage.external.context = context;
    mPixelStorage.external.freeFunc = freeFunc;
    reconfigure(info, rowBytes, ctable);
}

Bitmap::Bitmap(void* address, int fd, size_t mappedSize,
                const SkImageInfo& info, size_t rowBytes, SkColorTable* ctable)
            : SkPixelRef(info)
            , mPixelStorageType(PixelStorageType::Ashmem) {
    mPixelStorage.ashmem.address = address;
    mPixelStorage.ashmem.fd = fd;
    mPixelStorage.ashmem.size = mappedSize;
    reconfigure(info, rowBytes, ctable);
}

// Bitmap::Bitmap(GraphicBuffer* buffer, const SkImageInfo& info)
//         : SkPixelRef(info)
//         , mPixelStorageType(PixelStorageType::Hardware) {
//     mPixelStorage.hardware.buffer = buffer;
//     buffer->incStrong(buffer);
//     mRowBytes = bytesPerPixel(buffer->getPixelFormat()) * buffer->getStride();
// }

Bitmap::~Bitmap() {
    switch (mPixelStorageType) {
    case PixelStorageType::External:
        mPixelStorage.external.freeFunc(mPixelStorage.external.address,
                mPixelStorage.external.context);
        break;
    case PixelStorageType::Ashmem:
        munmap(mPixelStorage.ashmem.address, mPixelStorage.ashmem.size);
        close(mPixelStorage.ashmem.fd);
        break;
    case PixelStorageType::Heap:
        free(mPixelStorage.heap.address);
        break;
        break;

    }

    // android::uirenderer::renderthread::RenderProxy::onBitmapDestroyed(getGenerationID());
}

bool Bitmap::hasHardwareMipMap() const {
    return mHasHardwareMipMap;
}

void Bitmap::setHasHardwareMipMap(bool hasMipMap) {
    mHasHardwareMipMap = hasMipMap;
}

void* Bitmap::getStorage() const {
    switch (mPixelStorageType) {
    case PixelStorageType::External:
        return mPixelStorage.external.address;
    case PixelStorageType::Ashmem:
        return mPixelStorage.ashmem.address;
    case PixelStorageType::Heap:
        return mPixelStorage.heap.address;
    case PixelStorageType::Hardware:
        return nullptr;
    }
}

bool Bitmap::onNewLockPixels(LockRec* rec) {
    rec->fPixels = getStorage();
    rec->fRowBytes = mRowBytes;
    rec->fColorTable = mColorTable.get();
    return true;
}

size_t Bitmap::getAllocatedSizeInBytes() const {
    return info().getSafeSize(mRowBytes);
}

int Bitmap::getAshmemFd() const {
    switch (mPixelStorageType) {
    case PixelStorageType::Ashmem:
        return mPixelStorage.ashmem.fd;
    default:
        return -1;
    }
}

size_t Bitmap::getAllocationByteCount() const {
    switch (mPixelStorageType) {
    case PixelStorageType::Heap:
        return mPixelStorage.heap.size;
    default:
        return rowBytes() * height();
    }
}

void Bitmap::reconfigure(const SkImageInfo& info) {
    reconfigure(info, info.minRowBytes(), nullptr);
}

void Bitmap::setAlphaType(SkAlphaType alphaType) {
    if (!SkColorTypeValidateAlphaType(info().colorType(), alphaType, &alphaType)) {
        return;
    }

    changeAlphaType(alphaType);
}

void Bitmap::getSkBitmap(SkBitmap* outBitmap) {
    outBitmap->setInfo(info(), rowBytes());
    outBitmap->setPixelRef(this);
}

void Bitmap::getSkBitmapForShaders(SkBitmap* outBitmap) {
    if (isHardware() && uirenderer::Properties::isSkiaEnabled()) {
        getSkBitmap(outBitmap);
    } else {
        outBitmap->setInfo(info(), rowBytes());
        outBitmap->setPixelRef(this);
    }
}

void Bitmap::getBounds(SkRect* bounds) const {
    SkASSERT(bounds);
    bounds->set(0, 0, SkIntToScalar(info().width()), SkIntToScalar(info().height()));
}

} // namespace android
