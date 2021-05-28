/* This file is part of the dngconvert project
   Copyright (C) 2011 Jens Mueller <tschensensinger at gmx dot de>
   
   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Library General Public   
   License as published by the Free Software Foundation; either
   version 2 of the License, or (at your option) any later version.
   
   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Library General Public License for more details.
   
   You should have received a copy of the GNU Library General Public License
   along with this library; see the file COPYING.  If not, write to
   the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
   Boston, MA 02110-1301, USA.
*/

#include "librawimage.h"
#include "librawdngdatastream.h"

#include "dng_file_stream.h"
#include "dng_memory.h"

#include "libraw/libraw.h"

using std::min;
using std::max;

LibRawImage::LibRawImage(const char *filename, dng_memory_allocator &allocator)
    :	dng_image(dng_rect(0, 0), 0, ttShort),
      m_Allocator(allocator),
      m_Buffer(),
      m_Memory()
{
    dng_file_stream stream(filename);
    Parse(stream);
}

LibRawImage::LibRawImage(dng_stream &stream, dng_memory_allocator &allocator)
    :	dng_image(dng_rect(0, 0), 0, ttShort),
      m_Allocator(allocator),
      m_Buffer(),
      m_Memory()
{
    Parse(stream);
}

void LibRawImage::Parse(dng_stream &stream)
{
    AutoPtr<LibRawDngDataStream> rawStream(new LibRawDngDataStream(stream));
    AutoPtr<LibRaw> rawProcessor(new LibRaw());

#if (LIBRAW_COMPILE_CHECK_VERSION_NOTLESS(0,14))
    {
        rawProcessor->imgdata.params.user_flip = 0;
    }
#endif

    int ret = rawProcessor->open_datastream(rawStream.Get());
    if (ret != LIBRAW_SUCCESS)
    {
        printf("Cannot open stream: %s\n", libraw_strerror(ret));
        rawProcessor->recycle();
        return;
    }

    ret = rawProcessor->adjust_sizes_info_only();
    if (ret != LIBRAW_SUCCESS)
    {
        printf("LibRaw: failed to run adjust_sizes_info_only: %s\n", libraw_strerror(ret));
        rawProcessor->recycle();
        return;
    }

    uint32 finalWidth = 0;
    uint32 finalHeight = 0;

#if (LIBRAW_COMPILE_CHECK_VERSION_NOTLESS(0,14))
    finalWidth = rawProcessor->imgdata.sizes.iwidth;
    finalHeight = rawProcessor->imgdata.sizes.iheight;
#else
    if ((rawProcessor->imgdata.sizes.flip == 5) || (rawProcessor->imgdata.sizes.flip == 6))
    {
        finalHeight = rawProcessor->imgdata.sizes.iwidth;
        finalWidth = rawProcessor->imgdata.sizes.iheight;
    }
    else
    {
        finalHeight = rawProcessor->imgdata.sizes.iheight;
        finalWidth = rawProcessor->imgdata.sizes.iwidth;
    }
#endif

    rawProcessor->recycle();

    rawStream.Get()->seek(0, SEEK_SET);
    
    rawProcessor->imgdata.params.output_bps = 16;
//    rawProcessor->imgdata.params.document_mode = 2;
    rawProcessor->imgdata.params.shot_select = 0;
#if (LIBRAW_COMPILE_CHECK_VERSION_NOTLESS(0,14))
    {
        rawProcessor->imgdata.params.user_flip = -1;
    }
#endif

    ret = rawProcessor->open_datastream(rawStream.Get());
    if (ret != LIBRAW_SUCCESS)
    {
        printf("Cannot open stream: %s\n", libraw_strerror(ret));
        rawProcessor->recycle();
        return;
    }

    ret = rawProcessor->unpack();
    if (ret != LIBRAW_SUCCESS)
    {
        printf("LibRaw: failed to run unpack: %s\n", libraw_strerror(ret));
        rawProcessor->recycle();
        return;
    }

    libraw_image_sizes_t *sizes = NULL;
    libraw_iparams_t *iparams = NULL;
    libraw_colordata_t *colors = NULL;

    sizes = &rawProcessor->imgdata.sizes;
    iparams = &rawProcessor->imgdata.idata;
    colors = &rawProcessor->imgdata.color;

    uint32 activeWidth = sizes->width;
    uint32 activeHeight = sizes->height;
    uint32 rawWidth = sizes->raw_width;
    uint32 rawHeight = sizes->raw_height;

    bool entireSensorData = false;
#if (LIBRAW_COMPILE_CHECK_VERSION_NOTLESS(0,14))
    entireSensorData = true;
    rawWidth = max(rawWidth, static_cast<uint32>(sizes->width + sizes->left_margin));
    rawHeight = max(rawHeight, static_cast<uint32>(sizes->height + sizes->top_margin));
#else
    if (0 == strcmp(iparams->make, "Canon") && (iparams->filters != 0))
    {
        entireSensorData = true;
        ret = rawProcessor->add_masked_borders_to_bitmap();
        if (ret != LIBRAW_SUCCESS)
        {
            printf("LibRaw: failed to run add_masked_borders_to_bitmap: %s\n", libraw_strerror(ret));
            rawProcessor->recycle();
            return;
        }
    }
#endif

    switch (sizes->flip)
    {
    case 3:
        m_BaseOrientation = dng_orientation::Rotate180();
        break;

    case 5:
        m_BaseOrientation = dng_orientation::Rotate90CCW();
        break;

    case 6:
        m_BaseOrientation = dng_orientation::Rotate90CW();
        break;

    default:
        m_BaseOrientation = dng_orientation::Normal();
        break;
    }

    bool fujiRotate90 = false;
    if ((0 == memcmp("FUJIFILM", iparams->make, min((size_t)8, sizeof(iparams->make)))) &&
            (2 == rawProcessor->COLOR(0, 1)) &&
            (1 == rawProcessor->COLOR(1, 0)))
    {
        fujiRotate90 = true;

        uint32 tmp = activeWidth;
        activeWidth = activeHeight;
        activeHeight = tmp;

        tmp = finalWidth;
        finalWidth = finalHeight;
        finalHeight = tmp;

        tmp = rawWidth;
        rawWidth = rawHeight;
        rawHeight = tmp;

        m_BaseOrientation += dng_orientation::Mirror90CCW();
    }

    if (entireSensorData == true)
        fBounds = dng_rect(rawHeight, rawWidth);
    else
        fBounds = dng_rect(activeHeight, activeWidth);

    m_Pattern = iparams->filters;
    m_Channels = static_cast<uint32>(iparams->colors);

    fPlanes = (m_Pattern == 0) ? 3 : 1;
    uint32 pixelType = ttShort;
    uint32 pixelSize = TagTypeSize(pixelType);
    uint32 bytes = fBounds.H() * fBounds.W() * fPlanes * pixelSize;

    m_Memory.Reset(m_Allocator.Allocate(bytes));

    m_Buffer.fArea       = fBounds;
    m_Buffer.fPlane      = 0;
    m_Buffer.fPlanes     = fPlanes;
    m_Buffer.fRowStep    = m_Buffer.fPlanes * fBounds.W();
    m_Buffer.fColStep    = m_Buffer.fPlanes;
    m_Buffer.fPlaneStep  = 1;
    m_Buffer.fPixelType  = pixelType;
    m_Buffer.fPixelSize  = pixelSize;
    m_Buffer.fData       = m_Memory->Buffer();

#if (LIBRAW_COMPILE_CHECK_VERSION_NOTLESS(0,14))
    libraw_decoder_info_t decoder_info;
    rawProcessor->get_decoder_info(&decoder_info);
/*
    if (decoder_info.decoder_flags & LIBRAW_DECODER_LEGACY)
    {
        unsigned short* output = (unsigned short*)m_Buffer.fData;

        for (unsigned int row = 0; row < sizes->raw_height; row++)
        {
            for (unsigned int col = 0; col < sizes->raw_width; col++)
            {
                for (uint32 color = 0; color < m_Channels; color++)
                {
                    *output = rawProcessor->imgdata.rawdata.color_image[row * sizes->raw_width + col][color];
                    ++output;
                }
            }
        }
    }
    else if(decoder_info.decoder_flags & LIBRAW_DECODER_FLATFIELD)
*/
    {
        unsigned short* output = (unsigned short*)m_Buffer.fData;

        if (entireSensorData == true)
        {
            if (fujiRotate90 == false)
            {
                memcpy(output, rawProcessor->imgdata.rawdata.raw_image, sizes->raw_height * sizes->raw_width * sizeof(unsigned short));
            }
            else
            {
                for (unsigned int col = 0; col < sizes->raw_width; col++)
                {
                    for (unsigned int row = 0; row < sizes->raw_height; row++)
                    {
                        *output = rawProcessor->imgdata.rawdata.raw_image[row * sizes->raw_width + col];
                        ++output;
                    }
                }
            }
        }
        else
        {
            if (fujiRotate90 == false)
            {
                unsigned short* input = rawProcessor->imgdata.rawdata.raw_image;
                input += sizes->left_margin + sizes->top_margin * sizes->raw_width;
                for (unsigned int row = sizes->top_margin; row < sizes->height + sizes->top_margin; row++)
                {
                    memcpy(output, input, sizes->width * sizeof(unsigned short));
                    input += sizes->raw_width;
                    output += sizes->width;
                }
            }
            else
            {
                for (unsigned int col = sizes->left_margin; col < sizes->width + sizes->left_margin; col++)
                {
                    for (unsigned int row = sizes->top_margin; row < sizes->height + sizes->top_margin; row++)
                    {
                        *output = rawProcessor->imgdata.rawdata.raw_image[row * sizes->raw_width + col];
                        ++output;
                    }
                }
            }
        }

    }
/*
    else
    {
        printf("LibRaw: unsupported decoder\n");
        rawProcessor->recycle();
        return;
    }
*/
#else
    if (m_Pattern == 0)
    {
        unsigned short* output = (unsigned short*)m_Buffer.fData;

        for (unsigned int row = 0; row < sizes->iheight; row++)
        {
            for (unsigned int col = 0; col < sizes->iwidth; col++)
            {
                for (uint32 color = 0; color < m_Channels; color++)
                {
                    *output = rawProcessor->imgdata.image[row * sizes->iwidth + col][color];
                    ++output;
                }
            }
        }
    }
    else
    {
        if (!iparams->cdesc[3])
            iparams->cdesc[3] = 'G';

        unsigned short* output = (unsigned short*)m_Buffer.fData;

        if (fujiRotate90 == false)
        {
            for (unsigned int row = 0; row < sizes->iheight; row++)
            {
                for (unsigned int col = 0; col < sizes->iwidth; col++)
                {
                    *output = rawProcessor->imgdata.image[row * sizes->iwidth + col][rawProcessor->COLOR(row, col)];
                    ++output;
                }
            }
        }
        else
        {
            for (unsigned int col = 0; col < sizes->iwidth; col++)
            {
                for (unsigned int row = 0; row < sizes->iheight; row++)
                {
                    *output = rawProcessor->imgdata.image[row * sizes->iwidth + col][rawProcessor->COLOR(row, col)];
                    ++output;
                }
            }
        }
    }
#endif

    for(uint32 i = 0; i < 4; i++)
    {
        switch(iparams->cdesc[i])
        {
        case 'R':
            m_CFAPlaneColor[i] = colorKeyRed;
            break;
        case 'G':
            m_CFAPlaneColor[i] = colorKeyGreen;
            break;
        case 'B':
            m_CFAPlaneColor[i] = colorKeyBlue;
            break;
        case 'C':
            m_CFAPlaneColor[i] = colorKeyCyan;
            break;
        case 'M':
            m_CFAPlaneColor[i] = colorKeyMagenta;
            break;
        case 'Y':
            m_CFAPlaneColor[i] = colorKeyYellow;
            break;
        default:
            m_CFAPlaneColor[i] = colorKeyMaxEnum;
        }
    }

    m_DefaultScaleH = dng_urational(finalWidth, activeWidth);
    m_DefaultScaleV = dng_urational(finalHeight, activeHeight);

    if (m_Pattern != 0)
    {
        m_DefaultCropOriginH = dng_urational(8, 1);
        m_DefaultCropOriginV = dng_urational(8, 1);
        m_DefaultCropSizeH = dng_urational(activeWidth - 16, 1);
        m_DefaultCropSizeV = dng_urational(activeHeight - 16, 1);
    }
    else
    {
        m_DefaultCropOriginH = dng_urational(0, 1);
        m_DefaultCropOriginV = dng_urational(0, 1);
        m_DefaultCropSizeH = dng_urational(activeWidth, 1);
        m_DefaultCropSizeV = dng_urational(activeHeight, 1);
    }

    if (entireSensorData == true)
    {
        m_ActiveArea = dng_rect(sizes->top_margin,
                                sizes->left_margin,
                                sizes->top_margin + activeHeight,
                                sizes->left_margin + activeWidth);
    }
    else
    {
        m_ActiveArea = dng_rect(activeHeight, activeWidth);
    }

    m_CameraNeutral = dng_vector(m_Channels);
    for (uint32 i = 0; i < m_Channels; i++)
    {
        m_CameraNeutral[i] = 1.0 / colors->cam_mul[i];
    }

    m_MakeName.Set_ASCII(iparams->make);
    m_ModelName.Set_ASCII(iparams->model);

    m_BlackLevel = dng_vector(4);
    m_WhiteLevel = dng_vector(4);
    for (int i = 0; i < 4; i++)
    {
        m_BlackLevel[i] = colors->black + colors->cblack[i];
        m_WhiteLevel[i] = colors->maximum;
    }

    switch (m_Channels)
    {
    case 3:
    {
        dng_matrix_3by3 camXYZ;
        camXYZ[0][0] = colors->cam_xyz[0][0];
        camXYZ[0][1] = colors->cam_xyz[0][1];
        camXYZ[0][2] = colors->cam_xyz[0][2];
        camXYZ[1][0] = colors->cam_xyz[1][0];
        camXYZ[1][1] = colors->cam_xyz[1][1];
        camXYZ[1][2] = colors->cam_xyz[1][2];
        camXYZ[2][0] = colors->cam_xyz[2][0];
        camXYZ[2][1] = colors->cam_xyz[2][1];
        camXYZ[2][2] = colors->cam_xyz[2][2];
        if (camXYZ.MaxEntry() == 0.0)
        {
            printf("Warning, camera XYZ Matrix is null\n");
            camXYZ = dng_matrix_3by3(1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0);
        }

        m_ColorMatrix = camXYZ;

        break;
    }
    case 4:
    {
        dng_matrix_4by3 camXYZ;
        camXYZ[0][0] = colors->cam_xyz[0][0];
        camXYZ[0][1] = colors->cam_xyz[0][1];
        camXYZ[0][2] = colors->cam_xyz[0][2];
        camXYZ[1][0] = colors->cam_xyz[1][0];
        camXYZ[1][1] = colors->cam_xyz[1][1];
        camXYZ[1][2] = colors->cam_xyz[1][2];
        camXYZ[2][0] = colors->cam_xyz[2][0];
        camXYZ[2][1] = colors->cam_xyz[2][1];
        camXYZ[2][2] = colors->cam_xyz[2][2];
        camXYZ[3][0] = colors->cam_xyz[3][0];
        camXYZ[3][1] = colors->cam_xyz[3][1];
        camXYZ[3][2] = colors->cam_xyz[3][2];
        if (camXYZ.MaxEntry() == 0.0)
        {
            printf("Warning, camera XYZ Matrix is null\n");
            camXYZ = dng_matrix_4by3(0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0);
        }

        m_ColorMatrix = camXYZ;

        break;
    }
    }

    rawProcessor->recycle();
}

LibRawImage::LibRawImage(const dng_rect &bounds,
                         uint32 planes,
                         uint32 pixelType,
                         dng_memory_allocator &allocator)    
    : dng_image(bounds, planes, pixelType),
      m_Allocator(allocator),
      m_Buffer(),
      m_Memory()
{
    uint32 pixelSize = TagTypeSize(pixelType);

    uint32 bytes = bounds.H() * bounds.W() * planes * pixelSize;

    m_Memory.Reset(allocator.Allocate(bytes));

    m_Buffer.fArea = bounds;

    m_Buffer.fPlane  = 0;
    m_Buffer.fPlanes = planes;

    m_Buffer.fRowStep   = planes * bounds.W();
    m_Buffer.fColStep   = planes;
    m_Buffer.fPlaneStep = 1;

    m_Buffer.fPixelType = pixelType;
    m_Buffer.fPixelSize = pixelSize;

    m_Buffer.fData = m_Memory->Buffer();
}

LibRawImage::~LibRawImage(void)
{
}

dng_image* LibRawImage::Clone() const
{
    AutoPtr<LibRawImage> result(new LibRawImage(Bounds(), Planes(), PixelType(), m_Allocator));

    result->m_Buffer.CopyArea(m_Buffer, Bounds(), 0, Planes());

    return result.Release ();
}

void LibRawImage::AcquireTileBuffer(dng_tile_buffer &buffer,
                                    const dng_rect &area,
                                    bool dirty) const
{
    buffer.fArea = area;

    buffer.fPlane      = m_Buffer.fPlane;
    buffer.fPlanes     = m_Buffer.fPlanes;
    buffer.fRowStep    = m_Buffer.fRowStep;
    buffer.fColStep    = m_Buffer.fColStep;
    buffer.fPlaneStep  = m_Buffer.fPlaneStep;
    buffer.fPixelType  = m_Buffer.fPixelType;
    buffer.fPixelSize  = m_Buffer.fPixelSize;

    buffer.fData = (void *) m_Buffer.ConstPixel(buffer.fArea.t, buffer.fArea.l, buffer.fPlane);

    buffer.fDirty = dirty;
}

const dng_vector& LibRawImage::CameraNeutral() const
{
    return m_CameraNeutral;
}

const dng_string& LibRawImage::ModelName() const
{
    return m_ModelName;
}

const dng_string& LibRawImage::MakeName() const
{
    return m_MakeName;
}

const dng_rect& LibRawImage::ActiveArea() const
{
    return m_ActiveArea;
}

uint32 LibRawImage::Channels() const
{
    return m_Channels;
}

const dng_matrix& LibRawImage::ColorMatrix() const
{
    return m_ColorMatrix;
}

real64 LibRawImage::BlackLevel(uint32 channel) const
{
    if (channel < m_BlackLevel.Count())
    {
        return m_BlackLevel[channel];
    }

    return 0.0;
}

real64 LibRawImage::WhiteLevel(uint32 channel) const
{
    if (channel < m_WhiteLevel.Count())
    {
        return m_WhiteLevel[channel];
    }

    return 0.0;
}

const dng_urational& LibRawImage::DefaultScaleH() const
{
    return m_DefaultScaleH;
}

const dng_urational& LibRawImage::DefaultScaleV() const
{
    return m_DefaultScaleV;
}

const dng_urational& LibRawImage::DefaultCropSizeH() const
{
    return m_DefaultCropSizeH;
}

const dng_urational& LibRawImage::DefaultCropSizeV() const
{
    return m_DefaultCropSizeV;
}

const dng_urational& LibRawImage::DefaultCropOriginH() const
{
    return m_DefaultCropOriginH;
}

const dng_urational& LibRawImage::DefaultCropOriginV() const
{
    return m_DefaultCropOriginV;
}

const dng_orientation LibRawImage::Orientation() const
{
    return m_BaseOrientation;
}

uint32 LibRawImage::Pattern() const
{
    return m_Pattern;
}

ColorKeyCode LibRawImage::ColorKey(uint32 plane) const
{
    if (plane < 4)
    {
        return m_CFAPlaneColor[plane];
    }
    return colorKeyMaxEnum;
}
