#include "CDBTo3DTiles.h"
#include "CDB.h"
#include "Gltf.h"
#include "MathHelpers.h"
#include "TileFormatIO.h"
#include "cpl_conv.h"
#include "gdal.h"
#include "osgDB/WriteFile"
#include <cmath>
#include <limits>
#include <morton.h>
#include <nlohmann/json.hpp>
#include <unordered_map>
#include <unordered_set>
using json = nlohmann::json;

namespace CDBTo3DTiles {

const std::string MATERIALS_SCHEMA_NAME = "materials.json";

Converter::Converter(const std::filesystem::path &CDBPath, const std::filesystem::path &outputPath)
{
    m_impl = std::make_unique<CDBTilesetBuilder>(CDBPath, outputPath);
}

Converter::~Converter() noexcept {}

void Converter::combineDataset(const std::vector<std::string> &datasets)
{
    // Only combine when we have more than 1 tileset. Less than that, it means
    // the tileset doesn't exist (no action needed here) or
    // it is already combined from different geocell by default
    if (datasets.size() == 1) {
        return;
    }

    m_impl->requestedDatasetToCombine.emplace_back(datasets);
    for (const auto &dataset : datasets) {
        auto datasetNamePos = dataset.find("_");
        if (datasetNamePos == std::string::npos) {
            throw std::runtime_error("Wrong format. Required format should be: {DatasetName}_{Component "
                                     "Selector 1}_{Component Selector 2}");
        }

        auto datasetName = dataset.substr(0, datasetNamePos);
        if (m_impl->DATASET_PATHS.find(datasetName) == m_impl->DATASET_PATHS.end()) {
            std::string errorMessage = "Unrecognize dataset: " + datasetName + "\n";
            errorMessage += "Correct dataset names are: \n";
            for (const auto &requiredDataset : m_impl->DATASET_PATHS) {
                errorMessage += requiredDataset + "\n";
            }

            throw std::runtime_error(errorMessage);
        }

        auto CS_1Pos = dataset.find("_", datasetNamePos + 1);
        if (CS_1Pos == std::string::npos) {
            throw std::runtime_error("Wrong format. Required format should be: {DatasetName}_{Component "
                                     "Selector 1}_{Component Selector 2}");
        }

        auto CS_1 = dataset.substr(datasetNamePos + 1, CS_1Pos - datasetNamePos - 1);
        if (CS_1.empty() || !std::all_of(CS_1.begin(), CS_1.end(), ::isdigit)) {
            throw std::runtime_error("Component selector 1 has to be a number");
        }

        auto CS_2 = dataset.substr(CS_1Pos + 1);
        if (CS_2.empty() || !std::all_of(CS_2.begin(), CS_2.end(), ::isdigit)) {
            throw std::runtime_error("Component selector 2 has to be a number");
        }
    }
}

void Converter::setUse3dTilesNext(bool use3dTilesNext)
{
    m_impl->use3dTilesNext = use3dTilesNext;
}

void Converter::setExternalSchema(bool externalSchema)
{
    m_impl->externalSchema = externalSchema;
}

void Converter::setGenerateElevationNormal(bool elevationNormal)
{
    m_impl->elevationNormal = elevationNormal;
}

void Converter::setElevationLODOnly(bool elevationLOD)
{
    m_impl->elevationLOD = elevationLOD;
}

void Converter::setSubtreeLevels(int subtreeLevels)
{
    m_impl->subtreeLevels = subtreeLevels;
}

void Converter::setElevationThresholdIndices(float elevationThresholdIndices)
{
    m_impl->elevationThresholdIndices = elevationThresholdIndices;
}

void Converter::setElevationDecimateError(float elevationDecimateError)
{
    m_impl->elevationDecimateError = elevationDecimateError;
}

void Converter::convert()
{
    CDB cdb(m_impl->cdbPath);
    std::map<std::string, std::vector<std::filesystem::path>> combinedTilesets;
    std::map<std::string, std::vector<Core::BoundingRegion>> combinedTilesetsRegions;
    std::map<std::string, Core::BoundingRegion> aggregateTilesetsRegion;
    std::map<CDBDataset, std::filesystem::path> &datasetDirs = m_impl->datasetDirs;
    m_impl->initializeImplicitTilingParameters();

    std::filesystem::path materialsXMLPath = m_impl->cdbPath / "Metadata" / "Materials.xml";
    if (m_impl->use3dTilesNext) {
        // Parse Materials XML to build CDBBaseMaterials index.
        if (std::filesystem::exists(materialsXMLPath)) {
            m_impl->materials.readBaseMaterialsFile(materialsXMLPath);
        }
    }

    cdb.forEachGeoCell([&](CDBGeoCell geoCell) {
        m_impl->datasetCSSubtrees.clear();
        datasetDirs.clear();

        // create directories for converted GeoCell
        std::filesystem::path geoCellRelativePath = geoCell.getRelativePath();
        std::filesystem::path geoCellAbsolutePath = m_impl->outputPath / geoCellRelativePath;
        std::filesystem::path elevationDir = geoCellAbsolutePath / CDBTilesetBuilder::ELEVATIONS_PATH;
        std::filesystem::path GTModelDir = geoCellAbsolutePath / CDBTilesetBuilder::GTMODEL_PATH;
        std::filesystem::path GSModelDir = geoCellAbsolutePath / CDBTilesetBuilder::GSMODEL_PATH;
        std::filesystem::path roadNetworkDir = geoCellAbsolutePath / CDBTilesetBuilder::ROAD_NETWORK_PATH;
        std::filesystem::path railRoadNetworkDir = geoCellAbsolutePath
                                                   / CDBTilesetBuilder::RAILROAD_NETWORK_PATH;
        std::filesystem::path powerlineNetworkDir = geoCellAbsolutePath
                                                    / CDBTilesetBuilder::POWERLINE_NETWORK_PATH;
        std::filesystem::path hydrographyNetworkDir = geoCellAbsolutePath
                                                      / CDBTilesetBuilder::HYDROGRAPHY_NETWORK_PATH;
        datasetDirs.insert(std::pair<CDBDataset, std::filesystem::path>(CDBDataset::Elevation, elevationDir));
        datasetDirs.insert(std::pair<CDBDataset, std::filesystem::path>(CDBDataset::GSFeature, GSModelDir));
        datasetDirs.insert(
            std::pair<CDBDataset, std::filesystem::path>(CDBDataset::GSModelGeometry, GSModelDir));
        datasetDirs.insert(
            std::pair<CDBDataset, std::filesystem::path>(CDBDataset::GSModelTexture, GSModelDir));
        datasetDirs.insert(std::pair<CDBDataset, std::filesystem::path>(CDBDataset::GTFeature, GTModelDir));
        datasetDirs.insert(
            std::pair<CDBDataset, std::filesystem::path>(CDBDataset::GTModelGeometry_500, GTModelDir));
        datasetDirs.insert(
            std::pair<CDBDataset, std::filesystem::path>(CDBDataset::GTModelTexture, GTModelDir));
        datasetDirs.insert(
            std::pair<CDBDataset, std::filesystem::path>(CDBDataset::RoadNetwork, roadNetworkDir));
        datasetDirs.insert(
            std::pair<CDBDataset, std::filesystem::path>(CDBDataset::RailRoadNetwork, railRoadNetworkDir));
        datasetDirs.insert(
            std::pair<CDBDataset, std::filesystem::path>(CDBDataset::PowerlineNetwork, powerlineNetworkDir));
        datasetDirs.insert(std::pair<CDBDataset, std::filesystem::path>(CDBDataset::HydrographyNetwork,
                                                                        hydrographyNetworkDir));

        // process elevation
        cdb.forEachElevationTile(geoCell, [&](CDBElevation elevation) {
            m_impl->addElevationToTilesetCollection(elevation, cdb, elevationDir);
        });
        m_impl->flushTilesetCollection(geoCell, m_impl->elevationTilesets);
        std::unordered_map<CDBTile, Texture>().swap(m_impl->processedParentImagery);

        // process road network
        cdb.forEachRoadNetworkTile(geoCell, [&](const CDBGeometryVectors &roadNetwork) {
            m_impl->addVectorToTilesetCollection(roadNetwork, roadNetworkDir, m_impl->roadNetworkTilesets);
        });
        m_impl->flushTilesetCollection(geoCell, m_impl->roadNetworkTilesets);

        // process railroad network
        cdb.forEachRailRoadNetworkTile(geoCell, [&](const CDBGeometryVectors &railRoadNetwork) {
            m_impl->addVectorToTilesetCollection(railRoadNetwork,
                                                 railRoadNetworkDir,
                                                 m_impl->railRoadNetworkTilesets);
        });
        m_impl->flushTilesetCollection(geoCell, m_impl->railRoadNetworkTilesets);

        // process powerline network
        cdb.forEachPowerlineNetworkTile(geoCell, [&](const CDBGeometryVectors &powerlineNetwork) {
            m_impl->addVectorToTilesetCollection(powerlineNetwork,
                                                 powerlineNetworkDir,
                                                 m_impl->powerlineNetworkTilesets);
        });
        m_impl->flushTilesetCollection(geoCell, m_impl->powerlineNetworkTilesets);

        // process hydrography network
        cdb.forEachHydrographyNetworkTile(geoCell, [&](const CDBGeometryVectors &hydrographyNetwork) {
            m_impl->addVectorToTilesetCollection(hydrographyNetwork,
                                                 hydrographyNetworkDir,
                                                 m_impl->hydrographyNetworkTilesets);
        });
        m_impl->flushTilesetCollection(geoCell, m_impl->hydrographyNetworkTilesets);

        // process GTModel
        cdb.forEachGTModelTile(geoCell, [&](CDBGTModels GTModel) {
            m_impl->addGTModelToTilesetCollection(GTModel, GTModelDir);
        });
        m_impl->flushTilesetCollection(geoCell, m_impl->GTModelTilesets);
      
        // process GSModel
        cdb.forEachGSModelTile(geoCell, [&](CDBGSModels GSModel) {
            m_impl->addGSModelToTilesetCollection(GSModel, GSModelDir);
        });
        m_impl->flushTilesetCollection(geoCell, m_impl->GSModelTilesets, false);

        m_impl->flushAvailabilitiesAndWriteSubtrees();

        // get the converted dataset in each geocell to be combine at the end
        Core::BoundingRegion geoCellRegion = CDBTile::calcBoundRegion(geoCell, -10, 0, 0);
        for (auto tilesetJsonPath : m_impl->defaultDatasetToCombine) {
            auto componentSelectors = tilesetJsonPath.parent_path().filename().string();
            auto dataset = tilesetJsonPath.parent_path().parent_path().filename().string();
            auto combinedTilesetName = dataset + "_" + componentSelectors;

            combinedTilesets[combinedTilesetName].emplace_back(tilesetJsonPath);
            combinedTilesetsRegions[combinedTilesetName].emplace_back(geoCellRegion);
            auto tilesetAggregateRegion = aggregateTilesetsRegion.find(combinedTilesetName);
            if (tilesetAggregateRegion == aggregateTilesetsRegion.end()) {
                aggregateTilesetsRegion.insert({combinedTilesetName, geoCellRegion});
            } else {
                tilesetAggregateRegion->second = tilesetAggregateRegion->second.computeUnion(geoCellRegion);
            }
        }
        std::vector<std::filesystem::path>().swap(m_impl->defaultDatasetToCombine);
    });

    // combine all the default tileset in each geocell into a global one
    for (auto tileset : combinedTilesets) {
        std::ofstream fs(m_impl->outputPath / (tileset.first + ".json"));
        combineTilesetJson(tileset.second, combinedTilesetsRegions[tileset.first], fs);
    }

    // combine the requested tilesets
    for (const auto &tilesets : m_impl->requestedDatasetToCombine) {
        std::string combinedTilesetName;
        if (m_impl->requestedDatasetToCombine.size() > 1) {
            for (const auto &tileset : tilesets) {
                combinedTilesetName += tileset;
            }
            combinedTilesetName += ".json";
        } else {
            combinedTilesetName = "tileset.json";
        }

        std::vector<std::filesystem::path> existTilesets;
        std::vector<Core::BoundingRegion> regions;
        regions.reserve(tilesets.size());
        for (const auto &tileset : tilesets) {
            auto tilesetRegion = aggregateTilesetsRegion.find(tileset);
            if (tilesetRegion != aggregateTilesetsRegion.end()) {
                existTilesets.emplace_back(tilesetRegion->first + ".json");
                regions.emplace_back(tilesetRegion->second);
            }
        }

        std::ofstream fs(m_impl->outputPath / combinedTilesetName);
        combineTilesetJson(existTilesets, regions, fs);
    }

    if (std::filesystem::exists(materialsXMLPath) && m_impl->externalSchema) {
        std::ofstream schemaFile(m_impl->outputPath / MATERIALS_SCHEMA_NAME);
        schemaFile << m_impl->materials.generateSchema();
    }
}

USE_OSGPLUGIN(png)
USE_OSGPLUGIN(jpeg)
USE_OSGPLUGIN(zip)
USE_OSGPLUGIN(rgb)
USE_OSGPLUGIN(OpenFlight)

GlobalInitializer::GlobalInitializer()
{
    GDALAllRegister();
    CPLSetConfigOption("GDAL_PAM_ENABLED", "NO");
}

GlobalInitializer::~GlobalInitializer() noexcept
{
    osgDB::Registry::instance(true);
}

} // namespace CDBTo3DTiles
