/* -*- Mode: C++; tab-width: 20; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "WebGLTexture.h"

#include <algorithm>

#include "CanvasUtils.h"
#include "gfxPrefs.h"
#include "GLBlitHelper.h"
#include "GLContext.h"
#include "mozilla/gfx/2D.h"
#include "mozilla/dom/HTMLVideoElement.h"
#include "mozilla/dom/ImageData.h"
#include "mozilla/MathAlgorithms.h"
#include "mozilla/Scoped.h"
#include "mozilla/unused.h"
#include "ScopedGLHelpers.h"
#include "TexUnpackBlob.h"
#include "WebGLContext.h"
#include "WebGLContextUtils.h"
#include "WebGLFramebuffer.h"
#include "WebGLTexelConversions.h"

namespace mozilla {

/* This file handles:
 * TexStorage2D(texTarget, levels, internalFormat, width, height)
 * TexStorage3D(texTarget, levels, intenralFormat, width, height, depth)
 *
 * TexImage2D(texImageTarget, level, internalFormat, width, height, border, unpackFormat,
 *            unpackType, data)
 * TexImage3D(texImageTarget, level, internalFormat, width, height, depth, border,
 *            unpackFormat, unpackType, data)
 * TexSubImage2D(texImageTarget, level, xOffset, yOffset, width, height, unpackFormat,
 *               unpackType, data)
 * TexSubImage3D(texImageTarget, level, xOffset, yOffset, zOffset, width, height, depth,
 *               unpackFormat, unpackType, data)
 *
 * CompressedTexImage2D(texImageTarget, level, internalFormat, width, height, border,
 *                      imageSize, data)
 * CompressedTexImage3D(texImageTarget, level, internalFormat, width, height, depth,
 *                      border, imageSize, data)
 * CompressedTexSubImage2D(texImageTarget, level, xOffset, yOffset, width, height,
 *                         sizedUnpackFormat, imageSize, data)
 * CompressedTexSubImage3D(texImageTarget, level, xOffset, yOffset, zOffset, width,
 *                         height, depth, sizedUnpackFormat, imageSize, data)
 *
 * CopyTexImage2D(texImageTarget, level, internalFormat, x, y, width, height, border)
 * CopyTexImage3D - "Because the framebuffer is inhererntly two-dimensional, there is no
 *                   CopyTexImage3D command."
 * CopyTexSubImage2D(texImageTarget, level, xOffset, yOffset, x, y, width, height)
 * CopyTexSubImage3D(texImageTarget, level, xOffset, yOffset, zOffset, x, y, width,
 *                   height)
 */

static bool
ValidateExtents(WebGLContext* webgl, const char* funcName, GLsizei width, GLsizei height,
                GLsizei depth, GLint border, uint32_t* const out_width,
                uint32_t* const out_height, uint32_t* const out_depth)
{
    // Check border
    if (border != 0) {
        webgl->ErrorInvalidValue("%s: `border` must be 0.", funcName);
        return false;
    }

    if (width < 0 || height < 0 || depth < 0) {
        /* GL ES Version 2.0.25 - 3.7.1 Texture Image Specification
         *   "If wt and ht are the specified image width and height,
         *   and if either wt or ht are less than zero, then the error
         *   INVALID_VALUE is generated."
         */
        webgl->ErrorInvalidValue("%s: `width`/`height`/`depth` must be >= 0.", funcName);
        return false;
    }

    *out_width = width;
    *out_height = height;
    *out_depth = depth;
    return true;
}

////////////////////////////////////////
// ArrayBufferView?

static inline bool
DoesJSTypeMatchUnpackType(GLenum unpackType, js::Scalar::Type jsType)
{
    switch (unpackType) {
    case LOCAL_GL_BYTE:
        return jsType == js::Scalar::Type::Int8;

    case LOCAL_GL_UNSIGNED_BYTE:
        return jsType == js::Scalar::Type::Uint8 ||
               jsType == js::Scalar::Type::Uint8Clamped;

    case LOCAL_GL_SHORT:
        return jsType == js::Scalar::Type::Int16;

    case LOCAL_GL_UNSIGNED_SHORT:
    case LOCAL_GL_UNSIGNED_SHORT_4_4_4_4:
    case LOCAL_GL_UNSIGNED_SHORT_5_5_5_1:
    case LOCAL_GL_UNSIGNED_SHORT_5_6_5:
    case LOCAL_GL_HALF_FLOAT:
    case LOCAL_GL_HALF_FLOAT_OES:
        return jsType == js::Scalar::Type::Uint16;

    case LOCAL_GL_INT:
        return jsType == js::Scalar::Type::Int32;

    case LOCAL_GL_UNSIGNED_INT:
    case LOCAL_GL_UNSIGNED_INT_2_10_10_10_REV:
    case LOCAL_GL_UNSIGNED_INT_10F_11F_11F_REV:
    case LOCAL_GL_UNSIGNED_INT_5_9_9_9_REV:
    case LOCAL_GL_UNSIGNED_INT_24_8:
        return jsType == js::Scalar::Type::Uint32;

    case LOCAL_GL_FLOAT:
        return jsType == js::Scalar::Type::Float32;

    default:
        return false;
    }
}

bool
WebGLContext::ValidateUnpackPixels(const char* funcName, uint32_t fullRows,
                                   uint32_t tailPixels, webgl::TexUnpackBlob* blob)
{
    auto skipPixels = CheckedUint32(blob->mSkipPixels);
    skipPixels += CheckedUint32(blob->mSkipRows);

    const auto usedPixelsPerRow = CheckedUint32(blob->mSkipPixels) + blob->mWidth;
    if (!usedPixelsPerRow.isValid() || usedPixelsPerRow.value() > blob->mRowLength) {
        ErrorInvalidOperation("%s: UNPACK_SKIP_PIXELS + height > UNPACK_ROW_LENGTH.",
                              funcName);
        return false;
    }

    if (blob->mHeight > blob->mImageHeight) {
        ErrorInvalidOperation("%s: height > UNPACK_IMAGE_HEIGHT.", funcName);
        return false;
    }

    //////

    // The spec doesn't bound SKIP_ROWS + height <= IMAGE_HEIGHT, unfortunately.
    auto skipFullRows = CheckedUint32(blob->mSkipImages) * blob->mImageHeight;
    skipFullRows += blob->mSkipRows;

    MOZ_ASSERT(blob->mDepth >= 1);
    MOZ_ASSERT(blob->mHeight >= 1);
    auto usedFullRows = CheckedUint32(blob->mDepth - 1) * blob->mImageHeight;
    usedFullRows += blob->mHeight - 1; // Full rows in the final image, excluding the tail.

    const auto fullRowsNeeded = skipFullRows + usedFullRows;
    if (!fullRowsNeeded.isValid()) {
        ErrorOutOfMemory("%s: Invalid calculation for required row count.",
                         funcName);
        return false;
    }

    if (fullRows > fullRowsNeeded.value())
        return true;

    if (fullRows == fullRowsNeeded.value() && tailPixels >= usedPixelsPerRow.value()) {
        blob->mNeedsExactUpload = true;
        return true;
    }

    ErrorInvalidOperation("%s: Desired upload requires more data than is available: (%u"
                          " rows plus %u pixels needed, %u rows plus %u pixels"
                          " available)",
                          funcName, fullRowsNeeded.value(), usedPixelsPerRow.value(),
                          fullRows, tailPixels);
    return false;
}

static bool
ValidateUnpackBytes(WebGLContext* webgl, const char* funcName, uint32_t width,
                    uint32_t height, uint32_t depth, const webgl::PackingInfo& pi,
                    uint32_t byteCount, webgl::TexUnpackBlob* blob)
{
    const auto bytesPerPixel = webgl::BytesPerPixel(pi);
    const auto bytesPerRow = CheckedUint32(blob->mRowLength) * bytesPerPixel;
    const auto rowStride = RoundUpToMultipleOf(bytesPerRow, blob->mAlignment);

    const auto fullRows = byteCount / rowStride;
    if (!fullRows.isValid()) {
        webgl->ErrorOutOfMemory("%s: Unacceptable upload size calculated.");
        return false;
    }

    const auto bodyBytes = fullRows.value() * rowStride.value();
    const auto tailPixels = (byteCount - bodyBytes) / bytesPerPixel;

    return webgl->ValidateUnpackPixels(funcName, fullRows.value(), tailPixels, blob);
}

bool
WebGLContext::ValidateUnpackInfo(const char* funcName, bool usePBOs, GLenum format,
                                 GLenum type, webgl::PackingInfo* const out)
{
    if (usePBOs != bool(mBoundPixelUnpackBuffer)) {
        ErrorInvalidOperation("%s: PACK_BUFFER must be %s.", funcName,
                              (usePBOs ? "non-null" : "null"));
        return false;
    }

    if (!mFormatUsage->AreUnpackEnumsValid(format, type)) {
        ErrorInvalidEnum("%s: Invalid unpack format/type: 0x%04x/0x%04x", funcName,
                         format, type);
        return false;
    }

    out->format = format;
    out->type = type;
    return true;
}

void
WebGLTexture::TexOrSubImage(bool isSubImage, const char* funcName, TexImageTarget target,
                            GLint level, GLenum internalFormat, GLint xOffset,
                            GLint yOffset, GLint zOffset, GLsizei rawWidth,
                            GLsizei rawHeight, GLsizei rawDepth, GLint border,
                            GLenum unpackFormat, GLenum unpackType,
                            const dom::Nullable<dom::ArrayBufferView>& maybeView)
{
    uint32_t width, height, depth;
    if (!ValidateExtents(mContext, funcName, rawWidth, rawHeight, rawDepth, border,
                         &width, &height, &depth))
    {
        return;
    }

    const bool usePBOs = false;
    webgl::PackingInfo pi;
    if (!mContext->ValidateUnpackInfo(funcName, usePBOs, unpackFormat, unpackType, &pi))
        return;

    ////

    const uint8_t* bytes = nullptr;
    uint32_t byteCount = 0;

    if (!maybeView.IsNull()) {
        const auto& view = maybeView.Value();

        const auto jsType = JS_GetArrayBufferViewType(view.Obj());
        if (!DoesJSTypeMatchUnpackType(pi.type, jsType)) {
            mContext->ErrorInvalidOperation("%s: `pixels` not compatible with `type`.",
                                            funcName);
            return;
        }

        if (width && height && depth) {
            view.ComputeLengthAndData();

            bytes = view.DataAllowShared();
            byteCount = view.LengthAllowShared();
        }
    }

    const bool isClientData = true;
    webgl::TexUnpackBytes blob(mContext, target, width, height, depth, isClientData,
                               bytes);

    if (bytes &&
        !ValidateUnpackBytes(mContext, funcName, width, height, depth, pi, byteCount,
                             &blob))
    {
        return;
    }

    TexOrSubImageBlob(isSubImage, funcName, target, level, internalFormat, xOffset,
                      yOffset, zOffset, pi, &blob);
}

void
WebGLTexture::TexOrSubImage(bool isSubImage, const char* funcName, TexImageTarget target,
                            GLint level, GLenum internalFormat, GLint xOffset,
                            GLint yOffset, GLint zOffset, GLsizei rawWidth,
                            GLsizei rawHeight, GLsizei rawDepth, GLint border,
                            GLenum unpackFormat, GLenum unpackType,
                            WebGLsizeiptr offset)
{
    uint32_t width, height, depth;
    if (!ValidateExtents(mContext, funcName, rawWidth, rawHeight, rawDepth, border,
                         &width, &height, &depth))
    {
        return;
    }

    const bool usePBOs = true;
    webgl::PackingInfo pi;
    if (!mContext->ValidateUnpackInfo(funcName, usePBOs, unpackFormat, unpackType, &pi))
        return;

    ////

    if (offset < 0) {
        mContext->ErrorInvalidValue("%s: offset cannot be negative.", funcName);
        return;
    }

    const bool isClientData = false;
    const auto ptr = (const uint8_t*)offset;
    webgl::TexUnpackBytes blob(mContext, target, width, height, depth, isClientData, ptr);

    const auto& packBuffer = mContext->mBoundPixelUnpackBuffer;
    const auto bufferByteCount = packBuffer->ByteLength();

    uint32_t byteCount = 0;
    if (bufferByteCount >= offset) {
        byteCount = bufferByteCount - offset;
    }

    if (!ValidateUnpackBytes(mContext, funcName, width, height, depth, pi, byteCount,
                             &blob))
    {
        return;
    }

    TexOrSubImageBlob(isSubImage, funcName, target, level, internalFormat, xOffset,
                      yOffset, zOffset, pi, &blob);
}

////////////////////////////////////////
// ImageData

static already_AddRefed<gfx::DataSourceSurface>
FromImageData(WebGLContext* webgl, const char* funcName, GLenum unpackType,
              dom::ImageData* imageData, dom::Uint8ClampedArray* scopedArr)
{
    DebugOnly<bool> inited = scopedArr->Init(imageData->GetDataObject());
    MOZ_ASSERT(inited);

    scopedArr->ComputeLengthAndData();
    const DebugOnly<size_t> dataSize = scopedArr->Length();
    const void* const data = scopedArr->Data();

    const gfx::IntSize size(imageData->Width(), imageData->Height());
    const size_t stride = size.width * 4;
    const gfx::SurfaceFormat surfFormat = gfx::SurfaceFormat::R8G8B8A8;

    MOZ_ASSERT(dataSize == stride * size.height);

    uint8_t* wrappableData = (uint8_t*)data;

    RefPtr<gfx::DataSourceSurface> surf =
        gfx::Factory::CreateWrappingDataSourceSurface(wrappableData,
                                                      stride,
                                                      size,
                                                      surfFormat);
    if (!surf) {
        webgl->ErrorOutOfMemory("%s: OOM in FromImageData.", funcName);
        return nullptr;
    }

    return surf.forget();
}

void
WebGLTexture::TexOrSubImage(bool isSubImage, const char* funcName, TexImageTarget target,
                            GLint level, GLenum internalFormat, GLint xOffset,
                            GLint yOffset, GLint zOffset, GLenum unpackFormat,
                            GLenum unpackType, dom::ImageData* imageData)
{
    const bool usePBOs = false;
    webgl::PackingInfo pi;
    if (!mContext->ValidateUnpackInfo(funcName, usePBOs, unpackFormat, unpackType, &pi))
        return;

    if (!imageData) {
        // Spec says to generate an INVALID_VALUE error
        mContext->ErrorInvalidValue("%s: Null ImageData.", funcName);
        return;
    }

    // Eventually, these will be args.
    const uint32_t width = imageData->Width();
    const uint32_t height = imageData->Height();
    const uint32_t depth = 1;

    dom::RootedTypedArray<dom::Uint8ClampedArray> scopedArr(nsContentUtils::RootingCx());
    const RefPtr<gfx::DataSourceSurface> surf = FromImageData(mContext, funcName,
                                                              unpackType, imageData,
                                                              &scopedArr);
    if (!surf)
        return;

    // WhatWG "HTML Living Standard" (30 October 2015):
    // "The getImageData(sx, sy, sw, sh) method [...] Pixels must be returned as
    //  non-premultiplied alpha values."
    const bool isAlphaPremult = false;

    webgl::TexUnpackSurface blob(mContext, target, width, height, depth, surf,
                                 isAlphaPremult);

    const uint32_t fullRows = imageData->Height();
    const uint32_t tailPixels = 0;
    if (!mContext->ValidateUnpackPixels(funcName, fullRows, tailPixels, &blob))
        return;

    TexOrSubImageBlob(isSubImage, funcName, target, level, internalFormat, xOffset,
                      yOffset, zOffset, pi, &blob);
}

////////////////////////////////////////
// dom::Element

void
WebGLTexture::TexOrSubImage(bool isSubImage, const char* funcName, TexImageTarget target,
                            GLint level, GLenum internalFormat, GLint xOffset,
                            GLint yOffset, GLint zOffset, GLenum unpackFormat,
                            GLenum unpackType, dom::Element* elem,
                            ErrorResult* const out_error)
{
    const bool usePBOs = false;
    webgl::PackingInfo pi;
    if (!mContext->ValidateUnpackInfo(funcName, usePBOs, unpackFormat, unpackType, &pi))
        return;

    //////

    uint32_t flags = nsLayoutUtils::SFE_WANT_IMAGE_SURFACE |
                     nsLayoutUtils::SFE_USE_ELEMENT_SIZE_IF_VECTOR;

    if (mContext->mPixelStore_ColorspaceConversion == LOCAL_GL_NONE)
        flags |= nsLayoutUtils::SFE_NO_COLORSPACE_CONVERSION;

    if (!mContext->mPixelStore_PremultiplyAlpha)
        flags |= nsLayoutUtils::SFE_PREFER_NO_PREMULTIPLY_ALPHA;

    RefPtr<gfx::DrawTarget> idealDrawTarget = nullptr; // Don't care for now.
    auto sfer = nsLayoutUtils::SurfaceFromElement(elem, flags, idealDrawTarget);

    //////

    uint32_t elemWidth = 0;
    uint32_t elemHeight = 0;
    layers::Image* layersImage = nullptr;
    if (!gfxPrefs::WebGLDisableDOMBlitUploads() && sfer.mLayersImage) {
        layersImage = sfer.mLayersImage;
        elemWidth = layersImage->GetSize().width;
        elemHeight = layersImage->GetSize().height;
    }

    RefPtr<gfx::DataSourceSurface> dataSurf;
    if (!layersImage && sfer.GetSourceSurface()) {
        const auto surf = sfer.GetSourceSurface();
        elemWidth = surf->GetSize().width;
        elemHeight = surf->GetSize().height;

        // WARNING: OSX can lose our MakeCurrent here.
        dataSurf = surf->GetDataSurface();
    }

    //////

    // Eventually, these will be args.
    const uint32_t width = elemWidth;
    const uint32_t height = elemHeight;
    const uint32_t depth = 1;

    if (!layersImage && !dataSurf) {
        const bool isClientData = true;
        const webgl::TexUnpackBytes blob(mContext, target, width, height, depth,
                                         isClientData, nullptr);
        TexOrSubImageBlob(isSubImage, funcName, target, level, internalFormat, xOffset,
                          yOffset, zOffset, pi, &blob);
        return;
    }

    //////

    // While it's counter-intuitive, the shape of the SFEResult API means that we should
    // try to pull out a surface first, and then, if we do pull out a surface, check
    // CORS/write-only/etc..

    if (!sfer.mCORSUsed) {
        auto& srcPrincipal = sfer.mPrincipal;
        nsIPrincipal* dstPrincipal = mContext->GetCanvas()->NodePrincipal();

        if (!dstPrincipal->Subsumes(srcPrincipal)) {
            mContext->GenerateWarning("%s: Cross-origin elements require CORS.",
                                      funcName);
            out_error->Throw(NS_ERROR_DOM_SECURITY_ERR);
            return;
        }
    }

    if (sfer.mIsWriteOnly) {
        // mIsWriteOnly defaults to true, and so will be true even if SFE merely
        // failed. Thus we must test mIsWriteOnly after successfully retrieving an
        // Image or SourceSurface.
        mContext->GenerateWarning("%s: Element is write-only, thus cannot be"
                                  " uploaded.",
                                  funcName);
        out_error->Throw(NS_ERROR_DOM_SECURITY_ERR);
        return;
    }

    //////
    // Ok, we're good!

    UniquePtr<webgl::TexUnpackBlob> blob;
    const bool isAlphaPremult = sfer.mIsPremultiplied;

    if (layersImage) {
        blob.reset(new webgl::TexUnpackImage(mContext, target, width, height, depth,
                                             layersImage, isAlphaPremult));
    } else {
        MOZ_ASSERT(dataSurf);
        blob.reset(new webgl::TexUnpackSurface(mContext, target, width, height, depth,
                                               dataSurf, isAlphaPremult));
    }

    const uint32_t fullRows = elemHeight;
    const uint32_t tailPixels = 0;
    if (!mContext->ValidateUnpackPixels(funcName, fullRows, tailPixels, blob.get()))
        return;

    TexOrSubImageBlob(isSubImage, funcName, target, level, internalFormat, xOffset,
                      yOffset, zOffset, pi, blob.get());
}


//////////////////////////////////////////////////////////////////////////////////////////

void
WebGLTexture::TexOrSubImageBlob(bool isSubImage, const char* funcName,
                                TexImageTarget target, GLint level, GLenum internalFormat,
                                GLint xOffset, GLint yOffset, GLint zOffset,
                                const webgl::PackingInfo& pi,
                                const webgl::TexUnpackBlob* blob)
{
    if (isSubImage) {
        TexSubImage(funcName, target, level, xOffset, yOffset, zOffset, pi, blob);
    } else {
        TexImage(funcName, target, level, internalFormat, pi, blob);
    }
}

//////////////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////////////

static bool
ValidateTexImage(WebGLContext* webgl, WebGLTexture* texture, const char* funcName,
                 TexImageTarget target, GLint level,
                 WebGLTexture::ImageInfo** const out_imageInfo)
{
    // Check level
    if (level < 0) {
        webgl->ErrorInvalidValue("%s: `level` must be >= 0.", funcName);
        return false;
    }

    if (level >= WebGLTexture::kMaxLevelCount) {
        webgl->ErrorInvalidValue("%s: `level` is too large.", funcName);
        return false;
    }

    WebGLTexture::ImageInfo& imageInfo = texture->ImageInfoAt(target, level);

    *out_imageInfo = &imageInfo;
    return true;
}

// For *TexImage*
bool
WebGLTexture::ValidateTexImageSpecification(const char* funcName, TexImageTarget target,
                                            GLint level, uint32_t width, uint32_t height,
                                            uint32_t depth,
                                            WebGLTexture::ImageInfo** const out_imageInfo)
{
    if (mImmutable) {
        mContext->ErrorInvalidOperation("%s: Specified texture is immutable.", funcName);
        return false;
    }

    // Do this early to validate `level`.
    WebGLTexture::ImageInfo* imageInfo;
    if (!ValidateTexImage(mContext, this, funcName, target, level, &imageInfo))
        return false;

    if (mTarget == LOCAL_GL_TEXTURE_CUBE_MAP &&
        width != height)
    {
        mContext->ErrorInvalidValue("%s: Cube map images must be square.", funcName);
        return false;
    }

    /* GLES 3.0.4, p133-134:
     * GL_MAX_TEXTURE_SIZE is *not* the max allowed texture size. Rather, it is the
     * max (width/height) size guaranteed not to generate an INVALID_VALUE for too-large
     * dimensions. Sizes larger than GL_MAX_TEXTURE_SIZE *may or may not* result in an
     * INVALID_VALUE, or possibly GL_OOM.
     *
     * However, we have needed to set our maximums lower in the past to prevent resource
     * corruption. Therefore we have mImplMaxTextureSize, which is neither necessarily
     * lower nor higher than MAX_TEXTURE_SIZE.
     *
     * Note that mImplMaxTextureSize must be >= than the advertized MAX_TEXTURE_SIZE.
     * For simplicity, we advertize MAX_TEXTURE_SIZE as mImplMaxTextureSize.
     */

    uint32_t maxWidthHeight = 0;
    uint32_t maxDepth = 0;

    MOZ_ASSERT(level <= 31);
    switch (target.get()) {
    case LOCAL_GL_TEXTURE_2D:
        maxWidthHeight = mContext->mImplMaxTextureSize >> level;
        maxDepth = 1;
        break;

    case LOCAL_GL_TEXTURE_3D:
        maxWidthHeight = mContext->mImplMax3DTextureSize >> level;
        maxDepth = maxWidthHeight;
        break;

    case LOCAL_GL_TEXTURE_2D_ARRAY:
        maxWidthHeight = mContext->mImplMaxTextureSize >> level;
        // "The maximum number of layers for two-dimensional array textures (depth)
        //  must be at least MAX_ARRAY_TEXTURE_LAYERS for all levels."
        maxDepth = mContext->mImplMaxArrayTextureLayers;
        break;

    default: // cube maps
        MOZ_ASSERT(IsCubeMap());
        maxWidthHeight = mContext->mImplMaxCubeMapTextureSize >> level;
        maxDepth = 1;
        break;
    }

    if (width > maxWidthHeight ||
        height > maxWidthHeight ||
        depth > maxDepth)
    {
        mContext->ErrorInvalidValue("%s: Requested size at this level is unsupported.",
                                    funcName);
        return false;
    }

    {
        /* GL ES Version 2.0.25 - 3.7.1 Texture Image Specification
         *   "If level is greater than zero, and either width or
         *   height is not a power-of-two, the error INVALID_VALUE is
         *   generated."
         *
         * This restriction does not apply to GL ES Version 3.0+.
         */
        bool requirePOT = (!mContext->IsWebGL2() && level != 0);

        if (requirePOT) {
            if (!IsPowerOfTwo(width) || !IsPowerOfTwo(height)) {
                mContext->ErrorInvalidValue("%s: For level > 0, width and height must be"
                                            " powers of two.",
                                            funcName);
                return false;
            }
        }
    }

    *out_imageInfo = imageInfo;
    return true;
}

// For *TexSubImage*
bool
WebGLTexture::ValidateTexImageSelection(const char* funcName, TexImageTarget target,
                                        GLint level, GLint xOffset, GLint yOffset,
                                        GLint zOffset, uint32_t width, uint32_t height,
                                        uint32_t depth,
                                        WebGLTexture::ImageInfo** const out_imageInfo)
{
    // The conformance test wants bad arg checks before imageInfo checks.
    if (xOffset < 0 || yOffset < 0 || zOffset < 0) {
        mContext->ErrorInvalidValue("%s: Offsets must be >=0.", funcName);
        return false;
    }

    WebGLTexture::ImageInfo* imageInfo;
    if (!ValidateTexImage(mContext, this, funcName, target, level, &imageInfo))
        return false;

    if (!imageInfo->IsDefined()) {
        mContext->ErrorInvalidOperation("%s: The specified TexImage has not yet been"
                                        " specified.",
                                        funcName);
        return false;
    }

    const auto totalX = CheckedUint32(xOffset) + width;
    const auto totalY = CheckedUint32(yOffset) + height;
    const auto totalZ = CheckedUint32(zOffset) + depth;

    if (!totalX.isValid() || totalX.value() > imageInfo->mWidth ||
        !totalY.isValid() || totalY.value() > imageInfo->mHeight ||
        !totalZ.isValid() || totalZ.value() > imageInfo->mDepth)
    {
        mContext->ErrorInvalidValue("%s: Offset+size must be <= the size of the existing"
                                    " specified image.",
                                    funcName);
        return false;
    }

    *out_imageInfo = imageInfo;
    return true;
}

static bool
ValidateCompressedTexUnpack(WebGLContext* webgl, const char* funcName, GLsizei width,
                            GLsizei height, GLsizei depth,
                            const webgl::FormatInfo* format, size_t dataSize)
{
    auto compression = format->compression;

    auto bytesPerBlock = compression->bytesPerBlock;
    auto blockWidth = compression->blockWidth;
    auto blockHeight = compression->blockHeight;

    auto widthInBlocks = CheckedUint32(width) / blockWidth;
    auto heightInBlocks = CheckedUint32(height) / blockHeight;
    if (width % blockWidth) widthInBlocks += 1;
    if (height % blockHeight) heightInBlocks += 1;

    const CheckedUint32 blocksPerImage = widthInBlocks * heightInBlocks;
    const CheckedUint32 bytesPerImage = bytesPerBlock * blocksPerImage;
    const CheckedUint32 bytesNeeded = bytesPerImage * depth;

    if (!bytesNeeded.isValid()) {
        webgl->ErrorOutOfMemory("%s: Overflow while computing the needed buffer size.",
                                funcName);
        return false;
    }

    if (dataSize != bytesNeeded.value()) {
        webgl->ErrorInvalidValue("%s: Provided buffer's size must match expected size."
                                 " (needs %u, has %u)",
                                 funcName, bytesNeeded.value(), dataSize);
        return false;
    }

    return true;
}

static bool
DoChannelsMatchForCopyTexImage(const webgl::FormatInfo* srcFormat,
                               const webgl::FormatInfo* dstFormat)
{
    // GLES 3.0.4 p140 Table 3.16 "Valid CopyTexImage source framebuffer/destination
    // texture base internal format combinations."

    switch (srcFormat->unsizedFormat) {
    case webgl::UnsizedFormat::RGBA:
        switch (dstFormat->unsizedFormat) {
        case webgl::UnsizedFormat::A:
        case webgl::UnsizedFormat::L:
        case webgl::UnsizedFormat::LA:
        case webgl::UnsizedFormat::R:
        case webgl::UnsizedFormat::RG:
        case webgl::UnsizedFormat::RGB:
        case webgl::UnsizedFormat::RGBA:
            return true;
        default:
            return false;
        }

    case webgl::UnsizedFormat::RGB:
        switch (dstFormat->unsizedFormat) {
        case webgl::UnsizedFormat::L:
        case webgl::UnsizedFormat::R:
        case webgl::UnsizedFormat::RG:
        case webgl::UnsizedFormat::RGB:
            return true;
        default:
            return false;
        }

    case webgl::UnsizedFormat::RG:
        switch (dstFormat->unsizedFormat) {
        case webgl::UnsizedFormat::L:
        case webgl::UnsizedFormat::R:
        case webgl::UnsizedFormat::RG:
            return true;
        default:
            return false;
        }

    case webgl::UnsizedFormat::R:
        switch (dstFormat->unsizedFormat) {
        case webgl::UnsizedFormat::L:
        case webgl::UnsizedFormat::R:
            return true;
        default:
            return false;
        }

    default:
        return false;
    }
}

static bool
EnsureImageDataInitializedForUpload(WebGLTexture* tex, const char* funcName,
                                    TexImageTarget target, GLint level, GLint xOffset,
                                    GLint yOffset, GLint zOffset, uint32_t width,
                                    uint32_t height, uint32_t depth,
                                    WebGLTexture::ImageInfo* imageInfo,
                                    bool* const out_uploadWillInitialize)
{
    *out_uploadWillInitialize = false;

    if (!imageInfo->IsDataInitialized()) {
        const bool isFullUpload = (!xOffset && !yOffset && !zOffset &&
                                   width == imageInfo->mWidth &&
                                   height == imageInfo->mHeight &&
                                   depth == imageInfo->mDepth);
        if (isFullUpload) {
            *out_uploadWillInitialize = true;
        } else {
            WebGLContext* webgl = tex->mContext;
            webgl->GenerateWarning("%s: Texture has not been initialized prior to a"
                                   " partial upload, forcing the browser to clear it."
                                   " This may be slow.",
                                   funcName);
            if (!tex->InitializeImageData(funcName, target, level)) {
                MOZ_ASSERT(false, "Unexpected failure to init image data.");
                return false;
            }
        }
    }

    return true;
}

//////////////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////////////
// Actual calls

static inline GLenum
DoTexStorage(gl::GLContext* gl, TexTarget target, GLsizei levels, GLenum sizedFormat,
             GLsizei width, GLsizei height, GLsizei depth)
{
    gl::GLContext::LocalErrorScope errorScope(*gl);

    switch (target.get()) {
    case LOCAL_GL_TEXTURE_2D:
    case LOCAL_GL_TEXTURE_CUBE_MAP:
        MOZ_ASSERT(depth == 1);
        gl->fTexStorage2D(target.get(), levels, sizedFormat, width, height);
        break;

    case LOCAL_GL_TEXTURE_3D:
    case LOCAL_GL_TEXTURE_2D_ARRAY:
        gl->fTexStorage3D(target.get(), levels, sizedFormat, width, height, depth);
        break;

    default:
        MOZ_CRASH("GFX: bad target");
    }

    return errorScope.GetError();
}

bool
IsTarget3D(TexImageTarget target)
{
    switch (target.get()) {
    case LOCAL_GL_TEXTURE_2D:
    case LOCAL_GL_TEXTURE_CUBE_MAP_POSITIVE_X:
    case LOCAL_GL_TEXTURE_CUBE_MAP_NEGATIVE_X:
    case LOCAL_GL_TEXTURE_CUBE_MAP_POSITIVE_Y:
    case LOCAL_GL_TEXTURE_CUBE_MAP_NEGATIVE_Y:
    case LOCAL_GL_TEXTURE_CUBE_MAP_POSITIVE_Z:
    case LOCAL_GL_TEXTURE_CUBE_MAP_NEGATIVE_Z:
        return false;

    case LOCAL_GL_TEXTURE_3D:
    case LOCAL_GL_TEXTURE_2D_ARRAY:
        return true;

    default:
        MOZ_CRASH("GFX: bad target");
    }
}

GLenum
DoTexImage(gl::GLContext* gl, TexImageTarget target, GLint level,
           const webgl::DriverUnpackInfo* dui, GLsizei width, GLsizei height,
           GLsizei depth, const void* data)
{
    const GLint border = 0;

    gl::GLContext::LocalErrorScope errorScope(*gl);

    if (IsTarget3D(target)) {
        gl->fTexImage3D(target.get(), level, dui->internalFormat, width, height, depth,
                        border, dui->unpackFormat, dui->unpackType, data);
    } else {
        MOZ_ASSERT(depth == 1);
        gl->fTexImage2D(target.get(), level, dui->internalFormat, width, height, border,
                        dui->unpackFormat, dui->unpackType, data);
    }

    return errorScope.GetError();
}

GLenum
DoTexSubImage(gl::GLContext* gl, TexImageTarget target, GLint level, GLint xOffset,
              GLint yOffset, GLint zOffset, GLsizei width, GLsizei height, GLsizei depth,
              const webgl::PackingInfo& pi, const void* data)
{
    gl::GLContext::LocalErrorScope errorScope(*gl);

    if (IsTarget3D(target)) {
        gl->fTexSubImage3D(target.get(), level, xOffset, yOffset, zOffset, width, height,
                           depth, pi.format, pi.type, data);
    } else {
        MOZ_ASSERT(zOffset == 0);
        MOZ_ASSERT(depth == 1);
        gl->fTexSubImage2D(target.get(), level, xOffset, yOffset, width, height,
                           pi.format, pi.type, data);
    }

    return errorScope.GetError();
}

static inline GLenum
DoCompressedTexImage(gl::GLContext* gl, TexImageTarget target, GLint level,
                     GLenum internalFormat, GLsizei width, GLsizei height, GLsizei depth,
                     GLsizei dataSize, const void* data)
{
    const GLint border = 0;

    gl::GLContext::LocalErrorScope errorScope(*gl);

    if (IsTarget3D(target)) {
        gl->fCompressedTexImage3D(target.get(), level, internalFormat, width, height,
                                  depth, border, dataSize, data);
    } else {
        MOZ_ASSERT(depth == 1);
        gl->fCompressedTexImage2D(target.get(), level, internalFormat, width, height,
                                  border, dataSize, data);
    }

    return errorScope.GetError();
}

GLenum
DoCompressedTexSubImage(gl::GLContext* gl, TexImageTarget target, GLint level,
                        GLint xOffset, GLint yOffset, GLint zOffset, GLsizei width,
                        GLsizei height, GLsizei depth, GLenum sizedUnpackFormat,
                        GLsizei dataSize, const void* data)
{
    gl::GLContext::LocalErrorScope errorScope(*gl);

    if (IsTarget3D(target)) {
        gl->fCompressedTexSubImage3D(target.get(), level, xOffset, yOffset, zOffset,
                                     width, height, depth, sizedUnpackFormat, dataSize,
                                     data);
    } else {
        MOZ_ASSERT(zOffset == 0);
        MOZ_ASSERT(depth == 1);
        gl->fCompressedTexSubImage2D(target.get(), level, xOffset, yOffset, width,
                                     height, sizedUnpackFormat, dataSize, data);
    }

    return errorScope.GetError();
}

static inline GLenum
DoCopyTexImage2D(gl::GLContext* gl, TexImageTarget target, GLint level,
                 GLenum internalFormat, GLint x, GLint y, GLsizei width, GLsizei height)
{
    const GLint border = 0;

    gl::GLContext::LocalErrorScope errorScope(*gl);

    MOZ_ASSERT(!IsTarget3D(target));
    gl->fCopyTexImage2D(target.get(), level, internalFormat, x, y, width, height,
                        border);

    return errorScope.GetError();
}

static inline GLenum
DoCopyTexSubImage(gl::GLContext* gl, TexImageTarget target, GLint level, GLint xOffset,
                  GLint yOffset, GLint zOffset, GLint x, GLint y, GLsizei width,
                  GLsizei height)
{
    gl::GLContext::LocalErrorScope errorScope(*gl);

    if (IsTarget3D(target)) {
        gl->fCopyTexSubImage3D(target.get(), level, xOffset, yOffset, zOffset, x, y,
                               width, height);
    } else {
        MOZ_ASSERT(zOffset == 0);
        gl->fCopyTexSubImage2D(target.get(), level, xOffset, yOffset, x, y, width,
                               height);
    }

    return errorScope.GetError();
}

//////////////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////////////
// Actual (mostly generic) function implementations

static bool
ValidateCompressedTexImageRestrictions(const char* funcName, WebGLContext* webgl,
                                       TexImageTarget target, uint32_t level,
                                       const webgl::FormatInfo* format, uint32_t width,
                                       uint32_t height, uint32_t depth)
{
    const auto fnIsDimValid_S3TC = [level](uint32_t size, uint32_t blockSize) {
        if (size % blockSize == 0)
            return true;

        if (level == 0)
            return false;

        return (size == 0 || size == 1 || size == 2);
    };

    switch (format->compression->family) {
    case webgl::CompressionFamily::PVRTC:
        if (!IsPowerOfTwo(width) || !IsPowerOfTwo(height)) {
            webgl->ErrorInvalidValue("%s: %s requires power-of-two width and height.",
                                     funcName, format->name);
            return false;
        }

        break;

    case webgl::CompressionFamily::S3TC:
        if (!fnIsDimValid_S3TC(width, format->compression->blockWidth) ||
            !fnIsDimValid_S3TC(height, format->compression->blockHeight))
        {
            webgl->ErrorInvalidOperation("%s: %s requires that width and height are"
                                         " block-aligned, or, if level>0, equal to 0, 1,"
                                         " or 2.",
                                         funcName, format->name);
            return false;
        }

        break;

    // Default: There are no restrictions on CompressedTexImage.
    default: // ATC, ETC1, ES3
        break;
    }

    return true;
}

static bool
ValidateTargetForFormat(const char* funcName, WebGLContext* webgl, TexImageTarget target,
                        const webgl::FormatInfo* format)
{
    // GLES 3.0.4 p127:
    // "Textures with a base internal format of DEPTH_COMPONENT or DEPTH_STENCIL are
    //  supported by texture image specification commands only if `target` is TEXTURE_2D,
    //  TEXTURE_2D_ARRAY, or TEXTURE_CUBE_MAP. Using these formats in conjunction with any
    //  other `target` will result in an INVALID_OPERATION error."

    switch (format->effectiveFormat) {
    // TEXTURE_2D_ARRAY but not TEXTURE_3D:
    // D and DS formats
    case webgl::EffectiveFormat::DEPTH_COMPONENT16:
    case webgl::EffectiveFormat::DEPTH_COMPONENT24:
    case webgl::EffectiveFormat::DEPTH_COMPONENT32F:
    case webgl::EffectiveFormat::DEPTH24_STENCIL8:
    case webgl::EffectiveFormat::DEPTH32F_STENCIL8:
    // CompressionFamily::ES3
    case webgl::EffectiveFormat::COMPRESSED_R11_EAC:
    case webgl::EffectiveFormat::COMPRESSED_SIGNED_R11_EAC:
    case webgl::EffectiveFormat::COMPRESSED_RG11_EAC:
    case webgl::EffectiveFormat::COMPRESSED_SIGNED_RG11_EAC:
    case webgl::EffectiveFormat::COMPRESSED_RGB8_ETC2:
    case webgl::EffectiveFormat::COMPRESSED_SRGB8_ETC2:
    case webgl::EffectiveFormat::COMPRESSED_RGB8_PUNCHTHROUGH_ALPHA1_ETC2:
    case webgl::EffectiveFormat::COMPRESSED_SRGB8_PUNCHTHROUGH_ALPHA1_ETC2:
    case webgl::EffectiveFormat::COMPRESSED_RGBA8_ETC2_EAC:
    case webgl::EffectiveFormat::COMPRESSED_SRGB8_ALPHA8_ETC2_EAC:
    // CompressionFamily::S3TC
    case webgl::EffectiveFormat::COMPRESSED_RGB_S3TC_DXT1_EXT:
    case webgl::EffectiveFormat::COMPRESSED_RGBA_S3TC_DXT1_EXT:
    case webgl::EffectiveFormat::COMPRESSED_RGBA_S3TC_DXT3_EXT:
    case webgl::EffectiveFormat::COMPRESSED_RGBA_S3TC_DXT5_EXT:
        if (target == LOCAL_GL_TEXTURE_3D) {
            webgl->ErrorInvalidOperation("%s: Format %s cannot be used with TEXTURE_3D.",
                                         funcName, format->name);
            return false;
        }
        break;

    // No 3D targets:
    // CompressionFamily::ATC
    case webgl::EffectiveFormat::ATC_RGB_AMD:
    case webgl::EffectiveFormat::ATC_RGBA_EXPLICIT_ALPHA_AMD:
    case webgl::EffectiveFormat::ATC_RGBA_INTERPOLATED_ALPHA_AMD:
    // CompressionFamily::PVRTC
    case webgl::EffectiveFormat::COMPRESSED_RGB_PVRTC_4BPPV1:
    case webgl::EffectiveFormat::COMPRESSED_RGBA_PVRTC_4BPPV1:
    case webgl::EffectiveFormat::COMPRESSED_RGB_PVRTC_2BPPV1:
    case webgl::EffectiveFormat::COMPRESSED_RGBA_PVRTC_2BPPV1:
    // CompressionFamily::ETC1
    case webgl::EffectiveFormat::ETC1_RGB8_OES:
        if (target == LOCAL_GL_TEXTURE_3D ||
            target == LOCAL_GL_TEXTURE_2D_ARRAY)
        {
            webgl->ErrorInvalidOperation("%s: Format %s cannot be used with TEXTURE_3D or"
                                         " TEXTURE_2D_ARRAY.",
                                         funcName, format->name);
            return false;
        }
        break;

    default:
        break;
    }

    return true;
}

void
WebGLTexture::TexStorage(const char* funcName, TexTarget target, GLsizei levels,
                         GLenum sizedFormat, GLsizei width, GLsizei height, GLsizei depth)
{
    // Check levels
    if (levels < 1) {
        mContext->ErrorInvalidValue("%s: `levels` must be >= 1.", funcName);
        return;
    }

    if (!width || !height || !depth) {
        mContext->ErrorInvalidValue("%s: Dimensions must be non-zero.", funcName);
        return;
    }

    const TexImageTarget testTarget = IsCubeMap() ? LOCAL_GL_TEXTURE_CUBE_MAP_POSITIVE_X
                                                  : target.get();
    const GLint testLevel = 0;

    WebGLTexture::ImageInfo* testImageInfo;
    if (!ValidateTexImageSpecification(funcName, testTarget, testLevel, width, height,
                                       depth, &testImageInfo))
    {
        return;
    }
    MOZ_ASSERT(testImageInfo);
    mozilla::Unused << testImageInfo;

    auto dstUsage = mContext->mFormatUsage->GetSizedTexUsage(sizedFormat);
    if (!dstUsage) {
        mContext->ErrorInvalidEnum("%s: Invalid internalformat: 0x%04x", funcName,
                                   sizedFormat);
        return;
    }
    auto dstFormat = dstUsage->format;

    if (!ValidateTargetForFormat(funcName, mContext, testTarget, dstFormat))
        return;

    if (dstFormat->compression) {
        if (!ValidateCompressedTexImageRestrictions(funcName, mContext, testTarget,
                                                    testLevel, dstFormat, width, height,
                                                    depth))
        {
            return;
        }
    }

    ////////////////////////////////////

    const auto lastLevel = levels - 1;
    MOZ_ASSERT(lastLevel <= 31, "Right-shift is only defined for bits-1.");

    const uint32_t lastLevelWidth = uint32_t(width) >> lastLevel;
    const uint32_t lastLevelHeight = uint32_t(height) >> lastLevel;
    const uint32_t lastLevelDepth = uint32_t(depth) >> lastLevel;

    // If these are all zero, then some earlier level was the final 1x1x1 level.
    if (!lastLevelWidth && !lastLevelHeight && !lastLevelDepth) {
        mContext->ErrorInvalidOperation("%s: Too many levels requested for the given"
                                        " dimensions. (levels: %u, width: %u, height: %u,"
                                        " depth: %u)",
                                        funcName, levels, width, height, depth);
        return;
    }

    ////////////////////////////////////
    // Do the thing!

    mContext->gl->MakeCurrent();

    GLenum error = DoTexStorage(mContext->gl, target.get(), levels, sizedFormat, width,
                                height, depth);

    if (error == LOCAL_GL_OUT_OF_MEMORY) {
        mContext->ErrorOutOfMemory("%s: Ran out of memory during texture allocation.",
                                   funcName);
        return;
    }
    if (error) {
        MOZ_RELEASE_ASSERT(false, "GFX: We should have caught all other errors.");
        mContext->ErrorInvalidOperation("%s: Unexpected error during texture allocation.",
                                        funcName);
        return;
    }

    ////////////////////////////////////
    // Update our specification data.

    const bool isDataInitialized = false;
    const WebGLTexture::ImageInfo newInfo(dstUsage, width, height, depth,
                                          isDataInitialized);
    SetImageInfosAtLevel(0, newInfo);

    PopulateMipChain(0, levels-1);

    mImmutable = true;
}

////////////////////////////////////////
// Tex(Sub)Image

void
WebGLTexture::TexImage(const char* funcName, TexImageTarget target, GLint level,
                       GLenum internalFormat, const webgl::PackingInfo& pi,
                       const webgl::TexUnpackBlob* blob)
{
    ////////////////////////////////////
    // Get dest info

    WebGLTexture::ImageInfo* imageInfo;
    if (!ValidateTexImageSpecification(funcName, target, level, blob->mWidth,
                                       blob->mHeight, blob->mDepth, &imageInfo))
    {
        return;
    }
    MOZ_ASSERT(imageInfo);

    const auto& fua = mContext->mFormatUsage;
    if (!fua->IsInternalFormatEnumValid(internalFormat)) {
        mContext->ErrorInvalidValue("%s: Invalid internalformat: 0x%04x",
                                    funcName, internalFormat);
        return;
    }

    auto dstUsage = fua->GetSizedTexUsage(internalFormat);
    if (!dstUsage) {
        if (internalFormat != pi.format) {
            /* GL ES Version 3.0.4 - 3.8.3 Texture Image Specification
             *   "Specifying a combination of values for format, type, and
             *   internalformat that is not listed as a valid combination
             *   in tables 3.2 or 3.3 generates the error INVALID_OPERATION."
             */
            mContext->ErrorInvalidOperation("%s: Unsized internalFormat must match"
                                            " unpack format.",
                                            funcName);
            return;
        }

        dstUsage = fua->GetUnsizedTexUsage(pi);
    }

    if (!dstUsage) {
        mContext->ErrorInvalidOperation("%s: Invalid internalformat/format/type:"
                                        " 0x%04x/0x%04x/0x%04x",
                                        funcName, internalFormat, pi.format, pi.type);
        return;
    }

    const webgl::DriverUnpackInfo* driverUnpackInfo;
    if (!dstUsage->IsUnpackValid(pi, &driverUnpackInfo)) {
        mContext->ErrorInvalidOperation("%s: Mismatched internalFormat and format/type:"
                                        " 0x%04x and 0x%04x/0x%04x",
                                        funcName, internalFormat, pi.format, pi.type);
        return;
    }

    ////////////////////////////////////
    // Check that source and dest info are compatible
    auto dstFormat = dstUsage->format;

    if (!ValidateTargetForFormat(funcName, mContext, target, dstFormat))
        return;

    if (!mContext->IsWebGL2() && dstFormat->d) {
        if (target != LOCAL_GL_TEXTURE_2D ||
            blob->HasData() ||
            level != 0)
        {
            mContext->ErrorInvalidOperation("%s: With format %s, this function may only"
                                            " be called with target=TEXTURE_2D,"
                                            " data=null, and level=0.",
                                            funcName, dstFormat->name);
            return;
        }
    }

    ////////////////////////////////////
    // Do the thing!

    MOZ_ALWAYS_TRUE( mContext->gl->MakeCurrent() );
    MOZ_ASSERT(mContext->gl->IsCurrent());

    // It's tempting to do allocation first, and TexSubImage second, but this is generally
    // slower.

    const ImageInfo newImageInfo(dstUsage, blob->mWidth, blob->mHeight, blob->mDepth,
                                 blob->HasData());

    const bool isSubImage = false;
    const bool needsRespec = (imageInfo->mWidth  != newImageInfo.mWidth ||
                              imageInfo->mHeight != newImageInfo.mHeight ||
                              imageInfo->mDepth  != newImageInfo.mDepth ||
                              imageInfo->mFormat != newImageInfo.mFormat);
    const GLint xOffset = 0;
    const GLint yOffset = 0;
    const GLint zOffset = 0;

    GLenum glError;
    blob->TexOrSubImage(isSubImage, needsRespec, funcName, this, target, level,
                        driverUnpackInfo, xOffset, yOffset, zOffset, &glError);

    if (glError == LOCAL_GL_OUT_OF_MEMORY) {
        mContext->ErrorOutOfMemory("%s: Driver ran out of memory during upload.",
                                   funcName);
        return;
    }

    if (glError) {
        mContext->ErrorInvalidOperation("%s: Unexpected error during upload: 0x%04x",
                                        funcName, glError);
        printf_stderr("%s: dui: %x/%x/%x\n", funcName, driverUnpackInfo->internalFormat,
                      driverUnpackInfo->unpackFormat, driverUnpackInfo->unpackType);
        MOZ_ASSERT(false, "Unexpected GL error.");
        return;
    }

    ////////////////////////////////////
    // Update our specification data.

    SetImageInfo(imageInfo, newImageInfo);
}

void
WebGLTexture::TexSubImage(const char* funcName, TexImageTarget target, GLint level,
                          GLint xOffset, GLint yOffset, GLint zOffset,
                          const webgl::PackingInfo& pi, const webgl::TexUnpackBlob* blob)
{
    ////////////////////////////////////
    // Get dest info

    WebGLTexture::ImageInfo* imageInfo;
    if (!ValidateTexImageSelection(funcName, target, level, xOffset, yOffset, zOffset,
                                   blob->mWidth, blob->mHeight, blob->mDepth, &imageInfo))
    {
        return;
    }
    MOZ_ASSERT(imageInfo);

    auto dstUsage = imageInfo->mFormat;
    auto dstFormat = dstUsage->format;

    if (dstFormat->compression) {
        mContext->ErrorInvalidEnum("%s: Specified TexImage must not be compressed.",
                                   funcName);
        return;
    }

    if (!mContext->IsWebGL2() && dstFormat->d) {
        mContext->ErrorInvalidOperation("%s: Function may not be called on a texture of"
                                        " format %s.",
                                        funcName, dstFormat->name);
        return;
    }

    ////////////////////////////////////
    // Get source info

    const webgl::DriverUnpackInfo* driverUnpackInfo;
    if (!dstUsage->IsUnpackValid(pi, &driverUnpackInfo)) {
        mContext->ErrorInvalidOperation("%s: Mismatched internalFormat and format/type:"
                                        " %s and 0x%04x/0x%04x",
                                        funcName, dstFormat->name, pi.format, pi.type);
        return;
    }

    ////////////////////////////////////
    // Do the thing!

    mContext->gl->MakeCurrent();

    bool uploadWillInitialize;
    if (!EnsureImageDataInitializedForUpload(this, funcName, target, level, xOffset,
                                             yOffset, zOffset, blob->mWidth,
                                             blob->mHeight, blob->mDepth, imageInfo,
                                             &uploadWillInitialize))
    {
        return;
    }

    const bool isSubImage = true;
    const bool needsRespec = false;

    GLenum glError;
    blob->TexOrSubImage(isSubImage, needsRespec, funcName, this, target, level,
                        driverUnpackInfo, xOffset, yOffset, zOffset, &glError);

    if (glError == LOCAL_GL_OUT_OF_MEMORY) {
        mContext->ErrorOutOfMemory("%s: Driver ran out of memory during upload.",
                                   funcName);
        return;
    }

    if (glError) {
        mContext->ErrorInvalidOperation("%s: Unexpected error during upload: 0x04x",
                                        funcName, glError);
        MOZ_ASSERT(false, "Unexpected GL error.");
        return;
    }

    ////////////////////////////////////
    // Update our specification data?

    if (uploadWillInitialize) {
        imageInfo->SetIsDataInitialized(true, this);
    }
}

////////////////////////////////////////
// CompressedTex(Sub)Image

void
WebGLTexture::CompressedTexImage(const char* funcName, TexImageTarget target, GLint level,
                                 GLenum internalFormat, GLsizei rawWidth,
                                 GLsizei rawHeight, GLsizei rawDepth, GLint border,
                                 const dom::ArrayBufferView& view)
{
    uint32_t width, height, depth;
    if (!ValidateExtents(mContext, funcName, rawWidth, rawHeight, rawDepth, border,
                         &width, &height, &depth))
    {
        return;
    }

    ////////////////////////////////////
    // Get dest info

    WebGLTexture::ImageInfo* imageInfo;
    if (!ValidateTexImageSpecification(funcName, target, level, width, height, depth,
                                       &imageInfo))
    {
        return;
    }
    MOZ_ASSERT(imageInfo);

    auto usage = mContext->mFormatUsage->GetSizedTexUsage(internalFormat);
    if (!usage) {
        mContext->ErrorInvalidEnum("%s: Invalid internalFormat: 0x%04x", funcName,
                                   internalFormat);
        return;
    }

    auto format = usage->format;
    if (!format->compression) {
        mContext->ErrorInvalidEnum("%s: Specified internalFormat must be compressed.",
                                   funcName);
        return;
    }

    if (!ValidateTargetForFormat(funcName, mContext, target, format))
        return;

    ////////////////////////////////////
    // Get source info

    view.ComputeLengthAndData();
    const void* data = view.DataAllowShared();
    size_t dataSize = view.LengthAllowShared();

    if (!ValidateCompressedTexUnpack(mContext, funcName, width, height, depth, format,
                                     dataSize))
    {
        return;
    }

    ////////////////////////////////////
    // Check that source is compatible with dest

    if (!ValidateCompressedTexImageRestrictions(funcName, mContext, target, level, format,
                                                width, height, depth))
    {
        return;
    }

    ////////////////////////////////////
    // Do the thing!

    mContext->gl->MakeCurrent();

    // Warning: Possibly shared memory.  See bug 1225033.
    GLenum error = DoCompressedTexImage(mContext->gl, target, level, internalFormat,
                                        width, height, depth, dataSize, data);
    if (error == LOCAL_GL_OUT_OF_MEMORY) {
        mContext->ErrorOutOfMemory("%s: Ran out of memory during upload.", funcName);
        return;
    }
    if (error) {
        MOZ_RELEASE_ASSERT(false, "GFX: We should have caught all other errors.");
        mContext->GenerateWarning("%s: Unexpected error during texture upload. Context"
                                  " lost.",
                                  funcName);
        mContext->ForceLoseContext();
        return;
    }

    ////////////////////////////////////
    // Update our specification data.

    const bool isDataInitialized = true;
    const ImageInfo newImageInfo(usage, width, height, depth, isDataInitialized);
    SetImageInfo(imageInfo, newImageInfo);
}

static inline bool
IsSubImageBlockAligned(const webgl::CompressedFormatInfo* compression,
                       const WebGLTexture::ImageInfo* imageInfo, GLint xOffset,
                       GLint yOffset, uint32_t width, uint32_t height)
{
    if (xOffset % compression->blockWidth != 0 ||
        yOffset % compression->blockHeight != 0)
    {
        return false;
    }

    if (width % compression->blockWidth != 0 && xOffset + width != imageInfo->mWidth)
        return false;

    if (height % compression->blockHeight != 0 && yOffset + height != imageInfo->mHeight)
        return false;

    return true;
}

void
WebGLTexture::CompressedTexSubImage(const char* funcName, TexImageTarget target,
                                    GLint level, GLint xOffset, GLint yOffset,
                                    GLint zOffset, GLsizei rawWidth, GLsizei rawHeight,
                                    GLsizei rawDepth, GLenum sizedUnpackFormat,
                                    const dom::ArrayBufferView& view)
{
    uint32_t width, height, depth;
    if (!ValidateExtents(mContext, funcName, rawWidth, rawHeight, rawDepth, 0, &width,
                         &height, &depth))
    {
        return;
    }

    ////////////////////////////////////
    // Get dest info

    WebGLTexture::ImageInfo* imageInfo;
    if (!ValidateTexImageSelection(funcName, target, level, xOffset, yOffset, zOffset,
                                   width, height, depth, &imageInfo))
    {
        return;
    }
    MOZ_ASSERT(imageInfo);

    auto dstUsage = imageInfo->mFormat;
    auto dstFormat = dstUsage->format;

    ////////////////////////////////////
    // Get source info

    view.ComputeLengthAndData();
    size_t dataSize = view.LengthAllowShared();
    const void* data = view.DataAllowShared();

    auto srcUsage = mContext->mFormatUsage->GetSizedTexUsage(sizedUnpackFormat);
    if (!srcUsage->format->compression) {
        mContext->ErrorInvalidEnum("%s: Specified format must be compressed.", funcName);
        return;
    }

    if (srcUsage != dstUsage) {
        mContext->ErrorInvalidOperation("%s: `format` must match the format of the"
                                        " existing texture image.",
                                        funcName);
        return;
    }

    auto format = srcUsage->format;
    MOZ_ASSERT(format == dstFormat);
    if (!ValidateCompressedTexUnpack(mContext, funcName, width, height, depth, format,
                                     dataSize))
    {
        return;
    }

    ////////////////////////////////////
    // Check that source is compatible with dest

    switch (format->compression->family) {
    // Forbidden:
    case webgl::CompressionFamily::ETC1:
    case webgl::CompressionFamily::ATC:
        mContext->ErrorInvalidOperation("%s: Format does not allow sub-image"
                                        " updates.", funcName);
        return;

    // Block-aligned:
    case webgl::CompressionFamily::ES3:  // Yes, the ES3 formats don't match the ES3
    case webgl::CompressionFamily::S3TC: // default behavior.
        if (!IsSubImageBlockAligned(dstFormat->compression, imageInfo, xOffset, yOffset,
                                    width, height))
        {
            mContext->ErrorInvalidOperation("%s: Format requires block-aligned sub-image"
                                            " updates.",
                                            funcName);
            return;
        }
        break;

    // Full-only: (The ES3 default)
    default: // PVRTC
        if (xOffset || yOffset ||
            uint32_t(width) != imageInfo->mWidth ||
            uint32_t(height) != imageInfo->mHeight)
        {
            mContext->ErrorInvalidOperation("%s: Format does not allow partial sub-image"
                                            " updates.",
                                            funcName);
            return;
        }
        break;
    }

    ////////////////////////////////////
    // Do the thing!

    mContext->gl->MakeCurrent();

    bool uploadWillInitialize;
    if (!EnsureImageDataInitializedForUpload(this, funcName, target, level, xOffset,
                                             yOffset, zOffset, width, height, depth,
                                             imageInfo, &uploadWillInitialize))
    {
        return;
    }

    // Warning: Possibly shared memory.  See bug 1225033.
    GLenum error = DoCompressedTexSubImage(mContext->gl, target, level, xOffset, yOffset,
                                           zOffset, width, height, depth,
                                           sizedUnpackFormat, dataSize, data);
    if (error == LOCAL_GL_OUT_OF_MEMORY) {
        mContext->ErrorOutOfMemory("%s: Ran out of memory during upload.", funcName);
        return;
    }
    if (error) {
        MOZ_RELEASE_ASSERT(false, "GFX: We should have caught all other errors.");
        mContext->GenerateWarning("%s: Unexpected error during texture upload. Context"
                                  " lost.",
                                  funcName);
        mContext->ForceLoseContext();
        return;
    }

    ////////////////////////////////////
    // Update our specification data?

    if (uploadWillInitialize) {
        imageInfo->SetIsDataInitialized(true, this);
    }
}

////////////////////////////////////////
// CopyTex(Sub)Image

static bool
ValidateCopyTexImageFormats(WebGLContext* webgl, const char* funcName,
                            const webgl::FormatInfo* srcFormat,
                            const webgl::FormatInfo* dstFormat)
{
    MOZ_ASSERT(!srcFormat->compression);
    if (dstFormat->compression) {
        webgl->ErrorInvalidEnum("%s: Specified destination must not have a compressed"
                                " format.",
                                funcName);
        return false;
    }

    if (dstFormat->effectiveFormat == webgl::EffectiveFormat::RGB9_E5) {
        webgl->ErrorInvalidOperation("%s: RGB9_E5 is an invalid destination for"
                                     " CopyTex(Sub)Image. (GLES 3.0.4 p145)",
                                     funcName);
        return false;
    }

    if (!DoChannelsMatchForCopyTexImage(srcFormat, dstFormat)) {
        webgl->ErrorInvalidOperation("%s: Destination channels must be compatible with"
                                     " source channels. (GLES 3.0.4 p140 Table 3.16)",
                                     funcName);
        return false;
    }

    return true;
}

////////////////////////////////////////////////////////////////////////////////

class ScopedCopyTexImageSource
{
    WebGLContext* const mWebGL;
    GLuint mRB;
    GLuint mFB;

public:
    ScopedCopyTexImageSource(WebGLContext* webgl, const char* funcName, uint32_t srcWidth,
                             uint32_t srcHeight, const webgl::FormatInfo* srcFormat,
                             const webgl::FormatUsageInfo* dstUsage);
    ~ScopedCopyTexImageSource();
};

ScopedCopyTexImageSource::ScopedCopyTexImageSource(WebGLContext* webgl,
                                                   const char* funcName,
                                                   uint32_t srcWidth, uint32_t srcHeight,
                                                   const webgl::FormatInfo* srcFormat,
                                                   const webgl::FormatUsageInfo* dstUsage)
    : mWebGL(webgl)
    , mRB(0)
    , mFB(0)
{
    switch (dstUsage->format->unsizedFormat) {
    case webgl::UnsizedFormat::L:
    case webgl::UnsizedFormat::A:
    case webgl::UnsizedFormat::LA:
        webgl->GenerateWarning("%s: Copying to a LUMINANCE, ALPHA, or LUMINANCE_ALPHA"
                               " is deprecated, and has severely reduced performance"
                               " on some platforms.",
                               funcName);
        break;

    default:
        MOZ_ASSERT(!dstUsage->textureSwizzleRGBA);
        return;
    }

    if (!dstUsage->textureSwizzleRGBA)
        return;

    gl::GLContext* gl = webgl->gl;

    GLenum sizedFormat;

    switch (srcFormat->componentType) {
    case webgl::ComponentType::NormUInt:
        sizedFormat = LOCAL_GL_RGBA8;
        break;

    case webgl::ComponentType::Float:
        if (webgl->IsExtensionEnabled(WebGLExtensionID::WEBGL_color_buffer_float)) {
            sizedFormat = LOCAL_GL_RGBA32F;
            break;
        }

        if (webgl->IsExtensionEnabled(WebGLExtensionID::EXT_color_buffer_half_float)) {
            sizedFormat = LOCAL_GL_RGBA16F;
            break;
        }
        MOZ_CRASH("GFX: Should be able to request CopyTexImage from Float.");

    default:
        MOZ_CRASH("GFX: Should be able to request CopyTexImage from this type.");
    }

    gl::ScopedTexture scopedTex(gl);
    gl::ScopedBindTexture scopedBindTex(gl, scopedTex.Texture(), LOCAL_GL_TEXTURE_2D);

    gl->fTexParameteri(LOCAL_GL_TEXTURE_2D, LOCAL_GL_TEXTURE_MIN_FILTER, LOCAL_GL_NEAREST);
    gl->fTexParameteri(LOCAL_GL_TEXTURE_2D, LOCAL_GL_TEXTURE_MAG_FILTER, LOCAL_GL_NEAREST);

    GLint blitSwizzle[4] = {LOCAL_GL_ZERO};
    switch (dstUsage->format->unsizedFormat) {
    case webgl::UnsizedFormat::L:
        blitSwizzle[0] = LOCAL_GL_RED;
        break;

    case webgl::UnsizedFormat::A:
        blitSwizzle[0] = LOCAL_GL_ALPHA;
        break;

    case webgl::UnsizedFormat::LA:
        blitSwizzle[0] = LOCAL_GL_RED;
        blitSwizzle[1] = LOCAL_GL_ALPHA;
        break;

    default:
        MOZ_CRASH("GFX: Unhandled unsizedFormat.");
    }

    gl->fTexParameteri(LOCAL_GL_TEXTURE_2D, LOCAL_GL_TEXTURE_SWIZZLE_R, blitSwizzle[0]);
    gl->fTexParameteri(LOCAL_GL_TEXTURE_2D, LOCAL_GL_TEXTURE_SWIZZLE_G, blitSwizzle[1]);
    gl->fTexParameteri(LOCAL_GL_TEXTURE_2D, LOCAL_GL_TEXTURE_SWIZZLE_B, blitSwizzle[2]);
    gl->fTexParameteri(LOCAL_GL_TEXTURE_2D, LOCAL_GL_TEXTURE_SWIZZLE_A, blitSwizzle[3]);

    gl->fCopyTexImage2D(LOCAL_GL_TEXTURE_2D, 0, sizedFormat, 0, 0, srcWidth,
                        srcHeight, 0);

    // Now create the swizzled FB we'll be exposing.

    GLuint rgbaRB = 0;
    gl->fGenRenderbuffers(1, &rgbaRB);
    gl::ScopedBindRenderbuffer scopedRB(gl, rgbaRB);
    gl->fRenderbufferStorage(LOCAL_GL_RENDERBUFFER, sizedFormat, srcWidth, srcHeight);

    GLuint rgbaFB = 0;
    gl->fGenFramebuffers(1, &rgbaFB);
    gl->fBindFramebuffer(LOCAL_GL_FRAMEBUFFER, rgbaFB);
    gl->fFramebufferRenderbuffer(LOCAL_GL_FRAMEBUFFER, LOCAL_GL_COLOR_ATTACHMENT0,
                                 LOCAL_GL_RENDERBUFFER, rgbaRB);

    const GLenum status = gl->fCheckFramebufferStatus(LOCAL_GL_FRAMEBUFFER);
    if (status != LOCAL_GL_FRAMEBUFFER_COMPLETE) {
        MOZ_CRASH("GFX: Temp framebuffer is not complete.");
    }

    // Restore RB binding.
    scopedRB.Unwrap(); // This function should really have a better name.

    // Draw-blit rgbaTex into rgbaFB.
    const gfx::IntSize srcSize(srcWidth, srcHeight);
    gl->BlitHelper()->DrawBlitTextureToFramebuffer(scopedTex.Texture(), rgbaFB,
                                                   srcSize, srcSize);

    // Restore Tex2D binding and destroy the temp tex.
    scopedBindTex.Unwrap();
    scopedTex.Unwrap();

    // Leave RB and FB alive, and FB bound.
    mRB = rgbaRB;
    mFB = rgbaFB;
}

template<typename T>
static inline GLenum
ToGLHandle(const T& obj)
{
    return (obj ? obj->mGLName : 0);
}

ScopedCopyTexImageSource::~ScopedCopyTexImageSource()
{
    if (!mFB) {
        MOZ_ASSERT(!mRB);
        return;
    }
    MOZ_ASSERT(mRB);

    gl::GLContext* gl = mWebGL->gl;

    // If we're swizzling, it's because we're on a GL core (3.2+) profile, which has
    // split framebuffer support.
    gl->fBindFramebuffer(LOCAL_GL_DRAW_FRAMEBUFFER,
                         ToGLHandle(mWebGL->mBoundDrawFramebuffer));
    gl->fBindFramebuffer(LOCAL_GL_READ_FRAMEBUFFER,
                         ToGLHandle(mWebGL->mBoundReadFramebuffer));

    gl->fDeleteFramebuffers(1, &mFB);
    gl->fDeleteRenderbuffers(1, &mRB);
}

////////////////////////////////////////////////////////////////////////////////

static bool
GetUnsizedFormatForCopy(GLenum internalFormat, webgl::UnsizedFormat* const out)
{
    switch (internalFormat) {
    case LOCAL_GL_RED:             *out = webgl::UnsizedFormat::R;    break;
    case LOCAL_GL_RG:              *out = webgl::UnsizedFormat::RG;   break;
    case LOCAL_GL_RGB:             *out = webgl::UnsizedFormat::RGB;  break;
    case LOCAL_GL_RGBA:            *out = webgl::UnsizedFormat::RGBA; break;
    case LOCAL_GL_LUMINANCE:       *out = webgl::UnsizedFormat::L;    break;
    case LOCAL_GL_ALPHA:           *out = webgl::UnsizedFormat::A;    break;
    case LOCAL_GL_LUMINANCE_ALPHA: *out = webgl::UnsizedFormat::LA;   break;

    default:
        return false;
    }

    return true;
}

static const webgl::FormatUsageInfo*
ValidateCopyDestUsage(const char* funcName, WebGLContext* webgl,
                      const webgl::FormatInfo* srcFormat, GLenum internalFormat)
{
    const auto& fua = webgl->mFormatUsage;

    auto dstUsage = fua->GetSizedTexUsage(internalFormat);
    if (!dstUsage) {
        // Ok, maybe it's unsized.
        webgl::UnsizedFormat unsizedFormat;
        if (!GetUnsizedFormatForCopy(internalFormat, &unsizedFormat)) {
            webgl->ErrorInvalidEnum("%s: Unrecongnized internalFormat 0x%04x.", funcName,
                                    internalFormat);
            return nullptr;
        }

        const auto dstFormat = srcFormat->GetCopyDecayFormat(unsizedFormat);
        if (dstFormat) {
            dstUsage = fua->GetUsage(dstFormat->effectiveFormat);
        }
        if (!dstUsage) {
            webgl->ErrorInvalidOperation("%s: 0x%04x is not a valid unsized format for"
                                         " source format %s.",
                                         funcName, internalFormat, srcFormat->name);
            return nullptr;
        }

        return dstUsage;
    }
    // Alright, it's sized.

    const auto dstFormat = dstUsage->format;

    if (dstFormat->componentType != srcFormat->componentType) {
        webgl->ErrorInvalidOperation("%s: For sized internalFormats, source and dest"
                                     " component types must match. (source: %s, dest:"
                                     " %s)",
                                     funcName, srcFormat->name, dstFormat->name);
        return nullptr;
    }

    bool componentSizesMatch = true;
    if (dstFormat->r) {
        componentSizesMatch &= (dstFormat->r == srcFormat->r);
    }
    if (dstFormat->g) {
        componentSizesMatch &= (dstFormat->g == srcFormat->g);
    }
    if (dstFormat->b) {
        componentSizesMatch &= (dstFormat->b == srcFormat->b);
    }
    if (dstFormat->a) {
        componentSizesMatch &= (dstFormat->a == srcFormat->a);
    }

    if (!componentSizesMatch) {
        webgl->ErrorInvalidOperation("%s: For sized internalFormats, source and dest"
                                     " component sizes must match exactly. (source: %s,"
                                     " dest: %s)",
                                     funcName, srcFormat->name, dstFormat->name);
        return nullptr;
    }

    return dstUsage;
}

bool
WebGLTexture::ValidateCopyTexImageForFeedback(const char* funcName, uint32_t level) const
{
    const auto& fb = mContext->mBoundReadFramebuffer;
    if (fb) {
        const auto readBuffer = fb->ReadBufferMode();
        MOZ_ASSERT(readBuffer != LOCAL_GL_NONE);
        const uint32_t colorAttachment = readBuffer - LOCAL_GL_COLOR_ATTACHMENT0;
        const auto& attach = fb->ColorAttachment(colorAttachment);

        if (attach.Texture() == this &&
            uint32_t(attach.MipLevel()) == level)
        {
            // Note that the TexImageTargets *don't* have to match for this to be
            // undefined per GLES 3.0.4 p211, thus an INVALID_OP in WebGL.
            mContext->ErrorInvalidOperation("%s: Feedback loop detected, as this texture"
                                            " is already attached to READ_FRAMEBUFFER's"
                                            " READ_BUFFER-selected COLOR_ATTACHMENT%u.",
                                            funcName, colorAttachment);
            return false;
        }
    }
    return true;
}

// There is no CopyTexImage3D.
void
WebGLTexture::CopyTexImage2D(TexImageTarget target, GLint level, GLenum internalFormat,
                             GLint x, GLint y, GLsizei rawWidth, GLsizei rawHeight,
                             GLint border)
{
    const char funcName[] = "copyTexImage2D";

    ////////////////////////////////////
    // Get dest info

    uint32_t width, height, depth;
    if (!ValidateExtents(mContext, funcName, rawWidth, rawHeight, 1, border, &width,
                         &height, &depth))
    {
        return;
    }

    WebGLTexture::ImageInfo* imageInfo;
    if (!ValidateTexImageSpecification(funcName, target, level, width, height, depth,
                                       &imageInfo))
    {
        return;
    }
    MOZ_ASSERT(imageInfo);

    ////////////////////////////////////
    // Get source info

    const webgl::FormatUsageInfo* srcUsage;
    uint32_t srcWidth;
    uint32_t srcHeight;
    if (!mContext->ValidateCurFBForRead(funcName, &srcUsage, &srcWidth, &srcHeight))
        return;
    auto srcFormat = srcUsage->format;

    if (!ValidateCopyTexImageForFeedback(funcName, level))
        return;

    ////////////////////////////////////
    // Check that source and dest info are compatible

    const auto dstUsage = ValidateCopyDestUsage(funcName, mContext, srcFormat,
                                                internalFormat);
    if (!dstUsage)
        return;

    const auto dstFormat = dstUsage->format;

    if (!ValidateTargetForFormat(funcName, mContext, target, dstFormat))
        return;

    if (!mContext->IsWebGL2() && dstFormat->d) {
        mContext->ErrorInvalidOperation("%s: Function may not be called with format %s.",
                                        funcName, dstFormat->name);
        return;
    }

    if (!ValidateCopyTexImageFormats(mContext, funcName, srcFormat, dstFormat))
        return;

    ////////////////////////////////////
    // Do the thing!

    gl::GLContext* gl = mContext->gl;
    gl->MakeCurrent();

    ScopedCopyTexImageSource maybeSwizzle(mContext, funcName, srcWidth, srcHeight,
                                          srcFormat, dstUsage);

    uint32_t readX, readY;
    uint32_t writeX, writeY;
    uint32_t rwWidth, rwHeight;
    Intersect(srcWidth, x, width, &readX, &writeX, &rwWidth);
    Intersect(srcHeight, y, height, &readY, &writeY, &rwHeight);

    GLenum error;
    if (rwWidth == uint32_t(width) && rwHeight == uint32_t(height)) {
        error = DoCopyTexImage2D(gl, target, level, internalFormat, x, y, width, height);
    } else {
        // 1. Zero the texture data.
        // 2. CopyTexSubImage the subrect.

        const bool respecifyTexture = true;
        const uint8_t zOffset = 0;
        if (!ZeroTextureData(mContext, funcName, respecifyTexture, mGLName, target, level,
                             dstUsage, 0, 0, zOffset, width, height, depth))
        {
            mContext->ErrorOutOfMemory("%s: Failed to zero texture data.", funcName);
            MOZ_ASSERT(false, "Failed to zero texture data.");
            return;
        }

        if (!rwWidth || !rwHeight) {
            // There aren't any, so we're 'done'.
            mContext->DummyReadFramebufferOperation(funcName);
            return;
        }

        error = DoCopyTexSubImage(gl, target, level, writeX, writeY, zOffset, readX,
                                  readY, rwWidth, rwHeight);
    }

    if (error == LOCAL_GL_OUT_OF_MEMORY) {
        mContext->ErrorOutOfMemory("%s: Ran out of memory during texture copy.",
                                   funcName);
        return;
    }
    if (error) {
        MOZ_RELEASE_ASSERT(false, "GFX: We should have caught all other errors.");
        mContext->GenerateWarning("%s: Unexpected error during texture copy. Context"
                                  " lost.",
                                  funcName);
        mContext->ForceLoseContext();
        return;
    }

    ////////////////////////////////////
    // Update our specification data.

    const bool isDataInitialized = true;
    const ImageInfo newImageInfo(dstUsage, width, height, depth, isDataInitialized);
    SetImageInfo(imageInfo, newImageInfo);
}

void
WebGLTexture::CopyTexSubImage(const char* funcName, TexImageTarget target, GLint level,
                              GLint xOffset, GLint yOffset, GLint zOffset, GLint x,
                              GLint y, GLsizei rawWidth, GLsizei rawHeight)
{
    uint32_t width, height, depth;
    if (!ValidateExtents(mContext, funcName, rawWidth, rawHeight, 1, 0, &width,
                         &height, &depth))
    {
        return;
    }

    ////////////////////////////////////
    // Get dest info

    WebGLTexture::ImageInfo* imageInfo;
    if (!ValidateTexImageSelection(funcName, target, level, xOffset, yOffset, zOffset,
                                   width, height, depth, &imageInfo))
    {
        return;
    }
    MOZ_ASSERT(imageInfo);

    auto dstUsage = imageInfo->mFormat;
    MOZ_ASSERT(dstUsage);
    auto dstFormat = dstUsage->format;

    if (!mContext->IsWebGL2() && dstFormat->d) {
        mContext->ErrorInvalidOperation("%s: Function may not be called on a texture of"
                                        " format %s.",
                                        funcName, dstFormat->name);
        return;
    }

    ////////////////////////////////////
    // Get source info

    const webgl::FormatUsageInfo* srcUsage;
    uint32_t srcWidth;
    uint32_t srcHeight;
    if (!mContext->ValidateCurFBForRead(funcName, &srcUsage, &srcWidth, &srcHeight))
        return;
    auto srcFormat = srcUsage->format;

    if (!ValidateCopyTexImageForFeedback(funcName, level))
        return;

    ////////////////////////////////////
    // Check that source and dest info are compatible

    if (!ValidateCopyTexImageFormats(mContext, funcName, srcFormat, dstFormat))
        return;

    ////////////////////////////////////
    // Do the thing!

    mContext->gl->MakeCurrent();

    ScopedCopyTexImageSource maybeSwizzle(mContext, funcName, srcWidth, srcHeight,
                                          srcFormat, dstUsage);

    uint32_t readX, readY;
    uint32_t writeX, writeY;
    uint32_t rwWidth, rwHeight;
    Intersect(srcWidth, x, width, &readX, &writeX, &rwWidth);
    Intersect(srcHeight, y, height, &readY, &writeY, &rwHeight);

    if (!rwWidth || !rwHeight) {
        // There aren't any, so we're 'done'.
        mContext->DummyReadFramebufferOperation(funcName);
        return;
    }

    bool uploadWillInitialize;
    if (!EnsureImageDataInitializedForUpload(this, funcName, target, level, xOffset,
                                             yOffset, zOffset, width, height, depth,
                                             imageInfo, &uploadWillInitialize))
    {
        return;
    }

    GLenum error = DoCopyTexSubImage(mContext->gl, target, level, xOffset + writeX,
                                     yOffset + writeY, zOffset, readX, readY, rwWidth,
                                     rwHeight);

    if (error == LOCAL_GL_OUT_OF_MEMORY) {
        mContext->ErrorOutOfMemory("%s: Ran out of memory during texture copy.",
                                   funcName);
        return;
    }
    if (error) {
        MOZ_RELEASE_ASSERT(false, "GFX: We should have caught all other errors.");
        mContext->GenerateWarning("%s: Unexpected error during texture copy. Context"
                                  " lost.",
                                  funcName);
        mContext->ForceLoseContext();
        return;
    }

    ////////////////////////////////////
    // Update our specification data?

    if (uploadWillInitialize) {
        imageInfo->SetIsDataInitialized(true, this);
    }
}

} // namespace mozilla
