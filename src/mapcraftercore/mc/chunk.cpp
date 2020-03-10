/*
 * Copyright 2012-2016 Moritz Hilscher
 *
 * This file is part of Mapcrafter.
 *
 * Mapcrafter is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Mapcrafter is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Mapcrafter.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "chunk.h"

#include <cmath>
#include <iostream>

namespace mapcrafter {
namespace mc {

const uint8_t* ChunkSection::getArray(int i) const {
	if (i == 0)
		return data;
	else if (i == 1)
		return block_light;
	else
		return sky_light;
}

Chunk::Chunk()
	: chunkpos(42, 42), rotation(0), terrain_populated(false) {
	clear();
}

Chunk::~Chunk() {
}

void Chunk::setRotation(int rotation) {
	this->rotation = rotation;
}

void Chunk::setWorldCrop(const WorldCrop& world_crop) {
	this->world_crop = world_crop;
}

int Chunk::positionToKey(int x, int z, int y) const {
	return y + 256 * (x + 16 * z);
}

bool Chunk::readNBT(const char* data, size_t len, nbt::Compression compression) {
	clear();

	nbt::NBTFile nbt;
	nbt.readNBT(data, len, compression);

	// find "level" tag
	if (!nbt.hasTag<nbt::TagCompound>("Level")) {
		LOG(ERROR) << "Corrupt chunk: No level tag found!";
		return false;
	}
	const nbt::TagCompound& level = nbt.findTag<nbt::TagCompound>("Level");

	// then find x/z pos of the chunk
	if (!level.hasTag<nbt::TagInt>("xPos") || !level.hasTag<nbt::TagInt>("zPos")) {
		LOG(ERROR) << "Corrupt chunk: No x/z position found!";
		return false;
	}
	chunkpos_original = ChunkPos(level.findTag<nbt::TagInt>("xPos").payload,
	                             level.findTag<nbt::TagInt>("zPos").payload);
	chunkpos = chunkpos_original;
	if (rotation)
		chunkpos.rotate(rotation);

	// now we have the original chunk position:
	// check whether this chunk is completely contained within the cropped world
	chunk_completely_contained = world_crop.isChunkCompletelyContained(chunkpos_original);

	if (level.hasTag<nbt::TagByte>("TerrainPopulated"))
		terrain_populated = level.findTag<nbt::TagByte>("TerrainPopulated").payload;
	else
		LOG(ERROR) << "Corrupt chunk " << chunkpos << ": No terrain populated tag found!";

	if (level.hasArray<nbt::TagByteArray>("Biomes", 256)) {
		const nbt::TagByteArray& biomes_tag = level.findTag<nbt::TagByteArray>("Biomes");
		std::copy(biomes_tag.payload.begin(), biomes_tag.payload.end(), biomes);
	} else if (level.hasArray<nbt::TagIntArray>("Biomes", 256)) {
    // JustEnoughIDs https://www.curseforge.com/minecraft/mc-mods/jeid?page=7&sort=game-version
		const nbt::TagIntArray& biomes_tag = level.findTag<nbt::TagIntArray>("Biomes");
		std::copy(biomes_tag.payload.begin(), biomes_tag.payload.end(), biomes);
	} else {
		LOG(WARNING) << "Corrupt chunk " << chunkpos << ": No biome data found!";
	}

	const nbt::TagList& tile_entities_tag = level.findTag<nbt::TagList>("TileEntities");

	if (tile_entities_tag.tag_type == nbt::TagCompound::TAG_TYPE) {
		// go through all entities
		for (auto it = tile_entities_tag.payload.begin(); it != tile_entities_tag.payload.end(); ++it) {
			const nbt::TagCompound &entity = (*it)->cast<nbt::TagCompound>();
			std::string id = entity.findTag<nbt::TagString>("id").payload; // Not an integer, e.g. for beds: 'minecraft:bed'
			mc::BlockPos pos(
					entity.findTag<nbt::TagInt>("x").payload,
					entity.findTag<nbt::TagInt>("z").payload,
					entity.findTag<nbt::TagInt>("y").payload
			);

			if (id == "minecraft:bed") { // bed, stored as a string here
				uint16_t color = (uint16_t) entity.findTag<nbt::TagInt>("color").payload;
				insertExtraData(pos, color);
			}
		}
	}

	// find sections list
	// ignore it if section list does not exist, can happen sometimes with the empty
	// chunks of the end
	if (!level.hasList<nbt::TagCompound>("Sections"))
		return true;

	const nbt::TagList& sections_tag = level.findTag<nbt::TagList>("Sections");
	if (sections_tag.tag_type != nbt::TagCompound::TAG_TYPE) {
    LOG(WARNING) << "Could not find Sections";
		return true;
  }

	// go through all sections
	for (auto it = sections_tag.payload.begin(); it != sections_tag.payload.end(); ++it) {
		const nbt::TagCompound& section_tag = (*it)->cast<nbt::TagCompound>();

		// make sure section is valid
		if (!section_tag.hasTag<nbt::TagByte>("Y")
				|| !section_tag.hasArray<nbt::TagByteArray>("Blocks", 4096)
				|| !section_tag.hasArray<nbt::TagByteArray>("Data", 2048)
				|| !section_tag.hasArray<nbt::TagByteArray>("BlockLight", 2048)
				|| !section_tag.hasArray<nbt::TagByteArray>("SkyLight", 2048)) {
			continue;
      LOG(WARNING) << "Section not valid";
    }

		const nbt::TagByte& y = section_tag.findTag<nbt::TagByte>("Y");
		if (y.payload >= CHUNK_HEIGHT) {
      LOG(WARNING) << "Y too high";
			continue;
    }
		const nbt::TagByteArray& blocks = section_tag.findTag<nbt::TagByteArray>("Blocks");
		const nbt::TagByteArray& data = section_tag.findTag<nbt::TagByteArray>("Data");

		const nbt::TagByteArray& block_light = section_tag.findTag<nbt::TagByteArray>("BlockLight");
		const nbt::TagByteArray& sky_light = section_tag.findTag<nbt::TagByteArray>("SkyLight");

		// create a ChunkSection-object
		ChunkSection section;
		section.y = y.payload;

		if (!section_tag.hasArray<nbt::TagByteArray>("Add", 2048))
			std::fill(&section.add[0], &section.add[2048], 0);
		else {
			const nbt::TagByteArray& add = section_tag.findTag<nbt::TagByteArray>("Add");
			std::copy(add.payload.begin(), add.payload.end(), section.add);
		}

		std::copy(blocks.payload.begin(), blocks.payload.end(), section.blocks);
		std::copy(data.payload.begin(), data.payload.end(), section.data);


    if (section_tag.hasArray<nbt::TagIntArray>("Palette")) {
      // In some of our chunks, there is a palette with a dynamic number of
      // ints. This seems to be a 1.13 backport to 1.12 done by JustEnoughIDs
      // https://github.com/DimensionalDevelopment/JustEnoughIDs In others,
      // there is "Add"s
      const nbt::TagIntArray& palette = section_tag.findTag<nbt::TagIntArray>("Palette");
      int32_t plen = (unsigned) palette.payload.size();
      std::vector<uint16_t> palette_lookup(plen);

      size_t i = 0;
      for (auto pit = palette.payload.begin(); pit != palette.payload.end(); ++pit, ++i) {
        palette_lookup[i] = (*pit);
      }

      for (i=0; i<4096; ++i) {
        uint8_t d = 0;
        // Read data and zero it for now.
        if (i % 2 == 0) {
          d = section.data[i / 2] & 0x0f;
          section.data[i / 2] &= 0xf0;
        } else {
          d = ( section.data[i / 2] >> 4 ) & 0x0f;
          section.data[i / 2] &= 0x0f;
        }
        int32_t paletteId = (section.blocks[i] & 255) << 4 | d;
        if (paletteId > plen) {
          LOG(WARNING) << "Looking up Palette outside of boundaries at " << paletteId << " of " << plen;
        }
        int32_t val = palette_lookup[paletteId];
        if (val >= 4096) {
          switch(val) {
            // leaves
          case 35952:
            val = 18;
            break;
          case 35954:
            val = 18;
            break;
          case 35856:
            val = 18;
            break;
          case 35858:
            val = 18;
            break;
          case 35890:
            val = 18;
            break;
          case 35888:
            val = 18;
            break;
          case 35920:
            val = 18;
            break;
          case 35922:
            val = 18;
            break;
          case 14179:
            val = 18;
            break;
          case 14187:
            val = 18;
            break;
            // wood
          case 51971:
            val = 5;
            break;
            // unknown
          case 14131:
            val = 0;
            break;
          case 4156:
            val = 31;
            break;
          case 4159:
            val = 31;
            break;
          case 4161:
            val = 162;
            break;
          case 16726:
            val = 0;
            break;
          case 51955:
            val = 0;
            break;
          case 5536:
            val = 0;
            break;
          case 10487:
            val = 0;
            break;
          case 10535:
            val = 0;
            break;
          case 8103:
            val = 0;
            break;
          case 4202: // some plant
            val = 175;
            break;
          case 4104: // another plant?
            val = 175;
            break;
          case 6963: // more plants
            val = 175;
            break;
          case 51952:
            val = 175;
            break;
          case 4146:
            val = 175;
            break;
          case 4160:
            val = 175;
            break;
          case 51969:
            val = 175;
            break;
          case 51954:
            val = 175;
            break;
          case 4201:
            val = 175;
            break;
          case 4184:
            val = 175;
            break;
          case 4154:
            val = 175;
            break;
          case 4155:
            val = 175;
            break;
          case 4219: // top of 2-block high thinies
            val = 174;
            break;
          case 4235:
            val = 174;
            break;
          case 4212: // and the bottom
            val = 174;
            break;
          case 4206:
            val = 0;
            break;
          case 4180:
            val = 0;
            break;
          case 4185:
            val = 0;
            break;
          case 4209:
            val = 0;
            break;
          case 4228:
            val = 0;
            break;
          case 16722:
            val = 0;
            break;
          case 51953:
            val = 0;
            break;
          case 38112:
            val = 0;
            break;
          case 4193:
            val = 0;
            break;
          case 4168:
            val = 0;
            break;
          case 4227:
            val = 0;
            break;
          case 4194:
            val = 0;
            break;
          case 4145:
            val = 0;
            break;
          case 4200:
            val = 0;
            break;
          case 51968:
            val = 0;
            break;
          case 51970:
            val = 0;
            break;
          case 4151:
            val = 0;
            break;
          case 4210:
            val = 0;
            break;
          case 4167:
            val = 0;
            break;
          case 4197:
            val = 0;
            break;
          case 4192:
            val = 0;
            break;
          case 4196:
            val = 0;
            break;
          case 4170:
            val = 0;
            break;
            // TODO
          case 99999:
            val = 26;
            break;
          default:
            // .... .... .... ....
            // DATA BLOCK-ID_ GARB
            //LOG(WARNING) << "Unknown (modded) block id: " << val;
            if (i % 2 == 0) {
              section.data[i / 2] |= val >> 12 & 0x0f;
            } else {
              section.data[i / 2] |= val >> 8 & 0xf0;
            }
            val = val >> 4 & 0xff;
          }
          section.blocks[i] = 0;
        } else {
          section.blocks[i] = val >> 4;
        }
      }
    }


		std::copy(block_light.payload.begin(), block_light.payload.end(), section.block_light);
		std::copy(sky_light.payload.begin(), sky_light.payload.end(), section.sky_light);

		// add this section to the section list
		section_offsets[section.y] = sections.size();
		sections.push_back(section);
	}

	return true;
}

void Chunk::clear() {
	sections.clear();
	for (int i = 0; i < CHUNK_HEIGHT; i++)
		section_offsets[i] = -1;
}

bool Chunk::hasSection(int section) const {
	return section < CHUNK_HEIGHT && section_offsets[section] != -1;
}

void rotateBlockPos(int& x, int& z, int rotation) {
	int nx = x, nz = z;
	for (int i = 0; i < rotation; i++) {
		nx = z;
		nz = 15 - x;
		x = nx;
		z = nz;
	}
}

uint16_t Chunk::getBlockID(const LocalBlockPos& pos, bool force) const {
	// at first find out the section and check if it's valid and contained
	int section = pos.y / 16;
	if (section >= CHUNK_HEIGHT || section_offsets[section] == -1)
		return 0;
	// FIXME sometimes this happens, fix this
	//if (sections.size() > 16 || sections.size() <= (unsigned) section_offsets[section]) {
	//	return 0;
	//}

	// if rotated: rotate position to position with original rotation
	int x = pos.x;
	int z = pos.z;
	if (rotation)
		rotateBlockPos(x, z, rotation);

	// check whether this block is really rendered
	if (!checkBlockWorldCrop(x, z, pos.y))
		return 0;

	// calculate the offset and get the block ID
	// and don't forget the add data
	int offset = ((pos.y % 16) * 16 + z) * 16 + x;
	uint16_t add = 0;
	if ((offset % 2) == 0)
		add = sections[section_offsets[section]].add[offset / 2] & 0xf;
	else
		add = (sections[section_offsets[section]].add[offset / 2] >> 4) & 0x0f;
	uint16_t id = sections[section_offsets[section]].blocks[offset] + (add << 8);
	if (!force && world_crop.hasBlockMask()) {
		const BlockMask* mask = world_crop.getBlockMask();
		BlockMask::BlockState block_state = mask->getBlockState(id);
		if (block_state == BlockMask::BlockState::COMPLETELY_HIDDEN)
			return 0;
		else if (block_state == BlockMask::BlockState::COMPLETELY_SHOWN)
			return id;
		if (mask->isHidden(id, getBlockData(pos, true)))
			return 0;
	}
	return id;
}

bool Chunk::checkBlockWorldCrop(int x, int z, int y) const {
	// first of all check if we should crop unpopulated chunks
	if (!terrain_populated && world_crop.hasCropUnpopulatedChunks())
		return false;
	// now about the actual world cropping:
	// get the global position of the block, with the original world rotation
	BlockPos global_pos = LocalBlockPos(x, z, y).toGlobalPos(chunkpos_original);
	// check whether the block is contained in the y-bounds.
	if (!world_crop.isBlockContainedY(global_pos))
		return false;
	// only check x/z-bounds if the chunk is not completely contained
	if (!chunk_completely_contained && !world_crop.isBlockContainedXZ(global_pos))
		return false;
	return true;
}

uint8_t Chunk::getData(const LocalBlockPos& pos, int array, bool force) const {
	// at first find out the section and check if it's valid and contained
	int section = pos.y / 16;
	if (section >= CHUNK_HEIGHT || section_offsets[section] == -1)
		// not existing sections should always have skylight
		return array == 2 ? 15 : 0;

	// if rotated: rotate position to position with original rotation
	int x = pos.x;
	int z = pos.z;
	if (rotation)
		rotateBlockPos(x, z, rotation);

	// check whether this block is really rendered
	if (!checkBlockWorldCrop(x, z, pos.y))
		return array == 2 ? 15 : 0;

	uint8_t data = 0;
	// calculate the offset and get the block data
	int offset = ((pos.y % 16) * 16 + z) * 16 + x;
	// handle bottom/top nibble
	if ((offset % 2) == 0)
		data = sections[section_offsets[section]].getArray(array)[offset / 2] & 0xf;
	else
		data = (sections[section_offsets[section]].getArray(array)[offset / 2] >> 4) & 0x0f;
	if (!force && world_crop.hasBlockMask()) {
		const BlockMask* mask = world_crop.getBlockMask();
		if (mask->isHidden(getBlockID(pos, true), data))
			return array == 2 ? 15 : 0;
	}
	return data;
}

uint16_t Chunk::getBlockExtraData(const LocalBlockPos& pos, uint16_t id) const {
	if (id == 26) {
		return getExtraData(pos, 14); // Default is red
	}

	return 0;
}

uint8_t Chunk::getBlockData(const LocalBlockPos& pos, bool force) const {
	return getData(pos, 0, force);
}

uint8_t Chunk::getBlockLight(const LocalBlockPos& pos) const {
	return getData(pos, 1);
}

uint8_t Chunk::getSkyLight(const LocalBlockPos& pos) const {
	return getData(pos, 2);
}

uint8_t Chunk::getBiomeAt(const LocalBlockPos& pos) const {
	int x = pos.x;
	int z = pos.z;
	if (rotation)
		rotateBlockPos(x, z, rotation);

	return biomes[z * 16 + x];
}

const ChunkPos& Chunk::getPos() const {
	return chunkpos;
}

void Chunk::insertExtraData(const LocalBlockPos &pos, uint16_t extra_data) {
	int key = positionToKey(pos.x, pos.z, pos.y);
	std::pair<int,uint16_t> pair (key, extra_data);
	extra_data_map.insert(pair);
}

uint16_t Chunk::getExtraData(const LocalBlockPos &pos, uint16_t default_value) const {
	int x = pos.x;
	int z = pos.z;
	if (rotation)
		rotateBlockPos(x, z, rotation);
	int key = positionToKey(x, z, pos.y);

	auto result = extra_data_map.find(key);
	if (result == extra_data_map.end()) {
		// Not found, possibly from an old world
		// Default value is 14 = red
		return default_value;
	}

	return result->second;
}

}
}
