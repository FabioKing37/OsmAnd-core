#include "AtlasMapRenderer_OpenGL_Common.h"

#include <cassert>

#include <glm/gtc/type_ptr.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/transform.hpp>

#include <OsmAndCore/QtExtensions.h>
#include <QtGlobal>
#include <QtNumeric>
#include <QtMath>
#include <QThread>

#include <SkBitmap.h>

#include "IMapRenderer.h"
#include "IMapTileProvider.h"
#include "IMapBitmapTileProvider.h"
#include "IMapElevationDataProvider.h"
#include "IMapSymbolProvider.h"
#include "Logging.h"
#include "Utilities.h"
#include "OpenGL_Common/Utilities_OpenGL_Common.h"

const float OsmAnd::AtlasMapRenderer_OpenGL_Common::_zNear = 0.1f;

OsmAnd::AtlasMapRenderer_OpenGL_Common::AtlasMapRenderer_OpenGL_Common()
    : _tilePatchIndicesCount(0)
{
    memset(&_rasterMapStage, 0, sizeof(_rasterMapStage));
    memset(&_skyStage, 0, sizeof(_skyStage));
    memset(&_symbolsStage, 0, sizeof(_symbolsStage));
}

OsmAnd::AtlasMapRenderer_OpenGL_Common::~AtlasMapRenderer_OpenGL_Common()
{
}

void OsmAnd::AtlasMapRenderer_OpenGL_Common::allocateTilePatch( MapTileVertex* vertices, GLsizei verticesCount, GLushort* indices, GLsizei indicesCount )
{
    const auto gpuAPI = getGPUAPI();

    GL_CHECK_PRESENT(glGenBuffers);
    GL_CHECK_PRESENT(glBindBuffer);
    GL_CHECK_PRESENT(glBufferData);
    GL_CHECK_PRESENT(glEnableVertexAttribArray);
    GL_CHECK_PRESENT(glVertexAttribPointer);

    // Create VBO
    glGenBuffers(1, &_rasterMapStage.tilePatchVBO);
    GL_CHECK_RESULT;
    glBindBuffer(GL_ARRAY_BUFFER, _rasterMapStage.tilePatchVBO);
    GL_CHECK_RESULT;
    glBufferData(GL_ARRAY_BUFFER, verticesCount * sizeof(MapTileVertex), vertices, GL_STATIC_DRAW);
    GL_CHECK_RESULT;

    // Create IBO
    glGenBuffers(1, &_rasterMapStage.tilePatchIBO);
    GL_CHECK_RESULT;
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, _rasterMapStage.tilePatchIBO);
    GL_CHECK_RESULT;
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, indicesCount * sizeof(GLushort), indices, GL_STATIC_DRAW);
    GL_CHECK_RESULT;

    // Create VAO
    gpuAPI->glGenVertexArrays_wrapper(1, &_rasterMapStage.tilePatchVAO);
    GL_CHECK_RESULT;
    gpuAPI->glBindVertexArray_wrapper(_rasterMapStage.tilePatchVAO);
    GL_CHECK_RESULT;

    // Bind IBO to VAO
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, _rasterMapStage.tilePatchIBO);
    GL_CHECK_RESULT;

    // Bind VBO to VAO
    glBindBuffer(GL_ARRAY_BUFFER, _rasterMapStage.tilePatchVBO);
    GL_CHECK_RESULT;
    for(auto variationId = 0u, maxActiveMapLayers = 1u; variationId < RasterMapLayersCount; variationId++, maxActiveMapLayers++)
    {
        auto& stageVariation = _rasterMapStage.variations[variationId];
        
        glEnableVertexAttribArray(stageVariation.vs.in.vertexPosition);
        GL_CHECK_RESULT;
        glVertexAttribPointer(stageVariation.vs.in.vertexPosition, 2, GL_FLOAT, GL_FALSE, sizeof(MapTileVertex), reinterpret_cast<GLvoid*>(offsetof(MapTileVertex, positionXZ)));
        GL_CHECK_RESULT;
        glEnableVertexAttribArray(stageVariation.vs.in.vertexTexCoords);
        GL_CHECK_RESULT;
        glVertexAttribPointer(stageVariation.vs.in.vertexTexCoords, 2, GL_FLOAT, GL_FALSE, sizeof(MapTileVertex), reinterpret_cast<GLvoid*>(offsetof(MapTileVertex, textureUV)));
        GL_CHECK_RESULT;
    }

    gpuAPI->glBindVertexArray_wrapper(0);
    GL_CHECK_RESULT;
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    GL_CHECK_RESULT;
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    GL_CHECK_RESULT;
}

void OsmAnd::AtlasMapRenderer_OpenGL_Common::releaseTilePatch()
{
    const auto gpuAPI = getGPUAPI();

    GL_CHECK_PRESENT(glDeleteBuffers);

    if(_rasterMapStage.tilePatchVAO)
    {
        gpuAPI->glDeleteVertexArrays_wrapper(1, &_rasterMapStage.tilePatchVAO);
        GL_CHECK_RESULT;
        _rasterMapStage.tilePatchVAO = 0;
    }

    if(_rasterMapStage.tilePatchIBO)
    {
        glDeleteBuffers(1, &_skyStage.skyplaneIBO);
        GL_CHECK_RESULT;
        _skyStage.skyplaneIBO = 0;
    }

    if(_rasterMapStage.tilePatchVBO)
    {
        glDeleteBuffers(1, &_rasterMapStage.tilePatchVBO);
        GL_CHECK_RESULT;
        _rasterMapStage.tilePatchVBO = 0;
    }
}

void OsmAnd::AtlasMapRenderer_OpenGL_Common::initializeRasterMapStage()
{
    const auto gpuAPI = getGPUAPI();

    const auto& vertexShader = QString::fromLatin1(
        // Input data
        "INPUT vec2 in_vs_vertexPosition;                                                                                   ""\n"
        "INPUT vec2 in_vs_vertexTexCoords;                                                                                  ""\n"
        "#if !VERTEX_TEXTURE_FETCH_SUPPORTED                                                                                ""\n"
        "    INPUT float in_vs_vertexElevation;                                                                             ""\n"
        "#endif // !VERTEX_TEXTURE_FETCH_SUPPORTED                                                                          ""\n"
        "                                                                                                                   ""\n"
        // Output data to next shader stages
        "PARAM_OUTPUT vec2 v2f_texCoordsPerLayer[%RasterLayersCount%];                                                      ""\n"
        "#if TEXTURE_LOD_SUPPORTED                                                                                          ""\n"
        "    PARAM_OUTPUT float v2f_mipmapLOD;                                                                              ""\n"
        "#endif // TEXTURE_LOD_SUPPORTED                                                                                    ""\n"
        "                                                                                                                   ""\n"
        // Parameters: common data
        "uniform mat4 param_vs_mProjectionView;                                                                             ""\n"
        "uniform vec2 param_vs_targetInTilePosN;                                                                            ""\n"
        "#if TEXTURE_LOD_SUPPORTED                                                                                          ""\n"
        "    uniform float param_vs_distanceFromCameraToTarget;                                                             ""\n"
        "    uniform float param_vs_cameraElevationAngleN;                                                                  ""\n"
        "    uniform vec2 param_vs_groundCameraPosition;                                                                    ""\n"
        "    uniform float param_vs_scaleToRetainProjectedSize;                                                             ""\n"
        "#endif // TEXTURE_LOD_SUPPORTED                                                                                    ""\n"
        "                                                                                                                   ""\n"
        // Parameters: per-tile data
        "uniform ivec2 param_vs_tileCoordsOffset;                                                                           ""\n"
        "uniform float param_vs_elevationData_k;                                                                            ""\n"
        "uniform float param_vs_elevationData_upperMetersPerUnit;                                                           ""\n"
        "uniform float param_vs_elevationData_lowerMetersPerUnit;                                                           ""\n"
        "#if VERTEX_TEXTURE_FETCH_SUPPORTED                                                                                 ""\n"
        "    uniform sampler2D param_vs_elevationData_sampler;                                                              ""\n"
        "#endif // VERTEX_TEXTURE_FETCH_SUPPORTED                                                                           ""\n"
        "                                                                                                                   ""\n"
        // Parameters: per-layer-in-tile data
        "struct RasterLayerTile                                                                                             ""\n"
        "{                                                                                                                  ""\n"
        "    float tileSizeN;                                                                                               ""\n"
        "    float tilePaddingN;                                                                                            ""\n"
        "    int slotsPerSide;                                                                                              ""\n"
        "    int slotIndex;                                                                                                 ""\n"
        "};                                                                                                                 ""\n"
        "uniform RasterLayerTile param_vs_rasterTileLayers[%RasterLayersCount%];                                            ""\n"
        "#if VERTEX_TEXTURE_FETCH_SUPPORTED                                                                                 ""\n"
        "    uniform RasterLayerTile param_vs_elevationTileLayer;                                                           ""\n"
        "#endif // !VERTEX_TEXTURE_FETCH_SUPPORTED                                                                          ""\n"
        "                                                                                                                   ""\n"
        "void calculateTextureCoordinates(in RasterLayerTile tileLayer, out vec2 outTexCoords)                              ""\n"
        "{                                                                                                                  ""\n"
        "    int rowIndex = tileLayer.slotIndex / tileLayer.slotsPerSide;                                                   ""\n"
        "    int colIndex = tileLayer.slotIndex - rowIndex * tileLayer.slotsPerSide;                                        ""\n"
        "                                                                                                                   ""\n"
        "    float texCoordRescale = (tileLayer.tileSizeN - 2.0 * tileLayer.tilePaddingN) / tileLayer.tileSizeN;            ""\n"
        "                                                                                                                   ""\n"
        "    outTexCoords.s = float(colIndex) * tileLayer.tileSizeN;                                                        ""\n"
        "    outTexCoords.s += tileLayer.tilePaddingN + (in_vs_vertexTexCoords.s * tileLayer.tileSizeN) * texCoordRescale;  ""\n"
        "                                                                                                                   ""\n"
        "    outTexCoords.t = float(rowIndex) * tileLayer.tileSizeN;                                                        ""\n"
        "    outTexCoords.t += tileLayer.tilePaddingN + (in_vs_vertexTexCoords.t * tileLayer.tileSizeN) * texCoordRescale;  ""\n"
        "}                                                                                                                  ""\n"
        "                                                                                                                   ""\n"
        "void main()                                                                                                        ""\n"
        "{                                                                                                                  ""\n"
        "    vec4 v = vec4(in_vs_vertexPosition.x, 0.0, in_vs_vertexPosition.y, 1.0);                                       ""\n"
        "                                                                                                                   ""\n"
        //   Shift vertex to it's proper position
        "    float xOffset = float(param_vs_tileCoordsOffset.x) - param_vs_targetInTilePosN.x;                              ""\n"
        "    v.x += xOffset * %TileSize3D%.0;                                                                               ""\n"
        "    float yOffset = float(param_vs_tileCoordsOffset.y) - param_vs_targetInTilePosN.y;                              ""\n"
        "    v.z += yOffset * %TileSize3D%.0;                                                                               ""\n"
        "                                                                                                                   ""\n"
        //   Process each tile layer texture coordinates (except elevation)
        "%UnrolledPerRasterLayerTexCoordsProcessingCode%                                                                    ""\n"
        "                                                                                                                   ""\n"
        //   If elevation data is active, use it
        "    if(abs(param_vs_elevationData_k) > 0.0)                                                                        ""\n"
        "    {                                                                                                              ""\n"
        "        float metersToUnits = mix(param_vs_elevationData_upperMetersPerUnit,                                       ""\n"
        "            param_vs_elevationData_lowerMetersPerUnit, in_vs_vertexTexCoords.t);                                   ""\n"
        "                                                                                                                   ""\n"
        //       Calculate texcoords for elevation data (pixel-is-area)
        "        float heightInMeters;                                                                                      ""\n"
        "#if VERTEX_TEXTURE_FETCH_SUPPORTED                                                                                 ""\n"
        "        vec2 elevationDataTexCoords;                                                                               ""\n"
        "        calculateTextureCoordinates(                                                                               ""\n"
        "            param_vs_elevationTileLayer,                                                                           ""\n"
        "            elevationDataTexCoords);                                                                               ""\n"
        "        heightInMeters = SAMPLE_TEXTURE_2D(param_vs_elevationData_sampler, elevationDataTexCoords).r;              ""\n"
        "#else // !VERTEX_TEXTURE_FETCH_SUPPORTED                                                                           ""\n"
        "        heightInMeters = in_vs_vertexElevation;                                                                    ""\n"
        "#endif // VERTEX_TEXTURE_FETCH_SUPPORTED                                                                           ""\n"
        "                                                                                                                   ""\n"
        "        v.y = heightInMeters / metersToUnits;                                                                      ""\n"
        "        v.y *= param_vs_elevationData_k;                                                                           ""\n"
        "    }                                                                                                              ""\n"
        "                                                                                                                   ""\n"
        "#if TEXTURE_LOD_SUPPORTED                                                                                          ""\n"
        //   Calculate mipmap LOD
        "    vec2 groundVertex = v.xz;                                                                                      ""\n"
        "    vec2 groundCameraToVertex = groundVertex - param_vs_groundCameraPosition;                                      ""\n"
        "    float mipmapK = log(1.0 + 10.0 * log2(1.0 + param_vs_cameraElevationAngleN));                                  ""\n"
        "    float mipmapBaseLevelEndDistance = mipmapK * param_vs_distanceFromCameraToTarget;                              ""\n"
        "    v2f_mipmapLOD = 1.0 + (length(groundCameraToVertex) - mipmapBaseLevelEndDistance)                              ""\n"
        "        / (param_vs_scaleToRetainProjectedSize * %TileSize3D%.0);                                                  ""\n"
        "#endif // TEXTURE_LOD_SUPPORTED                                                                                    ""\n"
        "                                                                                                                   ""\n"
        //   Finally output processed modified vertex
        "    gl_Position = param_vs_mProjectionView * v;                                                                    ""\n"
        "}                                                                                                                  ""\n");
    const auto& vertexShader_perRasterLayerTexCoordsProcessing = QString::fromLatin1(
        "    calculateTextureCoordinates(                                                                                   ""\n"
        "        param_vs_rasterTileLayers[%rasterLayerId%],                                                                ""\n"
        "        v2f_texCoordsPerLayer[%rasterLayerId%]);                                                                   ""\n"
        "                                                                                                                   ""\n");

    const auto& fragmentShader = QString::fromLatin1(
        // Input data
        "PARAM_INPUT vec2 v2f_texCoordsPerLayer[%RasterLayersCount%];                                                       ""\n"
        "#if TEXTURE_LOD_SUPPORTED                                                                                          ""\n"
        "    PARAM_INPUT float v2f_mipmapLOD;                                                                               ""\n"
        "#endif // TEXTURE_LOD_SUPPORTED                                                                                    ""\n"
        "                                                                                                                   ""\n"
        // Parameters: per-layer data
        "struct RasterLayerTile                                                                                             ""\n"
        "{                                                                                                                  ""\n"
        "    lowp float k;                                                                                                  ""\n"
        "    lowp sampler2D sampler;                                                                                        ""\n"
        "};                                                                                                                 ""\n"
        "uniform RasterLayerTile param_fs_rasterTileLayers[%RasterLayersCount%];                                            ""\n"
        "                                                                                                                   ""\n"
        "void main()                                                                                                        ""\n"
        "{                                                                                                                  ""\n"
        "    lowp vec4 finalColor;                                                                                          ""\n"
        "                                                                                                                   ""\n"
        //   Mix colors of all layers
        "#if TEXTURE_LOD_SUPPORTED                                                                                          ""\n"
        "    finalColor = SAMPLE_TEXTURE_2D_LOD(                                                                            ""\n"
        "        param_fs_rasterTileLayers[0].sampler,                                                                      ""\n"
        "        v2f_texCoordsPerLayer[0], v2f_mipmapLOD);                                                                  ""\n"
        "#else // !TEXTURE_LOD_SUPPORTED                                                                                    ""\n"
        "    finalColor = SAMPLE_TEXTURE_2D(                                                                                ""\n"
        "        param_fs_rasterTileLayers[0].sampler,                                                                      ""\n"
        "        v2f_texCoordsPerLayer[0]);                                                                                 ""\n"
        "#endif // TEXTURE_LOD_SUPPORTED                                                                                    ""\n"
        "    finalColor.a *= param_fs_rasterTileLayers[0].k;                                                                ""\n"
        "%UnrolledPerRasterLayerProcessingCode%                                                                             ""\n"
        "                                                                                                                   ""\n"
#if 0
        //   NOTE: Useful for debugging mipmap levels
        "    {                                                                                                              ""\n"
        "        vec4 mipmapDebugColor;                                                                                     ""\n"
        "        mipmapDebugColor.a = 1.0;                                                                                  ""\n"
        "        float value = v2f_mipmapLOD;                                                                               ""\n"
        //"        float value = textureQueryLod(param_vs_rasterTileLayers[0].sampler, v2f_texCoordsPerLayer[0]).x;           ""\n"
        "        mipmapDebugColor.r = clamp(value, 0.0, 1.0);                                                               ""\n"
        "        value -= 1.0;                                                                                              ""\n"
        "        mipmapDebugColor.g = clamp(value, 0.0, 1.0);                                                               ""\n"
        "        value -= 1.0;                                                                                              ""\n"
        "        mipmapDebugColor.b = clamp(value, 0.0, 1.0);                                                               ""\n"
        "        finalColor = mix(finalColor, mipmapDebugColor, 0.5);                                                       ""\n"
        "    }                                                                                                              ""\n"
#endif
        "    FRAGMENT_COLOR_OUTPUT = finalColor;                                                                            ""\n"
        "}                                                                                                                  ""\n");
    const auto& fragmentShader_perRasterLayer = QString::fromLatin1(
        "    {                                                                                                              ""\n"
        "#if TEXTURE_LOD_SUPPORTED                                                                                          ""\n"
        "        lowp vec4 layerColor = SAMPLE_TEXTURE_2D_LOD(                                                              ""\n"
        "            param_fs_rasterTileLayers[%rasterLayerId%].sampler,                                                    ""\n"
        "            v2f_texCoordsPerLayer[%rasterLayerId%], v2f_mipmapLOD);                                                ""\n"
        "#else // !TEXTURE_LOD_SUPPORTED                                                                                    ""\n"
        "        lowp vec4 layerColor = SAMPLE_TEXTURE_2D(                                                                  ""\n"
        "            param_fs_rasterTileLayers[%rasterLayerId%].sampler,                                                    ""\n"
        "            v2f_texCoordsPerLayer[%rasterLayerId%]);                                                               ""\n"
        "#endif // TEXTURE_LOD_SUPPORTED                                                                                    ""\n"
        "                                                                                                                   ""\n"
        "        layerColor.a *= param_fs_rasterTileLayers[%rasterLayerId%].k;                                              ""\n"
        "        finalColor = mix(finalColor, layerColor, layerColor.a);                                                    ""\n"
        "    }                                                                                                              ""\n");
    for(auto variationId = 0u, maxActiveMapLayers = 1u; variationId < RasterMapLayersCount; variationId++, maxActiveMapLayers++)
    {
        auto& stageVariation = _rasterMapStage.variations[variationId];

        // Compile vertex shader
        auto preprocessedVertexShader = vertexShader;
        QString preprocessedVertexShader_UnrolledPerRasterLayerTexCoordsProcessingCode;
        for(int layerId = 0; layerId < maxActiveMapLayers; layerId++)
        {
            auto preprocessedVertexShader_perRasterLayerTexCoordsProcessing = vertexShader_perRasterLayerTexCoordsProcessing;
            preprocessedVertexShader_perRasterLayerTexCoordsProcessing.replace("%rasterLayerId%", QString::number(layerId));

            preprocessedVertexShader_UnrolledPerRasterLayerTexCoordsProcessingCode +=
                preprocessedVertexShader_perRasterLayerTexCoordsProcessing;
        }
        preprocessedVertexShader.replace("%UnrolledPerRasterLayerTexCoordsProcessingCode%",
            preprocessedVertexShader_UnrolledPerRasterLayerTexCoordsProcessingCode);
        preprocessedVertexShader.replace("%TileSize3D%", QString::number(TileSize3D));
        preprocessedVertexShader.replace("%RasterLayersCount%", QString::number(maxActiveMapLayers));
        gpuAPI->preprocessVertexShader(preprocessedVertexShader);
        gpuAPI->optimizeVertexShader(preprocessedVertexShader);
        stageVariation.vs.id = gpuAPI->compileShader(GL_VERTEX_SHADER, qPrintable(preprocessedVertexShader));
        assert(stageVariation.vs.id != 0);

        // Compile fragment shader
        auto preprocessedFragmentShader = fragmentShader;
        QString preprocessedFragmentShader_UnrolledPerRasterLayerProcessingCode;
        for(int layerId = static_cast<int>(RasterMapLayerId::BaseLayer) + 1; layerId < maxActiveMapLayers; layerId++)
        {
            auto linearIdx = layerId - static_cast<int>(RasterMapLayerId::BaseLayer);
            auto preprocessedFragmentShader_perRasterLayer = fragmentShader_perRasterLayer;
            preprocessedFragmentShader_perRasterLayer.replace("%rasterLayerId%", QString::number(linearIdx));

            preprocessedFragmentShader_UnrolledPerRasterLayerProcessingCode += preprocessedFragmentShader_perRasterLayer;
        }
        preprocessedFragmentShader.replace("%UnrolledPerRasterLayerProcessingCode%", preprocessedFragmentShader_UnrolledPerRasterLayerProcessingCode);
        preprocessedFragmentShader.replace("%RasterLayersCount%", QString::number(maxActiveMapLayers));
        gpuAPI->preprocessFragmentShader(preprocessedFragmentShader);
        gpuAPI->optimizeFragmentShader(preprocessedFragmentShader);
        stageVariation.fs.id = gpuAPI->compileShader(GL_FRAGMENT_SHADER, qPrintable(preprocessedFragmentShader));
        assert(stageVariation.fs.id != 0);

        // Link everything into program object
        GLuint shaders[] = {
            stageVariation.vs.id,
            stageVariation.fs.id
        };
        stageVariation.program = getGPUAPI()->linkProgram(2, shaders);
        assert(stageVariation.program != 0);

        gpuAPI->clearVariablesLookup();
        gpuAPI->findVariableLocation(stageVariation.program, stageVariation.vs.in.vertexPosition, "in_vs_vertexPosition", GLShaderVariableType::In);
        gpuAPI->findVariableLocation(stageVariation.program, stageVariation.vs.in.vertexTexCoords, "in_vs_vertexTexCoords", GLShaderVariableType::In);
        if(!gpuAPI->isSupported_vertexShaderTextureLookup)
        {
            gpuAPI->findVariableLocation(stageVariation.program, stageVariation.vs.in.vertexElevation, "in_vs_vertexElevation", GLShaderVariableType::In);
        }
        gpuAPI->findVariableLocation(stageVariation.program, stageVariation.vs.param.mProjectionView, "param_vs_mProjectionView", GLShaderVariableType::Uniform);
        gpuAPI->findVariableLocation(stageVariation.program, stageVariation.vs.param.targetInTilePosN, "param_vs_targetInTilePosN", GLShaderVariableType::Uniform);
        if(gpuAPI->isSupported_textureLod)
        {
            gpuAPI->findVariableLocation(stageVariation.program, stageVariation.vs.param.distanceFromCameraToTarget, "param_vs_distanceFromCameraToTarget", GLShaderVariableType::Uniform);
            gpuAPI->findVariableLocation(stageVariation.program, stageVariation.vs.param.cameraElevationAngleN, "param_vs_cameraElevationAngleN", GLShaderVariableType::Uniform);
            gpuAPI->findVariableLocation(stageVariation.program, stageVariation.vs.param.groundCameraPosition, "param_vs_groundCameraPosition", GLShaderVariableType::Uniform);
            gpuAPI->findVariableLocation(stageVariation.program, stageVariation.vs.param.scaleToRetainProjectedSize, "param_vs_scaleToRetainProjectedSize", GLShaderVariableType::Uniform);
        }
        gpuAPI->findVariableLocation(stageVariation.program, stageVariation.vs.param.tileCoordsOffset, "param_vs_tileCoordsOffset", GLShaderVariableType::Uniform);
        gpuAPI->findVariableLocation(stageVariation.program, stageVariation.vs.param.elevationData_k, "param_vs_elevationData_k", GLShaderVariableType::Uniform);
        gpuAPI->findVariableLocation(stageVariation.program, stageVariation.vs.param.elevationData_upperMetersPerUnit, "param_vs_elevationData_upperMetersPerUnit", GLShaderVariableType::Uniform);
        gpuAPI->findVariableLocation(stageVariation.program, stageVariation.vs.param.elevationData_lowerMetersPerUnit, "param_vs_elevationData_lowerMetersPerUnit", GLShaderVariableType::Uniform);
        if(gpuAPI->isSupported_vertexShaderTextureLookup)
        {
            gpuAPI->findVariableLocation(stageVariation.program, stageVariation.vs.param.elevationData_sampler, "param_vs_elevationData_sampler", GLShaderVariableType::Uniform);
            gpuAPI->findVariableLocation(stageVariation.program, stageVariation.vs.param.elevationTileLayer.tileSizeN, "param_vs_elevationTileLayer.tileSizeN", GLShaderVariableType::Uniform);
            gpuAPI->findVariableLocation(stageVariation.program, stageVariation.vs.param.elevationTileLayer.tilePaddingN, "param_vs_elevationTileLayer.tilePaddingN", GLShaderVariableType::Uniform);
            gpuAPI->findVariableLocation(stageVariation.program, stageVariation.vs.param.elevationTileLayer.slotsPerSide, "param_vs_elevationTileLayer.slotsPerSide", GLShaderVariableType::Uniform);
            gpuAPI->findVariableLocation(stageVariation.program, stageVariation.vs.param.elevationTileLayer.slotIndex, "param_vs_elevationTileLayer.slotIndex", GLShaderVariableType::Uniform);
        }
        for(int layerId = 0; layerId < maxActiveMapLayers; layerId++)
        {
            // Vertex shader
            {
                auto layerStructName =
                    QString::fromLatin1("param_vs_rasterTileLayers[%layerId%]")
                        .replace(QLatin1String("%layerId%"), QString::number(layerId));
                auto& layerStruct = stageVariation.vs.param.rasterTileLayers[layerId];

                gpuAPI->findVariableLocation(stageVariation.program, layerStruct.tileSizeN, layerStructName + ".tileSizeN", GLShaderVariableType::Uniform);
                gpuAPI->findVariableLocation(stageVariation.program, layerStruct.tilePaddingN, layerStructName + ".tilePaddingN", GLShaderVariableType::Uniform);
                gpuAPI->findVariableLocation(stageVariation.program, layerStruct.slotsPerSide, layerStructName + ".slotsPerSide", GLShaderVariableType::Uniform);
                gpuAPI->findVariableLocation(stageVariation.program, layerStruct.slotIndex, layerStructName + ".slotIndex", GLShaderVariableType::Uniform);
            }

            // Fragment shader
            {
                auto layerStructName =
                    QString::fromLatin1("param_fs_rasterTileLayers[%layerId%]")
                        .replace(QLatin1String("%layerId%"), QString::number(layerId));
                auto& layerStruct = stageVariation.fs.param.rasterTileLayers[layerId];

                gpuAPI->findVariableLocation(stageVariation.program, layerStruct.k, layerStructName + ".k", GLShaderVariableType::Uniform);
                gpuAPI->findVariableLocation(stageVariation.program, layerStruct.sampler, layerStructName + ".sampler", GLShaderVariableType::Uniform);
            }
        }
        gpuAPI->clearVariablesLookup();
    }
}

void OsmAnd::AtlasMapRenderer_OpenGL_Common::renderRasterMapStage()
{
    const auto gpuAPI = getGPUAPI();

    GL_CHECK_PRESENT(glUseProgram);
    GL_CHECK_PRESENT(glUniformMatrix4fv);
    GL_CHECK_PRESENT(glUniform1f);
    GL_CHECK_PRESENT(glUniform2f);
    GL_CHECK_PRESENT(glUniform3f);
    GL_CHECK_PRESENT(glUniform4f);
    GL_CHECK_PRESENT(glUniform1i);
    GL_CHECK_PRESENT(glUniform2i);
    GL_CHECK_PRESENT(glUniform2fv);
    GL_CHECK_PRESENT(glActiveTexture);
    GL_CHECK_PRESENT(glEnableVertexAttribArray);
    GL_CHECK_PRESENT(glVertexAttribPointer);
    GL_CHECK_PRESENT(glDisableVertexAttribArray);

    // Count active raster tile providers
    auto activeRasterTileProvidersCount = 0u;
    for(int layerIdx = 0, layerId = static_cast<int>(RasterMapLayerId::BaseLayer); layerIdx < RasterMapLayersCount; layerIdx++, layerId++)
    {
        if(!currentState.rasterLayerProviders[layerId])
            continue;

        activeRasterTileProvidersCount++;
    }

    // If there is no active raster tile providers at all, or no base layer, do not perform anything
    if(activeRasterTileProvidersCount == 0 || !currentState.rasterLayerProviders[static_cast<int>(RasterMapLayerId::BaseLayer)])
        return;

    // Get proper program variation
    const auto& stageVariation = _rasterMapStage.variations[activeRasterTileProvidersCount - 1];

    // Set tile patch VAO
    gpuAPI->glBindVertexArray_wrapper(_rasterMapStage.tilePatchVAO);
    GL_CHECK_RESULT;

    // Activate program
    glUseProgram(stageVariation.program);
    GL_CHECK_RESULT;

    // Set matrices
    auto mProjectionView = _internalState.mPerspectiveProjection * _internalState.mCameraView;
    glUniformMatrix4fv(stageVariation.vs.param.mProjectionView, 1, GL_FALSE, glm::value_ptr(mProjectionView));
    GL_CHECK_RESULT;

    // Set center offset
    glUniform2f(stageVariation.vs.param.targetInTilePosN, _internalState.targetInTileOffsetN.x, _internalState.targetInTileOffsetN.y);
    GL_CHECK_RESULT;

    if(gpuAPI->isSupported_textureLod)
    {
        // Set distance from camera to target
        glUniform1f(stageVariation.vs.param.distanceFromCameraToTarget, _internalState.distanceFromCameraToTarget);
        GL_CHECK_RESULT;

        // Set normalized [0.0 .. 1.0] angle of camera elevation
        glUniform1f(stageVariation.vs.param.cameraElevationAngleN, currentState.elevationAngle / 90.0f);
        GL_CHECK_RESULT;

        // Set position of camera in ground place
        glUniform2fv(stageVariation.vs.param.groundCameraPosition, 1, glm::value_ptr(_internalState.groundCameraPosition));
        GL_CHECK_RESULT;

        // Set scale to retain projected size
        glUniform1f(stageVariation.vs.param.scaleToRetainProjectedSize, _internalState.scaleToRetainProjectedSize);
        GL_CHECK_RESULT;
    }
    
    // Configure samplers
    if(gpuAPI->isSupported_vertexShaderTextureLookup)
    {
        glUniform1i(stageVariation.vs.param.elevationData_sampler, 0);
        GL_CHECK_RESULT;

        gpuAPI->setTextureBlockSampler(GL_TEXTURE0, GPUAPI_OpenGL_Common::SamplerType::ElevationDataTile);
    }
    auto bitmapTileSamplerType = GPUAPI_OpenGL_Common::SamplerType::BitmapTile_Bilinear;
    if(gpuAPI->isSupported_textureLod)
    {
        switch(currentConfiguration.texturesFilteringQuality)
        {
        case TextureFilteringQuality::Good:
            bitmapTileSamplerType = GPUAPI_OpenGL_Common::SamplerType::BitmapTile_BilinearMipmap;
            break;
        case TextureFilteringQuality::Best:
            bitmapTileSamplerType = GPUAPI_OpenGL_Common::SamplerType::BitmapTile_TrilinearMipmap;
            break;
        }
    }
    for(int layerIdx = 0, layerId = static_cast<int>(RasterMapLayerId::BaseLayer); layerIdx < activeRasterTileProvidersCount; layerIdx++, layerId++)
    {
        const auto samplerIndex = (gpuAPI->isSupported_vertexShaderTextureLookup ? 1 : 0) + layerId;

        glUniform1i(stageVariation.fs.param.rasterTileLayers[layerId].sampler, samplerIndex);
        GL_CHECK_RESULT;

        gpuAPI->setTextureBlockSampler(GL_TEXTURE0 + samplerIndex, bitmapTileSamplerType);
    }

    // Check if we need to process elevation data
    const bool elevationDataEnabled = static_cast<bool>(currentState.elevationDataProvider);
    if(!elevationDataEnabled)
    {
        // We have no elevation data provider, so we can not do anything
        glUniform1f(stageVariation.vs.param.elevationData_k, 0.0f);
        GL_CHECK_RESULT;
    }
    bool elevationVertexAttribArrayEnabled = false;
    if(!gpuAPI->isSupported_vertexShaderTextureLookup)
    {
        glDisableVertexAttribArray(stageVariation.vs.in.vertexElevation);
        GL_CHECK_RESULT;
    }

    // For each visible tile, render it
    for(auto itTileId = _internalState.visibleTiles.cbegin(); itTileId != _internalState.visibleTiles.cend(); ++itTileId)
    {
        const auto& tileId = *itTileId;

        // Get normalized tile index
        auto tileIdN = Utilities::normalizeTileId(tileId, currentState.zoomBase);

        // Set tile coordinates offset
        glUniform2i(stageVariation.vs.param.tileCoordsOffset,
            tileId.x - _internalState.targetTileId.x,
            tileId.y - _internalState.targetTileId.y);
        GL_CHECK_RESULT;

        // Set elevation data
        bool appliedElevationVertexAttribArray = false;
        if(elevationDataEnabled)
        {
            const auto resourcesCollection = getResources().getCollection(ResourceType::ElevationData, currentState.elevationDataProvider);

            // Obtain tile entry by normalized tile coordinates, since tile may repeat several times
            std::shared_ptr< const GPUAPI::ResourceInGPU > gpuResource;
            std::shared_ptr<Resources::BaseTiledResource> resource_;
            if(resourcesCollection->obtainEntry(resource_, tileIdN, currentState.zoomBase))
            {
                const auto resource = std::static_pointer_cast<Resources::MapTileResource>(resource_);

                // Check state and obtain GPU resource
                if(resource->setStateIf(ResourceState::Uploaded, ResourceState::IsBeingUsed))
                {
                    // Capture GPU resource
                    gpuResource = resource->resourceInGPU;

                    resource->setState(ResourceState::Uploaded);
                }
            }
            
            if(!gpuResource)
            {
                // We have no elevation data, so we can not do anything
                glUniform1f(stageVariation.vs.param.elevationData_k, 0.0f);
                GL_CHECK_RESULT;
            }
            else
            {
                glUniform1f(stageVariation.vs.param.elevationData_k, currentState.elevationDataScaleFactor);
                GL_CHECK_RESULT;

                const auto upperMetersPerUnit = Utilities::getMetersPerTileUnit(currentState.zoomBase, tileIdN.y, TileSize3D);
                glUniform1f(stageVariation.vs.param.elevationData_upperMetersPerUnit, upperMetersPerUnit);
                const auto lowerMetersPerUnit = Utilities::getMetersPerTileUnit(currentState.zoomBase, tileIdN.y + 1, TileSize3D);
                glUniform1f(stageVariation.vs.param.elevationData_lowerMetersPerUnit, lowerMetersPerUnit);

                const auto& perTile_vs = stageVariation.vs.param.elevationTileLayer;

                if(gpuAPI->isSupported_vertexShaderTextureLookup)
                {
                    glActiveTexture(GL_TEXTURE0);
                    GL_CHECK_RESULT;

                    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(reinterpret_cast<intptr_t>(gpuResource->refInGPU)));
                    GL_CHECK_RESULT;

                    gpuAPI->applyTextureBlockToTexture(GL_TEXTURE_2D, GL_TEXTURE0);

                    if(gpuResource->type == GPUAPI::ResourceInGPU::Type::SlotOnAtlasTexture)
                    {
                        const auto tileOnAtlasTexture = std::static_pointer_cast<const GPUAPI::SlotOnAtlasTextureInGPU>(gpuResource);

                        glUniform1i(perTile_vs.slotIndex, tileOnAtlasTexture->slotIndex);
                        GL_CHECK_RESULT;
                        glUniform1f(perTile_vs.tileSizeN, tileOnAtlasTexture->atlasTexture->tileSizeN);
                        GL_CHECK_RESULT;
                        glUniform1f(perTile_vs.tilePaddingN, tileOnAtlasTexture->atlasTexture->uHalfTexelSizeN);
                        GL_CHECK_RESULT;
                        glUniform1i(perTile_vs.slotsPerSide, tileOnAtlasTexture->atlasTexture->slotsPerSide);
                        GL_CHECK_RESULT;
                    }
                    else
                    {
                        const auto& texture = std::static_pointer_cast<const GPUAPI::TextureInGPU>(gpuResource);

                        glUniform1i(perTile_vs.slotIndex, 0);
                        GL_CHECK_RESULT;
                        glUniform1f(perTile_vs.tileSizeN, 1.0f);
                        GL_CHECK_RESULT;
                        glUniform1f(perTile_vs.tilePaddingN, texture->uHalfTexelSizeN);
                        GL_CHECK_RESULT;
                        glUniform1i(perTile_vs.slotsPerSide, 1);
                        GL_CHECK_RESULT;
                    }
                }
                else
                {
                    assert(gpuResource->type == GPUAPI::ResourceInGPU::Type::ArrayBuffer);

                    const auto& arrayBuffer = std::static_pointer_cast<const GPUAPI::ArrayBufferInGPU>(gpuResource);
                    assert(arrayBuffer->itemsCount == currentConfiguration.heixelsPerTileSide*currentConfiguration.heixelsPerTileSide);

                    if(!elevationVertexAttribArrayEnabled)
                    {
                        glEnableVertexAttribArray(stageVariation.vs.in.vertexElevation);
                        GL_CHECK_RESULT;

                        elevationVertexAttribArrayEnabled = true;
                    }

                    glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(reinterpret_cast<intptr_t>(gpuResource->refInGPU)));
                    GL_CHECK_RESULT;

                    glVertexAttribPointer(stageVariation.vs.in.vertexElevation, 1, GL_FLOAT, GL_FALSE, sizeof(float), nullptr);
                    GL_CHECK_RESULT;
                    appliedElevationVertexAttribArray = true;
                }
            }
        }

        // We need to pass each layer of this tile to shader
        for(int layerIdx = 0, layerId = static_cast<int>(RasterMapLayerId::BaseLayer), layerLinearIdx = -1; layerIdx < RasterMapLayersCount; layerIdx++, layerId++)
        {
            if(!currentState.rasterLayerProviders[layerId])
                continue;
            layerLinearIdx++;

            // Get resources collection
            const auto resourcesCollection = getResources().getCollection(ResourceType::RasterMap, currentState.rasterLayerProviders[layerId]);

            const auto& perTile_vs = stageVariation.vs.param.rasterTileLayers[layerLinearIdx];
            const auto& perTile_fs = stageVariation.fs.param.rasterTileLayers[layerLinearIdx];
            const auto samplerIndex = (gpuAPI->isSupported_vertexShaderTextureLookup ? 1 : 0) + layerLinearIdx;

            // Obtain tile entry by normalized tile coordinates, since tile may repeat several times
            std::shared_ptr< const GPUAPI::ResourceInGPU > gpuResource;
            std::shared_ptr<Resources::BaseTiledResource> resource_;
            if(resourcesCollection->obtainEntry(resource_, tileIdN, currentState.zoomBase))
            {
                const auto resource = std::static_pointer_cast<Resources::MapTileResource>(resource_);

                // Check state and obtain GPU resource
                if(resource->setStateIf(ResourceState::Uploaded, ResourceState::IsBeingUsed))
                {
                    // Capture GPU resource
                    gpuResource = resource->resourceInGPU;

                    resource->setState(ResourceState::Uploaded);
                }
                else if(resource->getState() == ResourceState::Unavailable)
                    gpuResource = getResources().unavailableTileStub;
                else
                    gpuResource = getResources().processingTileStub;
            }
            else
            {
                // No resource entry means that it's not yet processed
                gpuResource = getResources().processingTileStub;
            }

            glUniform1f(perTile_fs.k, currentState.rasterLayerOpacity[layerId]);
            GL_CHECK_RESULT;

            glActiveTexture(GL_TEXTURE0 + samplerIndex);
            GL_CHECK_RESULT;

            glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(reinterpret_cast<intptr_t>(gpuResource->refInGPU)));
            GL_CHECK_RESULT;

            gpuAPI->applyTextureBlockToTexture(GL_TEXTURE_2D, GL_TEXTURE0 + samplerIndex);

            if(gpuResource->type == GPUAPI::ResourceInGPU::Type::SlotOnAtlasTexture)
            {
                const auto tileOnAtlasTexture = std::static_pointer_cast<const GPUAPI::SlotOnAtlasTextureInGPU>(gpuResource);

                glUniform1i(perTile_vs.slotIndex, tileOnAtlasTexture->slotIndex);
                GL_CHECK_RESULT;
                glUniform1f(perTile_vs.tileSizeN, tileOnAtlasTexture->atlasTexture->tileSizeN);
                GL_CHECK_RESULT;
                glUniform1f(perTile_vs.tilePaddingN, tileOnAtlasTexture->atlasTexture->tilePaddingN);
                GL_CHECK_RESULT;
                glUniform1i(perTile_vs.slotsPerSide, tileOnAtlasTexture->atlasTexture->slotsPerSide);
                GL_CHECK_RESULT;
            }
            else
            {
                glUniform1i(perTile_vs.slotIndex, 0);
                GL_CHECK_RESULT;
                glUniform1f(perTile_vs.tileSizeN, 1.0f);
                GL_CHECK_RESULT;
                glUniform1f(perTile_vs.tilePaddingN, 0.0f);
                GL_CHECK_RESULT;
                glUniform1i(perTile_vs.slotsPerSide, 1);
                GL_CHECK_RESULT;
            }
        }

        if(!appliedElevationVertexAttribArray && elevationVertexAttribArrayEnabled)
        {
            elevationVertexAttribArrayEnabled = false;

            glDisableVertexAttribArray(stageVariation.vs.in.vertexElevation);
            GL_CHECK_RESULT;
        }

        glDrawElements(GL_TRIANGLES, _tilePatchIndicesCount, GL_UNSIGNED_SHORT, nullptr);
        GL_CHECK_RESULT;
    }

    // Disable textures
    for(int layerIdx = 0; layerIdx < (gpuAPI->isSupported_vertexShaderTextureLookup ? 1 : 0) + activeRasterTileProvidersCount; layerIdx++)
    {
        glActiveTexture(GL_TEXTURE0 + layerIdx);
        GL_CHECK_RESULT;

        glBindTexture(GL_TEXTURE_2D, 0);
        GL_CHECK_RESULT;
    }

    // Unbind any binded buffer
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    GL_CHECK_RESULT;

    // Disable elevation vertex attrib array (if enabled)
    if(elevationVertexAttribArrayEnabled)
    {
        glDisableVertexAttribArray(stageVariation.vs.in.vertexElevation);
        GL_CHECK_RESULT;
    }

    // Deactivate program
    glUseProgram(0);
    GL_CHECK_RESULT;

    // Deselect VAO
    gpuAPI->glBindVertexArray_wrapper(0);
    GL_CHECK_RESULT;
}

void OsmAnd::AtlasMapRenderer_OpenGL_Common::releaseRasterMapStage()
{
    GL_CHECK_PRESENT(glDeleteProgram);
    GL_CHECK_PRESENT(glDeleteShader);

    for(auto variationId = 0u, maxActiveMapLayers = 1u; variationId < RasterMapLayersCount; variationId++, maxActiveMapLayers++)
    {
        auto& stageVariation = _rasterMapStage.variations[variationId];

        if(stageVariation.program)
        {
            glDeleteProgram(stageVariation.program);
            GL_CHECK_RESULT;
        }
        if(stageVariation.fs.id)
        {
            glDeleteShader(stageVariation.fs.id);
            GL_CHECK_RESULT;
        }
        if(stageVariation.vs.id)
        {
            glDeleteShader(stageVariation.vs.id);
            GL_CHECK_RESULT;
        }
    }

    releaseTilePatch();

    memset(&_rasterMapStage, 0, sizeof(_rasterMapStage));
}

void OsmAnd::AtlasMapRenderer_OpenGL_Common::initializeSkyStage()
{
    const auto gpuAPI = getGPUAPI();

    GL_CHECK_PRESENT(glGenBuffers);
    GL_CHECK_PRESENT(glBindBuffer);
    GL_CHECK_PRESENT(glBufferData);
    GL_CHECK_PRESENT(glEnableVertexAttribArray);
    GL_CHECK_PRESENT(glVertexAttribPointer);

    // Compile vertex shader
    const QString vertexShader = QLatin1String(
        // Input data
        "INPUT vec2 in_vs_vertexPosition;                                                                                   ""\n"
        "                                                                                                                   ""\n"
        // Parameters: common data
        "uniform mat4 param_vs_mProjectionViewModel;                                                                        ""\n"
        "uniform vec2 param_vs_planeSize;                                                                                   ""\n"
        "                                                                                                                   ""\n"
        "void main()                                                                                                        ""\n"
        "{                                                                                                                  ""\n"
        "    vec4 v;                                                                                                        ""\n"
        "    v.xy = in_vs_vertexPosition * param_vs_planeSize;                                                              ""\n"
        "    v.w = 1.0;                                                                                                     ""\n"
        "                                                                                                                   ""\n"
        "    gl_Position = param_vs_mProjectionViewModel * v;                                                               ""\n"
        "}                                                                                                                  ""\n");
    auto preprocessedVertexShader = vertexShader;
    gpuAPI->preprocessVertexShader(preprocessedVertexShader);
    gpuAPI->optimizeVertexShader(preprocessedVertexShader);
    _skyStage.vs.id = gpuAPI->compileShader(GL_VERTEX_SHADER, qPrintable(preprocessedVertexShader));
    assert(_skyStage.vs.id != 0);

    // Compile fragment shader
    const QString fragmentShader = QLatin1String(
        // Parameters: common data
        "uniform lowp vec4 param_fs_skyColor;                                                                               ""\n"
        "                                                                                                                   ""\n"
        "void main()                                                                                                        ""\n"
        "{                                                                                                                  ""\n"
        "    FRAGMENT_COLOR_OUTPUT = param_fs_skyColor;                                                                     ""\n"
        "}                                                                                                                  ""\n");
    auto preprocessedFragmentShader = fragmentShader;
    QString preprocessedFragmentShader_UnrolledPerLayerProcessingCode;
    gpuAPI->preprocessFragmentShader(preprocessedFragmentShader);
    gpuAPI->optimizeFragmentShader(preprocessedFragmentShader);
    _skyStage.fs.id = gpuAPI->compileShader(GL_FRAGMENT_SHADER, qPrintable(preprocessedFragmentShader));
    assert(_skyStage.fs.id != 0);

    // Link everything into program object
    GLuint shaders[] = {
        _skyStage.vs.id,
        _skyStage.fs.id
    };
    _skyStage.program = gpuAPI->linkProgram(2, shaders);
    assert(_skyStage.program != 0);

    gpuAPI->clearVariablesLookup();
    gpuAPI->findVariableLocation(_skyStage.program, _skyStage.vs.in.vertexPosition, "in_vs_vertexPosition", GLShaderVariableType::In);
    gpuAPI->findVariableLocation(_skyStage.program, _skyStage.vs.param.mProjectionViewModel, "param_vs_mProjectionViewModel", GLShaderVariableType::Uniform);
    gpuAPI->findVariableLocation(_skyStage.program, _skyStage.vs.param.planeSize, "param_vs_planeSize", GLShaderVariableType::Uniform);
    gpuAPI->findVariableLocation(_skyStage.program, _skyStage.fs.param.skyColor, "param_fs_skyColor", GLShaderVariableType::Uniform);
    gpuAPI->clearVariablesLookup();

    // Vertex data (x,y)
    float vertices[4][2] =
    {
        { -0.5f, -0.5f },
        { -0.5f,  0.5f },
        {  0.5f,  0.5f },
        {  0.5f, -0.5f }
    };
    const auto verticesCount = 4;

    // Index data
    GLushort indices[6] =
    {
        0, 1, 2,
        0, 2, 3
    };
    const auto indicesCount = 6;

    // Create Vertex Array Object
    gpuAPI->glGenVertexArrays_wrapper(1, &_skyStage.skyplaneVAO);
    GL_CHECK_RESULT;
    gpuAPI->glBindVertexArray_wrapper(_skyStage.skyplaneVAO);
    GL_CHECK_RESULT;

    // Create vertex buffer and associate it with VAO
    glGenBuffers(1, &_skyStage.skyplaneVBO);
    GL_CHECK_RESULT;
    glBindBuffer(GL_ARRAY_BUFFER, _skyStage.skyplaneVBO);
    GL_CHECK_RESULT;
    glBufferData(GL_ARRAY_BUFFER, verticesCount * sizeof(float) * 2, vertices, GL_STATIC_DRAW);
    GL_CHECK_RESULT;
    glEnableVertexAttribArray(_skyStage.vs.in.vertexPosition);
    GL_CHECK_RESULT;
    glVertexAttribPointer(_skyStage.vs.in.vertexPosition, 2, GL_FLOAT, GL_FALSE, sizeof(float) * 2, nullptr);
    GL_CHECK_RESULT;

    // Create index buffer and associate it with VAO
    glGenBuffers(1, &_skyStage.skyplaneIBO);
    GL_CHECK_RESULT;
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, _skyStage.skyplaneIBO);
    GL_CHECK_RESULT;
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, indicesCount * sizeof(GLushort), indices, GL_STATIC_DRAW);
    GL_CHECK_RESULT;

    gpuAPI->glBindVertexArray_wrapper(0);
    GL_CHECK_RESULT;
}

void OsmAnd::AtlasMapRenderer_OpenGL_Common::renderSkyStage()
{
    const auto gpuAPI = getGPUAPI();

    GL_CHECK_PRESENT(glUseProgram);
    GL_CHECK_PRESENT(glUniformMatrix4fv);
    GL_CHECK_PRESENT(glUniform1f);
    GL_CHECK_PRESENT(glUniform2f);
    GL_CHECK_PRESENT(glUniform3f);
    GL_CHECK_PRESENT(glDrawElements);

    // Set sky plane VAO
    gpuAPI->glBindVertexArray_wrapper(_skyStage.skyplaneVAO);
    GL_CHECK_RESULT;

    // Activate program
    glUseProgram(_skyStage.program);
    GL_CHECK_RESULT;

    // Set projection*view*model matrix:
    const auto mFogTranslate = glm::translate(0.0f, 0.0f, -_internalState.correctedFogDistance);
    const auto mModel = _internalState.mAzimuthInv * mFogTranslate;
    const auto mProjectionViewModel = _internalState.mPerspectiveProjection * _internalState.mCameraView * mModel;
    glUniformMatrix4fv(_skyStage.vs.param.mProjectionViewModel, 1, GL_FALSE, glm::value_ptr(mProjectionViewModel));
    GL_CHECK_RESULT;

    // Set size of the skyplane
    glUniform2f(_skyStage.vs.param.planeSize, _internalState.skyplaneSize.x, _internalState.skyplaneSize.y);
    GL_CHECK_RESULT;

    // Set sky parameters
    glUniform4f(_skyStage.fs.param.skyColor, currentState.skyColor.r, currentState.skyColor.g, currentState.skyColor.b, 1.0f);
    GL_CHECK_RESULT;

    // Draw the skyplane actually
    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, nullptr);
    GL_CHECK_RESULT;

    // Deactivate program
    glUseProgram(0);
    GL_CHECK_RESULT;

    // Deselect VAO
    gpuAPI->glBindVertexArray_wrapper(0);
    GL_CHECK_RESULT;
}

void OsmAnd::AtlasMapRenderer_OpenGL_Common::releaseSkyStage()
{
    const auto gpuAPI = getGPUAPI();

    GL_CHECK_PRESENT(glDeleteBuffers);
    GL_CHECK_PRESENT(glDeleteProgram);
    GL_CHECK_PRESENT(glDeleteShader);

    if(_skyStage.skyplaneIBO)
    {
        glDeleteBuffers(1, &_skyStage.skyplaneIBO);
        GL_CHECK_RESULT;
    }

    if(_skyStage.skyplaneVBO)
    {
        glDeleteBuffers(1, &_skyStage.skyplaneVBO);
        GL_CHECK_RESULT;
    }

    if(_skyStage.skyplaneVAO)
    {
        gpuAPI->glDeleteVertexArrays_wrapper(1, &_skyStage.skyplaneVAO);
        GL_CHECK_RESULT;
    }

    if(_skyStage.program)
    {
        glDeleteProgram(_skyStage.program);
        GL_CHECK_RESULT;
    }
    if(_skyStage.fs.id)
    {
        glDeleteShader(_skyStage.fs.id);
        GL_CHECK_RESULT;
    }
    if(_skyStage.vs.id)
    {
        glDeleteShader(_skyStage.vs.id);
        GL_CHECK_RESULT;
    }

    memset(&_skyStage, 0, sizeof(_skyStage));
}

void OsmAnd::AtlasMapRenderer_OpenGL_Common::initializeSymbolsStage()
{
    const auto gpuAPI = getGPUAPI();

    GL_CHECK_PRESENT(glGenBuffers);
    GL_CHECK_PRESENT(glBindBuffer);
    GL_CHECK_PRESENT(glBufferData);
    GL_CHECK_PRESENT(glEnableVertexAttribArray);
    GL_CHECK_PRESENT(glVertexAttribPointer);

    // Compile vertex shader
    const QString vertexShader = QLatin1String(
        // Input data
        "INPUT vec2 in_vs_vertexPosition;                                                                                   ""\n"
        "INPUT vec2 in_vs_vertexTexCoords;                                                                                  ""\n"
        "                                                                                                                   ""\n"
        // Output data to next shader stages
        "PARAM_OUTPUT vec2 v2f_texCoords;                                                                                   ""\n"
        "                                                                                                                   ""\n"
        // Parameters: common data
        "uniform mat4 param_vs_mPerspectiveProjectionView;                                                                  ""\n"
        "uniform mat4 param_vs_mOrthographicProjection;                                                                     ""\n"
        "uniform vec4 param_vs_viewport; // x, y, width, height                                                             ""\n"
        "                                                                                                                   ""\n"
        // Parameters: per-symbol data
        "uniform highp vec2 param_vs_symbolOffsetFromTarget;                                                                ""\n"
        "uniform ivec2 param_vs_symbolSize;                                                                                 ""\n"
        "uniform float param_vs_distanceFromCamera;                                                                         ""\n"
        "uniform ivec2 param_vs_onScreenOffset;                                                                             ""\n"
        "                                                                                                                   ""\n"
        "void main()                                                                                                        ""\n"
        "{                                                                                                                  ""\n"
        // Calculate location of symbol in world coordinate system.
        "    vec4 symbolLocation;                                                                                           ""\n"
        "    symbolLocation.xz = param_vs_symbolOffsetFromTarget.xy * %TileSize3D%.0;                                       ""\n"
        "    symbolLocation.y = 0.0; //TODO: A height from heightmap should be used here                                    ""\n"
        "    symbolLocation.w = 1.0;                                                                                        ""\n"
        "                                                                                                                   ""\n"
        // Project location of symbol from world coordinate system to screen
        "    vec4 projectedSymbolLocation = param_vs_mPerspectiveProjectionView * symbolLocation;                           ""\n"
        "                                                                                                                   ""\n"
        // "Normalize" location in screen coordinates to get [-1 .. 1] range
        "    vec3 symbolLocationOnScreen = projectedSymbolLocation.xyz / projectedSymbolLocation.w;                         ""\n"
        "                                                                                                                   ""\n"
        // Using viewport size, get real screen coordinates and correct depth to be [0 .. 1]
        "    symbolLocationOnScreen.xy = symbolLocationOnScreen.xy * 0.5 + 0.5;                                             ""\n"
        "    symbolLocationOnScreen.x = symbolLocationOnScreen.x * param_vs_viewport.z + param_vs_viewport.x;               ""\n"
        "    symbolLocationOnScreen.y = symbolLocationOnScreen.y * param_vs_viewport.w + param_vs_viewport.y;               ""\n"
        "    symbolLocationOnScreen.z = (1.0 + symbolLocationOnScreen.z) * 0.5;                                             ""\n"
        "                                                                                                                   ""\n"
        // Add on-screen offset
        "    symbolLocationOnScreen.xy -= vec2(param_vs_onScreenOffset);                                                    ""\n"
        "                                                                                                                   ""\n"
        // symbolLocationOnScreen.xy now contains correct coordinates in viewport,
        // which can be used in orthographic projection (if it was configured to match viewport).
        //
        // So it's possible to calculate current vertex location:
        // Initially, get location of current vertex in screen coordinates
        "    vec2 vertexOnScreen;                                                                                           ""\n"
        "    vertexOnScreen.x = in_vs_vertexPosition.x * float(param_vs_symbolSize.x);                                      ""\n"
        "    vertexOnScreen.y = in_vs_vertexPosition.y * float(param_vs_symbolSize.y);                                      ""\n"
        "    vertexOnScreen = vertexOnScreen + symbolLocationOnScreen.xy;                                                   ""\n"
        "                                                                                                                   ""\n"
        // There's no need to perform unprojection into orthographic world space, just multiply these coordinates by
        // orthographic projection matrix (View and Model being identity)
        "  vec4 vertex;                                                                                                     ""\n"
        "  vertex.xy = vertexOnScreen.xy;                                                                                   ""\n"
        "  vertex.z = -param_vs_distanceFromCamera;                                                                         ""\n"
        "  vertex.w = 1.0;                                                                                                  ""\n"
        "  gl_Position = param_vs_mOrthographicProjection * vertex;                                                         ""\n"
        "                                                                                                                   ""\n"
        // Texture coordinates are simply forwarded from input
        "   v2f_texCoords = in_vs_vertexTexCoords;                                                                          ""\n"
        "}                                                                                                                  ""\n");
    auto preprocessedVertexShader = vertexShader;
    preprocessedVertexShader.replace("%TileSize3D%", QString::number(TileSize3D));
    gpuAPI->preprocessVertexShader(preprocessedVertexShader);
    gpuAPI->optimizeVertexShader(preprocessedVertexShader);
    _symbolsStage.vs.id = gpuAPI->compileShader(GL_VERTEX_SHADER, qPrintable(preprocessedVertexShader));
    assert(_symbolsStage.vs.id != 0);

    // Compile fragment shader
    const QString fragmentShader = QLatin1String(
        // Input data
        "PARAM_INPUT vec2 v2f_texCoords;                                                                                    ""\n"
        "                                                                                                                   ""\n"
        // Parameters: common data
        // Parameters: per-symbol data
        "uniform lowp sampler2D param_fs_sampler;                                                                           ""\n"
        "                                                                                                                   ""\n"
        "void main()                                                                                                        ""\n"
        "{                                                                                                                  ""\n"
        "    FRAGMENT_COLOR_OUTPUT = SAMPLE_TEXTURE_2D(                                                                     ""\n"
        "        param_fs_sampler,                                                                                          ""\n"
        "        v2f_texCoords);                                                                                            ""\n"
        "}                                                                                                                  ""\n");
    auto preprocessedFragmentShader = fragmentShader;
    QString preprocessedFragmentShader_UnrolledPerLayerProcessingCode;
    gpuAPI->preprocessFragmentShader(preprocessedFragmentShader);
    gpuAPI->optimizeFragmentShader(preprocessedFragmentShader);
    _symbolsStage.fs.id = gpuAPI->compileShader(GL_FRAGMENT_SHADER, qPrintable(preprocessedFragmentShader));
    assert(_symbolsStage.fs.id != 0);

    // Link everything into program object
    GLuint shaders[] = {
        _symbolsStage.vs.id,
        _symbolsStage.fs.id
    };
    _symbolsStage.program = gpuAPI->linkProgram(2, shaders);
    assert(_symbolsStage.program != 0);

    gpuAPI->clearVariablesLookup();
    gpuAPI->findVariableLocation(_symbolsStage.program, _symbolsStage.vs.in.vertexPosition, "in_vs_vertexPosition", GLShaderVariableType::In);
    gpuAPI->findVariableLocation(_symbolsStage.program, _symbolsStage.vs.in.vertexTexCoords, "in_vs_vertexTexCoords", GLShaderVariableType::In);
    gpuAPI->findVariableLocation(_symbolsStage.program, _symbolsStage.vs.param.mPerspectiveProjectionView, "param_vs_mPerspectiveProjectionView", GLShaderVariableType::Uniform);
    gpuAPI->findVariableLocation(_symbolsStage.program, _symbolsStage.vs.param.mOrthographicProjection, "param_vs_mOrthographicProjection", GLShaderVariableType::Uniform);
    gpuAPI->findVariableLocation(_symbolsStage.program, _symbolsStage.vs.param.viewport, "param_vs_viewport", GLShaderVariableType::Uniform);
    gpuAPI->findVariableLocation(_symbolsStage.program, _symbolsStage.vs.param.symbolOffsetFromTarget, "param_vs_symbolOffsetFromTarget", GLShaderVariableType::Uniform);
    gpuAPI->findVariableLocation(_symbolsStage.program, _symbolsStage.vs.param.symbolSize, "param_vs_symbolSize", GLShaderVariableType::Uniform);
    gpuAPI->findVariableLocation(_symbolsStage.program, _symbolsStage.vs.param.distanceFromCamera, "param_vs_distanceFromCamera", GLShaderVariableType::Uniform);
    gpuAPI->findVariableLocation(_symbolsStage.program, _symbolsStage.vs.param.onScreenOffset, "param_vs_onScreenOffset", GLShaderVariableType::Uniform);
    gpuAPI->findVariableLocation(_symbolsStage.program, _symbolsStage.fs.param.sampler, "param_fs_sampler", GLShaderVariableType::Uniform);
    gpuAPI->clearVariablesLookup();

#pragma pack(push)
#pragma pack(1)
    struct Vertex
    {
        // XY coordinates. Z is assumed to be 0
        float positionXY[2];

        // UV coordinates
        float textureUV[2];
    };
#pragma pack(pop)

    // Vertex data
    Vertex vertices[4] =
    {
        // In OpenGL, UV origin is BL. But since same rule applies to uploading texture data,
        // texture in memory is vertically flipped, so swap bottom and top UVs
        { { -0.5f, -0.5f }, { 0.0f, 1.0f } },//BL
        { { -0.5f,  0.5f }, { 0.0f, 0.0f } },//TL
        { {  0.5f,  0.5f }, { 1.0f, 0.0f } },//TR
        { {  0.5f, -0.5f }, { 1.0f, 1.0f } } //BR
    };
    const auto verticesCount = 4;

    // Index data
    GLushort indices[6] =
    {
        0, 1, 2,
        0, 2, 3
    };
    const auto indicesCount = 6;

    // Create Vertex Array Object
    gpuAPI->glGenVertexArrays_wrapper(1, &_symbolsStage.symbolVAO);
    GL_CHECK_RESULT;
    gpuAPI->glBindVertexArray_wrapper(_symbolsStage.symbolVAO);
    GL_CHECK_RESULT;

    // Create vertex buffer and associate it with VAO
    glGenBuffers(1, &_symbolsStage.symbolVBO);
    GL_CHECK_RESULT;
    glBindBuffer(GL_ARRAY_BUFFER, _symbolsStage.symbolVBO);
    GL_CHECK_RESULT;
    glBufferData(GL_ARRAY_BUFFER, verticesCount * sizeof(Vertex), vertices, GL_STATIC_DRAW);
    GL_CHECK_RESULT;
    glEnableVertexAttribArray(_symbolsStage.vs.in.vertexPosition);
    GL_CHECK_RESULT;
    glVertexAttribPointer(_symbolsStage.vs.in.vertexPosition, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<GLvoid*>(offsetof(Vertex, positionXY)));
    GL_CHECK_RESULT;
    glEnableVertexAttribArray(_symbolsStage.vs.in.vertexTexCoords);
    GL_CHECK_RESULT;
    glVertexAttribPointer(_symbolsStage.vs.in.vertexTexCoords, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<GLvoid*>(offsetof(Vertex, textureUV)));
    GL_CHECK_RESULT;

    // Create index buffer and associate it with VAO
    glGenBuffers(1, &_symbolsStage.symbolIBO);
    GL_CHECK_RESULT;
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, _symbolsStage.symbolIBO);
    GL_CHECK_RESULT;
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, indicesCount * sizeof(GLushort), indices, GL_STATIC_DRAW);
    GL_CHECK_RESULT;

    gpuAPI->glBindVertexArray_wrapper(0);
    GL_CHECK_RESULT;
}

void OsmAnd::AtlasMapRenderer_OpenGL_Common::renderSymbolsStage()
{
    const auto gpuAPI = getGPUAPI();

    GL_CHECK_PRESENT(glUseProgram);
    GL_CHECK_PRESENT(glUniformMatrix4fv);
    GL_CHECK_PRESENT(glUniform1f);
    GL_CHECK_PRESENT(glUniform2f);
    GL_CHECK_PRESENT(glUniform3f);
    GL_CHECK_PRESENT(glDrawElements);

    // Set symbol VAO
    gpuAPI->glBindVertexArray_wrapper(_symbolsStage.symbolVAO);
    GL_CHECK_RESULT;

    // Activate program
    glUseProgram(_symbolsStage.program);
    GL_CHECK_RESULT;

    // Set perspective projection-view matrix
    const auto mPerspectiveProjectionView = _internalState.mPerspectiveProjection * _internalState.mCameraView;
    glUniformMatrix4fv(_symbolsStage.vs.param.mPerspectiveProjectionView, 1, GL_FALSE, glm::value_ptr(mPerspectiveProjectionView));
    GL_CHECK_RESULT;

    // Set orthographic projection matrix
    glUniformMatrix4fv(_symbolsStage.vs.param.mOrthographicProjection, 1, GL_FALSE, glm::value_ptr(_internalState.mOrthographicProjection));
    GL_CHECK_RESULT;

    // Set viewport
    glUniform4f(_symbolsStage.vs.param.viewport,
        currentState.viewport.left,
        currentState.windowSize.y - currentState.viewport.bottom,
        currentState.viewport.width(),
        currentState.viewport.height());
    GL_CHECK_RESULT;

    // Activate texture block for symbol textures
    glActiveTexture(GL_TEXTURE0 + 0);
    GL_CHECK_RESULT;

    // Set proper sampler for texture block
    gpuAPI->setTextureBlockSampler(GL_TEXTURE0 + 0, GPUAPI_OpenGL_Common::SamplerType::Symbol);

    {
        QMutexLocker scopedLocker(&getResources().getSymbolsMapMutex());
        const auto& symbolsMap = getResources().getSymbolsMap();
        typedef std::pair< std::shared_ptr<const MapSymbol>, std::shared_ptr<const GPUAPI::ResourceInGPU> > SymbolPair;

        // Get tile size in 31 coordinates
        const auto tileSize31 = (1u << (ZoomLevel::MaxZoomLevel - currentState.zoomBase));

        // Iterate over symbols by "order" in ascending direction
        for(auto itSymbols = symbolsMap.cbegin(); itSymbols != symbolsMap.cend(); ++itSymbols)
        {
            //TODO: check if symbols have intersection

            // For each "order" value, obtain list of entries and sort them
            const auto& unsortedSymbols = *itSymbols;
            if(unsortedSymbols.isEmpty())
                continue;
            QMultiMap< float, SymbolPair > sortedSymbols;
            for(auto itSymbolEntry = unsortedSymbols.cbegin(); itSymbolEntry != unsortedSymbols.cend(); ++itSymbolEntry)
            {
                const auto& symbol = itSymbolEntry.key();
                const auto resource = itSymbolEntry.value().lock();
                assert(static_cast<bool>(resource));

                // Calculate location of symbol in world coordinates.
                // World (0;0;0) is in the target, so subtract target position
                const auto symbolOffset31 = symbol->location31 - currentState.target31;
                glm::vec4 symbolPos;
                symbolPos.x = static_cast<float>(static_cast<double>(symbolOffset31.x) / tileSize31);
                symbolPos.y = 0.0f;
                symbolPos.z = static_cast<float>(static_cast<double>(symbolOffset31.y) / tileSize31);
                symbolPos.w = 1.0f;

                // Get distance from symbol to camera
                const auto distance = glm::distance(_internalState.worldCameraPosition, symbolPos);

                // Insert into map
                sortedSymbols.insert(distance, qMove(SymbolPair(symbol, resource)));
            }

            // Render symbols in sorted order, in reversed order
            QMapIterator< float, SymbolPair > itSymbolEntry(sortedSymbols);
            itSymbolEntry.toBack();
            while(itSymbolEntry.hasPrevious())
            {
                itSymbolEntry.previous();

                const auto distanceFromCamera = itSymbolEntry.key();
                const auto& symbolEntry = itSymbolEntry.value();
                const auto& symbol = symbolEntry.first;
                const auto gpuResource = std::static_pointer_cast<const GPUAPI::TextureInGPU>(symbolEntry.second);

                // Set symbol offset from target
                const auto symbolOffset31 = symbol->location31 - currentState.target31;
                glUniform2f(_symbolsStage.vs.param.symbolOffsetFromTarget,
                    static_cast<float>((static_cast<double>(symbolOffset31.x) / tileSize31)),
                    static_cast<float>((static_cast<double>(symbolOffset31.y) / tileSize31)));
                GL_CHECK_RESULT;
                //////////////////////////////////////////////////////////////////////////
                //////////////////////////////////////////////////////////////////////////
                //////////////////////////////////////////////////////////////////////////
                // this may be needed when calculating self-intersection
                /*auto sx = (static_cast<double>(symbolOffset31.x) / tileSize31) * TileSize3D;
                auto sy = (static_cast<double>(symbolOffset31.y) / tileSize31) * TileSize3D;

                glm::vec4 viewport(
                    currentState.viewport.left,
                    currentState.windowSize.y - currentState.viewport.bottom,
                    currentState.viewport.width(),
                    currentState.viewport.height());

                auto projectedV = glm::project(glm::vec3(sx, 0.0f, sy), _internalState.mCameraView, _internalState.mPerspectiveProjection, viewport);*/
                //////////////////////////////////////////////////////////////////////////
                //////////////////////////////////////////////////////////////////////////
                //////////////////////////////////////////////////////////////////////////

                // Set symbol size
                glUniform2i(_symbolsStage.vs.param.symbolSize, gpuResource->width, gpuResource->height);
                GL_CHECK_RESULT;

                // Set distance from camera to symbol
                glUniform1f(_symbolsStage.vs.param.distanceFromCamera, distanceFromCamera);
                GL_CHECK_RESULT;

                // Set on-screen offset
                glUniform2i(_symbolsStage.vs.param.onScreenOffset, symbol->offset.x, symbol->offset.y);
                GL_CHECK_RESULT;

                // Activate symbol texture
                glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(reinterpret_cast<intptr_t>(gpuResource->refInGPU)));
                GL_CHECK_RESULT;

                // Apply settings from texture block to texture
                gpuAPI->applyTextureBlockToTexture(GL_TEXTURE_2D, GL_TEXTURE0 + 0);

                // Draw symbol actually
                glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, nullptr);
                GL_CHECK_RESULT;
            }
        }
    }

    // Deactivate any symbol texture
    glActiveTexture(GL_TEXTURE0 + 0);
    GL_CHECK_RESULT;
    glBindTexture(GL_TEXTURE_2D, 0);
    GL_CHECK_RESULT;
    
    // Deactivate program
    glUseProgram(0);
    GL_CHECK_RESULT;

    // Deselect VAO
    gpuAPI->glBindVertexArray_wrapper(0);
    GL_CHECK_RESULT;
}

void OsmAnd::AtlasMapRenderer_OpenGL_Common::releaseSymbolsStage()
{
    const auto gpuAPI = getGPUAPI();

    GL_CHECK_PRESENT(glDeleteBuffers);
    GL_CHECK_PRESENT(glDeleteProgram);
    GL_CHECK_PRESENT(glDeleteShader);

    if(_symbolsStage.symbolIBO)
    {
        glDeleteBuffers(1, &_symbolsStage.symbolIBO);
        GL_CHECK_RESULT;
    }

    if(_symbolsStage.symbolVBO)
    {
        glDeleteBuffers(1, &_symbolsStage.symbolVBO);
        GL_CHECK_RESULT;
    }

    if(_symbolsStage.symbolVAO)
    {
        gpuAPI->glDeleteVertexArrays_wrapper(1, &_symbolsStage.symbolVAO);
        GL_CHECK_RESULT;
    }

    if(_symbolsStage.program)
    {
        glDeleteProgram(_symbolsStage.program);
        GL_CHECK_RESULT;
    }
    if(_symbolsStage.fs.id)
    {
        glDeleteShader(_symbolsStage.fs.id);
        GL_CHECK_RESULT;
    }
    if(_symbolsStage.vs.id)
    {
        glDeleteShader(_symbolsStage.vs.id);
        GL_CHECK_RESULT;
    }

    memset(&_symbolsStage, 0, sizeof(_symbolsStage));
}

bool OsmAnd::AtlasMapRenderer_OpenGL_Common::doInitializeRendering()
{
    GL_CHECK_PRESENT(glClearColor);
    GL_CHECK_PRESENT(glClearDepthf);

    bool ok;

    ok = AtlasMapRenderer::doInitializeRendering();
    if(!ok)
        return false;

    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    GL_CHECK_RESULT;

    glClearDepthf(1.0f);
    GL_CHECK_RESULT;

    initializeSkyStage();
    initializeRasterMapStage();
    initializeSymbolsStage();

    return true;
}

bool OsmAnd::AtlasMapRenderer_OpenGL_Common::doRenderFrame()
{
    GL_CHECK_PRESENT(glViewport);
    GL_CHECK_PRESENT(glEnable);
    GL_CHECK_PRESENT(glDisable);
    GL_CHECK_PRESENT(glBlendFunc);
    GL_CHECK_PRESENT(glClear);

    // Setup viewport
    glViewport(
        currentState.viewport.left,
        currentState.windowSize.y - currentState.viewport.bottom,
        currentState.viewport.width(),
        currentState.viewport.height());
    GL_CHECK_RESULT;

    // Clear buffers
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    GL_CHECK_RESULT;

    // Turn off blending for sky and map stages
    glDisable(GL_BLEND);
    GL_CHECK_RESULT;

    // Turn off depth testing for sky stage
    glDisable(GL_DEPTH_TEST);
    GL_CHECK_RESULT;

    // Render the sky
    renderSkyStage();

    // Turn on depth prior to raster map stage and further stages
    glEnable(GL_DEPTH_TEST);
    GL_CHECK_RESULT;
    glDepthFunc(GL_LEQUAL);
    GL_CHECK_RESULT;

    // Raster map stage is rendered without blending, since it's done in fragment shader
    renderRasterMapStage();

    // Turn on blending since now objects with transparency are going to be rendered
    glEnable(GL_BLEND);
    GL_CHECK_RESULT;
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    GL_CHECK_RESULT;

    // Render map symbols
    renderSymbolsStage();

    //TODO: render special fog object some day

    return true;
}

bool OsmAnd::AtlasMapRenderer_OpenGL_Common::doReleaseRendering()
{
    bool ok;

    releaseSymbolsStage();
    releaseRasterMapStage();
    releaseSkyStage();

    ok = AtlasMapRenderer::doReleaseRendering();
    if(!ok)
        return false;

    return true;
}

void OsmAnd::AtlasMapRenderer_OpenGL_Common::onValidateResourcesOfType(const MapRendererResources::ResourceType type)
{
    AtlasMapRenderer::onValidateResourcesOfType(type);

    if(type == MapRendererResources::ResourceType::ElevationData)
    {
        // Recreate tile patch since elevation data influences density of tile patch
        releaseTilePatch();
        createTilePatch();
    }
}

bool OsmAnd::AtlasMapRenderer_OpenGL_Common::updateInternalState(MapRenderer::InternalState* internalState_, const MapRendererState& state)
{
    bool ok;
    ok = AtlasMapRenderer::updateInternalState(internalState_, state);
    if(!ok)
        return false;

    const auto internalState = static_cast<InternalState*>(internalState_);

    // Prepare values for projection matrix
    const auto viewportWidth = state.viewport.width();
    const auto viewportHeight = state.viewport.height();
    if(viewportWidth == 0 || viewportHeight == 0)
        return false;
    internalState->aspectRatio = static_cast<float>(viewportWidth) / static_cast<float>(viewportHeight);
    internalState->fovInRadians = qDegreesToRadians(state.fieldOfView);
    internalState->projectionPlaneHalfHeight = _zNear * internalState->fovInRadians;
    internalState->projectionPlaneHalfWidth = internalState->projectionPlaneHalfHeight * internalState->aspectRatio;

    // Setup perspective projection with fake Z-far plane
    internalState->mPerspectiveProjection = glm::frustum(
        -internalState->projectionPlaneHalfWidth, internalState->projectionPlaneHalfWidth,
        -internalState->projectionPlaneHalfHeight, internalState->projectionPlaneHalfHeight,
        _zNear, 1000.0f);

    // Calculate limits of camera distance to target and actual distance
    const auto screenTileSize = getReferenceTileSizeOnScreen(state);
    internalState->nearDistanceFromCameraToTarget = Utilities_OpenGL_Common::calculateCameraDistance(internalState->mPerspectiveProjection, state.viewport, TileSize3D / 2.0f, screenTileSize / 2.0f, 1.5f);
    internalState->baseDistanceFromCameraToTarget = Utilities_OpenGL_Common::calculateCameraDistance(internalState->mPerspectiveProjection, state.viewport, TileSize3D / 2.0f, screenTileSize / 2.0f, 1.0f);
    internalState->farDistanceFromCameraToTarget = Utilities_OpenGL_Common::calculateCameraDistance(internalState->mPerspectiveProjection, state.viewport, TileSize3D / 2.0f, screenTileSize / 2.0f, 0.75f);

    // zoomFraction == [ 0.0 ... 0.5] scales tile [1.0x ... 1.5x]
    // zoomFraction == [-0.5 ...-0.0] scales tile [.75x ... 1.0x]
    if(state.zoomFraction >= 0.0f)
        internalState->distanceFromCameraToTarget = internalState->baseDistanceFromCameraToTarget - (internalState->baseDistanceFromCameraToTarget - internalState->nearDistanceFromCameraToTarget) * (2.0f * state.zoomFraction);
    else
        internalState->distanceFromCameraToTarget = internalState->baseDistanceFromCameraToTarget - (internalState->farDistanceFromCameraToTarget - internalState->baseDistanceFromCameraToTarget) * (2.0f * state.zoomFraction);
    internalState->groundDistanceFromCameraToTarget = internalState->distanceFromCameraToTarget * qCos(qDegreesToRadians(state.elevationAngle));
    internalState->tileScaleFactor = ((state.zoomFraction >= 0.0f) ? (1.0f + state.zoomFraction) : (1.0f + 0.5f * state.zoomFraction));
    internalState->scaleToRetainProjectedSize = internalState->distanceFromCameraToTarget / internalState->baseDistanceFromCameraToTarget;

    // Recalculate perspective projection with obtained value
    internalState->zSkyplane = state.fogDistance * internalState->scaleToRetainProjectedSize + internalState->distanceFromCameraToTarget;
    internalState->zFar = glm::length(glm::vec3(
        internalState->projectionPlaneHalfWidth * (internalState->zSkyplane / _zNear),
        internalState->projectionPlaneHalfHeight * (internalState->zSkyplane / _zNear),
        internalState->zSkyplane));
    internalState->mPerspectiveProjection = glm::frustum(
        -internalState->projectionPlaneHalfWidth, internalState->projectionPlaneHalfWidth,
        -internalState->projectionPlaneHalfHeight, internalState->projectionPlaneHalfHeight,
        _zNear, internalState->zFar);
    internalState->mPerspectiveProjectionInv = glm::inverse(internalState->mPerspectiveProjection);

    // Calculate orthographic projection
    const auto viewportBottom = state.windowSize.y - state.viewport.bottom;
    internalState->mOrthographicProjection = glm::ortho(
        static_cast<float>(state.viewport.left), static_cast<float>(state.viewport.right),
        static_cast<float>(viewportBottom) /*bottom*/, static_cast<float>(viewportBottom + viewportHeight) /*top*/,
        _zNear, internalState->zFar);

    // Setup camera
    internalState->mDistance = glm::translate(0.0f, 0.0f, -internalState->distanceFromCameraToTarget);
    internalState->mElevation = glm::rotate(state.elevationAngle, glm::vec3(1.0f, 0.0f, 0.0f));
    internalState->mAzimuth = glm::rotate(state.azimuth, glm::vec3(0.0f, 1.0f, 0.0f));
    internalState->mCameraView = internalState->mDistance * internalState->mElevation * internalState->mAzimuth;

    // Get inverse camera
    internalState->mDistanceInv = glm::translate(0.0f, 0.0f, internalState->distanceFromCameraToTarget);
    internalState->mElevationInv = glm::rotate(-state.elevationAngle, glm::vec3(1.0f, 0.0f, 0.0f));
    internalState->mAzimuthInv = glm::rotate(-state.azimuth, glm::vec3(0.0f, 1.0f, 0.0f));
    internalState->mCameraViewInv = internalState->mAzimuthInv * internalState->mElevationInv * internalState->mDistanceInv;

    // Get camera positions
    internalState->groundCameraPosition = (internalState->mAzimuthInv * glm::vec4(0.0f, 0.0f, internalState->distanceFromCameraToTarget, 1.0f)).xz;
    internalState->worldCameraPosition = _internalState.mCameraViewInv * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);

    // Correct fog distance
    internalState->correctedFogDistance = state.fogDistance * internalState->scaleToRetainProjectedSize + (internalState->distanceFromCameraToTarget - internalState->groundDistanceFromCameraToTarget);

    // Calculate skyplane size
    float zSkyplaneK = internalState->zSkyplane / _zNear;
    internalState->skyplaneSize.x = zSkyplaneK * internalState->projectionPlaneHalfWidth * 2.0f;
    internalState->skyplaneSize.y = zSkyplaneK * internalState->projectionPlaneHalfHeight * 2.0f;

    // Compute visible tileset
    computeVisibleTileset(internalState, state);

    return true;
}

const OsmAnd::AtlasMapRenderer::InternalState* OsmAnd::AtlasMapRenderer_OpenGL_Common::getInternalStateRef() const
{
    return &_internalState;
}

OsmAnd::AtlasMapRenderer::InternalState* OsmAnd::AtlasMapRenderer_OpenGL_Common::getInternalStateRef()
{
    return &_internalState;
}

void OsmAnd::AtlasMapRenderer_OpenGL_Common::computeVisibleTileset(InternalState* internalState, const MapRendererState& state)
{
    // 4 points of frustum near clipping box in camera coordinate space
    const glm::vec4 nTL_c(-internalState->projectionPlaneHalfWidth, +internalState->projectionPlaneHalfHeight, -_zNear, 1.0f);
    const glm::vec4 nTR_c(+internalState->projectionPlaneHalfWidth, +internalState->projectionPlaneHalfHeight, -_zNear, 1.0f);
    const glm::vec4 nBL_c(-internalState->projectionPlaneHalfWidth, -internalState->projectionPlaneHalfHeight, -_zNear, 1.0f);
    const glm::vec4 nBR_c(+internalState->projectionPlaneHalfWidth, -internalState->projectionPlaneHalfHeight, -_zNear, 1.0f);

    // 4 points of frustum far clipping box in camera coordinate space
    const auto zFar = internalState->zSkyplane;
    const auto zFarK = zFar / _zNear;
    const glm::vec4 fTL_c(zFarK * nTL_c.x, zFarK * nTL_c.y, zFarK * nTL_c.z, 1.0f);
    const glm::vec4 fTR_c(zFarK * nTR_c.x, zFarK * nTR_c.y, zFarK * nTR_c.z, 1.0f);
    const glm::vec4 fBL_c(zFarK * nBL_c.x, zFarK * nBL_c.y, zFarK * nBL_c.z, 1.0f);
    const glm::vec4 fBR_c(zFarK * nBR_c.x, zFarK * nBR_c.y, zFarK * nBR_c.z, 1.0f);

    // Transform 8 frustum vertices + camera center to global space
    const auto eye_g = internalState->mCameraViewInv * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
    const auto fTL_g = internalState->mCameraViewInv * fTL_c;
    const auto fTR_g = internalState->mCameraViewInv * fTR_c;
    const auto fBL_g = internalState->mCameraViewInv * fBL_c;
    const auto fBR_g = internalState->mCameraViewInv * fBR_c;
    const auto nTL_g = internalState->mCameraViewInv * nTL_c;
    const auto nTR_g = internalState->mCameraViewInv * nTR_c;
    const auto nBL_g = internalState->mCameraViewInv * nBL_c;
    const auto nBR_g = internalState->mCameraViewInv * nBR_c;

    // Get (up to) 4 points of frustum edges & plane intersection
    const glm::vec3 planeN(0.0f, 1.0f, 0.0f);
    const glm::vec3 planeO(0.0f, 0.0f, 0.0f);
    auto intersectionPointsCounter = 0u;
    glm::vec3 intersectionPoint;
    glm::vec2 intersectionPoints[4];

    if(intersectionPointsCounter < 4 && Utilities_OpenGL_Common::lineSegmentIntersectPlane(planeN, planeO, nBL_g.xyz, fBL_g.xyz, intersectionPoint))
    {
        intersectionPoints[intersectionPointsCounter] = intersectionPoint.xz;
        intersectionPointsCounter++;
    }
    if(intersectionPointsCounter < 4 && Utilities_OpenGL_Common::lineSegmentIntersectPlane(planeN, planeO, nBR_g.xyz, fBR_g.xyz, intersectionPoint))
    {
        intersectionPoints[intersectionPointsCounter] = intersectionPoint.xz;
        intersectionPointsCounter++;
    }
    if(intersectionPointsCounter < 4 && Utilities_OpenGL_Common::lineSegmentIntersectPlane(planeN, planeO, nTR_g.xyz, fTR_g.xyz, intersectionPoint))
    {
        intersectionPoints[intersectionPointsCounter] = intersectionPoint.xz;
        intersectionPointsCounter++;
    }
    if(intersectionPointsCounter < 4 && Utilities_OpenGL_Common::lineSegmentIntersectPlane(planeN, planeO, nTL_g.xyz, fTL_g.xyz, intersectionPoint))
    {
        intersectionPoints[intersectionPointsCounter] = intersectionPoint.xz;
        intersectionPointsCounter++;
    }
    if(intersectionPointsCounter < 4 && Utilities_OpenGL_Common::lineSegmentIntersectPlane(planeN, planeO, fTR_g.xyz, fBR_g.xyz, intersectionPoint))
    {
        intersectionPoints[intersectionPointsCounter] = intersectionPoint.xz;
        intersectionPointsCounter++;
    }
    if(intersectionPointsCounter < 4 && Utilities_OpenGL_Common::lineSegmentIntersectPlane(planeN, planeO, fTL_g.xyz, fBL_g.xyz, intersectionPoint))
    {
        intersectionPoints[intersectionPointsCounter] = intersectionPoint.xz;
        intersectionPointsCounter++;
    }
    if(intersectionPointsCounter < 4 && Utilities_OpenGL_Common::lineSegmentIntersectPlane(planeN, planeO, nTR_g.xyz, nBR_g.xyz, intersectionPoint))
    {
        intersectionPoints[intersectionPointsCounter] = intersectionPoint.xz;
        intersectionPointsCounter++;
    }
    if(intersectionPointsCounter < 4 && Utilities_OpenGL_Common::lineSegmentIntersectPlane(planeN, planeO, nTL_g.xyz, nBL_g.xyz, intersectionPoint))
    {
        intersectionPoints[intersectionPointsCounter] = intersectionPoint.xz;
        intersectionPointsCounter++;
    }

    assert(intersectionPointsCounter == 4);

    // Normalize intersection points to tiles
    intersectionPoints[0] /= static_cast<float>(TileSize3D);
    intersectionPoints[1] /= static_cast<float>(TileSize3D);
    intersectionPoints[2] /= static_cast<float>(TileSize3D);
    intersectionPoints[3] /= static_cast<float>(TileSize3D);

    // "Round"-up tile indices
    // In-tile normalized position is added, since all tiles are going to be
    // translated in opposite direction during rendering
    const auto& ip = intersectionPoints;
    const PointF p[4] =
    {
        PointF(ip[0].x + internalState->targetInTileOffsetN.x, ip[0].y + internalState->targetInTileOffsetN.y),
        PointF(ip[1].x + internalState->targetInTileOffsetN.x, ip[1].y + internalState->targetInTileOffsetN.y),
        PointF(ip[2].x + internalState->targetInTileOffsetN.x, ip[2].y + internalState->targetInTileOffsetN.y),
        PointF(ip[3].x + internalState->targetInTileOffsetN.x, ip[3].y + internalState->targetInTileOffsetN.y),
    };

    //NOTE: so far scanline does not work exactly as expected, so temporary switch to old implementation
    {
        QSet<TileId> visibleTiles;
        PointI p0(qFloor(p[0].x), qFloor(p[0].y));
        PointI p1(qFloor(p[1].x), qFloor(p[1].y));
        PointI p2(qFloor(p[2].x), qFloor(p[2].y));
        PointI p3(qFloor(p[3].x), qFloor(p[3].y));

        const auto xMin = qMin(qMin(p0.x, p1.x), qMin(p2.x, p3.x));
        const auto xMax = qMax(qMax(p0.x, p1.x), qMax(p2.x, p3.x));
        const auto yMin = qMin(qMin(p0.y, p1.y), qMin(p2.y, p3.y));
        const auto yMax = qMax(qMax(p0.y, p1.y), qMax(p2.y, p3.y));
        for(auto x = xMin; x <= xMax; x++)
        {
            for(auto y = yMin; y <= yMax; y++)
            {
                TileId tileId;
                tileId.x = x + internalState->targetTileId.x;
                tileId.y = y + internalState->targetTileId.y;

                visibleTiles.insert(tileId);
            }
        }

        internalState->visibleTiles = visibleTiles.toList();
    }
    /*
    //TODO: Find visible tiles using scanline fill
    _visibleTiles.clear();
    Utilities::scanlineFillPolygon(4, &p[0],
    [this, pC](const PointI& point)
    {
    TileId tileId;
    tileId.x = point.x;// + _targetTile.x;
    tileId.y = point.y;// + _targetTile.y;

    _visibleTiles.insert(tileId);
    });
    */
}

bool OsmAnd::AtlasMapRenderer_OpenGL_Common::postInitializeRendering()
{
    bool ok;

    ok = AtlasMapRenderer::postInitializeRendering();
    if(!ok)
        return false;

    createTilePatch();

    return true;
}

bool OsmAnd::AtlasMapRenderer_OpenGL_Common::preReleaseRendering()
{
    bool ok;

    ok = AtlasMapRenderer::preReleaseRendering();
    if(!ok)
        return false;

    releaseTilePatch();

    return true;
}

void OsmAnd::AtlasMapRenderer_OpenGL_Common::createTilePatch()
{
    MapTileVertex* pVertices = nullptr;
    GLsizei verticesCount = 0;
    GLushort* pIndices = nullptr;
    GLsizei indicesCount = 0;

    if(!currentState.elevationDataProvider)
    {
        // Simple tile patch, that consists of 4 vertices

        // Vertex data
        const GLfloat tsz = static_cast<GLfloat>(TileSize3D);
        static MapTileVertex vertices[4] =
        {
            // In OpenGL, UV origin is BL. But since same rule applies to uploading texture data,
            // texture in memory is vertically flipped, so swap bottom and top UVs
            { {0.0f,  tsz}, {0.0f, 1.0f} },//BL
            { {0.0f, 0.0f}, {0.0f, 0.0f} },//TL
            { { tsz, 0.0f}, {1.0f, 0.0f} },//TR
            { { tsz,  tsz}, {1.0f, 1.0f} } //BR
        };
        pVertices = new MapTileVertex[4];
        memcpy(pVertices, vertices, 4*sizeof(MapTileVertex));
        verticesCount = 4;

        // Index data
        static GLushort indices[6] =
        {
            0, 1, 2,
            0, 2, 3
        };
        pIndices = new GLushort[6];
        memcpy(pIndices, indices, 6*sizeof(GLushort));
        indicesCount = 6;
    }
    else
    {
        // Complex tile patch, that consists of (TileElevationNodesPerSide*TileElevationNodesPerSide) number of
        // height clusters. Height cluster itself consists of 4 vertices, 6 indices and 2 polygons
        const auto heightPrimitivesPerSide = currentConfiguration.heixelsPerTileSide - 1;
        const GLfloat clusterSize = static_cast<GLfloat>(TileSize3D) / static_cast<float>(heightPrimitivesPerSide);
        verticesCount = currentConfiguration.heixelsPerTileSide * currentConfiguration.heixelsPerTileSide;
        pVertices = new MapTileVertex[verticesCount];
        indicesCount = (heightPrimitivesPerSide * heightPrimitivesPerSide) * 6;
        pIndices = new GLushort[indicesCount];

        MapTileVertex* pV = pVertices;

        // Form vertices
        assert(verticesCount <= std::numeric_limits<GLushort>::max());
        for(auto row = 0u; row < currentConfiguration.heixelsPerTileSide; row++)
        {
            for(auto col = 0u; col < currentConfiguration.heixelsPerTileSide; col++, pV++)
            {
                pV->positionXZ[0] = static_cast<float>(col) * clusterSize;
                pV->positionXZ[1] = static_cast<float>(row) * clusterSize;

                pV->textureUV[0] = static_cast<float>(col) / static_cast<float>(heightPrimitivesPerSide);
                pV->textureUV[1] = static_cast<float>(row) / static_cast<float>(heightPrimitivesPerSide);
            }
        }

        // Form indices
        GLushort* pI = pIndices;
        for(auto row = 0u; row < heightPrimitivesPerSide; row++)
        {
            for(auto col = 0u; col < heightPrimitivesPerSide; col++)
            {
                const auto p0 = (row + 1) * currentConfiguration.heixelsPerTileSide + col + 0;//BL
                const auto p1 = (row + 0) * currentConfiguration.heixelsPerTileSide + col + 0;//TL
                const auto p2 = (row + 0) * currentConfiguration.heixelsPerTileSide + col + 1;//TR
                const auto p3 = (row + 1) * currentConfiguration.heixelsPerTileSide + col + 1;//BR
                assert(p0 <= verticesCount);
                assert(p1 <= verticesCount);
                assert(p2 <= verticesCount);
                assert(p3 <= verticesCount);

                // Triangle 0
                pI[0] = p0;
                pI[1] = p1;
                pI[2] = p2;
                pI += 3;

                // Triangle 1
                pI[0] = p0;
                pI[1] = p2;
                pI[2] = p3;
                pI += 3;
            }
        }
    }

    _tilePatchIndicesCount = indicesCount;
    allocateTilePatch(pVertices, verticesCount, pIndices, indicesCount);

    delete[] pVertices;
    delete[] pIndices;
}

OsmAnd::GPUAPI_OpenGL_Common* OsmAnd::AtlasMapRenderer_OpenGL_Common::getGPUAPI() const
{
    return static_cast<OsmAnd::GPUAPI_OpenGL_Common*>(gpuAPI.get());
}

float OsmAnd::AtlasMapRenderer_OpenGL_Common::getReferenceTileSizeOnScreen( const MapRendererState& state )
{
    const auto& rasterMapProvider = state.rasterLayerProviders[static_cast<int>(RasterMapLayerId::BaseLayer)];
    if(!rasterMapProvider)
        return static_cast<float>(DefaultReferenceTileSizeOnScreen) * setupOptions.displayDensityFactor;

    auto tileProvider = std::static_pointer_cast<IMapBitmapTileProvider>(rasterMapProvider);
    return tileProvider->getTileSize() * (setupOptions.displayDensityFactor / tileProvider->getTileDensity());
}

float OsmAnd::AtlasMapRenderer_OpenGL_Common::getReferenceTileSizeOnScreen()
{
    return getReferenceTileSizeOnScreen(state);
}

float OsmAnd::AtlasMapRenderer_OpenGL_Common::getScaledTileSizeOnScreen()
{
    InternalState internalState;
    bool ok = updateInternalState(&internalState, state);

    return getReferenceTileSizeOnScreen(state) * internalState.tileScaleFactor;
}

bool OsmAnd::AtlasMapRenderer_OpenGL_Common::getLocationFromScreenPoint( const PointI& screenPoint, PointI& location31 )
{
    PointI64 location;
    if(!getLocationFromScreenPoint(screenPoint, location))
        return false;
    location31 = Utilities::normalizeCoordinates(location, ZoomLevel31);

    return true;
}

bool OsmAnd::AtlasMapRenderer_OpenGL_Common::getLocationFromScreenPoint( const PointI& screenPoint, PointI64& location )
{
    InternalState internalState;
    bool ok = updateInternalState(&internalState, state);
    if(!ok)
        return false;

    glm::vec4 viewport(
        state.viewport.left,
        state.windowSize.y - state.viewport.bottom,
        state.viewport.width(),
        state.viewport.height());
    const auto nearInWorld = glm::unProject(glm::vec3(screenPoint.x, state.windowSize.y - screenPoint.y, 0.0f), internalState.mCameraView, internalState.mPerspectiveProjection, viewport);
    const auto farInWorld = glm::unProject(glm::vec3(screenPoint.x, state.windowSize.y - screenPoint.y, 1.0f), internalState.mCameraView, internalState.mPerspectiveProjection, viewport);
    const auto rayD = glm::normalize(farInWorld - nearInWorld);

    const glm::vec3 planeN(0.0f, 1.0f, 0.0f);
    const glm::vec3 planeO(0.0f, 0.0f, 0.0f);
    float distance;
    const auto intersects = Utilities_OpenGL_Common::rayIntersectPlane(planeN, planeO, rayD, nearInWorld, distance);
    if(!intersects)
        return false;

    auto intersection = nearInWorld + distance*rayD;
    intersection /= static_cast<float>(TileSize3D);

    double x = intersection.x + internalState.targetInTileOffsetN.x;
    double y = intersection.z + internalState.targetInTileOffsetN.y;

    const auto zoomDiff = ZoomLevel::MaxZoomLevel - state.zoomBase;
    const auto tileSize31 = (1u << zoomDiff);
    x *= tileSize31;
    y *= tileSize31;

    location.x = static_cast<int64_t>(x) + (internalState.targetTileId.x << zoomDiff);
    location.y = static_cast<int64_t>(y) + (internalState.targetTileId.y << zoomDiff);
    
    return true;
}
