#include "CDBTilesetBuilder.h"
#include "CDB.h"
#include "CDBRMDescriptor.h"
#include "FileUtil.h"
#include "Gltf.h"
#include "Math.h"
#include "TileFormatIO.h"
#include "gdal.h"
#include "osgDB/WriteFile"
#include <morton.h>
#include <nlohmann/json.hpp>
#include <unordered_map>
#include <unordered_set>
using json = nlohmann::json;
using namespace CDBTo3DTiles;

const std::string CDBTilesetBuilder::ELEVATIONS_PATH = "Elevation";
const std::string CDBTilesetBuilder::ROAD_NETWORK_PATH = "RoadNetwork";
const std::string CDBTilesetBuilder::RAILROAD_NETWORK_PATH = "RailRoadNetwork";
const std::string CDBTilesetBuilder::POWERLINE_NETWORK_PATH = "PowerlineNetwork";
const std::string CDBTilesetBuilder::HYDROGRAPHY_NETWORK_PATH = "HydrographyNetwork";
const std::string CDBTilesetBuilder::GTMODEL_PATH = "GTModels";
const std::string CDBTilesetBuilder::GSMODEL_PATH = "GSModels";
const int CDBTilesetBuilder::MAX_LEVEL = 23;

const std::unordered_set<std::string> CDBTilesetBuilder::DATASET_PATHS = {ELEVATIONS_PATH,
                                                                          ROAD_NETWORK_PATH,
                                                                          RAILROAD_NETWORK_PATH,
                                                                          POWERLINE_NETWORK_PATH,
                                                                          HYDROGRAPHY_NETWORK_PATH,
                                                                          GTMODEL_PATH,
                                                                          GSMODEL_PATH};
void CDBTilesetBuilder::flushTilesetCollection(
    const CDBGeoCell &geoCell,
    std::unordered_map<CDBGeoCell, TilesetCollection> &tilesetCollections,
    bool replace)
{
    auto geoCellCollectionIt = tilesetCollections.find(geoCell);
    if (geoCellCollectionIt != tilesetCollections.end()) {
        const auto &tilesetCollection = geoCellCollectionIt->second;
        const auto &CSToPaths = tilesetCollection.CSToPaths;
        for (const auto &CSTotileset : tilesetCollection.CSToTilesets) {
            const auto &tileset = CSTotileset.second;
            auto root = tileset.getRoot();
            if (!root) {
                continue;
            }
            int maxLevel = 0;
            for (int level = 0; level < MAX_LEVEL + 1; level += 1) {
                if (tileset.getFirstTileAtLevel(level))
                    maxLevel = level;
            }

            auto tilesetDirectory = CSToPaths.at(CSTotileset.first);
            auto tilesetJsonPath = tilesetDirectory
                                   / (CDBTile::retrieveGeoCellDatasetFromTileName(*root) + ".json");

            // write to tileset.json file
            std::ofstream fs(tilesetJsonPath);

            writeToTilesetJson(tileset, replace, fs, use3dTilesNext, subtreeLevels, maxLevel, {});

            // add tileset json path to be combined later for multiple geocell
            // remove the output root path to become relative path
            tilesetJsonPath = std::filesystem::relative(tilesetJsonPath, outputPath);
            defaultDatasetToCombine.emplace_back(tilesetJsonPath);
        }

        tilesetCollections.erase(geoCell);
    }
}

void CDBTilesetBuilder::flushAvailabilitiesAndWriteSubtrees()
{
    std::set<std::string> subtreeRoots;

    // write all of the availability buffers and subtree files for each dataset group
    for (auto &[dataset, csTileAndChildAvailabilities] : datasetCSTileAndChildAvailabilities) {
        if (datasetCSSubtrees.count(dataset) == 0) {
            continue;
        }
        for (auto &[CSKey, subtreeMap] : datasetCSSubtrees.at(dataset)) {
            std::map<std::string, SubtreeAvailability> &tileAndChildAvailabilities
                = csTileAndChildAvailabilities.at(CSKey);
            for (auto &[key, subtree] : subtreeMap) {
                subtreeRoots.insert(key);

                bool constantNodeAvailability = (subtree.nodeCount == 0)
                                                || (subtree.nodeCount == subtreeNodeCount);

                if (constantNodeAvailability) {
                    continue;
                }

                std::vector<uint8_t> outputBuffer(nodeAvailabilityByteLengthWithPadding);
                uint8_t *outBuffer = &outputBuffer[0];
                memset(&outBuffer[0], 0, nodeAvailabilityByteLengthWithPadding);
                memcpy(&outBuffer[0], &subtree.nodeBuffer[0], nodeAvailabilityByteLengthWithPadding);
                std::filesystem::path path = datasetDirs.at(dataset) / CSKey / "availability"
                                             / (key + ".bin");
                Utilities::writeBinaryFile(path,
                                           (const char *) &outBuffer[0],
                                           nodeAvailabilityByteLengthWithPadding);
            }

            // write .subtree files for every subtree
            for (std::string subtreeRoot : subtreeRoots) {
                json subtreeJson;

                nlohmann::json buffers = nlohmann::json::array();
                int bufferIndex = 0;
                nlohmann::json bufferViews = nlohmann::json::array();
                SubtreeAvailability tileAndChildAvailability = tileAndChildAvailabilities.at(subtreeRoot);
                tileAndChildAvailability.nodeCount = countSetBitsInVectorOfInts(
                    tileAndChildAvailability.nodeBuffer);
                tileAndChildAvailability.childCount = countSetBitsInVectorOfInts(
                    tileAndChildAvailability.childBuffer);
                bool constantTileAvailability = (tileAndChildAvailability.nodeCount == 0)
                                                || (tileAndChildAvailability.nodeCount == subtreeNodeCount);
                bool constantChildAvailability = (tileAndChildAvailability.childCount == 0)
                                                 || (tileAndChildAvailability.childCount == childSubtreeCount);

                uint64_t nodeBufferLengthToWrite = static_cast<int>(!constantTileAvailability)
                                                   * nodeAvailabilityByteLengthWithPadding;
                uint64_t childBufferLengthToWrite = static_cast<int>(!constantChildAvailability)
                                                    * childSubtreeAvailabilityByteLengthWithPadding;
                long unsigned int bufferByteLength = nodeBufferLengthToWrite + childBufferLengthToWrite;
                if (bufferByteLength != 0) {
                    nlohmann::json byteLength;
                    byteLength["byteLength"] = bufferByteLength;
                    buffers.emplace_back(byteLength);
                    bufferIndex += 1;
                }

                std::vector<uint8_t> internalBuffer(bufferByteLength);
                memset(&internalBuffer[0], 0, bufferByteLength);
                uint8_t *outInternalBuffer = &internalBuffer[0];
                nlohmann::json tileAvailabilityJson;
                int bufferViewIndex = 0;
                uint64_t internalBufferOffset = 0;
                if (constantTileAvailability)
                    tileAvailabilityJson["constant"] = static_cast<int>(tileAndChildAvailability.nodeCount
                                                                        == subtreeNodeCount);
                else {
                    memcpy(&outInternalBuffer[0],
                           &tileAndChildAvailability.nodeBuffer[0],
                           nodeAvailabilityByteLengthWithPadding);
                    nlohmann::json bufferViewObj;
                    bufferViewObj["buffer"] = 0;
                    bufferViewObj["byteOffset"] = 0;
                    bufferViewObj["byteLength"] = availabilityByteLength;
                    bufferViews.emplace_back(bufferViewObj);
                    internalBufferOffset += nodeAvailabilityByteLengthWithPadding;
                    tileAvailabilityJson["bufferView"] = bufferViewIndex;
                    bufferViewIndex += 1;
                }
                subtreeJson["tileAvailability"] = tileAvailabilityJson;

                nlohmann::json childAvailabilityJson;
                if (constantChildAvailability)
                    childAvailabilityJson["constant"] = static_cast<int>(tileAndChildAvailability.childCount
                                                                         == childSubtreeCount);
                else {
                    memcpy(&outInternalBuffer[internalBufferOffset],
                           &tileAndChildAvailability.childBuffer[0],
                           childSubtreeAvailabilityByteLengthWithPadding);
                    nlohmann::json bufferViewObj;
                    bufferViewObj["buffer"] = 0;
                    bufferViewObj["byteOffset"] = internalBufferOffset;
                    bufferViewObj["byteLength"] = childSubtreeAvailabilityByteLength;
                    bufferViews.emplace_back(bufferViewObj);
                    childAvailabilityJson["bufferView"] = bufferViewIndex;
                    bufferViewIndex += 1;
                }
                subtreeJson["childSubtreeAvailability"] = childAvailabilityJson;

                std::string availabilityFileName = subtreeRoot + ".bin";

                std::filesystem::path datasetDir = datasetDirs.at(dataset);

                std::map<std::string, SubtreeAvailability> csSubtreeRoots = datasetCSSubtrees.at(dataset).at(
                    CSKey);
                nlohmann::json contentObj;
                if (std::filesystem::exists(datasetDir / CSKey / "availability" / availabilityFileName)) {
                    nlohmann::json bufferObj;
                    auto datasetDirIt = datasetDir.end();
                    --datasetDirIt; // point to the dataset directory name
                    bufferObj["uri"] = "../availability/" + availabilityFileName;
                    bufferObj["byteLength"] = nodeAvailabilityByteLengthWithPadding;
                    buffers.emplace_back(bufferObj);
                    nlohmann::json bufferViewObj;
                    bufferViewObj["buffer"] = bufferIndex;
                    bufferViewObj["byteOffset"] = 0;
                    bufferViewObj["byteLength"] = availabilityByteLength;
                    bufferViews.emplace_back(bufferViewObj);
                    contentObj["bufferView"] = bufferViewIndex;
                    bufferViewIndex += 1;
                    bufferIndex += 1;
                } else if (csSubtreeRoots.count(subtreeRoot) != 0) {
                    SubtreeAvailability subtree = datasetCSSubtrees.at(dataset).at(CSKey).at(subtreeRoot);
                    contentObj["constant"] = static_cast<int>(subtree.nodeCount == subtreeNodeCount);
                } else
                    contentObj["constant"] = 0;
                subtreeJson["contentAvailability"] = contentObj;
                if (!buffers.empty())
                    subtreeJson["buffers"] = buffers;
                if (!bufferViews.empty())
                    subtreeJson["bufferViews"] = bufferViews;

                // get json length
                const std::string jsonString = subtreeJson.dump();
                const uint64_t jsonStringByteLength = jsonString.size();
                const uint64_t jsonStringByteLengthWithPadding = alignTo8(jsonStringByteLength);

                // Write subtree binary
                uint64_t outputBufferLength = jsonStringByteLengthWithPadding + bufferByteLength
                                              + headerByteLength;
                std::vector<uint8_t> outputBuffer(outputBufferLength);
                uint8_t *outBuffer = &outputBuffer[0];
                *(uint32_t *) &outBuffer[0] = 0x74627573;                      // magic: "subt"
                *(uint32_t *) &outBuffer[4] = 1;                               // version
                *(uint64_t *) &outBuffer[8] = jsonStringByteLengthWithPadding; // JSON byte length with padding
                *(uint64_t *) &outBuffer[16] = bufferByteLength;               // BIN byte length with padding

                memcpy(&outBuffer[headerByteLength], &jsonString[0], jsonStringByteLength);
                memset(&outBuffer[headerByteLength + jsonStringByteLength],
                       ' ',
                       jsonStringByteLengthWithPadding - jsonStringByteLength);

                if (bufferByteLength != 0) {
                    memcpy(&outBuffer[headerByteLength + jsonStringByteLengthWithPadding],
                           outInternalBuffer,
                           bufferByteLength);
                }
                std::filesystem::path path = datasetDir / CSKey / "subtrees" / (subtreeRoot + ".subtree");
                Utilities::writeBinaryFile(path, (const char *) outBuffer, outputBufferLength);
            }
            tileAndChildAvailabilities.clear();
            subtreeRoots.clear();
        }
    }
}

void CDBTilesetBuilder::initializeImplicitTilingParameters()
{
    subtreeNodeCount = static_cast<int>((pow(4, subtreeLevels) - 1) / 3);
    childSubtreeCount = static_cast<int>(pow(4, subtreeLevels)); // 4^N
    availabilityByteLength = static_cast<int>(ceil(static_cast<double>(subtreeNodeCount) / 8.0));
    nodeAvailabilityByteLengthWithPadding = alignTo8(availabilityByteLength);
    childSubtreeAvailabilityByteLength = static_cast<int>(ceil(static_cast<double>(childSubtreeCount) / 8.0));
    childSubtreeAvailabilityByteLengthWithPadding = alignTo8(childSubtreeAvailabilityByteLength);
}

std::string CDBTilesetBuilder::levelXYtoSubtreeKey(int level, int x, int y)
{
    return std::to_string(level) + "_" + std::to_string(x) + "_" + std::to_string(y);
}

std::string CDBTilesetBuilder::cs1cs2ToCSKey(int cs1, int cs2)
{
    return std::to_string(cs1) + "_" + std::to_string(cs2);
}

void CDBTilesetBuilder::addAvailability(const CDBTile &cdbTile)
{
    CDBDataset dataset = cdbTile.getDataset();
    if (datasetTilesetCollections.count(dataset) == 0) {
        throw std::invalid_argument(getCDBDatasetDirectoryName(dataset) + " is not currently supported.");
    }
    if (datasetCSSubtrees.count(dataset) == 0)
        datasetCSSubtrees.insert(
            std::pair<CDBDataset, std::map<std::string, std::map<std::string, SubtreeAvailability>>>(dataset,
                                                                                                     {}));
    std::map<std::string, std::map<std::string, SubtreeAvailability>> &csSubtrees = datasetCSSubtrees.at(
        dataset);

    std::string csKey = cs1cs2ToCSKey(cdbTile.getCS_1(), cdbTile.getCS_2());
    if (csSubtrees.count(csKey) == 0) {
        csSubtrees.insert(std::pair<std::string, std::map<std::string, SubtreeAvailability>>(
            csKey, std::map<std::string, SubtreeAvailability>{}));
    }

    std::map<std::string, SubtreeAvailability> &subtreeMap = csSubtrees.at(csKey);

    int level = cdbTile.getLevel();

    int x = cdbTile.getRREF();
    int y = cdbTile.getUREF();

    SubtreeAvailability *subtree;

    if (level >= 0) {
        // get the root of the subtree that this tile belongs to
        int subtreeRootLevel = (level / subtreeLevels) * subtreeLevels; // the level of the subtree root

        // from Volume 1: OGC CDB Core Standard: Model and Physical Data Store Structure page 120
        int levelWithinSubtree = level - subtreeRootLevel;
        int subtreeRootX = x / static_cast<int>(glm::pow(2, levelWithinSubtree));
        int subtreeRootY = y / static_cast<int>(glm::pow(2, levelWithinSubtree));

        std::string subtreeKey = levelXYtoSubtreeKey(subtreeRootLevel, subtreeRootX, subtreeRootY);
        if (subtreeMap.find(subtreeKey) == subtreeMap.end()) // the buffer isn't in the map
        {
            subtreeMap.insert(
                std::pair<std::string, SubtreeAvailability>(subtreeKey, createSubtreeAvailability()));
        }

        subtree = &subtreeMap.at(subtreeKey);

        addAvailability(cdbTile, subtree, subtreeRootLevel, subtreeRootX, subtreeRootY);
    }
}

void CDBTilesetBuilder::addAvailability(const CDBTile &cdbTile,
                                        SubtreeAvailability *subtree,
                                        int subtreeRootLevel,
                                        int subtreeRootX,
                                        int subtreeRootY)
{
    if (subtree == NULL) {
        throw std::invalid_argument("Subtree availability pointer is null. Check if initialized.");
    }
    if (subtreeLevels < 1) {
        throw std::invalid_argument("Subtree level must be positive.");
    }
    int level = cdbTile.getLevel();
    int levelWithinSubtree = level - subtreeRootLevel;

    int localX = cdbTile.getRREF() - subtreeRootX * static_cast<int>(pow(2, levelWithinSubtree));
    int localY = cdbTile.getUREF() - subtreeRootY * static_cast<int>(pow(2, levelWithinSubtree));

    setBitAtXYLevelMorton(subtree->nodeBuffer, localX, localY, levelWithinSubtree);
    subtree->nodeCount += 1;

    std::string csKey = cs1cs2ToCSKey(cdbTile.getCS_1(), cdbTile.getCS_2());

    CDBDataset tileDataset = cdbTile.getDataset();
    if (datasetCSTileAndChildAvailabilities.count(tileDataset) == 0)
        datasetCSTileAndChildAvailabilities.insert(
            std::pair<CDBDataset, std::map<std::string, std::map<std::string, SubtreeAvailability>>>(
                tileDataset, std::map<std::string, std::map<std::string, SubtreeAvailability>>{}));
    std::map<std::string, std::map<std::string, SubtreeAvailability>> &csTileAndChildAvailabilities
        = datasetCSTileAndChildAvailabilities.at(tileDataset);

    if (csTileAndChildAvailabilities.count(csKey) == 0)
        csTileAndChildAvailabilities.insert(std::pair<std::string, std::map<std::string, SubtreeAvailability>>(
            csKey, std::map<std::string, SubtreeAvailability>{}));
    std::map<std::string, SubtreeAvailability> &tileAndChildAvailabilities = csTileAndChildAvailabilities.at(
        csKey);
    std::string subtreeKey = levelXYtoSubtreeKey(subtreeRootLevel, subtreeRootX, subtreeRootY);
    createTileAndChildSubtreeAtKey(tileAndChildAvailabilities, subtreeKey);
    setBitAtXYLevelMorton(tileAndChildAvailabilities.at(subtreeKey).nodeBuffer,
                          localX,
                          localY,
                          levelWithinSubtree);
    setParentBitsRecursively(tileAndChildAvailabilities,
                             level,
                             cdbTile.getRREF(),
                             cdbTile.getUREF(),
                             subtreeRootLevel,
                             subtreeRootX,
                             subtreeRootY);
}

bool CDBTilesetBuilder::setBitAtXYLevelMorton(std::vector<uint8_t> &buffer,
                                              int localX,
                                              int localY,
                                              int localLevel)
{
    const uint64_t mortonIndex = libmorton::morton2D_64_encode(localX, localY);
    // https://github.com/CesiumGS/3d-tiles/tree/3d-tiles-next/extensions/3DTILES_implicit_tiling/0.0.0#accessing-availability-bits
    const uint64_t nodeCountUpToThisLevel = (static_cast<uint64_t>(pow(4, localLevel)) - 1) / 3;

    const uint64_t index = nodeCountUpToThisLevel + mortonIndex;
    const uint64_t byte = index / 8;
    const uint64_t bit = index % 8;
    if (byte >= buffer.size())
        throw std::invalid_argument("x, y, level coordinates too large for given buffer.");
    int mask = (1 << bit);
    bool bitAlreadySet = (buffer[byte] & mask) >> bit == 1;
    const uint8_t availability = static_cast<uint8_t>(1 << bit);
    buffer[byte] |= availability;
    return bitAlreadySet;
}

void CDBTilesetBuilder::setParentBitsRecursively(
    std::map<std::string, SubtreeAvailability> &tileAndChildAvailabilities,
    int level,
    int x,
    int y,
    int subtreeRootLevel,
    int subtreeRootX,
    int subtreeRootY)
{
    if (level == 0) // we reached the root tile
        return;
    if (level == subtreeRootLevel) // need to set childSubtree bit of parent subtree
    {
        subtreeRootLevel -= subtreeLevels;
        subtreeRootX /= static_cast<int>(glm::pow(2, subtreeLevels));
        subtreeRootY /= static_cast<int>(glm::pow(2, subtreeLevels));

        int localChildX = x - subtreeRootX * static_cast<int>(pow(2, subtreeLevels));
        int localChildY = y - subtreeRootY * static_cast<int>(pow(2, subtreeLevels));

        std::string subtreeKey = levelXYtoSubtreeKey(subtreeRootLevel, subtreeRootX, subtreeRootY);
        createTileAndChildSubtreeAtKey(tileAndChildAvailabilities, subtreeKey);
        setBitAtXYLevelMorton(tileAndChildAvailabilities[subtreeKey].childBuffer, localChildX, localChildY);
    } else {
        level -= 1;
        x /= 2;
        y /= 2;
        std::string subtreeKey = levelXYtoSubtreeKey(subtreeRootLevel, subtreeRootX, subtreeRootY);
        createTileAndChildSubtreeAtKey(tileAndChildAvailabilities, subtreeKey);

        int localLevel = level - subtreeRootLevel;
        int localX = x - subtreeRootX * static_cast<int>(pow(2, localLevel));
        int localY = y - subtreeRootY * static_cast<int>(pow(2, localLevel));

        bool bitAlreadySet = setBitAtXYLevelMorton(tileAndChildAvailabilities[subtreeKey].nodeBuffer,
                                                   localX,
                                                   localY,
                                                   localLevel);
        if (bitAlreadySet) // cut the recursion short
            return;
    }
    setParentBitsRecursively(tileAndChildAvailabilities,
                             level,
                             x,
                             y,
                             subtreeRootLevel,
                             subtreeRootX,
                             subtreeRootY);
}

void CDBTilesetBuilder::addElevationToTilesetCollection(CDBElevation &elevation,
                                                        const CDB &cdb,
                                                        const std::filesystem::path &collectionOutputDirectory)
{
    const auto &cdbTile = elevation.getTile();
    auto currentImagery = cdb.getImagery(cdbTile);
    auto currentRMTexture = cdb.getRMTexture(cdbTile);
    auto currentRMDescriptor = cdb.getRMDescriptor(cdbTile);

    std::filesystem::path tilesetDirectory;
    CDBTileset *tileset;
    getTileset(cdbTile, collectionOutputDirectory, elevationTilesets, tileset, tilesetDirectory);

    if (currentImagery) {
        Texture imageryTexture = createImageryTexture(*currentImagery, tilesetDirectory);
        if (currentRMTexture) {
            Texture featureIDTexture = createFeatureIDTexture(*currentRMTexture, tilesetDirectory);
            addElevationToTileset(elevation,
                                  &imageryTexture,
                                  cdb,
                                  tilesetDirectory,
                                  *tileset,
                                  &featureIDTexture,
                                  currentRMDescriptor);
        } else {
            addElevationToTileset(elevation, &imageryTexture, cdb, tilesetDirectory, *tileset);
        }

    } else {
        // find parent imagery if the current one doesn't exist
        Texture *parentTexture = nullptr;
        auto current = CDBTile::createParentTile(cdbTile);
        while (current) {
            // if not in the cache, then write the image and save its name in the cache
            auto it = processedParentImagery.find(*current);
            if (it == processedParentImagery.end()) {
                auto parentImagery = cdb.getImagery(*current);
                if (parentImagery) {
                    auto newTexture = createImageryTexture(*parentImagery, tilesetDirectory);
                    auto cacheImageryTexture = processedParentImagery.insert(
                        {*current, std::move(newTexture)});

                    parentTexture = &(cacheImageryTexture.first->second);

                    break;
                }
            } else {
                // found it, we don't need to read the image again. Just use saved name of the saved image
                parentTexture = &it->second;
                break;
            }

            current = CDBTile::createParentTile(*current);
        }

        // we need to re-index UV of the mesh so that it is relative to the parent tile UVs for this case.
        // This step is not necessary for negative LOD since the tile and the parent covers the whole geo cell
        if (parentTexture && cdbTile.getLevel() > 0) {
            elevation.indexUVRelativeToParent(*current);
        }

        if (parentTexture) {
            addElevationToTileset(elevation, parentTexture, cdb, tilesetDirectory, *tileset);
        } else {
            addElevationToTileset(elevation, nullptr, cdb, tilesetDirectory, *tileset);
        }
    }
}

void CDBTilesetBuilder::addElevationToTileset(CDBElevation &elevation,
                                              const Texture *imagery,
                                              const CDB &cdb,
                                              const std::filesystem::path &tilesetDirectory,
                                              CDBTileset &tileset,
                                              const Texture *featureIdTexture,
                                              CDBRMDescriptor *materialDescriptor)
{
    const auto &mesh = elevation.getUniformGridMesh();
    if (mesh.positionRTCs.empty()) {
        return;
    }

    size_t targetIndexCount = static_cast<size_t>(static_cast<float>(mesh.indices.size())
                                                  * elevationThresholdIndices);
    float targetError = elevationDecimateError;
    Mesh simplifed = elevation.createSimplifiedMesh(targetIndexCount, targetError);
    if (simplifed.positionRTCs.empty()) {
        simplifed = mesh;
    }

    if (elevationNormal) {
        generateElevationNormal(simplifed);
    }

    CDBTile tile = elevation.getTile();
    CDBTile tileWithBoundRegion = CDBTile(tile.getGeoCell(),
                                          tile.getDataset(),
                                          tile.getCS_1(),
                                          tile.getCS_2(),
                                          tile.getLevel(),
                                          tile.getUREF(),
                                          tile.getRREF());
    tileWithBoundRegion.setBoundRegion(
        Core::BoundingRegion(tileWithBoundRegion.getBoundRegion().getRectangle(),
                             elevation.getMinElevation(),
                             elevation.getMaxElevation()));
    elevation.setTile(tileWithBoundRegion);
    auto &cdbTile = elevation.getTile();

    tinygltf::Model gltf;
    // create material for mesh if there are imagery
    if (imagery) {
        Material material;
        material.doubleSided = true;
        material.unlit = !elevationNormal;
        material.texture = 0;
        simplifed.material = 0;

        gltf = createGltf(simplifed, &material, imagery, use3dTilesNext, featureIdTexture);
        if (featureIdTexture && materialDescriptor) {
            materialDescriptor->addFeatureTableToGltf(&materials, &gltf, externalSchema);
        }
    } else {
        gltf = createGltf(simplifed, nullptr, nullptr, use3dTilesNext);
    }

    if (use3dTilesNext) {
        createGLTFForTileset(gltf, cdbTile, nullptr, tilesetDirectory, tileset);
    } else {
        createB3DMForTileset(gltf, cdbTile, nullptr, tilesetDirectory, tileset);
    }

    if (cdbTile.getLevel() < 0) {
        fillMissingNegativeLODElevation(elevation, cdb, tilesetDirectory, tileset);
    } else {
        fillMissingPositiveLODElevation(elevation, imagery, cdb, tilesetDirectory, tileset);
    }
}

void CDBTilesetBuilder::fillMissingPositiveLODElevation(const CDBElevation &elevation,
                                                        const Texture *currentImagery,
                                                        const CDB &cdb,
                                                        const std::filesystem::path &tilesetDirectory,
                                                        CDBTileset &tileset)
{
    const auto &cdbTile = elevation.getTile();
    auto nw = CDBTile::createNorthWestForPositiveLOD(cdbTile);
    auto ne = CDBTile::createNorthEastForPositiveLOD(cdbTile);
    auto sw = CDBTile::createSouthWestForPositiveLOD(cdbTile);
    auto se = CDBTile::createSouthEastForPositiveLOD(cdbTile);

    // check if elevation exist
    bool isNorthWestExist = cdb.isElevationExist(nw);
    bool isNorthEastExist = cdb.isElevationExist(ne);
    bool isSouthWestExist = cdb.isElevationExist(sw);
    bool isSouthEastExist = cdb.isElevationExist(se);
    bool shouldFillHole = isNorthEastExist || isNorthWestExist || isSouthWestExist || isSouthEastExist;

    // If we don't need to make elevation and imagery have the same LOD, then hasMoreImagery is false.
    // Otherwise, check if imagery exist even the elevation has no child
    bool hasMoreImagery;
    if (elevationLOD) {
        hasMoreImagery = false;
    } else {
        bool isNorthWestImageryExist = cdb.isImageryExist(nw);
        bool isNorthEastImageryExist = cdb.isImageryExist(ne);
        bool isSouthWestImageryExist = cdb.isImageryExist(sw);
        bool isSouthEastImageryExist = cdb.isImageryExist(se);
        hasMoreImagery = isNorthEastImageryExist || isNorthWestImageryExist || isSouthEastImageryExist
                         || isSouthWestImageryExist;
    }

    if (shouldFillHole || hasMoreImagery) {
        if (!isNorthWestExist) {
            auto subRegionImagery = cdb.getImagery(nw);
            bool reindexUV = subRegionImagery != std::nullopt;
            auto subRegion = elevation.createNorthWestSubRegion(reindexUV);
            if (subRegion) {
                addSubRegionElevationToTileset(*subRegion,
                                               cdb,
                                               subRegionImagery,
                                               currentImagery,
                                               tilesetDirectory,
                                               tileset);
            }
        }

        if (!isNorthEastExist) {
            auto subRegionImagery = cdb.getImagery(ne);
            bool reindexUV = subRegionImagery != std::nullopt;
            auto subRegion = elevation.createNorthEastSubRegion(reindexUV);
            if (subRegion) {
                addSubRegionElevationToTileset(*subRegion,
                                               cdb,
                                               subRegionImagery,
                                               currentImagery,
                                               tilesetDirectory,
                                               tileset);
            }
        }

        if (!isSouthEastExist) {
            auto subRegionImagery = cdb.getImagery(se);
            bool reindexUV = subRegionImagery != std::nullopt;
            auto subRegion = elevation.createSouthEastSubRegion(reindexUV);
            if (subRegion) {
                addSubRegionElevationToTileset(*subRegion,
                                               cdb,
                                               subRegionImagery,
                                               currentImagery,
                                               tilesetDirectory,
                                               tileset);
            }
        }

        if (!isSouthWestExist) {
            auto subRegionImagery = cdb.getImagery(sw);
            bool reindexUV = subRegionImagery != std::nullopt;
            auto subRegion = elevation.createSouthWestSubRegion(reindexUV);
            if (subRegion) {
                addSubRegionElevationToTileset(*subRegion,
                                               cdb,
                                               subRegionImagery,
                                               currentImagery,
                                               tilesetDirectory,
                                               tileset);
            }
        }
    }
}

void CDBTilesetBuilder::fillMissingNegativeLODElevation(CDBElevation &elevation,
                                                        const CDB &cdb,
                                                        const std::filesystem::path &outputDirectory,
                                                        CDBTileset &tileset)
{
    const auto &cdbTile = elevation.getTile();
    auto child = CDBTile::createChildForNegativeLOD(cdbTile);

    // if imagery exist, but we have no more terrain, then duplicate it. However,
    // when we only care about elevation LOD, don't duplicate it
    if (!cdb.isElevationExist(child)) {
        if (!elevationLOD) {
            auto childImagery = cdb.getImagery(child);
            if (childImagery) {
                Texture imageryTexture = createImageryTexture(*childImagery, outputDirectory);
                elevation.setTile(child);
                addElevationToTileset(elevation, &imageryTexture, cdb, outputDirectory, tileset);
            }
        }
    }
}

void CDBTilesetBuilder::generateElevationNormal(Mesh &simplifed)
{
    size_t totalVertices = simplifed.positions.size();

    // calculate normals
    const auto &ellipsoid = Core::Ellipsoid::WGS84;
    simplifed.normals.resize(totalVertices, glm::vec3(0.0));
    for (size_t i = 0; i < simplifed.indices.size(); i += 3) {
        uint32_t idx0 = simplifed.indices[i];
        uint32_t idx1 = simplifed.indices[i + 1];
        uint32_t idx2 = simplifed.indices[i + 2];

        glm::dvec3 p0 = simplifed.positionRTCs[idx0];
        glm::dvec3 p1 = simplifed.positionRTCs[idx1];
        glm::dvec3 p2 = simplifed.positionRTCs[idx2];

        glm::vec3 normal = glm::cross(p1 - p0, p2 - p0);
        simplifed.normals[idx0] += normal;
        simplifed.normals[idx1] += normal;
        simplifed.normals[idx2] += normal;
    }

    // normalize normal and calculate position rtc
    for (size_t i = 0; i < totalVertices; ++i) {
        auto &normal = simplifed.normals[i];
        if (glm::abs(glm::dot(normal, normal)) > Core::Math::EPSILON10) {
            normal = glm::normalize(normal);
        } else {
            auto cartographic = ellipsoid.cartesianToCartographic(simplifed.positions[i]);
            if (cartographic) {
                normal = ellipsoid.geodeticSurfaceNormal(*cartographic);
            }
        }
    }
}

void CDBTilesetBuilder::addSubRegionElevationToTileset(CDBElevation &subRegion,
                                                       const CDB &cdb,
                                                       std::optional<CDBImagery> &subRegionImagery,
                                                       const Texture *parentTexture,
                                                       const std::filesystem::path &outputDirectory,
                                                       CDBTileset &tileset)
{
    // Use the sub region imagery. If sub region doesn't have imagery, reuse parent imagery if we don't have any higher LOD imagery
    if (subRegionImagery) {
        Texture subRegionTexture = createImageryTexture(*subRegionImagery, outputDirectory);
        addElevationToTileset(subRegion, &subRegionTexture, cdb, outputDirectory, tileset);
    } else if (parentTexture) {
        addElevationToTileset(subRegion, parentTexture, cdb, outputDirectory, tileset);
    } else {
        addElevationToTileset(subRegion, nullptr, cdb, outputDirectory, tileset);
    }
}

Texture CDBTilesetBuilder::createFeatureIDTexture(CDBRMTexture &rmTexture,
                                                  const std::filesystem::path &tilesetOutputDirectory) const
{
    static const std::filesystem::path MODEL_TEXTURE_SUB_DIR = "Textures";
    const auto &tile = rmTexture.getTile();
    auto textureRelativePath = MODEL_TEXTURE_SUB_DIR / (tile.getRelativePath().filename().string() + ".png");
    auto textureAbsolutePath = tilesetOutputDirectory / textureRelativePath;
    auto textureDirectory = tilesetOutputDirectory / MODEL_TEXTURE_SUB_DIR;
    if (!std::filesystem::exists(textureDirectory)) {
        std::filesystem::create_directories(textureDirectory);
    }

    auto driver = (GDALDriver *) GDALGetDriverByName("png");
    if (driver) {
        GDALDatasetUniquePtr pngDataset = GDALDatasetUniquePtr(driver->CreateCopy(textureAbsolutePath.c_str(),
                                                                                  &rmTexture.getData(),
                                                                                  false,
                                                                                  nullptr,
                                                                                  nullptr,
                                                                                  nullptr));
    }

    Texture texture;
    texture.uri = textureRelativePath;
    texture.magFilter = TextureFilter::NEAREST;
    texture.minFilter = TextureFilter::NEAREST_MIPMAP_NEAREST;

    return texture;
}

Texture CDBTilesetBuilder::createImageryTexture(CDBImagery &imagery,
                                                const std::filesystem::path &tilesetOutputDirectory) const
{
    static const std::filesystem::path MODEL_TEXTURE_SUB_DIR = "Textures";

    const auto &tile = imagery.getTile();
    auto textureRelativePath = MODEL_TEXTURE_SUB_DIR / (tile.getRelativePath().filename().string() + ".jpeg");
    auto textureAbsolutePath = tilesetOutputDirectory / textureRelativePath;
    auto textureDirectory = tilesetOutputDirectory / MODEL_TEXTURE_SUB_DIR;
    if (!std::filesystem::exists(textureDirectory)) {
        std::filesystem::create_directories(textureDirectory);
    }

    auto driver = (GDALDriver *) GDALGetDriverByName("jpeg");
    if (driver) {
        GDALDatasetUniquePtr jpegDataset = GDALDatasetUniquePtr(driver->CreateCopy(
            textureAbsolutePath.string().c_str(), &imagery.getData(), false, nullptr, nullptr, nullptr));
    }

    Texture texture;
    texture.uri = textureRelativePath;
    texture.magFilter = TextureFilter::LINEAR;
    texture.minFilter = TextureFilter::LINEAR_MIPMAP_NEAREST;

    return texture;
}

void CDBTilesetBuilder::addVectorToTilesetCollection(
    const CDBGeometryVectors &vectors,
    const std::filesystem::path &collectionOutputDirectory,
    std::unordered_map<CDBGeoCell, TilesetCollection> &tilesetCollections)
{
    const auto &cdbTile = vectors.getTile();
    const auto &mesh = vectors.getMesh();
    if (mesh.positionRTCs.empty()) {
        return;
    }

    std::filesystem::path tilesetDirectory;
    CDBTileset *tileset;
    getTileset(cdbTile, collectionOutputDirectory, tilesetCollections, tileset, tilesetDirectory);

    tinygltf::Model gltf = createGltf(mesh, nullptr, nullptr, use3dTilesNext);
    if (use3dTilesNext) {
        createGLTFForTileset(gltf, cdbTile, &vectors.getInstancesAttributes(), tilesetDirectory, *tileset);
    } else {
        createB3DMForTileset(gltf, cdbTile, &vectors.getInstancesAttributes(), tilesetDirectory, *tileset);
    }
    if (use3dTilesNext && cdbTile.getLevel() >= 0)
        addAvailability(cdbTile);
    tileset->insertTile(cdbTile);
}

void CDBTilesetBuilder::addGTModelToTilesetCollection(const CDBGTModels &model,
                                                      const std::filesystem::path &collectionOutputDirectory)
{
    static const std::filesystem::path MODEL_GLTF_SUB_DIR = "Gltf";
    static const std::filesystem::path MODEL_TEXTURE_SUB_DIR = "Textures";

    auto cdbTile = model.getModelsAttributes().getTile();

    std::filesystem::path tilesetDirectory;
    CDBTileset *tileset;
    getTileset(cdbTile, collectionOutputDirectory, GTModelTilesets, tileset, tilesetDirectory);

    // create gltf file
    auto gltfOutputDIr = tilesetDirectory / MODEL_GLTF_SUB_DIR;
    std::filesystem::create_directories(gltfOutputDIr);

    std::map<std::string, std::vector<int>> instances;
    const auto &modelsAttribs = model.getModelsAttributes();
    const auto &instancesAttribs = modelsAttribs.getInstancesAttributes();
    for (size_t i = 0; i < instancesAttribs.getInstancesCount(); ++i) {
        std::string modelKey;
        auto model3D = model.locateModel3D(i, modelKey);
        if (model3D) {
            if (GTModelsToGltf.find(modelKey) == GTModelsToGltf.end()) {
                // write textures to files
                auto textures = writeModeTextures(model3D->getTextures(),
                                                  model3D->getImages(),
                                                  MODEL_TEXTURE_SUB_DIR,
                                                  gltfOutputDIr);

                // create gltf for the instance
                tinygltf::Model gltf = createGltf(model3D->getMeshes(),
                                                  model3D->getMaterials(),
                                                  textures,
                                                  use3dTilesNext);

                // write to glb
                tinygltf::TinyGLTF loader;
                std::filesystem::path modelGltfURI = MODEL_GLTF_SUB_DIR / (modelKey + ".glb");
                loader.WriteGltfSceneToFile(&gltf, tilesetDirectory / modelGltfURI, false, false, false, true);
                GTModelsToGltf.insert({modelKey, modelGltfURI});
            }

            auto &instance = instances[modelKey];
            instance.emplace_back(i);
        }
    }

    std::string cdbTileFilename = cdbTile.getRelativePathWithNonZeroPaddedLevel().filename().string();
    if (use3dTilesNext) {
        std::filesystem::path gltfPath = cdbTileFilename + std::string(".glb");
        std::filesystem::path gltfFullPath = tilesetDirectory / gltfPath;

        // Create glTF.
        tinygltf::Model gltf;
        gltf.asset.version = "2.0";
        tinygltf::Scene scene;
        scene.nodes = {0};
        gltf.scenes.emplace_back(scene);
        // Create buffer.
        tinygltf::Buffer buffer;
        gltf.buffers.emplace_back(buffer);
        // Create root node.
        tinygltf::Node rootNode;
        rootNode.matrix = {1, 0, 0, 0, 0, 0, -1, 0, 0, 1, 0, 0, 0, 0, 0, 1};
        gltf.nodes.emplace_back(rootNode);
        // Create default sampler.
        tinygltf::Sampler sampler;
        sampler.magFilter = TINYGLTF_TEXTURE_FILTER_LINEAR;
        sampler.minFilter = TINYGLTF_TEXTURE_FILTER_LINEAR;
        sampler.wrapR = TINYGLTF_TEXTURE_WRAP_REPEAT;
        sampler.wrapS = TINYGLTF_TEXTURE_WRAP_REPEAT;
        sampler.wrapT = TINYGLTF_TEXTURE_WRAP_REPEAT;
        gltf.samplers.emplace_back(sampler);

        std::string error, warning;
        tinygltf::TinyGLTF io;
        std::vector<tinygltf::Model> glbs;

        for (const auto &instance : instances) {
            const auto &instanceIndices = instance.second;
            tinygltf::Model loadedModel;
            io.LoadBinaryFromFile(&loadedModel,
                                  &error,
                                  &warning,
                                  tilesetDirectory / GTModelsToGltf[instance.first]);

            createInstancingExtension(&loadedModel, modelsAttribs, instanceIndices);
            glbs.emplace_back(loadedModel);
        }

        combineGltfs(&gltf, glbs);

        cdbTile.setCustomContentURI(gltfPath);

        // Enable writing textures to output folder.
        auto originalPath = std::filesystem::current_path();
        std::filesystem::current_path(tilesetDirectory);

        // Create glTF stringstream
        std::stringstream ss;
        tinygltf::TinyGLTF gltfIO;
        std::ofstream fs(gltfFullPath, std::ios::binary);
        writePaddedGLB(&gltf, fs);

        std::filesystem::current_path(originalPath);
    } else {
        // write i3dm to cmpt
        std::filesystem::path cmpt = cdbTileFilename + std::string(".cmpt");
        std::filesystem::path cmptFullPath = tilesetDirectory / cmpt;
        std::ofstream fs(cmptFullPath, std::ios::binary);
        auto instance = instances.begin();
        writeToCMPT(static_cast<uint32_t>(instances.size()), fs, [&](std::ofstream &os, size_t) {
            const auto &GltfURI = GTModelsToGltf[instance->first];
            const auto &instanceIndices = instance->second;
            size_t totalWrite = writeToI3DM(GltfURI, modelsAttribs, instanceIndices, os);
            instance = std::next(instance);
            return totalWrite;
        });

        // add it to tileset
        cdbTile.setCustomContentURI(cmpt);
    }
    if (use3dTilesNext && cdbTile.getLevel() >= 0)
        addAvailability(cdbTile);
    tileset->insertTile(cdbTile);
}

void CDBTilesetBuilder::addGSModelToTilesetCollection(const CDBGSModels &model,
                                                      const std::filesystem::path &collectionOutputDirectory)
{
    static const std::filesystem::path MODEL_TEXTURE_SUB_DIR = "Textures";

    const auto &cdbTile = model.getTile();
    const auto &model3D = model.getModel3D();

    std::filesystem::path tilesetDirectory;
    CDBTileset *tileset;
    getTileset(cdbTile, collectionOutputDirectory, GSModelTilesets, tileset, tilesetDirectory);

    auto textures = writeModeTextures(model3D.getTextures(),
                                      model3D.getImages(),
                                      MODEL_TEXTURE_SUB_DIR,
                                      tilesetDirectory);

    auto gltf = createGltf(model3D.getMeshes(), model3D.getMaterials(), textures, use3dTilesNext);
    if (use3dTilesNext) {
        createGLTFForTileset(gltf, cdbTile, &model.getInstancesAttributes(), tilesetDirectory, *tileset);
    } else {
        createB3DMForTileset(gltf, cdbTile, &model.getInstancesAttributes(), tilesetDirectory, *tileset);
    }
}

std::vector<Texture> CDBTilesetBuilder::writeModeTextures(const std::vector<Texture> &modelTextures,
                                                          const std::vector<osg::ref_ptr<osg::Image>> &images,
                                                          const std::filesystem::path &textureSubDir,
                                                          const std::filesystem::path &gltfPath)
{
    auto textureDirectory = gltfPath / textureSubDir;
    if (!std::filesystem::exists(textureDirectory)) {
        std::filesystem::create_directories(textureDirectory);
    }

    auto textures = modelTextures;
    for (size_t i = 0; i < modelTextures.size(); ++i) {
        auto textureRelativePath = textureSubDir / modelTextures[i].uri;
        auto textureAbsolutePath = gltfPath / textureSubDir / modelTextures[i].uri;

        if (processedModelTextures.find(textureAbsolutePath) == processedModelTextures.end()) {
            osgDB::writeImageFile(*images[i], textureAbsolutePath.string(), nullptr);
        }

        textures[i].uri = textureRelativePath.string();
    }

    return textures;
}

void CDBTilesetBuilder::createB3DMForTileset(tinygltf::Model &gltf,
                                             CDBTile cdbTile,
                                             const CDBInstancesAttributes *instancesAttribs,
                                             const std::filesystem::path &outputDirectory,
                                             CDBTileset &tileset)
{
    // create b3dm file
    std::string cdbTileFilename = cdbTile.getRelativePathWithNonZeroPaddedLevel().filename().string();
    std::filesystem::path b3dm = cdbTileFilename + std::string(".b3dm");
    std::filesystem::path b3dmFullPath = outputDirectory / b3dm;

    // write to b3dm
    std::ofstream fs(b3dmFullPath, std::ios::binary);
    writeToB3DM(&gltf, instancesAttribs, fs);
    cdbTile.setCustomContentURI(b3dm);

    if (use3dTilesNext) {
        if (cdbTile.getLevel() >= 0)
            addAvailability(cdbTile);
    }
    tileset.insertTile(cdbTile);
}

void CDBTilesetBuilder::createGLTFForTileset(tinygltf::Model &gltf,
                                             CDBTile cdbTile,
                                             const CDBInstancesAttributes *instancesAttribs,
                                             const std::filesystem::path &outputDirectory,
                                             CDBTileset &tileset)
{
    // Create glTF file
    std::string cdbTileFilename = cdbTile.getRelativePathWithNonZeroPaddedLevel().filename().string();
    std::filesystem::path gltfFile = cdbTileFilename + std::string(".glb");
    std::filesystem::path gltfFullPath = outputDirectory / gltfFile;

    // Write to glTF
    std::ofstream fs(gltfFullPath, std::ios::binary);
    writeToGLTF(&gltf, instancesAttribs, fs);
    cdbTile.setCustomContentURI(gltfFile);

    if (use3dTilesNext) {
        if (cdbTile.getLevel() >= 0)
            addAvailability(cdbTile);
    }
    tileset.insertTile(cdbTile);
}

size_t CDBTilesetBuilder::hashComponentSelectors(int CS_1, int CS_2)
{
    size_t CSHash = 0;
    hashCombine(CSHash, CS_1);
    hashCombine(CSHash, CS_2);
    return CSHash;
}

std::filesystem::path CDBTilesetBuilder::getTilesetDirectory(
    int CS_1, int CS_2, const std::filesystem::path &collectionOutputDirectory)
{
    return collectionOutputDirectory / (std::to_string(CS_1) + "_" + std::to_string(CS_2));
}

void CDBTilesetBuilder::getTileset(const CDBTile &cdbTile,
                                   const std::filesystem::path &collectionOutputDirectory,
                                   std::unordered_map<CDBGeoCell, TilesetCollection> &tilesetCollections,
                                   CDBTileset *&tileset,
                                   std::filesystem::path &path)
{
    const auto &geoCell = cdbTile.getGeoCell();
    auto &tilesetCollection = tilesetCollections[geoCell];

    // find output directory
    size_t CSHash = hashComponentSelectors(cdbTile.getCS_1(), cdbTile.getCS_2());

    auto &CSToPaths = tilesetCollection.CSToPaths;
    auto CSPathIt = CSToPaths.find(CSHash);
    if (CSPathIt == CSToPaths.end()) {
        path = getTilesetDirectory(cdbTile.getCS_1(), cdbTile.getCS_2(), collectionOutputDirectory);
        std::filesystem::create_directories(path);
        CSToPaths.insert({CSHash, path});
    } else {
        path = CSPathIt->second;
    }

    tileset = &tilesetCollection.CSToTilesets[CSHash];
}