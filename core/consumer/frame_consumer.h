/*
* Copyright (c) 2011 Sveriges Television AB <info@casparcg.com>
*
* This file is part of CasparCG (www.casparcg.com).
*
* CasparCG is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* CasparCG is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with CasparCG. If not, see <http://www.gnu.org/licenses/>.
*
* Author: Robert Nagy, ronag89@gmail.com
*/

#pragma once

#include <common/no_copy.h>
#include <common/memory/safe_ptr.h>

#include <boost/property_tree/ptree_fwd.hpp>

#include <functional>
#include <string>
#include <vector>

namespace caspar { namespace core {
	
struct frame_consumer
{
	CASPAR_NO_COPY(frame_consumer);

	frame_consumer(){}
	virtual ~frame_consumer() {}
	
	virtual bool send(const safe_ptr<class read_frame>& frame) = 0;
	virtual void initialize(const struct video_format_desc& format_desc, int channel_index) = 0;
	virtual std::wstring print() const = 0;
	virtual boost::property_tree::wptree info() const = 0;
	virtual bool has_synchronization_clock() const {return true;}
	virtual int buffer_depth() const = 0;
	virtual int index() const = 0;

	static const safe_ptr<frame_consumer>& empty();
};

safe_ptr<frame_consumer> create_consumer_cadence_guard(const safe_ptr<frame_consumer>& consumer);

typedef std::function<safe_ptr<frame_consumer>(const std::vector<std::wstring>&)> consumer_factory_t;

void register_consumer_factory(const consumer_factory_t& factory);
safe_ptr<frame_consumer> create_consumer(const std::vector<std::wstring>& params);

}}