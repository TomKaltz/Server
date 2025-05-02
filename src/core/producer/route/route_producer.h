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

#include <string>
#include <vector>

namespace caspar { namespace core {

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
    route_producer(std::shared_ptr<route> route, video_format_desc format_desc, int buffer, int source_channel, int source_layer, const spl::shared_ptr<frame_factory>& frame_factory)
        : route_(route)
        , format_desc_(format_desc)
        , source_channel_(source_channel)
        , source_layer_(source_layer)
        , tag_fix_(this)
        , frame_factory_(frame_factory)
    {
        graph_ = spl::make_shared<diagnostics::graph>();
        buffer_.set_capacity(buffer > 0 ? buffer : 1);

        graph_->set_color("late-frame", diagnostics::color(0.6f, 0.3f, 0.3f));
        graph_->set_color("produce-time", caspar::diagnostics::color(0.0f, 1.0f, 0.0f));
        graph_->set_color("consume-time", caspar::diagnostics::color(1.0f, 0.4f, 0.0f, 0.8f));
        graph_->set_color("dropped-frame", diagnostics::color(0.3f, 0.6f, 0.3f));
        graph_->set_text(print());
     
        CASPAR_LOG(debug) << print() << L" Initialized";
    }

    virtual ~route_producer() {}
};

spl::shared_ptr<core::frame_producer> create_route_producer(const core::frame_producer_dependencies& dependencies,
                                                            const std::vector<std::wstring>&         params);

}} // namespace caspar::core
