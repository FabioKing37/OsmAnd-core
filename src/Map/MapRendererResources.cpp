#include "MapRendererResources.h"

#include "MapRenderer.h"
#include "IMapProvider.h"
#include "IMapBitmapTileProvider.h"
#include "IMapElevationDataProvider.h"
#include "IMapSymbolProvider.h"
#include "IRetainableResource.h"
#include "MapObject.h"
#include "EmbeddedResources.h"
#include "Utilities.h"
#include "Logging.h"

#include <SkBitmap.h>
#include <SkImageDecoder.h>

OsmAnd::MapRendererResources::MapRendererResources(MapRenderer* const owner_)
    : _taskHostBridge(this)
    , _symbolsMapCount(0)
    , _invalidatedResourcesTypesMask(0)
    , _workerThreadIsAlive(false)
    , _workerThreadId(nullptr)
    , _workerThread(new Concurrent::Thread(std::bind(&MapRendererResources::workerThreadProcedure, this)))
    , renderer(owner_)
    , processingTileStub(_processingTileStub)
    , unavailableTileStub(_unavailableTileStub)
{
#if defined(DEBUG) || defined(_DEBUG)
    LogPrintf(LogSeverityLevel::Info, "Map renderer will use max %d worker thread(s) to process requests", _resourcesRequestWorkersPool.maxThreadCount());
#endif
    // Raster resources collections are special, so preallocate them
    {
        auto& resources = _storage[static_cast<int>(ResourceType::RasterMap)];
        resources.reserve(RasterMapLayersCount);
        while(resources.size() < RasterMapLayersCount)
            resources.push_back(qMove(std::shared_ptr<TiledResourcesCollection>()));
    }

    // Initialize default resources
    initializeDefaultResources();

    // Start worker thread
    _workerThreadIsAlive = true;
    _workerThread->start();
}

OsmAnd::MapRendererResources::~MapRendererResources()
{
    // Stop worker thread
    _workerThreadIsAlive = false;
    {
        QMutexLocker scopedLocker(&_workerThreadWakeupMutex);
        _workerThreadWakeup.wakeAll();
    }
    _workerThread->wait();

    // Release all resources
    for(auto itResourcesCollections = _storage.begin(); itResourcesCollections != _storage.end(); ++itResourcesCollections)
    {
        auto& resourcesCollections = *itResourcesCollections;

        for(auto itResourcesCollection = resourcesCollections.cbegin(); itResourcesCollection != resourcesCollections.cend(); ++itResourcesCollection)
            releaseResourcesFrom(*itResourcesCollection);
        resourcesCollections.clear();
    }

    // Release all bindings
    for(auto resourceType = 0; resourceType < ResourceTypesCount; resourceType++)
    {
        _bindings[resourceType].providersToCollections.clear();
        _bindings[resourceType].collectionsToProviders.clear();
    }

    // Release default resources
    releaseDefaultResources();

    // Wait for all tasks to complete
    _taskHostBridge.onOwnerIsBeingDestructed();
}

bool OsmAnd::MapRendererResources::initializeDefaultResources()
{
    // Upload stubs
    {
        const auto& data = EmbeddedResources::decompressResource(QLatin1String("map/stubs/processing_tile.png"));
        auto bitmap = new SkBitmap();
        if(!SkImageDecoder::DecodeMemory(data.data(), data.size(), bitmap, SkBitmap::Config::kNo_Config, SkImageDecoder::kDecodePixels_Mode))
        {
            delete bitmap;
            return false;
        }
        else
        {
            std::shared_ptr<const MapTile> bitmapTile(new MapBitmapTile(bitmap, AlphaChannelData::Undefined));
            uploadTileToGPU(bitmapTile, _processingTileStub);
        }
    }
    {
        const auto& data = EmbeddedResources::decompressResource(QLatin1String("map/stubs/unavailable_tile.png"));
        auto bitmap = new SkBitmap();
        if(!SkImageDecoder::DecodeMemory(data.data(), data.size(), bitmap, SkBitmap::Config::kNo_Config, SkImageDecoder::kDecodePixels_Mode))
        {
            delete bitmap;
            return false;
        }
        else
        {
            std::shared_ptr<const MapTile> bitmapTile(new MapBitmapTile(bitmap, AlphaChannelData::Undefined));
            uploadTileToGPU(bitmapTile, _unavailableTileStub);
        }
    }

    return true;
}

bool OsmAnd::MapRendererResources::releaseDefaultResources()
{
    // Release stubs
    if(_unavailableTileStub)
    {
        assert(_unavailableTileStub.use_count() == 1);
        _unavailableTileStub.reset();
    }
    if(_processingTileStub)
    {
        assert(_processingTileStub.use_count() == 1);
        _processingTileStub.reset();
    }

    return true;
}

bool OsmAnd::MapRendererResources::uploadTileToGPU(const std::shared_ptr<const MapTile>& mapTile, std::shared_ptr<const GPUAPI::ResourceInGPU>& outResourceInGPU)
{
    std::shared_ptr<const MapTile> convertedTile;
    const auto wasConverted = renderer->convertMapTile(mapTile, convertedTile);
    return renderer->gpuAPI->uploadTileToGPU(wasConverted ? convertedTile : mapTile, outResourceInGPU);
}

bool OsmAnd::MapRendererResources::uploadSymbolToGPU(const std::shared_ptr<const MapSymbol>& mapSymbol, std::shared_ptr<const GPUAPI::ResourceInGPU>& outResourceInGPU)
{
    std::shared_ptr<const MapSymbol> convertedSymbol;
    const auto wasConverted = renderer->convertMapSymbol(mapSymbol, convertedSymbol);
    return renderer->gpuAPI->uploadSymbolToGPU(wasConverted ? convertedSymbol : mapSymbol, outResourceInGPU);
}

void OsmAnd::MapRendererResources::updateBindings(const MapRendererState& state, const uint32_t updatedMask)
{
    if(updatedMask & (1u << static_cast<int>(MapRendererStateChange::ElevationData_Provider)))
    {
        auto& bindings = _bindings[static_cast<int>(ResourceType::ElevationData)];
        auto& resources = _storage[static_cast<int>(ResourceType::ElevationData)];

        // Clean-up and unbind gone providers and their resources
        QMutableHashIterator< std::shared_ptr<IMapProvider>, std::shared_ptr<TiledResourcesCollection> > itBindedProvider(bindings.providersToCollections);
        while(itBindedProvider.hasNext())
        {
            itBindedProvider.next();

            // Skip binding if it's still active
            if(itBindedProvider.key() == state.elevationDataProvider)
                continue;

            // Clean-up resources
            releaseResourcesFrom(itBindedProvider.value());

            // Remove resources collection
            resources.removeOne(itBindedProvider.value());

            // Remove binding
            bindings.collectionsToProviders.remove(itBindedProvider.value());
            itBindedProvider.remove();
        }

        // Create new binding and storage
        if(state.elevationDataProvider && !bindings.providersToCollections.contains(state.elevationDataProvider))
        {
            // Create new resources collection
            const std::shared_ptr< TiledResourcesCollection > newResourcesCollection(new TiledResourcesCollection(ResourceType::ElevationData));

            // Add binding
            bindings.providersToCollections.insert(state.elevationDataProvider, newResourcesCollection);
            bindings.collectionsToProviders.insert(newResourcesCollection, state.elevationDataProvider);

            // Add resources collection
            resources.push_back(qMove(newResourcesCollection));
        }
    }
    if(updatedMask & (1u << static_cast<int>(MapRendererStateChange::RasterLayers_Providers)))
    {
        auto& bindings = _bindings[static_cast<int>(ResourceType::RasterMap)];
        auto& resources = _storage[static_cast<int>(ResourceType::RasterMap)];

        // Clean-up and unbind gone providers and their resources
        QMutableHashIterator< std::shared_ptr<IMapProvider>, std::shared_ptr<TiledResourcesCollection> > itBindedProvider(bindings.providersToCollections);
        while(itBindedProvider.hasNext())
        {
            itBindedProvider.next();

            // Skip binding if it's still active
            if(std::find(state.rasterLayerProviders.cbegin(), state.rasterLayerProviders.cend(),
                std::static_pointer_cast<IMapBitmapTileProvider>(itBindedProvider.key())) != state.rasterLayerProviders.cend())
            {
                continue;
            }

            // Clean-up resources
            releaseResourcesFrom(itBindedProvider.value());

            // Reset reference to resources collection, but keep the space in array
            qFind(resources.begin(), resources.end(), itBindedProvider.value())->reset();

            // Remove binding
            bindings.collectionsToProviders.remove(itBindedProvider.value());
            itBindedProvider.remove();
        }

        // Create new binding and storage
        auto rasterLayerIdx = 0;
        for(auto itProvider = state.rasterLayerProviders.cbegin(); itProvider != state.rasterLayerProviders.cend(); ++itProvider, rasterLayerIdx++)
        {
            // Skip empty providers
            if(!*itProvider)
                continue;

            // If binding already exists, skip creation
            if(bindings.providersToCollections.contains(std::static_pointer_cast<IMapProvider>(*itProvider)))
                continue;

            // Create new resources collection
            const std::shared_ptr< TiledResourcesCollection > newResourcesCollection(new TiledResourcesCollection(ResourceType::RasterMap));

            // Add binding
            bindings.providersToCollections.insert(*itProvider, newResourcesCollection);
            bindings.collectionsToProviders.insert(newResourcesCollection, *itProvider);

            // Add resources collection
            assert(rasterLayerIdx < resources.size());
            assert(static_cast<bool>(resources[rasterLayerIdx]) == false);
            resources[rasterLayerIdx] = newResourcesCollection;
        }
    }
    if(updatedMask & (1u << static_cast<int>(MapRendererStateChange::Symbols_Providers)))
    {
        auto& bindings = _bindings[static_cast<int>(ResourceType::Symbols)];
        auto& resources = _storage[static_cast<int>(ResourceType::Symbols)];

        // Clean-up and unbind gone providers and their resources
        QMutableHashIterator< std::shared_ptr<IMapProvider>, std::shared_ptr<TiledResourcesCollection> > itBindedProvider(bindings.providersToCollections);
        while(itBindedProvider.hasNext())
        {
            itBindedProvider.next();

            // Skip binding if it's still active
            if(state.symbolProviders.contains(std::static_pointer_cast<IMapSymbolProvider>(itBindedProvider.key())))
                continue;

            // Clean-up resources
            releaseResourcesFrom(itBindedProvider.value());

            // Remove resources collection
            resources.removeOne(itBindedProvider.value());

            // Remove binding
            bindings.collectionsToProviders.remove(itBindedProvider.value());
            itBindedProvider.remove();
        }

        // Create new binding and storage
        for(auto itProvider = state.symbolProviders.cbegin(); itProvider != state.symbolProviders.cend(); ++itProvider)
        {
            // If binding already exists, skip creation
            if(bindings.providersToCollections.contains(std::static_pointer_cast<IMapProvider>(*itProvider)))
                continue;

            // Create new resources collection
            const std::shared_ptr< TiledResourcesCollection > newResourcesCollection(new SymbolsResourcesCollection());

            // Add binding
            bindings.providersToCollections.insert(*itProvider, newResourcesCollection);
            bindings.collectionsToProviders.insert(newResourcesCollection, *itProvider);

            // Add resources collection
            resources.push_back(qMove(newResourcesCollection));
        }
    }
}

void OsmAnd::MapRendererResources::updateActiveZone(const QSet<TileId>& tiles, const ZoomLevel zoom)
{
    // Lock worker wakeup mutex
    QMutexLocker scopedLocker(&_workerThreadWakeupMutex);

    // Update active zone
    _activeTiles = tiles;
    _activeZoom = zoom;

    // Wake up the worker
    _workerThreadWakeup.wakeAll();
}

bool OsmAnd::MapRendererResources::obtainProviderFor(TiledResourcesCollection* const resourcesRef, std::shared_ptr<IMapProvider>& provider) const
{
    assert(resourcesRef != nullptr);

    const auto& bindings = _bindings[static_cast<int>(resourcesRef->type)];
    for(auto itBinding = bindings.collectionsToProviders.cbegin(); itBinding != bindings.collectionsToProviders.cend(); ++itBinding)
    {
        if(itBinding.key().get() != resourcesRef)
            continue;

        if(!static_cast<bool>(itBinding.value()))
            return false;

        provider = itBinding.value();
        return true;
    }

    return false;
}

bool OsmAnd::MapRendererResources::isDataSourceAvailableFor(const std::shared_ptr<TiledResourcesCollection>& collection) const
{
    const auto& binding = _bindings[static_cast<int>(collection->type)];

    return binding.collectionsToProviders.contains(collection);
}

void OsmAnd::MapRendererResources::notifyNewResourceAvailable()
{
    renderer->invalidateFrame();
}

void OsmAnd::MapRendererResources::workerThreadProcedure()
{
    // Capture worker thread ID
    _workerThreadId = QThread::currentThreadId();

    while(_workerThreadIsAlive)
    {
        // Local copy of active zone
        QSet<TileId> activeTiles;
        ZoomLevel activeZoom;

        // Wait until we're unblocked by host
        {
            _workerThreadWakeupMutex.lock();
            _workerThreadWakeup.wait(&_workerThreadWakeupMutex);

            // Copy active zone to local copy
            activeTiles = _activeTiles;
            activeZoom = _activeZoom;

            _workerThreadWakeupMutex.unlock();
        }
        if(!_workerThreadIsAlive)
            break;

        // Update resources
        updateResources(activeTiles, activeZoom);
    }

    _workerThreadId = nullptr;
}

void OsmAnd::MapRendererResources::requestNeededResources(const QSet<TileId>& tiles, const ZoomLevel zoom)
{
    // Request missing resources
    for(auto itTileId = tiles.cbegin(); itTileId != tiles.cend(); ++itTileId)
    {
        const auto& tileId = *itTileId;

        for(auto itResourcesCollections = _storage.cbegin(); itResourcesCollections != _storage.cend(); ++itResourcesCollections)
        {
            const auto& resourcesCollections = *itResourcesCollections;

            for(auto itResourcesCollection = resourcesCollections.cbegin(); itResourcesCollection != resourcesCollections.cend(); ++itResourcesCollection)
            {
                const auto& resourcesCollection = *itResourcesCollection;

                // Skip empty entries
                if(!static_cast<bool>(resourcesCollection))
                    continue;

                // Skip resource types that do not have an available data source
                if(!isDataSourceAvailableFor(resourcesCollection))
                    continue;

                // Obtain a resource entry and if it's state is "Unknown", create a task that will
                // request resource data
                std::shared_ptr<BaseTiledResource> resource;
                const auto resourceType = resourcesCollection->type;
                resourcesCollection->obtainOrAllocateEntry(resource, tileId, zoom,
                    [this, resourceType](const TilesCollection<BaseTiledResource>& collection, const TileId tileId, const ZoomLevel zoom) -> BaseTiledResource*
                {
                    if(resourceType == ResourceType::ElevationData || resourceType == ResourceType::RasterMap)
                        return new MapTileResource(this, resourceType, collection, tileId, zoom);
                    else if(resourceType == ResourceType::Symbols)
                        return new SymbolsTileResource(this, collection, tileId, zoom);
                    else
                        return nullptr;
                });

                // Only if tile entry has "Unknown" state proceed to "Requesting" state
                if(!resource->setStateIf(ResourceState::Unknown, ResourceState::Requesting))
                    continue;

                // Create async-task that will obtain needed resource data
                const auto executeProc = [this](Concurrent::Task* task_, QEventLoop& eventLoop)
                {
                    const auto task = static_cast<ResourceRequestTask*>(task_);
                    const auto resource = std::static_pointer_cast<BaseTiledResource>(task->requestedResource);

                    // Only if resource entry has "Requested" state proceed to "ProcessingRequest" state
                    if(!resource->setStateIf(ResourceState::Requested, ResourceState::ProcessingRequest))
                    {
                        // This actually can happen in following situation(s):
                        //   - if request task was canceled before has started it's execution,
                        //     since in that case a state change "Requested => JustBeforeDeath" must have happend.
                        //     In this case entry will be removed in post-execute handler.
                        assert(resource->getState() == ResourceState::JustBeforeDeath);
                        return;
                    }

                    // Ask resource to obtain it's data
                    bool dataAvailable = false;
                    const auto requestSucceeded = resource->obtainData(dataAvailable);

                    // If failed to obtain resource data, remove resource entry to repeat try later
                    if(!requestSucceeded)
                    {
                        // It's safe to simply remove entry, since it's not yet uploaded
                        if(const auto link = resource->link.lock())
                            link->collection.removeEntry(resource->tileId, resource->zoom);
                        return;
                    }

                    // Finalize execution of task
                    if(!resource->setStateIf(ResourceState::ProcessingRequest, dataAvailable ? ResourceState::Ready : ResourceState::Unavailable))
                    {
                        assert(resource->getState() == ResourceState::JustBeforeDeath);

                        // While request was processed, state may have changed to "JustBeforeDeath"
                        // to indicate that this request was sort of "canceled"
                        task->requestCancellation();
                        return;
                    }
                    resource->_requestTask = nullptr;

                    // There is data to upload to GPU, request uploading. Or just ask to show that resource is unavailable
                    if(dataAvailable)
                        requestResourcesUpload();
                    else
                        notifyNewResourceAvailable();
                };
                const auto postExecuteProc = [this](Concurrent::Task* task_, bool wasCancelled)
                {
                    const auto task = static_cast<const ResourceRequestTask*>(task_);
                    const auto resource = std::static_pointer_cast<BaseTiledResource>(task->requestedResource);

                    if(wasCancelled)
                    {
                        // If request task was canceled, if could have happened:
                        //  - before it has started it's execution.
                        //    In this case, state has to be "Requested", and it won't change. So just remove it
                        //  - during it's execution.
                        //    In this case, this handler will be called after execution was finished,
                        //    and state _should_ be "Ready" or "Unavailable", but in general it can be any:
                        //    Uploading, Uploaded, Unloading, Unloaded. In case state is "Ready" or "Unavailable",
                        //    change it to "JustBeforeDeath" and delete it.

                        if(
                            resource->setStateIf(ResourceState::Requested, ResourceState::JustBeforeDeath) ||
                            resource->setStateIf(ResourceState::Ready, ResourceState::JustBeforeDeath) ||
                            resource->setStateIf(ResourceState::Unavailable, ResourceState::JustBeforeDeath))
                        {
                            if(const auto link = resource->link.lock())
                                link->collection.removeEntry(resource->tileId, resource->zoom);
                        }

                        // All other cases must be handled in other places, since here there is no access to GPU
                    }
                };
                const auto asyncTask = new ResourceRequestTask(resource, _taskHostBridge, executeProc, nullptr, postExecuteProc);

                // Register tile as requested
                resource->_requestTask = asyncTask;
                assert(resource->getState() == ResourceState::Requesting);
                resource->setState(ResourceState::Requested);

                // Finally start the request
                _resourcesRequestWorkersPool.start(asyncTask);
            }
        }
    }
}

void OsmAnd::MapRendererResources::invalidateResourcesOfType(const ResourceType type)
{
    _invalidatedResourcesTypesMask |= 1u << static_cast<int>(type);
}

void OsmAnd::MapRendererResources::validateResources()
{
    uint32_t typeIndex = 0;
    while(_invalidatedResourcesTypesMask)
    {
        if(_invalidatedResourcesTypesMask & 0x1)
            validateResourcesOfType(static_cast<ResourceType>(typeIndex));

        typeIndex++;
        _invalidatedResourcesTypesMask >>= 1;
    }
}

void OsmAnd::MapRendererResources::validateResourcesOfType(const ResourceType type)
{
    const auto& resourcesCollections = _storage[static_cast<int>(type)];
    const auto& bindings = _bindings[static_cast<int>(type)];

    // Notify owner
    renderer->onValidateResourcesOfType(type);

    for(auto itResourcesCollection = resourcesCollections.cbegin(); itResourcesCollection != resourcesCollections.cend(); ++itResourcesCollection)
    {
        if(!bindings.collectionsToProviders.contains(*itResourcesCollection))
            continue;

        releaseResourcesFrom(*itResourcesCollection);
    }
}

void OsmAnd::MapRendererResources::updateResources(const QSet<TileId>& tiles, const ZoomLevel zoom)
{
    // Before requesting missing tiled resources, clean up cache to free some space
    cleanupJunkResources(tiles, zoom);

    // In the end of rendering processing, request tiled resources that are neither
    // present in requested list, nor in pending, nor in uploaded
    requestNeededResources(tiles, zoom);
}

unsigned int OsmAnd::MapRendererResources::unloadResources()
{
    unsigned int totalUnloaded = 0u;

    for(auto itResourcesCollections = _storage.cbegin(); itResourcesCollections != _storage.cend(); ++itResourcesCollections)
    {
        const auto& resourcesCollections = *itResourcesCollections;

        for(auto itResourcesCollection = resourcesCollections.cbegin(); itResourcesCollection != resourcesCollections.cend(); ++itResourcesCollection)
        {
            const auto& resourcesCollection = *itResourcesCollection;

            // Skip empty entries
            if(!static_cast<bool>(resourcesCollection))
                continue;

            // Select all resources with "UnloadPending" state
            QList< std::shared_ptr<BaseTiledResource> > resources;
            resourcesCollection->obtainEntries(&resources, [&resources](const std::shared_ptr<BaseTiledResource>& entry, bool& cancel) -> bool
            {
                // Skip not-"UnloadPending" resources
                if(entry->getState() != ResourceState::UnloadPending)
                    return false;

                // Accept this resource
                return true;
            });
            if(resources.isEmpty())
                continue;

            // Unload from GPU all selected resources
            for(auto itResource = resources.cbegin(); itResource != resources.cend(); ++itResource)
            {
                const auto& resource = *itResource;

                // Since state change is allowed (it's not changed to "Unloading" during query), check state here
                if(!resource->setStateIf(ResourceState::UnloadPending, ResourceState::Unloading))
                    continue;

                // Unload from GPU
                resource->unloadFromGPU();

                // Don't wait until GPU will execute unloading, since this resource won't be used anymore and will eventually be deleted

                // Mark as unloaded
                assert(resource->getState() == ResourceState::Unloading);
                resource->setState(ResourceState::Unloaded);

                // Count uploaded resources
                totalUnloaded++;
            }
        }
    }

    return totalUnloaded;
}

unsigned int OsmAnd::MapRendererResources::uploadResources(const unsigned int limit /*= 0u*/, bool* const outMoreThanLimitAvailable /*= nullptr*/)
{
    unsigned int totalUploaded = 0u;
    bool moreThanLimitAvailable = false;
    bool atLeastOneUploadFailed = false;

    for(auto itResourcesCollections = _storage.cbegin(); itResourcesCollections != _storage.cend(); ++itResourcesCollections)
    {
        const auto& resourcesCollections = *itResourcesCollections;

        for(auto itResourcesCollection = resourcesCollections.cbegin(); itResourcesCollection != resourcesCollections.cend(); ++itResourcesCollection)
        {
            const auto& resourcesCollection = *itResourcesCollection;

            // Skip empty entries
            if(!static_cast<bool>(resourcesCollection))
                continue;

            // Select all resources with "Ready" state
            QList< std::shared_ptr<BaseTiledResource> > resources;
            resourcesCollection->obtainEntries(&resources, [&resources, limit, totalUploaded, &moreThanLimitAvailable](const std::shared_ptr<BaseTiledResource>& entry, bool& cancel) -> bool
            {
                // Skip not-ready resources
                if(entry->getState() != ResourceState::Ready)
                    return false;

                // Check if limit was reached
                if(limit > 0 && (totalUploaded + resources.size()) > limit)
                {
                    // Tell that more resources are available for upload
                    moreThanLimitAvailable = true;

                    cancel = true;
                    return false;
                }

                // Accept this resource
                return true;
            });
            if(resources.isEmpty())
                continue;

            // Upload to GPU all selected resources
            for(auto itResource = resources.cbegin(); itResource != resources.cend(); ++itResource)
            {
                const auto& resource = *itResource;

                // Since state change is allowed (it's not changed to "Uploading" during query), check state here
                if(!resource->setStateIf(ResourceState::Ready, ResourceState::Uploading))
                    continue;

                // Actually upload resource to GPU
                const auto didUpload = resource->uploadToGPU();
                if(!atLeastOneUploadFailed && !didUpload)
                    atLeastOneUploadFailed = true;
                if(!didUpload)
                {
                    LogPrintf(LogSeverityLevel::Error, "Failed to upload tiled resource for %dx%d@%d to GPU", resource->tileId.x, resource->tileId.y, resource->zoom);
                    continue;
                }

                // Before marking as uploaded, if uploading is done from GPU worker thread,
                // wait until operation completes
                if(renderer->setupOptions.gpuWorkerThread.enabled)
                    renderer->gpuAPI->waitUntilUploadIsComplete();

                // Mark as uploaded
                assert(resource->getState() == ResourceState::Uploading);
                resource->setState(ResourceState::Uploaded);

                // Count uploaded resources
                totalUploaded++;
            }
        }
    }

    // If any resource failed to upload, report that more ready resources are available
    if(atLeastOneUploadFailed)
        moreThanLimitAvailable = true;

    if(outMoreThanLimitAvailable)
        *outMoreThanLimitAvailable = moreThanLimitAvailable;
    return totalUploaded;
}

void OsmAnd::MapRendererResources::cleanupJunkResources(const QSet<TileId>& tiles, const ZoomLevel zoom)
{
    // Use aggressive cache cleaning: remove all tiled resources that are not needed
    for(auto itResourcesCollections = _storage.cbegin(); itResourcesCollections != _storage.cend(); ++itResourcesCollections)
    {
        const auto& resourcesCollections = *itResourcesCollections;

        for(auto itResourcesCollection = resourcesCollections.cbegin(); itResourcesCollection != resourcesCollections.cend(); ++itResourcesCollection)
        {
            const auto& resourcesCollection = *itResourcesCollection;

            // Skip empty entries
            if(!static_cast<bool>(resourcesCollection))
                continue;

            const auto dataSourceAvailable = isDataSourceAvailableFor(resourcesCollection);

            resourcesCollection->removeEntries([this, dataSourceAvailable, tiles, zoom](const std::shared_ptr<BaseTiledResource>& entry, bool& cancel) -> bool
            {
                // Skip cleaning if this tiled resource is needed
                if((tiles.contains(entry->tileId) && entry->zoom == zoom) && dataSourceAvailable)
                    return false;

                if(entry->setStateIf(ResourceState::Uploaded, ResourceState::UnloadPending))
                {
                    // If resource is not needed anymore, change its state to "UnloadPending",
                    // but keep the resource entry, since it must be unload from GPU in another place
                    return false;
                }
                else if(entry->setStateIf(ResourceState::Ready, ResourceState::JustBeforeDeath))
                {
                    // If resource was not yet uploaded, just remove it.
                    return true;
                }
                else if(entry->setStateIf(ResourceState::ProcessingRequest, ResourceState::JustBeforeDeath))
                {
                    // If resource request is being processed, remove the entry.
                    return true;
                }
                else if(entry->setStateIf(ResourceState::Requested, ResourceState::JustBeforeDeath))
                {
                    // If resource was just requested, cancel its task and remove the entry.

                    // Cancel the task
                    assert(entry->_requestTask != nullptr);
                    entry->_requestTask->requestCancellation();

                    return true;
                }
                else if(entry->setStateIf(ResourceState::Unloaded, ResourceState::JustBeforeDeath))
                {
                    // If resource was unloaded from GPU, remove the entry.
                    return true;
                }
                else if(entry->setStateIf(ResourceState::Unavailable, ResourceState::JustBeforeDeath))
                {
                    // If resource was never available, just remove the entry
                    return true;
                }

                // Following situations are ignored
                const auto state = entry->getState();
                if(state == ResourceState::Uploading || state == ResourceState::UnloadPending || state == ResourceState::Unloading)
                    return false;
                
                assert(false);
                return false;
            });
        }
    }
}

void OsmAnd::MapRendererResources::releaseResourcesFrom(const std::shared_ptr<TiledResourcesCollection>& collection)
{
    // Remove all tiles, releasing associated GPU resources
    collection->removeEntries([](const std::shared_ptr<BaseTiledResource>& entry, bool& cancel) -> bool
    {
        // When resources are released, it's a termination process, so all entries have to be removed.
        // This limits set of possible states of the entries to:
        // - Requested
        // - ProcessingRequest
        // - Ready
        // - Unloaded
        // - Uploaded
        // - Unavailable
        // All other states are considered invalid.

        if(entry->setStateIf(ResourceState::Requested, ResourceState::JustBeforeDeath))
        {
            // Cancel the task
            assert(entry->_requestTask != nullptr);
            entry->_requestTask->requestCancellation();

            return true;
        }
        else if(entry->setStateIf(ResourceState::ProcessingRequest, ResourceState::JustBeforeDeath))
        {
            return true;
        }
        else if(entry->setStateIf(ResourceState::Ready, ResourceState::JustBeforeDeath))
        {
            return true;
        }
        else if(entry->setStateIf(ResourceState::Unloaded, ResourceState::JustBeforeDeath))
        {
            return true;
        }
        else if(entry->setStateIf(ResourceState::Uploaded, ResourceState::UnloadPending))
        {
            entry->setState(ResourceState::Unloading);

            entry->unloadFromGPU();

            entry->setState(ResourceState::Unloaded);
            entry->setState(ResourceState::JustBeforeDeath);
            return true;
        }
        else if(entry->setStateIf(ResourceState::Unavailable, ResourceState::JustBeforeDeath))
        {
            return true;
        }

        //NOTE: this may happen when (and indeed, most of these situations are impossible to handle here. Probably an entity should be added that will store floating resource collections!):
        // - 'requesting' state: task is only being initialized, not much can be done in this case
        // - 'uploading'
        const auto state = entry->getState();
        assert(false);
        return false;
    });
}

void OsmAnd::MapRendererResources::requestResourcesUpload()
{
    // This is obsolete, should be redesigned
    renderer->requestResourcesUpload();
}

void OsmAnd::MapRendererResources::syncResourcesInGPU(
    const unsigned int limitUploads /*= 0u*/,
    bool* const outMoreUploadsThanLimitAvailable /*= nullptr*/,
    unsigned int* const outResourcesUploaded /*= nullptr*/,
    unsigned int* const outResourcesUnloaded /*= nullptr*/)
{
    // Unload resources
    const auto resourcesUnloaded = unloadResources();
    if(outResourcesUnloaded)
        *outResourcesUnloaded = resourcesUnloaded;

    // Upload resources
    const auto resourcesUploaded = uploadResources(limitUploads, outMoreUploadsThanLimitAvailable);
    if(outResourcesUploaded)
        *outResourcesUploaded = resourcesUploaded;
}

std::shared_ptr<const OsmAnd::MapRendererResources::TiledResourcesCollection> OsmAnd::MapRendererResources::getCollection(const ResourceType type, const std::shared_ptr<IMapProvider>& provider) const
{
    return _bindings[static_cast<int>(type)].providersToCollections[provider];
}

QMutex& OsmAnd::MapRendererResources::getSymbolsMapMutex() const
{
    return _symbolsMapMutex;
}

const OsmAnd::MapRendererResources::SymbolsMap& OsmAnd::MapRendererResources::getSymbolsMap() const
{
    return _symbolsMap;
}

unsigned int OsmAnd::MapRendererResources::getSymbolsCount() const
{
    return _symbolsMapCount;
}

OsmAnd::MapRendererResources::GenericResource::GenericResource(MapRendererResources* owner_, const ResourceType type_)
    : _requestTask(nullptr)
    , owner(owner_)
    , type(type_)
{
}

OsmAnd::MapRendererResources::GenericResource::~GenericResource()
{
}

OsmAnd::MapRendererResources::BaseTiledResource::BaseTiledResource(MapRendererResources* owner, const ResourceType type, const TilesCollection<BaseTiledResource>& collection, const TileId tileId, const ZoomLevel zoom)
    : GenericResource(owner, type)
    , TilesCollectionEntryWithState(collection, tileId, zoom)
{
}

OsmAnd::MapRendererResources::BaseTiledResource::~BaseTiledResource()
{
    const volatile auto state = getState();
    if(state == ResourceState::Uploading || state == ResourceState::Uploaded || state == ResourceState::IsBeingUsed || state == ResourceState::Unloading)
        LogPrintf(LogSeverityLevel::Error, "Tiled resource for %dx%d@%d still resides in GPU memory. This may cause GPU memory leak", tileId.x, tileId.y, zoom);
}

OsmAnd::MapRendererResources::TiledResourcesCollection::TiledResourcesCollection(const ResourceType& type_)
    : type(type_)
{
}

OsmAnd::MapRendererResources::TiledResourcesCollection::~TiledResourcesCollection()
{
    verifyNoUploadedTilesPresent();
}

void OsmAnd::MapRendererResources::TiledResourcesCollection::removeAllEntries()
{
    verifyNoUploadedTilesPresent();

    TilesCollection::removeAllEntries();
}

void OsmAnd::MapRendererResources::TiledResourcesCollection::verifyNoUploadedTilesPresent() const
{
    // Ensure that no tiles have "Uploaded" state
    bool stillUploadedTilesPresent = false;
    obtainEntries(nullptr, [&stillUploadedTilesPresent](const std::shared_ptr<BaseTiledResource>& tileEntry, bool& cancel) -> bool
    {
        const auto state = tileEntry->getState();
        if(state == ResourceState::Uploading || state == ResourceState::Uploaded || state == ResourceState::IsBeingUsed || state == ResourceState::Unloading)
        {
            stillUploadedTilesPresent = true;
            cancel = true;
            return false;
        }

        return false;
    });
    if(stillUploadedTilesPresent)
        LogPrintf(LogSeverityLevel::Error, "One or more tiled resources still reside in GPU memory. This may cause GPU memory leak");
    assert(stillUploadedTilesPresent == false);
}

OsmAnd::MapRendererResources::MapTileResource::MapTileResource(MapRendererResources* owner, const ResourceType type, const TilesCollection<BaseTiledResource>& collection, const TileId tileId, const ZoomLevel zoom)
    : BaseTiledResource(owner, type, collection, tileId, zoom)
    , resourceInGPU(_resourceInGPU)
{
}

OsmAnd::MapRendererResources::MapTileResource::~MapTileResource()
{
}

bool OsmAnd::MapRendererResources::MapTileResource::obtainData(bool& dataAvailable)
{
    bool ok = false;

    // Get source of tile
    std::shared_ptr<IMapProvider> provider_;
    if(const auto link_ = link.lock())
        ok = owner->obtainProviderFor(static_cast<TiledResourcesCollection*>(&link_->collection), provider_);
    if(!ok)
        return false;
    const auto provider = std::static_pointer_cast<IMapTileProvider>(provider_);

    // Obtain tile from provider
    std::shared_ptr<const MapTile> tile;
    const auto requestSucceeded = provider->obtainTile(tileId, zoom, tile);
    if(!requestSucceeded)
        return false;

    // Store data
    _sourceData = tile;
    dataAvailable = static_cast<bool>(tile);

    return true;
}

bool OsmAnd::MapRendererResources::MapTileResource::uploadToGPU()
{
    bool ok = owner->uploadTileToGPU(_sourceData, _resourceInGPU);
    if(!ok)
        return false;

    // Release source data:
    if(const auto retainedSource = std::dynamic_pointer_cast<const IRetainableResource>(_sourceData))
    {
        // If map tile implements 'Retained' interface, it must be kept, but 
        std::const_pointer_cast<IRetainableResource>(retainedSource)->releaseNonRetainedData();
    }
    else
    {
        // or simply release entire tile
        _sourceData.reset();
    }

    return true;
}

void OsmAnd::MapRendererResources::MapTileResource::unloadFromGPU()
{
    assert(_resourceInGPU.use_count() == 1);
    _resourceInGPU.reset();
}

OsmAnd::MapRendererResources::SymbolsResourcesCollection::SymbolsResourcesCollection()
    : TiledResourcesCollection(ResourceType::Symbols)
{
}

OsmAnd::MapRendererResources::SymbolsResourcesCollection::~SymbolsResourcesCollection()
{
}

OsmAnd::MapRendererResources::SymbolsTileResource::SymbolsTileResource(MapRendererResources* owner, const TilesCollection<BaseTiledResource>& collection, const TileId tileId, const ZoomLevel zoom)
    : BaseTiledResource(owner, ResourceType::Symbols, collection, tileId, zoom)
{
}

OsmAnd::MapRendererResources::SymbolsTileResource::~SymbolsTileResource()
{
    // Upon detaching of tile resource, all resources must already be cleared
    assert(_uniqueResourcesInGPU.isEmpty());
    assert(_sharedResourcesInGPU.isEmpty());
}

bool OsmAnd::MapRendererResources::SymbolsTileResource::obtainData(bool& dataAvailable)
{
    // Obtain collection link and maintain it
    const auto link_ = link.lock();
    if(!link_)
        return false;
    const auto collection = static_cast<SymbolsResourcesCollection*>(&link_->collection);

    // Get source of tile
    std::shared_ptr<IMapProvider> provider_;
    bool ok = owner->obtainProviderFor(static_cast<TiledResourcesCollection*>(&link_->collection), provider_);
    if(!ok)
        return false;
    const auto provider = std::static_pointer_cast<IMapSymbolProvider>(provider_);

    auto& dataCacheLevel = collection->_dataCache[zoom];

    // Obtain tile from provider
    const auto tileBBox31 = Utilities::tileBoundingBox31(tileId, zoom);
    std::shared_ptr<const MapSymbolsTile> tile;
    const auto requestSucceeded = provider->obtainSymbols(tileId, zoom, tile,
        [this, provider, &dataCacheLevel, tileBBox31](const std::shared_ptr<const Model::MapObject>& mapObject) -> bool
        {
            // All symbols that come from map object which can not be cached,
            // should be received.
            if(!provider->canSymbolsBeSharedFrom(mapObject))
                return true;

            // Symbols from map object can be shared only in case this map object does not lay inside it's tile bbox completely
            if(tileBBox31.contains(mapObject->bbox31))
                return true;

            // Check if map symbols that come from this map object are already in cache
            {
                QReadLocker scopedLocker(&dataCacheLevel._lock);

                const auto itCacheEntry = dataCacheLevel._cache.constFind(mapObject->id);
                if(itCacheEntry != dataCacheLevel._cache.cend())
                {
                    if(const auto sharedGroup = itCacheEntry->lock())
                    {
                        _sharedSymbolsGroups.push_back(sharedGroup);
                        return false;
                    }
                }
            }

            // By default, accept all
            return true;
        });
    if(!requestSucceeded)
        return false;

    // Store data
    _sourceData = tile;
    dataAvailable = static_cast<bool>(tile);

    // Process data
    if(!dataAvailable)
        return true;

    // tile->symbolsGroups contains only groups that derived from unique symbols
    // (that are currently unique, or can not be shared by their type)
    for(auto itGroup = tile->symbolsGroups.cbegin(); itGroup != tile->symbolsGroups.cend(); ++itGroup)
    {
        const auto& group = *itGroup;

        // All groups that can not be cached should be added to unique
        if(!provider->canSymbolsBeSharedFrom(group->mapObject) || tileBBox31.contains(group->mapObject->bbox31))
        {
            _uniqueSymbolsGroups.push_back(group);
            continue;
        }

        // Check if this group was already cached, and if not - cache it
        {
            QWriteLocker scopedLocker(&dataCacheLevel._lock);

            const auto itSharedGroup = dataCacheLevel._cache.find(group->mapObject->id);
            if(itSharedGroup != dataCacheLevel._cache.end())
            {
                if(const auto sharedMapObject = itSharedGroup->lock())
                {
                    // If entry already exits, use that object instead of this one
                    _sharedSymbolsGroups.push_back(sharedMapObject);
                }
                else
                {
                    // Or replace with current one
                    *itSharedGroup = group;
                    _sharedSymbolsGroups.push_back(group);
                }
            }
            else
            {
                dataCacheLevel._cache.insert(group->mapObject->id, group);
                _sharedSymbolsGroups.push_back(group);
            }
        }
    }

    // Release source data:
    if(const auto retainedSource = std::dynamic_pointer_cast<const IRetainableResource>(_sourceData))
    {
        // If map tile implements 'Retained' interface, it must be kept, but 
        std::const_pointer_cast<IRetainableResource>(retainedSource)->releaseNonRetainedData();
    }
    else
    {
        // or simply release entire tile
        _sourceData.reset();
    }

    return true;
}

bool OsmAnd::MapRendererResources::SymbolsTileResource::uploadToGPU()
{
    bool ok;
    bool atLeastOneFailed = false;

    // Obtain collection link and maintain it
    const auto link_ = link.lock();
    const auto collection = static_cast<SymbolsResourcesCollection*>(&link_->collection);

    typedef std::pair< std::shared_ptr<const MapSymbol>, std::shared_ptr<const GPUAPI::ResourceInGPU> > UploadedSymbolEntry;
    QList< UploadedSymbolEntry > uploadedSymbols;

    // Upload all unique symbols to GPU
    for(auto itGroup = _uniqueSymbolsGroups.cbegin(); itGroup != _uniqueSymbolsGroups.cend(); ++itGroup)
    {
        const auto& group = *itGroup;

        for(auto itSymbol = group->symbols.cbegin(); itSymbol != group->symbols.cend(); ++itSymbol)
        {
            const auto& symbol = *itSymbol;

            // Prepare data and upload to GPU
            std::shared_ptr<const GPUAPI::ResourceInGPU> resourceInGPU;
            ok = owner->uploadSymbolToGPU(symbol, resourceInGPU);

            // If upload have failed, stop
            if(!ok)
            {
                LogPrintf(LogSeverityLevel::Error, "Failed to upload unique symbol (size %dx%d) in %dx%d@%d tile",
                    symbol->bitmap->width(), symbol->bitmap->height(),
                    tileId.x, tileId.y, zoom);

                atLeastOneFailed = true;
                break;
            }

            // Mark this symbol as uploaded
            uploadedSymbols.push_back(qMove(UploadedSymbolEntry(symbol, resourceInGPU)));

            // Save reference to GPU resource
            _uniqueResourcesInGPU.insert(symbol, qMove(resourceInGPU));
        }

        // If at least one symbol have failed, interrupt
        if(atLeastOneFailed)
            break;
    }
    
    // Upload all shared symbols that are missing, or use from cache
    {
        QMutexLocker scopedLocker(&collection->_gpuResourcesCacheMutex);

        for(auto itGroup = _sharedSymbolsGroups.cbegin(); itGroup != _sharedSymbolsGroups.cend(); ++itGroup)
        {
            const auto& group = *itGroup;

            for(auto itSymbol = group->symbols.cbegin(); itSymbol != group->symbols.cend(); ++itSymbol)
            {
                const auto& symbol = *itSymbol;

                // Check if this symbol was already uploaded to GPU and is present in cache
                std::shared_ptr<const GPUAPI::ResourceInGPU> sharedResourceInGPU;
                auto itSharedResourceInGPU = collection->_gpuResourcesCache.find(symbol);
                if(itSharedResourceInGPU == collection->_gpuResourcesCache.end())
                {
                    // Prepare data and upload to GPU
                    ok = owner->uploadSymbolToGPU(symbol, sharedResourceInGPU);

                    // If upload have failed, stop
                    if(!ok)
                    {
                        LogPrintf(LogSeverityLevel::Error, "Failed to upload shared symbol (size %dx%d) in %dx%d@%d tile",
                            symbol->bitmap->width(), symbol->bitmap->height(),
                            tileId.x, tileId.y, zoom);

                        atLeastOneFailed = true;
                        break;
                    }

                    // Mark this symbol as uploaded
                    uploadedSymbols.push_back(qMove(UploadedSymbolEntry(symbol, sharedResourceInGPU)));

                    // Insert new resource in shared GPU resources cache
                    itSharedResourceInGPU = collection->_gpuResourcesCache.insert(symbol, sharedResourceInGPU);
                }
                else
                {
                    // There should be no expired resources
                    assert(!itSharedResourceInGPU->expired());

                    // Use shared GPU resource
                    sharedResourceInGPU = itSharedResourceInGPU->lock();
                }

                // Maintain reference to shared resource in GPU
                _sharedResourcesInGPU.insert(symbol, sharedResourceInGPU);
            }

            // If at least one symbol have failed, interrupt
            if(atLeastOneFailed)
                break;
        }
    }
    
    // If at least one symbol failed to upload, entire tile failed to upload, so unload it
    if(atLeastOneFailed)
    {
        // Clear all references
        uploadedSymbols.clear();

        // Unload inconsistent data from GPU
        unloadFromGPU();

        return false;
    }

    // Process uploaded symbols
    for(auto itSymbolEntry = uploadedSymbols.begin(); itSymbolEntry != uploadedSymbols.end(); ++itSymbolEntry)
    {
        // Unload source data from symbol
        std::const_pointer_cast<MapSymbol>(itSymbolEntry->first)->releaseNonRetainedData();

        // Publish symbol to global map
        {
            QMutexLocker scopedLocker(&owner->_symbolsMapMutex);
            owner->_symbolsMap[itSymbolEntry->first->order].insert(itSymbolEntry->first, itSymbolEntry->second);

            owner->_symbolsMapCount++;
        }
    }

    return true;
}

void OsmAnd::MapRendererResources::SymbolsTileResource::unloadFromGPU()
{
    // Obtain collection link and maintain it
    const auto link_ = link.lock();
    const auto collection = static_cast<SymbolsResourcesCollection*>(&link_->collection);

    // Unload unique symbols
    for(auto itResourceInGPU = _uniqueResourcesInGPU.begin(); itResourceInGPU != _uniqueResourcesInGPU.end(); ++itResourceInGPU)
    {
        const auto& symbol = itResourceInGPU.key();
        auto& resourceInGPU = itResourceInGPU.value();

        // Remove symbol from global map
        {
            QMutexLocker scopedLocker(&owner->_symbolsMapMutex);

            const auto itSymbolsLayer = owner->_symbolsMap.find(symbol->order);
            const auto removedCount = itSymbolsLayer->remove(symbol);
            if(itSymbolsLayer->isEmpty())
                owner->_symbolsMap.erase(itSymbolsLayer);

            owner->_symbolsMapCount -= removedCount;
        }

        // Unload symbol from GPU
        assert(resourceInGPU.use_count() == 1);
        resourceInGPU.reset();
    }
    _uniqueResourcesInGPU.clear();

    // Dereference all unique symbols groups
    _uniqueSymbolsGroups.clear();

    // Unload/dereference shared symbols
    for(auto itResourceInGPU = _sharedResourcesInGPU.begin(); itResourceInGPU != _sharedResourcesInGPU.end(); ++itResourceInGPU)
    {
        const auto& symbol = itResourceInGPU.key();
        auto& resourceInGPU = itResourceInGPU.value();

        // Check if this is the last instance
        if(resourceInGPU.unique())
        {
            // Since it's the last reference, remove from cache
            {
                QMutexLocker scopedLocker(&collection->_gpuResourcesCacheMutex);
                collection->_gpuResourcesCache.remove(itResourceInGPU.key());
            }

            // And also remove from global map
            {
                QMutexLocker scopedLocker(&owner->_symbolsMapMutex);

                const auto itSymbolsLayer = owner->_symbolsMap.find(symbol->order);
                const auto removedCount = itSymbolsLayer->remove(symbol);
                if(itSymbolsLayer->isEmpty())
                    owner->_symbolsMap.erase(itSymbolsLayer);

                owner->_symbolsMapCount -= removedCount;
            }
        }

        // Dereference resource
        resourceInGPU.reset();
    }
    _sharedResourcesInGPU.clear();

    // Dereference all shared symbols groups
    auto& dataCacheLevel = collection->_dataCache[zoom];
    for(auto itGroup = _sharedSymbolsGroups.begin(); itGroup != _sharedSymbolsGroups.end(); ++itGroup)
    {
        auto& group = *itGroup;

        // Check if this is the last instance
        if(group.unique())
        {
            // Since it's the last reference, remove from cache
            {
                QWriteLocker scopedLocker(&dataCacheLevel._lock);
                dataCacheLevel._cache.remove(group->mapObject->id);
            }
        }

        // Dereference group
        group.reset();
    }
    _sharedSymbolsGroups.clear();
}

OsmAnd::MapRendererResources::ResourceRequestTask::ResourceRequestTask(
    const std::shared_ptr<IResource>& requestedResource_,
    const Concurrent::TaskHost::Bridge& bridge, ExecuteSignature executeMethod, PreExecuteSignature preExecuteMethod /*= nullptr*/, PostExecuteSignature postExecuteMethod /*= nullptr*/)
    : HostedTask(bridge, executeMethod, preExecuteMethod, postExecuteMethod)
    , requestedResource(requestedResource_)
{
}

OsmAnd::MapRendererResources::ResourceRequestTask::~ResourceRequestTask()
{
}
