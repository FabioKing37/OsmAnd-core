#include "MapRenderer_OpenGL.h"

#include <assert.h>

#include "IMapRenderer.h"
#include "IMapTileProvider.h"
#include "IMapBitmapTileProvider.h"
#include "IMapElevationDataProvider.h"
#include "OsmAndLogging.h"
#include "OsmAndUtilities.h"

OsmAnd::MapRenderer_OpenGL::MapRenderer_OpenGL()
    : _textureSampler_Bitmap_NoAtlas(0)
    , _textureSampler_Bitmap_Atlas(0)
    , _textureSampler_ElevationData_NoAtlas(0)
    , _textureSampler_ElevationData_Atlas(0)
{
}

OsmAnd::MapRenderer_OpenGL::~MapRenderer_OpenGL()
{
}

GLenum OsmAnd::MapRenderer_OpenGL::validateResult()
{
    auto result = glGetError();
    if(result == GL_NO_ERROR)
        return result;

    LogPrintf(LogSeverityLevel::Error, "OpenGL error 0x%08x : %s\n", result, gluErrorString(result));

    return result;
}

GLuint OsmAnd::MapRenderer_OpenGL::compileShader( GLenum shaderType, const char* source )
{
    GLuint shader;

    assert(glCreateShader);
    shader = glCreateShader(shaderType);
    GL_CHECK_RESULT;

    const GLint sourceLen = static_cast<GLint>(strlen(source));
    assert(glShaderSource);
    glShaderSource(shader, 1, &source, &sourceLen);
    GL_CHECK_RESULT;

    assert(glCompileShader);
    glCompileShader(shader);
    GL_CHECK_RESULT;

    // Check if compiled
    GLint didCompile;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &didCompile);
    GL_CHECK_RESULT;
    if(didCompile != GL_TRUE)
    {
        GLint logBufferLen = 0;	
        GLsizei logLen = 0;
        assert(glGetShaderiv);
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &logBufferLen);       
        GL_CHECK_RESULT;
        if (logBufferLen > 1)
        {
            GLchar* log = (GLchar*)malloc(logBufferLen);
            glGetShaderInfoLog(shader, logBufferLen, &logLen, log);
            GL_CHECK_RESULT;
            assert(logLen + 1 == logBufferLen);
            LogPrintf(LogSeverityLevel::Error, "Failed to compile GLSL shader:\n%s\n", log);
            free(log);
        }

        glDeleteShader(shader);
        shader = 0;
        return shader;
    }

    return shader;
}

GLuint OsmAnd::MapRenderer_OpenGL::linkProgram( GLuint shadersCount, GLuint *shaders )
{
    GLuint program = 0;

    assert(glCreateProgram);
    program = glCreateProgram();
    GL_CHECK_RESULT;

    assert(glAttachShader);
    for(int shaderIdx = 0; shaderIdx < shadersCount; shaderIdx++)
    {
        glAttachShader(program, shaders[shaderIdx]);
        GL_CHECK_RESULT;
    }
    
    assert(glLinkProgram);
    glLinkProgram(program);
    GL_CHECK_RESULT;

    GLint linkSuccessful;
    assert(glGetProgramiv);
    glGetProgramiv(program, GL_LINK_STATUS, &linkSuccessful);
    GL_CHECK_RESULT;
    if(linkSuccessful != GL_TRUE)
    {
        GLint logBufferLen = 0;	
        GLsizei logLen = 0;
        glGetProgramiv(program, GL_INFO_LOG_LENGTH, &logBufferLen);       
        GL_CHECK_RESULT;
        if (logBufferLen > 1)
        {
            GLchar* log = (GLchar*)malloc(logBufferLen);
            glGetProgramInfoLog(program, logBufferLen, &logLen, log);
            GL_CHECK_RESULT;
            assert(logLen + 1 == logBufferLen);
            LogPrintf(LogSeverityLevel::Error, "Failed to link GLSL program:\n%s\n", log);
            free(log);
        }

        glDeleteProgram(program);
        GL_CHECK_RESULT;
        program = 0;
        return program;
    }

    // Show some info
    assert(glGetProgramiv);
    GLint attributesCount;
    glGetProgramiv(program, GL_ACTIVE_ATTRIBUTES, &attributesCount);
    GL_CHECK_RESULT;
    LogPrintf(LogSeverityLevel::Info, "GLSL program %d has %d input variable(s)\n", program, attributesCount);

    assert(glGetProgramiv);
    GLint uniformsCount;
    glGetProgramiv(program, GL_ACTIVE_UNIFORMS, &uniformsCount);
    GL_CHECK_RESULT;
    LogPrintf(LogSeverityLevel::Info, "GLSL program %d has %d parameter variable(s)\n", program, uniformsCount);

    return program;
}

void OsmAnd::MapRenderer_OpenGL::initializeRendering()
{
    MapRenderer_BaseOpenGL::initializeRendering();

    const auto glVersionString = glGetString(GL_VERSION);
    GL_CHECK_RESULT;
    GLint glVersion[2];
    glGetIntegerv(GL_MAJOR_VERSION, &glVersion[0]);
    GL_CHECK_RESULT;
    glGetIntegerv(GL_MINOR_VERSION, &glVersion[1]);
    GL_CHECK_RESULT;
    LogPrintf(LogSeverityLevel::Info, "Using OpenGL version %d.%d [%s]\n", glVersion[0], glVersion[1], glVersionString);
    assert(glVersion[0] >= 3);

    glGetIntegerv(GL_MAX_TEXTURE_SIZE, reinterpret_cast<GLint*>(&_maxTextureSize));
    GL_CHECK_RESULT;
    LogPrintf(LogSeverityLevel::Info, "OpenGL maximal texture size %dx%d\n", _maxTextureSize, _maxTextureSize);

    GLint maxTextureUnitsInFragmentShader;
    glGetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS, &maxTextureUnitsInFragmentShader);
    GL_CHECK_RESULT;
    LogPrintf(LogSeverityLevel::Info, "OpenGL maximal texture units in fragment shader %d\n", maxTextureUnitsInFragmentShader);
    assert(maxTextureUnitsInFragmentShader >= (IMapRenderer::TileLayerId::IdsCount - IMapRenderer::RasterMap));

    GLint maxTextureUnitsInVertexShader;
    glGetIntegerv(GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS, &maxTextureUnitsInVertexShader);
    GL_CHECK_RESULT;
    LogPrintf(LogSeverityLevel::Info, "OpenGL maximal texture units in vertex shader %d\n", maxTextureUnitsInVertexShader);
    assert(maxTextureUnitsInVertexShader >= IMapRenderer::RasterMap);
    
    GLint maxUniformsPerProgram;
    glGetIntegerv(GL_MAX_UNIFORM_LOCATIONS, &maxUniformsPerProgram);
    GL_CHECK_RESULT;
    LogPrintf(LogSeverityLevel::Info, "OpenGL maximal parameter variables per program %d\n", maxUniformsPerProgram);

    GL_CHECK_RESULT;
    glewExperimental = GL_TRUE;
    glewInit();
    // For now, silence OpenGL error here, it's inside GLEW, so it's not ours
    (void)glGetError();
    //GL_CHECK_RESULT;

    // Bitmap (Atlas)
    glGenSamplers(1, &_textureSampler_Bitmap_Atlas);
    GL_CHECK_RESULT;
    glSamplerParameteri(_textureSampler_Bitmap_Atlas, GL_TEXTURE_WRAP_S, GL_CLAMP);
    GL_CHECK_RESULT;
    glSamplerParameteri(_textureSampler_Bitmap_Atlas, GL_TEXTURE_WRAP_T, GL_CLAMP);
    GL_CHECK_RESULT;
    glSamplerParameteri(_textureSampler_Bitmap_Atlas, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    GL_CHECK_RESULT;
    glSamplerParameteri(_textureSampler_Bitmap_Atlas, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    GL_CHECK_RESULT;

    // ElevationData (Atlas)
    glGenSamplers(1, &_textureSampler_ElevationData_Atlas);
    GL_CHECK_RESULT;
    glSamplerParameteri(_textureSampler_ElevationData_Atlas, GL_TEXTURE_WRAP_S, GL_CLAMP);
    GL_CHECK_RESULT;
    glSamplerParameteri(_textureSampler_ElevationData_Atlas, GL_TEXTURE_WRAP_T, GL_CLAMP);
    GL_CHECK_RESULT;
    glSamplerParameteri(_textureSampler_ElevationData_Atlas, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    GL_CHECK_RESULT;
    glSamplerParameteri(_textureSampler_ElevationData_Atlas, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    GL_CHECK_RESULT;

    // Bitmap (No atlas)
    glGenSamplers(1, &_textureSampler_Bitmap_NoAtlas);
    GL_CHECK_RESULT;
    glSamplerParameteri(_textureSampler_Bitmap_NoAtlas, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    GL_CHECK_RESULT;
    glSamplerParameteri(_textureSampler_Bitmap_NoAtlas, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    GL_CHECK_RESULT;
    glSamplerParameteri(_textureSampler_Bitmap_NoAtlas, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    GL_CHECK_RESULT;
    glSamplerParameteri(_textureSampler_Bitmap_NoAtlas, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    GL_CHECK_RESULT;

    // ElevationData (No atlas)
    glGenSamplers(1, &_textureSampler_ElevationData_NoAtlas);
    GL_CHECK_RESULT;
    glSamplerParameteri(_textureSampler_ElevationData_NoAtlas, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    GL_CHECK_RESULT;
    glSamplerParameteri(_textureSampler_ElevationData_NoAtlas, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    GL_CHECK_RESULT;
    glSamplerParameteri(_textureSampler_ElevationData_NoAtlas, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    GL_CHECK_RESULT;
    glSamplerParameteri(_textureSampler_ElevationData_NoAtlas, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    GL_CHECK_RESULT;

    glShadeModel(GL_SMOOTH); 
    GL_CHECK_RESULT;

    glHint(GL_POLYGON_SMOOTH_HINT, GL_NICEST);
    GL_CHECK_RESULT;
    glEnable(GL_POLYGON_SMOOTH);
    GL_CHECK_RESULT;

    glHint(GL_POINT_SMOOTH_HINT, GL_NICEST);
    GL_CHECK_RESULT;
    glEnable(GL_POINT_SMOOTH);
    GL_CHECK_RESULT;

    glHint(GL_LINE_SMOOTH_HINT, GL_NICEST);
    GL_CHECK_RESULT;
    glEnable(GL_LINE_SMOOTH);
    GL_CHECK_RESULT;

    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    GL_CHECK_RESULT;

    glClearDepth(1.0f);
    GL_CHECK_RESULT;

    glEnable(GL_DEPTH_TEST);
    GL_CHECK_RESULT;

    glDepthFunc(GL_LEQUAL);
    GL_CHECK_RESULT;
}

void OsmAnd::MapRenderer_OpenGL::releaseRendering()
{
    if(_textureSampler_Bitmap_NoAtlas != 0)
    {
        glDeleteSamplers(1, &_textureSampler_Bitmap_NoAtlas);
        GL_CHECK_RESULT;
        _textureSampler_Bitmap_NoAtlas = 0;
    }
    
    if(_textureSampler_Bitmap_Atlas != 0)
    {
        glDeleteSamplers(1, &_textureSampler_Bitmap_Atlas);
        GL_CHECK_RESULT;
        _textureSampler_Bitmap_Atlas = 0;
    }

    if(_textureSampler_ElevationData_NoAtlas != 0)
    {
        glDeleteSamplers(1, &_textureSampler_ElevationData_NoAtlas);
        GL_CHECK_RESULT;
        _textureSampler_ElevationData_NoAtlas = 0;
    }

    if(_textureSampler_ElevationData_Atlas != 0)
    {
        glDeleteSamplers(1, &_textureSampler_ElevationData_Atlas);
        GL_CHECK_RESULT;
        _textureSampler_ElevationData_Atlas = 0;
    }

    MapRenderer_BaseOpenGL::releaseRendering();
}

void OsmAnd::MapRenderer_OpenGL::uploadTileToTexture( TileLayerId layerId, const TileId& tileId, uint32_t zoom, const std::shared_ptr<IMapTileProvider::Tile>& tile, uint64_t& outAtlasPoolId, void*& outTextureRef, int& outAtlasSlotIndex, size_t& outUsedMemory )
{
    auto& tileLayer = _tileLayers[layerId];

    GLenum textureFormat = GL_INVALID_ENUM;
    uint32_t tileSize = GL_INVALID_ENUM;
    uint32_t padding = GL_INVALID_ENUM;
    GLenum sourceFormat = GL_INVALID_ENUM;
    GLenum sourceFormatType = GL_INVALID_ENUM;
    size_t sourcePixelByteSize = 0;
    if(tile->type == IMapTileProvider::Bitmap)
    {
        auto bitmapTile = static_cast<IMapBitmapTileProvider::Tile*>(tile.get());

        switch (bitmapTile->format)
        {
        case IMapBitmapTileProvider::ARGB_8888:
            textureFormat = _activeConfig.force16bitColorDepthLimit ? GL_RGB5_A1 /* or GL_RGBA4? */ : GL_RGBA8;
            sourceFormat = GL_BGRA;
            sourceFormatType = GL_UNSIGNED_INT_8_8_8_8_REV;
            sourcePixelByteSize = 4;
            break;
        case IMapBitmapTileProvider::ARGB_4444:
            textureFormat = GL_RGBA4;
            sourceFormat = GL_BGRA;
            sourceFormatType = GL_UNSIGNED_SHORT_4_4_4_4_REV;
            sourcePixelByteSize = 2;
            break;
        case IMapBitmapTileProvider::RGB_565:
            textureFormat = GL_RGB5;
            sourceFormat = GL_RGB;
            sourceFormatType = GL_UNSIGNED_SHORT_5_6_5;
            sourcePixelByteSize = 2;
            break;
        }
        tileSize = bitmapTile->width;
        assert(bitmapTile->width == bitmapTile->height);
        padding = BitmapTileTexelPadding;
    }
    else if(tile->type == IMapTileProvider::ElevationData)
    {
        //TODO: set proper format for elevation data
        textureFormat = GL_R16I;
        tileSize = 100500;
        padding = 0;

        assert(false);
    }
    const auto fullTileSize = tileSize + 2*padding;

    outAtlasPoolId = (static_cast<uint64_t>(textureFormat) << 32) | fullTileSize;
    outUsedMemory = fullTileSize * fullTileSize * sourcePixelByteSize;

    auto itAtlasPool = tileLayer._atlasTexturePools.find(outAtlasPoolId);
    if(_activeConfig.textureAtlasesAllowed)
    {
        if(itAtlasPool == tileLayer._atlasTexturePools.end())
            itAtlasPool = tileLayer._atlasTexturePools.insert(outAtlasPoolId, IMapRenderer::AtlasTexturePool());
        auto& atlasPool = *itAtlasPool;

        if(atlasPool._textureSize == 0)
        {
            const auto idealSize = MaxTilesPerTextureAtlasSide * fullTileSize;
            const auto largerSize = Utilities::getNextPowerOfTwo(idealSize);
            const auto smallerSize = largerSize >> 1;

            // If larger texture is more than ideal over 15%, select reduced size
            if(static_cast<float>(largerSize) > idealSize * 1.15f)
                atlasPool._textureSize = qMin(_maxTextureSize, smallerSize);
            else
                atlasPool._textureSize = qMin(_maxTextureSize, largerSize);
        }
    }
    else
    {
        const auto tileSizeNPOT = Utilities::getNextPowerOfTwo(tileSize);
        if(tileSizeNPOT != tileSize)
        {
            if(itAtlasPool == tileLayer._atlasTexturePools.end())
                itAtlasPool = tileLayer._atlasTexturePools.insert(outAtlasPoolId, IMapRenderer::AtlasTexturePool());
            auto& atlasPool = *itAtlasPool;

            atlasPool._textureSize = Utilities::getNextPowerOfTwo(fullTileSize);
            outUsedMemory = atlasPool._textureSize * atlasPool._textureSize * sourcePixelByteSize;
        }
    }
    // Use atlas textures if possible
    if(itAtlasPool != tileLayer._atlasTexturePools.end() && itAtlasPool->_textureSize != 0)
    {
        auto& atlasPool = *itAtlasPool;
        atlasPool._padding = padding;
        
        atlasPool._tilePaddingN = static_cast<float>(padding) / static_cast<float>(atlasPool._textureSize);
        atlasPool._tileSizeN = static_cast<float>(fullTileSize) / static_cast<float>(atlasPool._textureSize);
        atlasPool._slotsPerSide = atlasPool._textureSize / fullTileSize;

        GLuint atlasTexture = 0;
        int atlasSlotIndex = -1;

        // If we have freed slots on previous atlases, let's use them
        if(!atlasPool._freedSlots.isEmpty())
        {
            const auto& itFreeSlot = atlasPool._freedSlots.begin();
            atlasTexture = reinterpret_cast<GLuint>(itFreeSlot.key());
            atlasSlotIndex = itFreeSlot.value();

            // Mark slot as occupied
            atlasPool._freedSlots.erase(itFreeSlot);
            tileLayer._textureRefCount[itFreeSlot.key()]++;
        }
        // If we've never allocated any atlas yet, or free slot is beyond allowed range - allocate new
        else if(atlasPool._lastNonFullTextureRef == nullptr || atlasPool._firstFreeSlotIndex == atlasPool._slotsPerSide * atlasPool._slotsPerSide)
        {
            // Allocate texture id
            GLuint texture;
            glGenTextures(1, &texture);
            GL_CHECK_RESULT;
            assert(texture != 0);

            // Select this texture
            glBindTexture(GL_TEXTURE_2D, texture);
            GL_CHECK_RESULT;

            // Allocate space for this texture
            assert(glTexStorage2D);
            glTexStorage2D(GL_TEXTURE_2D, 1, textureFormat, atlasPool._textureSize, atlasPool._textureSize);
            GL_CHECK_RESULT;

            // Deselect texture
            glBindTexture(GL_TEXTURE_2D, 0);
            GL_CHECK_RESULT;

            atlasPool._lastNonFullTextureRef = reinterpret_cast<void*>(texture);
            atlasPool._firstFreeSlotIndex = 1;
            tileLayer._textureRefCount.insert(atlasPool._lastNonFullTextureRef, 1);

            atlasTexture = texture;
            atlasSlotIndex = 0;

#if 0
            if(tile->type == IMapTileProvider::Bitmap)
            {
                // In debug mode, fill entire texture with single RED color if this is bitmap
                uint8_t* fillBuffer = new uint8_t[atlasPool._textureSize * atlasPool._textureSize * 4];
                for(uint32_t idx = 0; idx < atlasPool._textureSize * atlasPool._textureSize; idx++)
                {
                    *reinterpret_cast<uint32_t*>(&fillBuffer[idx * 4]) = 0xFFFF0000;
                }
                glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, atlasPool._textureSize, atlasPool._textureSize, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, fillBuffer);
                delete[] fillBuffer;
            }
#endif
        }
        // Or let's just continue using current atlas
        else
        {
            atlasTexture = (GLuint)atlasPool._lastNonFullTextureRef;
            atlasSlotIndex = atlasPool._firstFreeSlotIndex;
            atlasPool._firstFreeSlotIndex++;
            tileLayer._textureRefCount[atlasPool._lastNonFullTextureRef]++;
        }

        // Select atlas as active texture
        glBindTexture(GL_TEXTURE_2D, atlasTexture);
        GL_CHECK_RESULT;

        // Tile area offset
        const auto yOffset = (atlasSlotIndex / atlasPool._slotsPerSide) * fullTileSize;
        const auto xOffset = (atlasSlotIndex % atlasPool._slotsPerSide) * fullTileSize;

        if(padding > 0)
        {
            // Fill corners
            const auto pixelsCount = padding * padding;
            const size_t cornerDataSize = sourcePixelByteSize * pixelsCount;
            uint8_t* pCornerData = new uint8_t[cornerDataSize];
            const GLvoid* pCornerPixel;
            
            // Set stride
            glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
            GL_CHECK_RESULT;

            // Top-left corner
            pCornerPixel = tile->data;
            for(int idx = 0; idx < pixelsCount; idx++)
                memcpy(pCornerData + (sourcePixelByteSize * idx), pCornerPixel, sourcePixelByteSize);
            glTexSubImage2D(GL_TEXTURE_2D, 0,
                xOffset, yOffset, padding, padding,
                sourceFormat,
                sourceFormatType,
                pCornerData);
            GL_CHECK_RESULT;

            // Top-right corner
            pCornerPixel = reinterpret_cast<const uint8_t*>(tile->data) + (tileSize - 1) * sourcePixelByteSize;
            for(int idx = 0; idx < pixelsCount; idx++)
                memcpy(pCornerData + (sourcePixelByteSize * idx), pCornerPixel, sourcePixelByteSize);
            glTexSubImage2D(GL_TEXTURE_2D, 0,
                xOffset + padding + tileSize, yOffset, padding, padding,
                sourceFormat,
                sourceFormatType,
                pCornerData);
            GL_CHECK_RESULT;

            // Bottom-left corner
            pCornerPixel = reinterpret_cast<const uint8_t*>(tile->data) + (tileSize - 1) * tile->rowLength;
            for(int idx = 0; idx < pixelsCount; idx++)
                memcpy(pCornerData + (sourcePixelByteSize * idx), pCornerPixel, sourcePixelByteSize);
            glTexSubImage2D(GL_TEXTURE_2D, 0,
                xOffset, yOffset + padding + tileSize, padding, padding,
                sourceFormat,
                sourceFormatType,
                pCornerData);
            GL_CHECK_RESULT;

            // Bottom-right corner
            pCornerPixel = reinterpret_cast<const uint8_t*>(tile->data) + (tileSize - 1) * tile->rowLength + (tileSize - 1) * sourcePixelByteSize;
            for(int idx = 0; idx < pixelsCount; idx++)
                memcpy(pCornerData + (sourcePixelByteSize * idx), pCornerPixel, sourcePixelByteSize);
            glTexSubImage2D(GL_TEXTURE_2D, 0,
                xOffset + padding + tileSize, yOffset + padding + tileSize, padding, padding,
                sourceFormat,
                sourceFormatType,
                pCornerData);
            GL_CHECK_RESULT;

            delete[] pCornerData;

            // Set stride
            glPixelStorei(GL_UNPACK_ROW_LENGTH, tile->rowLength / sourcePixelByteSize);
            GL_CHECK_RESULT;

            // Left column duplicate
            for(int idx = 0; idx < padding; idx++)
            {
                glTexSubImage2D(GL_TEXTURE_2D, 0,
                    xOffset + idx, yOffset + padding, 1, (GLsizei)tileSize,
                    sourceFormat,
                    sourceFormatType,
                    tile->data);
                GL_CHECK_RESULT;
            }

            // Top row duplicate
            for(int idx = 0; idx < padding; idx++)
            {
                glTexSubImage2D(GL_TEXTURE_2D, 0,
                    xOffset + padding, yOffset + idx, (GLsizei)tileSize, 1,
                    sourceFormat,
                    sourceFormatType,
                    tile->data);
                GL_CHECK_RESULT;
            }

            // Right column duplicate
            for(int idx = 0; idx < padding; idx++)
            {
                glTexSubImage2D(GL_TEXTURE_2D, 0,
                    xOffset + padding + tileSize + idx, yOffset + padding, 1, (GLsizei)tileSize,
                    sourceFormat,
                    sourceFormatType,
                    reinterpret_cast<const uint8_t*>(tile->data) + (tileSize - 1) * sourcePixelByteSize);
                GL_CHECK_RESULT;
            }

            // Bottom row duplicate
            for(int idx = 0; idx < padding; idx++)
            {
                glTexSubImage2D(GL_TEXTURE_2D, 0,
                    xOffset + padding, yOffset + padding + tileSize + idx, (GLsizei)tileSize, 1,
                    sourceFormat,
                    sourceFormatType,
                    reinterpret_cast<const uint8_t*>(tile->data) + (tileSize - 1) * tile->rowLength);
                GL_CHECK_RESULT;
            }
        }

        // Set stride
        glPixelStorei(GL_UNPACK_ROW_LENGTH, tile->rowLength / sourcePixelByteSize);
        GL_CHECK_RESULT;

        // Main data
        glTexSubImage2D(GL_TEXTURE_2D, 0,
            xOffset + padding, yOffset + padding, (GLsizei)tileSize, (GLsizei)tileSize,
            sourceFormat,
            sourceFormatType,
            tile->data);
        GL_CHECK_RESULT;

        // Set stride
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        GL_CHECK_RESULT;

        // Deselect atlas as active texture
        glBindTexture(GL_TEXTURE_2D, 0);
        GL_CHECK_RESULT;

        outTextureRef = reinterpret_cast<void*>(atlasTexture);
        outAtlasSlotIndex = atlasSlotIndex;
    }
    // If not, fall back to use of one texture per tile. This will only happen in case if atlases are prohibited and tiles are POT
    else
    {
        // Create texture id
        GLuint texture;
        glGenTextures(1, &texture);
        GL_CHECK_RESULT;
        assert(texture != 0);

        // Activate texture
        glBindTexture(GL_TEXTURE_2D, texture);
        GL_CHECK_RESULT;

        // Set texture packing
        glPixelStorei(GL_UNPACK_ROW_LENGTH, tile->rowLength / sourcePixelByteSize);
        GL_CHECK_RESULT;

        // Allocate data
        glTexStorage2D(GL_TEXTURE_2D, 1,
            textureFormat,
            tileSize, tileSize);
        GL_CHECK_RESULT;

        // Upload data
        glTexSubImage2D(GL_TEXTURE_2D, 0,
            0, 0, (GLsizei)tileSize, (GLsizei)tileSize,
            sourceFormat,
            sourceFormatType,
            tile->data);
        GL_CHECK_RESULT;

        // Set stride
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        GL_CHECK_RESULT;

        // Deselect atlas as active texture
        glBindTexture(GL_TEXTURE_2D, 0);
        GL_CHECK_RESULT;

        outTextureRef = reinterpret_cast<void*>(texture);
        outAtlasSlotIndex = -1;
    }
}

void OsmAnd::MapRenderer_OpenGL::releaseTexture( void* textureRef )
{
    GLuint texture = reinterpret_cast<GLuint>(textureRef);

    glDeleteTextures(1, &texture);
    GL_CHECK_RESULT;
}

void OsmAnd::MapRenderer_OpenGL::findVariableLocation( GLuint program, GLint& location, const QString& name, const VariableType& type )
{
    if(type == VariableType::In)
        location = glGetAttribLocation(program, name.toStdString().c_str());
    else if(type == VariableType::Uniform)
        location = glGetUniformLocation(program, name.toStdString().c_str());
    GL_CHECK_RESULT;
    if(location == -1)
        LogPrintf(LogSeverityLevel::Error, "Variable '%s' (%s) was not found in GLSL program %d\n", name.toStdString().c_str(), type == In ? "In" : "Uniform", program);
    assert(location != -1);
    assert(!_programVariables[program].contains(type, location));
    _programVariables[program].insert(type, location);
}
