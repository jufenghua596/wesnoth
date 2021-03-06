/*
   Copyright (C) 2003 - 2016 by David White <dave@whitevine.net>
   Part of the Battle for Wesnoth Project http://www.wesnoth.org/

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.
   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY.

   See the COPYING file for more details.
*/

/**
 * @file
 * Routines related to game-maps, terrain, locations, directions. etc.
 */

#include "map/map.hpp"

#include "global.hpp"

#include "config.hpp"
#include "formula/string_utils.hpp"
#include "log.hpp"
#include "map/exception.hpp"
#include "serialization/parser.hpp"
#include "serialization/string_utils.hpp"
#include "terrain/terrain.hpp"
#include "terrain/type_data.hpp"
#include "wml_exception.hpp"

#include <algorithm>
#include <sstream>
#include <utility>

#include <boost/optional.hpp>

static lg::log_domain log_config("config");
#define ERR_CF LOG_STREAM(err, log_config)
#define LOG_G LOG_STREAM(info, lg::general())
#define DBG_G LOG_STREAM(debug, lg::general())

/** Gets the list of terrains. */
const t_translation::t_list& gamemap::get_terrain_list() const
{
	return tdata_->list();
}

/** Shortcut to get_terrain_info(get_terrain(loc)). */
const terrain_type& gamemap::get_terrain_info(const map_location &loc) const
{
	return tdata_->get_terrain_info(get_terrain(loc));
}

const t_translation::t_list& gamemap::underlying_mvt_terrain(const map_location& loc) const
	{ return underlying_mvt_terrain(get_terrain(loc)); }
const t_translation::t_list& gamemap::underlying_def_terrain(const map_location& loc) const
	{ return underlying_def_terrain(get_terrain(loc)); }
const t_translation::t_list& gamemap::underlying_union_terrain(const map_location& loc) const
	{ return underlying_union_terrain(get_terrain(loc)); }
std::string gamemap::get_terrain_string(const map_location& loc) const
	{ return get_terrain_string(get_terrain(loc)); }
std::string gamemap::get_terrain_editor_string(const map_location& loc) const
	{ return get_terrain_editor_string(get_terrain(loc)); }

bool gamemap::is_village(const map_location& loc) const
	{ return on_board(loc) && is_village(get_terrain(loc)); }
int gamemap::gives_healing(const map_location& loc) const
	{ return on_board(loc) ?  gives_healing(get_terrain(loc)) : 0; }
bool gamemap::is_castle(const map_location& loc) const
	{ return on_board(loc) && is_castle(get_terrain(loc)); }
bool gamemap::is_keep(const map_location& loc) const
	{ return on_board(loc) && is_keep(get_terrain(loc)); }


/* Forwarded methods of tdata_ */
const t_translation::t_list& gamemap::underlying_mvt_terrain(const t_translation::t_terrain & terrain) const
	{ return tdata_->underlying_mvt_terrain(terrain); }
const t_translation::t_list& gamemap::underlying_def_terrain(const t_translation::t_terrain & terrain) const
	{ return tdata_->underlying_def_terrain(terrain); }
const t_translation::t_list& gamemap::underlying_union_terrain(const t_translation::t_terrain & terrain) const
	{ return tdata_->underlying_union_terrain(terrain); }
std::string gamemap::get_terrain_string(const t_translation::t_terrain & terrain) const
	{ return tdata_->get_terrain_string(terrain); }
std::string gamemap::get_terrain_editor_string(const t_translation::t_terrain & terrain) const
	{ return tdata_->get_terrain_editor_string(terrain); }
std::string gamemap::get_underlying_terrain_string(const t_translation::t_terrain& terrain) const
	{ return tdata_->get_underlying_terrain_string(terrain); }
bool gamemap::is_village(const t_translation::t_terrain & terrain) const
	{ return tdata_->get_terrain_info(terrain).is_village(); }
int gamemap::gives_healing(const t_translation::t_terrain & terrain) const
	{ return tdata_->get_terrain_info(terrain).gives_healing(); }
bool gamemap::is_castle(const t_translation::t_terrain & terrain) const
	{ return tdata_->get_terrain_info(terrain).is_castle(); }
bool gamemap::is_keep(const t_translation::t_terrain & terrain) const
	{ return tdata_->get_terrain_info(terrain).is_keep(); }

const terrain_type& gamemap::get_terrain_info(const t_translation::t_terrain & terrain) const
	{ return tdata_->get_terrain_info(terrain); }

void gamemap::write_terrain(const map_location &loc, config& cfg) const
{
	cfg["terrain"] = t_translation::write_terrain_code(get_terrain(loc));
}

gamemap::gamemap(const tdata_cache& tdata, const std::string& data):
		tiles_(1),
		tdata_(tdata),
		villages_(),
		borderCache_(),
		terrainFrequencyCache_(),
		w_(-1),
		h_(-1),
		total_width_(0),
		total_height_(0),
		border_size_(default_border)
{
	DBG_G << "loading map: '" << data << "'\n";

	read(data);
}

gamemap::gamemap(const tdata_cache& tdata, const config& level):
		tiles_(1),
		tdata_(tdata),
		villages_(),
		borderCache_(),
		terrainFrequencyCache_(),
		w_(-1),
		h_(-1),
		total_width_(0),
		total_height_(0),
		border_size_(default_border)
{
	DBG_G << "loading map: '" << level.debug() << "'\n";

	const std::string& map_data = level["map_data"];
	if (!map_data.empty()) {
		read(map_data);
	} else {
		w_ = 0;
		h_ = 0;
		total_width_ = 0;
		total_height_ = 0;
	}
}

gamemap::~gamemap()
{
}

void gamemap::read(const std::string& data, const bool allow_invalid, int border_size) {

	// Initial stuff
	border_size_ = border_size;
	tiles_.clear();
	villages_.clear();
	starting_positions_.clear();
	std::map<int, t_translation::coordinate> starting_positions;

	if(data.empty()) {
		w_ = 0;
		h_ = 0;
		total_width_ = 0;
		total_height_ = 0;
		if(allow_invalid) return;
	}

	int offset = read_header(data);

	const std::string& data_only = std::string(data, offset);

	try {
		tiles_ = t_translation::read_game_map(data_only, starting_positions);

	} catch(t_translation::error& e) {
		// We re-throw the error but as map error.
		// Since all codepaths test for this, it's the least work.
		throw incorrect_map_format_error(e.message);
	}

	// Convert the starting positions to the array
	int max_stating_pos = 0;
	for(const auto& pair : starting_positions) {
		// Check for valid position,
		// the first valid position is 1,
		// so the offset 0 in the array is never used.
		if(pair.first < 1 || pair.first >= MAX_PLAYERS+1) {
			std::stringstream ss;
			ss << "Starting position " << pair.first << " out of range\n";
			ERR_CF << ss.str();
			ss << "The map cannot be loaded.";
			throw incorrect_map_format_error(ss.str().c_str());
		}

		// Add to the starting position array
		max_stating_pos = std::max<int>(max_stating_pos, pair.first);
	}
	starting_positions_.resize(max_stating_pos);
	for(const auto& pair : starting_positions) {
		starting_positions_[pair.first - 1] =  map_location(pair.second.x - border_size_, pair.second.y - border_size_);
	}
	// Post processing on the map
	total_width_ = tiles_.size();
	total_height_ = total_width_ > 0 ? tiles_[0].size() : 0;
	w_ = total_width_ - 2 * border_size_;
	h_ = total_height_ - 2 * border_size_;
	//Disabled since there are callcases which pass along a valid map header but empty
	//map data. Still, loading (and actually applying) an empty map causes problems later on.
	//Other callcases which need to load a dummy map use completely empty data :(.
	//VALIDATE((w_ >= 1 && h_ >= 1), "A map needs at least 1 tile, the map cannot be loaded.");

	for(int x = 0; x < total_width_; ++x) {
		for(int y = 0; y < total_height_; ++y) {

			// Is the terrain valid?
			if(tdata_->map().count(tiles_[x][y]) == 0) {
				if(!tdata_->try_merge_terrains(tiles_[x][y])) {
					std::stringstream ss;
					ss << "Illegal tile in map: (" << t_translation::write_terrain_code(tiles_[x][y])
						   << ") '" << tiles_[x][y] << "'\n";
					ERR_CF << ss.str();
					ss << "The map cannot be loaded.";
					throw incorrect_map_format_error(ss.str().c_str());
				}
			}

			// Is it a village?
			if(x >= border_size_ && y >= border_size_
					&& x < total_width_-border_size_  && y < total_height_-border_size_
					&& tdata_->is_village(tiles_[x][y])) {
				villages_.push_back(map_location(x-border_size_, y-border_size_));
			}
		}
	}
}

int gamemap::read_header(const std::string& data)
{
	// Test whether there is a header section
	size_t header_offset = data.find("\n\n");
	if(header_offset == std::string::npos) {
		// For some reason Windows will fail to load a file with \r\n
		// lineending properly no problems on Linux with those files.
		// This workaround fixes the problem the copy later will copy
		// the second \r\n to the map, but that's no problem.
		header_offset = data.find("\r\n\r\n");
	}
	const size_t comma_offset = data.find(",");
	// The header shouldn't contain commas, so if the comma is found
	// before the header, we hit a \n\n inside or after a map.
	// This is no header, so don't parse it as it would be.

	if (!(!(header_offset == std::string::npos || comma_offset < header_offset)))
		return 0;

	std::string header_str(std::string(data, 0, header_offset + 1));
	config header;
	::read(header, header_str);

	border_size_ = header["border_size"];

	return header_offset + 2;
}


std::string gamemap::write() const
{
	// Convert the starting positions to a map
	std::map<int, t_translation::coordinate> starting_positions;
	
	for(int i = 0, size = starting_positions_.size(); i < size; ++i) {
		if(on_board(starting_positions_[i])) {
			starting_positions[i + 1] = t_translation::coordinate(
				starting_positions_[i].x + border_size_,
				starting_positions_[i].y + border_size_
			);
		}	
	}

	// Let the low level converter do the conversion
	std::ostringstream s;
	s << t_translation::write_game_map(tiles_, starting_positions)
		<< "\n";
	return s.str();
}
namespace
{
	struct overlay_rule
	{
		t_translation::t_list old_;
		t_translation::t_list new_;
		terrain_type_data::tmerge_mode mode_;
		boost::optional<t_translation::t_terrain> terrain_;
		bool use_old_;
		bool replace_if_failed_;

		overlay_rule()
			: old_()
			, new_()
			, mode_(terrain_type_data::BOTH)
			, terrain_()
			, use_old_(false)
			, replace_if_failed_(false)
		{

		}
	};
}
void gamemap::overlay(const gamemap& m, const config& rules_cfg, int xpos, int ypos, bool border)
{
	//const config::const_child_itors &rules = rules_cfg.child_range("rule");
	std::vector<overlay_rule> rules(rules_cfg.child_count("rule"));
	for(size_t i = 0; i <rules.size(); ++i)
	{
		const config& cfg = rules_cfg.child("rule", i);
		rules[i].old_ = t_translation::read_list(cfg["old"]);
		rules[i].new_ = t_translation::read_list(cfg["new"]);
		rules[i].mode_ = cfg["layer"] == "base" ? terrain_type_data::BASE : cfg["layer"] == "overlay" ? terrain_type_data::OVERLAY : terrain_type_data::BOTH;
		const t_translation::t_list& terrain = t_translation::read_list(cfg["terrain"]);
		if(!terrain.empty()) {
			rules[i].terrain_ = terrain[0];
		}
		rules[i].use_old_ = cfg["use_old"].to_bool();
		rules[i].replace_if_failed_ = cfg["replace_if_failed"].to_bool();
	}
	
	int actual_border = (m.border_size() == border_size()) && border ? border_size() : 0;

	const int xstart = std::max<int>(-actual_border, -xpos - actual_border);
	const int ystart = std::max<int>(-actual_border, -ypos - actual_border - ((xpos & 1) ? 1 : 0));
	const int xend = std::min<int>(m.w() + actual_border, w() + actual_border - xpos);
	const int yend = std::min<int>(m.h() + actual_border, h() + actual_border - ypos);

	for(int x1 = xstart; x1 < xend; ++x1) {
		for(int y1 = ystart; y1 < yend; ++y1) {
			const int x2 = x1 + xpos;
			const int y2 = y1 + ypos + ((xpos & 1) && (x1 & 1) ? 1 : 0);

			const t_translation::t_terrain t = m.tiles_[x1 + m.border_size_][y1 + m.border_size_];
			const t_translation::t_terrain current = tiles_[x2 + border_size_][y2 + border_size_];

			if(t == t_translation::FOGGED || t == t_translation::VOID_TERRAIN) {
				continue;
			}

			// See if there is a matching rule
			const overlay_rule* rule = nullptr;
			for(const overlay_rule& current_rule : rules)
			{
				if(!current_rule.old_.empty() && !t_translation::terrain_matches(current, current_rule.old_)) {
					continue;
				}
				if(!current_rule.new_.empty() && !t_translation::terrain_matches(t, current_rule.new_)) {
					continue;
				}
				rule = &current_rule;
				break;
			}

			if (rule && !rule->use_old_) {
				set_terrain(map_location(x2, y2), rule->terrain_ ? *rule->terrain_ : t , rule->mode_, rule->replace_if_failed_);
			}
			else {
				set_terrain(map_location(x2,y2), t);
			}
		}
	}
	if (m.starting_positions_ > starting_positions_) {
		starting_positions_.resize(m.starting_positions_.size());
	}
	for(int i = 0, size = m.starting_positions_.size(); i < size; ++i) {
		if(m.starting_positions_[i].valid()) {
			starting_positions_[i] = m.starting_positions_[i];
		}	
	}
}

t_translation::t_terrain gamemap::get_terrain(const map_location& loc) const
{

	if(on_board_with_border(loc)) {
		return tiles_[loc.x + border_size_][loc.y + border_size_];
	}

	if ( loc == map_location::null_location() ) {
		return t_translation::NONE_TERRAIN;
	}

	const std::map<map_location, t_translation::t_terrain>::const_iterator itor = borderCache_.find(loc);
	if(itor != borderCache_.end())
		return itor->second;

	// If not on the board, decide based on what surrounding terrain is
	t_translation::t_terrain items[6];
	int number_of_items = 0;

	map_location adj[6];
	get_adjacent_tiles(loc,adj);
	for(int n = 0; n != 6; ++n) {
		if(on_board(adj[n])) {
			items[number_of_items] = tiles_[adj[n].x][adj[n].y];
			++number_of_items;
		} else {
			// If the terrain is off map but already in the border cache,
			// this will be used to determine the terrain.
			// This avoids glitches
			// * on map with an even width in the top right corner
			// * on map with an odd height in the bottom left corner.
			// It might also change the result on other map and become random,
			// but the border tiles will be determined in the future, so then
			// this will no longer be used in the game
			// (The editor will use this feature to expand maps in a better way).
			std::map<map_location, t_translation::t_terrain>::const_iterator itor =
				borderCache_.find(adj[n]);

			// Only add if it is in the cache and a valid terrain
			if(itor != borderCache_.end() &&
					itor->second != t_translation::NONE_TERRAIN)  {

				items[number_of_items] = itor->second;
				++number_of_items;
			}
		}

	}

	// Count all the terrain types found,
	// and see which one is the most common, and use it.
	t_translation::t_terrain used_terrain;
	int terrain_count = 0;
	for(int i = 0; i != number_of_items; ++i) {
		if(items[i] != used_terrain && !tdata_->is_village(items[i]) && !tdata_->is_keep(items[i])) {
			const int c = std::count(items+i+1,items+number_of_items,items[i]) + 1;
			if(c > terrain_count) {
				used_terrain = items[i];
				terrain_count = c;
			}
		}
	}

	borderCache_.insert(std::pair<map_location, t_translation::t_terrain>(loc,used_terrain));
	return used_terrain;

}

const map_location& gamemap::starting_position(int n) const
{
	size_t index = n - 1;
	if(index < starting_positions_.size()) {
		return starting_positions_[index];
	} else {
		static const map_location null_loc;
		return null_loc;
	}
}

int gamemap::num_valid_starting_positions() const
{
	const int res = is_starting_position(map_location());
	if(res == 0)
		return num_starting_positions();
	else
		return res;
}

int gamemap::is_starting_position(const map_location& loc) const
{
	std::vector<map_location>::const_iterator pos = std::find(starting_positions_.begin(), starting_positions_.end(), loc);

	return pos == starting_positions_.end() ? 0 : pos - starting_positions_.begin() + 1;
}

void gamemap::set_starting_position(int side, const map_location& loc)
{
	int index = side - 1;

	if(index < 0) {
		return;
	}
	if(index >= int(starting_positions_.size())) {
		starting_positions_.resize(index + 1);
	}
	starting_positions_[index] = loc;
}

bool gamemap::on_board(const map_location& loc) const
{
	return loc.valid() && loc.x < w_ && loc.y < h_;
}

bool gamemap::on_board_with_border(const map_location& loc) const
{
	return !(tiles_.empty()  ||  tiles_[0].empty())  &&  // tiles_ is not empty when initialized.
	       loc.x >= -border_size_  &&  loc.x < w_ + border_size_  &&
	       loc.y >= -border_size_  &&  loc.y < h_ + border_size_;
}

void gamemap::set_terrain(const map_location& loc, const t_translation::t_terrain & terrain, const terrain_type_data::tmerge_mode mode, bool replace_if_failed) {
	if(!on_board_with_border(loc)) {
		// off the map: ignore request
		return;
	}

	t_translation::t_terrain new_terrain = tdata_->merge_terrains(get_terrain(loc), terrain, mode, replace_if_failed);

	if(new_terrain == t_translation::NONE_TERRAIN) {
		return;
	}

	if(on_board(loc)) {
		const bool old_village = is_village(loc);
		const bool new_village = tdata_->is_village(new_terrain);

		if(old_village && !new_village) {
			villages_.erase(std::remove(villages_.begin(),villages_.end(),loc),villages_.end());
		} else if(!old_village && new_village) {
			villages_.push_back(loc);
		}
	}

	tiles_[loc.x + border_size_][loc.y + border_size_] = new_terrain;

	// Update the off-map autogenerated tiles
	map_location adj[6];
	get_adjacent_tiles(loc,adj);

	for(int n = 0; n < 6; ++n) {
		remove_from_border_cache(adj[n]);
	}
}

const std::map<t_translation::t_terrain, size_t>& gamemap::get_weighted_terrain_frequencies() const
{
	if(terrainFrequencyCache_.empty() == false) {
		return terrainFrequencyCache_;
	}

	const map_location center(w()/2,h()/2);

	const size_t furthest_distance = distance_between(map_location::ZERO(),center);

	const size_t weight_at_edge = 100;
	const size_t additional_weight_at_center = 200;

	for(size_t i = 0; i != size_t(w()); ++i) {
		for(size_t j = 0; j != size_t(h()); ++j) {
			const size_t distance = distance_between(map_location(i,j),center);
			terrainFrequencyCache_[tiles_[i + border_size_][j]] += weight_at_edge +
			    (furthest_distance-distance)*additional_weight_at_center;
		}
	}

	return terrainFrequencyCache_;
}

std::vector<map_location> gamemap::parse_location_range(const std::string &x, const std::string &y,
	bool with_border) const
{
	std::vector<map_location> res;
	const std::vector<std::string> xvals = utils::split(x);
	const std::vector<std::string> yvals = utils::split(y);
	int xmin = 1, xmax = w(), ymin = 1, ymax = h();
	if (with_border) {
		int bs = border_size();
		xmin -= bs;
		xmax += bs;
		ymin -= bs;
		ymax += bs;
	}

	for (unsigned i = 0; i < xvals.size() || i < yvals.size(); ++i)
	{
		std::pair<int,int> xrange, yrange;

		if (i < xvals.size()) {
			xrange = utils::parse_range(xvals[i]);
			if (xrange.first < xmin) xrange.first = xmin;
			if (xrange.second > xmax) xrange.second = xmax;
		} else {
			xrange.first = xmin;
			xrange.second = xmax;
		}

		if (i < yvals.size()) {
			yrange = utils::parse_range(yvals[i]);
			if (yrange.first < ymin) yrange.first = ymin;
			if (yrange.second > ymax) yrange.second = ymax;
		} else {
			yrange.first = ymin;
			yrange.second = ymax;
		}

		for(int x = xrange.first; x <= xrange.second; ++x) {
			for(int y = yrange.first; y <= yrange.second; ++y) {
				res.push_back(map_location(x-1,y-1));
			}
		}
	}
	return res;
}
