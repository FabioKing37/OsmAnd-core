/**
* @file
*
* @section LICENSE
*
* OsmAnd - Android navigation software based on OSM maps.
* Copyright (C) 2010-2013  OsmAnd Authors listed in AUTHORS file
*
* This program is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.

* You should have received a copy of the GNU General Public License
* along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/
#ifndef _OSMAND_CORE_MAP_RENDERER_H_
#define _OSMAND_CORE_MAP_RENDERER_H_

#include <OsmAndCore/stdlib_common.h>

#include <OsmAndCore/QtExtensions.h>
#include <QAtomicInt>

#include <OsmAndCore.h>
#include <CommonTypes.h>
#include <MapTypes.h>
#include <Concurrent.h>
#include <IMapRenderer.h>
#include <GPUAPI.h>
#include <IMapTileProvider.h>
#include <TilesCollection.h>
#include "MapRendererResources.h"

namespace OsmAnd
{
    class IMapProvider;
    class MapRendererTiledResources;
    class MapSymbol;
    
    class MapRenderer : public IMapRenderer
    {
    public:
        // Declare short aliases for resource-related entities
        typedef OsmAnd::MapRendererResources Resources;
        typedef OsmAnd::MapRendererResources::ResourceType ResourceType;
        typedef OsmAnd::MapRendererResources::ResourceState ResourceState;

    private:
        // Configuration-related:
        mutable QReadWriteLock _configurationLock;
        MapRendererConfiguration _currentConfiguration;
        volatile uint32_t _currentConfigurationInvalidatedMask;
        void invalidateCurrentConfiguration(const uint32_t changesMask);
        bool updateCurrentConfiguration();

        // State-related:
        mutable QAtomicInt _frameInvalidatesCounter;
        int _frameInvalidatesToBeProcessed;
        mutable QMutex _requestedStateMutex;
        mutable QReadWriteLock _internalStateLock;
        MapRendererState _currentState;
        volatile uint32_t _requestedStateUpdatedMask;
        bool revalidateState();
        void notifyRequestedStateWasUpdated(const MapRendererStateChange change);

        // Resources-related:
        std::unique_ptr<MapRendererResources> _resources;
        
        // GPU worker related:
        volatile bool _gpuWorkerIsAlive;
        Qt::HANDLE _gpuWorkerThreadId;
        std::unique_ptr<Concurrent::Thread> _gpuWorkerThread;
        QMutex _gpuWorkerThreadWakeupMutex;
        QWaitCondition _gpuWorkerThreadWakeup;
        void gpuWorkerThreadProcedure();

        // General:
        QSet<TileId> _uniqueTiles;
        std::unique_ptr<GPUAPI> _gpuAPI;
        void invalidateFrame();
        Qt::HANDLE _renderThreadId;
    protected:
        MapRenderer();

        // General:
        const std::unique_ptr<GPUAPI>& gpuAPI;

        // Configuration-related:
        const MapRendererConfiguration& currentConfiguration;
        enum ConfigurationChange
        {
            ColorDepthForcing = 1 << 0,
            ElevationDataResolution = 1 << 1,
            TexturesFilteringMode = 1 << 2,
            PaletteTexturesUsage = 1 << 3,
        };
        virtual void validateConfigurationChange(const ConfigurationChange& change);

        // State-related:
        const MapRendererState& currentState;
        struct InternalState
        {
            virtual ~InternalState()
            {}

            TileId targetTileId;
            PointF targetInTileOffsetN;
            QList<TileId> visibleTiles;
        };
        virtual const InternalState* getInternalStateRef() const = 0;
        virtual InternalState* getInternalStateRef() = 0;
        virtual bool updateInternalState(InternalState* internalState, const MapRendererState& state);

        // Resources-related:
        const MapRendererResources& getResources() const;
        virtual void onValidateResourcesOfType(const MapRendererResources::ResourceType type);
        void requestResourcesUpload();
        bool convertBitmap(const std::shared_ptr<const SkBitmap>& input, std::shared_ptr<const SkBitmap>& output, const AlphaChannelData alphaChannelData = AlphaChannelData::Undefined) const;
        bool convertMapTile(std::shared_ptr<const MapTile>& mapTile) const;
        bool convertMapTile(const std::shared_ptr<const MapTile>& input, std::shared_ptr<const MapTile>& output) const;
        bool convertMapSymbol(std::shared_ptr<const MapSymbol>& mapSymbol) const;
        bool convertMapSymbol(const std::shared_ptr<const MapSymbol>& input, std::shared_ptr<const MapSymbol>& output) const;

        // General:
        virtual GPUAPI* allocateGPUAPI() = 0;

        // Customization points:
        virtual bool preInitializeRendering();
        virtual bool doInitializeRendering();
        virtual bool postInitializeRendering();

        virtual bool prePrepareFrame();
        virtual bool doPrepareFrame();
        virtual bool postPrepareFrame();

        virtual bool preRenderFrame();
        virtual bool doRenderFrame() = 0;
        virtual bool postRenderFrame();

        virtual bool preProcessRendering();
        virtual bool doProcessRendering();
        virtual bool postProcessRendering();

        virtual bool preReleaseRendering();
        virtual bool doReleaseRendering();
        virtual bool postReleaseRendering();
    public:
        virtual ~MapRenderer();

        virtual bool setup(const MapRendererSetupOptions& setupOptions);

        // General:

        // Configuration-related:
        virtual void setConfiguration(const MapRendererConfiguration& configuration, bool forcedUpdate = false);

        virtual bool initializeRendering();
        virtual bool prepareFrame();
        virtual bool renderFrame();
        virtual bool processRendering();
        virtual bool releaseRendering();

        virtual bool isFrameInvalidated() const;

        virtual unsigned int getVisibleTilesCount() const;
        virtual unsigned int getSymbolsCount() const;

        virtual void setRasterLayerProvider(const RasterMapLayerId layerId, const std::shared_ptr<IMapBitmapTileProvider>& tileProvider, bool forcedUpdate = false);
        virtual void resetRasterLayerProvider(const RasterMapLayerId layerId, bool forcedUpdate = false);
        virtual void setRasterLayerOpacity(const RasterMapLayerId layerId, const float opacity, bool forcedUpdate = false);
        virtual void setElevationDataProvider(const std::shared_ptr<IMapElevationDataProvider>& tileProvider, bool forcedUpdate = false);
        virtual void resetElevationDataProvider(bool forcedUpdate = false);
        virtual void setElevationDataScaleFactor(const float factor, bool forcedUpdate = false);
        virtual void addSymbolProvider(const std::shared_ptr<IMapSymbolProvider>& provider, bool forcedUpdate = false);
        virtual void removeSymbolProvider(const std::shared_ptr<IMapSymbolProvider>& provider, bool forcedUpdate = false);
        virtual void removeAllSymbolProviders(bool forcedUpdate = false);
        virtual void setWindowSize(const PointI& windowSize, bool forcedUpdate = false);
        virtual void setViewport(const AreaI& viewport, bool forcedUpdate = false);
        virtual void setFieldOfView(const float fieldOfView, bool forcedUpdate = false);
        virtual void setDistanceToFog(const float fogDistance, bool forcedUpdate = false);
        virtual void setFogOriginFactor(const float factor, bool forcedUpdate = false);
        virtual void setFogHeightOriginFactor(const float factor, bool forcedUpdate = false);
        virtual void setFogDensity(const float fogDensity, bool forcedUpdate = false);
        virtual void setFogColor(const FColorRGB& color, bool forcedUpdate = false);
        virtual void setSkyColor(const FColorRGB& color, bool forcedUpdate = false);
        virtual void setAzimuth(const float azimuth, bool forcedUpdate = false);
        virtual void setElevationAngle(const float elevationAngle, bool forcedUpdate = false);
        virtual void setTarget(const PointI& target31, bool forcedUpdate = false);
        virtual void setZoom(const float zoom, bool forcedUpdate = false);

    friend class OsmAnd::MapRendererResources;
    };

}

#endif // _OSMAND_CORE_MAP_RENDERER_H_
