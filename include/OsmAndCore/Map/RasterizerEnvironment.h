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

#ifndef _OSMAND_CORE_RASTERIZER_ENVIRONMENT_H_
#define _OSMAND_CORE_RASTERIZER_ENVIRONMENT_H_

#include <OsmAndCore/stdlib_common.h>

#include <OsmAndCore/QtExtensions.h>
#include <QMap>

#include <OsmAndCore.h>
#include <OsmAndCore/CommonTypes.h>
#include <OsmAndCore/IExternalResourcesProvider.h>
#include <OsmAndCore/Map/MapStyle.h>

namespace OsmAnd
{
    class MapStyleValueDefinition;
    class Rasterizer;
    struct MapStyleValue;
    class ObfMapSectionInfo;

    class RasterizerEnvironment_P;
    class OSMAND_CORE_API RasterizerEnvironment
    {
        Q_DISABLE_COPY(RasterizerEnvironment);
    private:
        const std::unique_ptr<RasterizerEnvironment_P> _d;
    protected:
    public:
        RasterizerEnvironment(const std::shared_ptr<const MapStyle>& style, const float displayDensityFactor, const std::shared_ptr<const IExternalResourcesProvider>& externalResourcesProvider = nullptr);
        virtual ~RasterizerEnvironment();

        const std::shared_ptr<const MapStyle> style;
        const float displayDensityFactor;
        const std::shared_ptr<const IExternalResourcesProvider> externalResourcesProvider;

        const std::shared_ptr<const ObfMapSectionInfo>& dummyMapSection;

        QHash< std::shared_ptr<const MapStyleValueDefinition>, MapStyleValue > getSettings() const;
        void setSettings(const QHash< std::shared_ptr<const MapStyleValueDefinition>, MapStyleValue >& newSettings);

    friend class OsmAnd::Rasterizer;
    };

} // namespace OsmAnd

#endif // _OSMAND_CORE_RASTERIZER_ENVIRONMENT_H_
