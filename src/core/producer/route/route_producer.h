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

#include <core/producer/frame_producer.h>
#include <core/frame/frame_visitor.h>

#include <string>
#include <vector>
#include <memory>
#include <boost/signals2.hpp>

namespace caspar { namespace core {

class route;
class frame_factory;
class video_format_desc;

class route_control
{
  public:
    virtual ~route_control() {}

    virtual int get_source_channel() const = 0;
    virtual int get_source_layer() const   = 0;

    virtual void set_cross_channel(bool cross) = 0;
};

class route_producer
    : public frame_producer
    , public route_control
    , public std::enable_shared_from_this<route_producer>
{
    spl::shared_ptr<diagnostics::graph> graph_;
    spl::shared_ptr<frame_factory> frame_factory_;

    tbb::concurrent_bounded_queue<std::pair<core::draw_frame, core::draw_frame>> buffer_;

    caspar::timer produce_timer_;
    caspar::timer consume_timer_;

    std::shared_ptr<route> route_;
    const video_format_desc format_desc_;

    std::optional<std::pair<core::draw_frame, core::draw_frame>> frame_;
    int                                                          source_channel_;
    int                                                          source_layer_;
    fix_stream_tag                                               tag_fix_;
    core::video_format_desc                                      source_format_;
    bool                                                         is_cross_channel_ = false;

    boost::signals2::scoped_connection connection_;

    int get_source_channel() const override { return source_channel_; }
    int get_source_layer() const override { return source_layer_; }

    // set the buffer depth to 2 for cross-channel routes, 1 otherwise
    void set_cross_channel(bool cross) override
    {
        is_cross_channel_ = cross;
        if (cross) {
            buffer_.set_capacity(2);
            source_format_ = route_->format_desc;
        } else {
            buffer_.set_capacity(1);
            source_format_ = core::video_format_desc();
        }
    }

  public:
    route_producer(std::shared_ptr<route> route, video_format_desc format_desc, int buffer, int source_channel, int source_layer, const spl::shared_ptr<frame_factory>& frame_factory);
    virtual ~route_producer() {}

    void connect_slot();
    draw_frame last_frame(const core::video_field field) override;
    draw_frame receive_impl(core::video_field field, int nb_samples) override;
    bool is_ready() override { return true; }
    std::wstring print() const override { return L"route[" + route_->name + L"]"; }
    std::wstring name() const override { return L"route"; }
    core::monitor::state state() const override;
};

spl::shared_ptr<core::frame_producer> create_route_producer(const core::frame_producer_dependencies& dependencies,
                                                            const std::vector<std::wstring>&         params);

}} // namespace caspar::core
