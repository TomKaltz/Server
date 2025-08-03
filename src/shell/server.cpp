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
#include "included_modules.h"

#include "server.h"

#include <accelerator/accelerator.h>

#include <common/bit_depth.h>
#include <common/env.h>
#include <common/except.h>
#include <common/memory.h>
#include <common/ptree.h>
#include <common/utf.h>

#include <core/consumer/output.h>
#include <core/diagnostics/call_context.h>
#include <core/diagnostics/osd_graph.h>
#include <core/frame/pixel_format.h>
#include <core/mixer/image/image_mixer.h>
#include <core/producer/cg_proxy.h>
#include <core/producer/color/color_producer.h>
#include <core/producer/frame_producer.h>
#include <core/video_channel.h>
#include <core/video_format.h>

#include <modules/image/consumer/image_consumer.h>

#include <protocol/amcp/AMCPCommandsImpl.h>
#include <protocol/amcp/AMCPProtocolStrategy.h>
#include <protocol/amcp/amcp_command_repository.h>
#include <protocol/amcp/amcp_shared.h>
#include <protocol/osc/client.h>
#include <protocol/util/AsyncEventServer.h>
#include <protocol/util/strategy_adapters.h>
#include <protocol/util/tokenize.h>
#include <protocol/util/websocket_monitor_client.h>
#include <protocol/util/websocket_monitor_server.h>
#include <protocol/util/websocket_server.h>

#include <boost/algorithm/string.hpp>
#include <boost/asio.hpp>
#include <boost/format.hpp>
#include <boost/property_tree/ptree.hpp>
#include <unordered_set>

#include <tbb/concurrent_hash_map.h>
#include <thread>
#include <utility>

namespace caspar {
using namespace core;
using namespace protocol;

std::shared_ptr<boost::asio::io_context> create_io_context_with_running_service()
{
    auto io_context = std::make_shared<boost::asio::io_context>();
    // To keep the io_context::run() running although no pending async
    // operations are posted.
    auto work = std::make_shared<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>>(
        boost::asio::make_work_guard(*io_context));
    auto weak_work = std::weak_ptr<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>>(work);
    auto thread    = std::make_shared<std::thread>([io_context, weak_work] {
        while (auto strong = weak_work.lock()) {
            try {
                io_context->run();
            } catch (...) {
                CASPAR_LOG_CURRENT_EXCEPTION();
            }
        }

        CASPAR_LOG(info) << "[asio] Global io_context uninitialized.";
    });

    return std::shared_ptr<boost::asio::io_context>(io_context.get(), [io_context, work, thread](void*) mutable {
        CASPAR_LOG(info) << "[asio] Shutting down global io_context.";
        work.reset();
        io_context->stop();
        if (thread->get_id() != std::this_thread::get_id())
            thread->join();
        else
            thread->detach();
    });
}

struct server::impl
{
    std::shared_ptr<boost::asio::io_context> io_context_ = create_io_context_with_running_service();
    // Dedicated IO context for monitor operations to prevent blocking
    std::shared_ptr<boost::asio::io_context>       monitor_io_context_ = create_io_context_with_running_service();
    video_format_repository                        video_format_repository_;
    accelerator::accelerator                       accelerator_;
    std::shared_ptr<amcp::amcp_command_repository> amcp_command_repo_;
    std::shared_ptr<amcp::amcp_command_repository_wrapper>         amcp_command_repo_wrapper_;
    std::shared_ptr<amcp::command_context_factory>                 amcp_context_factory_;
    std::vector<spl::shared_ptr<IO::AsyncEventServer>>             async_servers_;
    std::shared_ptr<IO::AsyncEventServer>                          primary_amcp_server_;
    std::shared_ptr<IO::websocket_server>                          websocket_server_;
    std::shared_ptr<protocol::websocket::websocket_monitor_client> websocket_monitor_client_shared_;
    std::atomic<protocol::websocket::websocket_monitor_client*>    websocket_monitor_client_{nullptr};
    std::shared_ptr<protocol::websocket::websocket_monitor_server> websocket_monitor_server_;
    std::shared_ptr<osc::client>       osc_client_ = std::make_shared<osc::client>(io_context_);
    std::vector<std::shared_ptr<void>> predefined_osc_subscriptions_;
    spl::shared_ptr<std::vector<protocol::amcp::channel_context>> channels_;
    spl::shared_ptr<core::cg_producer_registry>                   cg_registry_;
    spl::shared_ptr<core::frame_producer_registry>                producer_registry_;
    spl::shared_ptr<core::frame_consumer_registry>                consumer_registry_;
    std::function<void(bool)>                                     shutdown_server_now_;

    // NEW ARCHITECTURE: Per-channel monitor data storage (eliminates global state contention)
    struct channel_monitor_data
    {
        std::array<core::monitor::state, 2> state_buffers;
        std::atomic<int>                    active_buffer{0};
        std::atomic<bool>                   state_updated{false};
        std::string                         channel_prefix;
        double                              frame_rate{60.0};
        std::atomic<bool>                   is_fastest_channel{false};
        std::string                         current_format;
    };
    std::vector<std::unique_ptr<channel_monitor_data>> channel_monitor_data_;

    std::atomic<int>    fastest_channel_index{-1};
    std::atomic<double> global_fastest_fps{0.0};

    impl(const impl&)            = delete;
    impl& operator=(const impl&) = delete;

    explicit impl(std::function<void(bool)> shutdown_server_now)
        : video_format_repository_()
        , accelerator_(video_format_repository_)
        , producer_registry_(spl::make_shared<core::frame_producer_registry>())
        , consumer_registry_(spl::make_shared<core::frame_consumer_registry>())
        , shutdown_server_now_(std::move(shutdown_server_now))
    {
        caspar::core::diagnostics::osd::register_sink();
    }

    void start()
    {
        setup_video_modes(env::properties());
        CASPAR_LOG(info) << L"Initialized video modes.";

        // Initialize channels first to get the channels vector
        channels_ = spl::make_shared<std::vector<protocol::amcp::channel_context>>();

        setup_amcp_command_repo();
        CASPAR_LOG(info) << L"Initialized command repository.";

        // Initialize websocket monitor client (lock-free)
        websocket_monitor_client_shared_ =
            std::make_shared<protocol::websocket::websocket_monitor_client>(monitor_io_context_);
        websocket_monitor_client_.store(websocket_monitor_client_shared_.get());

        // Ensure the websocket_monitor_client is fully initialized before proceeding
        if (!websocket_monitor_client_.load()) {
            CASPAR_LOG(error) << L"Failed to create websocket_monitor_client";
        }
        CASPAR_LOG(info) << L"Initialized dedicated monitor IO context and WebSocket monitor client.";

        auto xml_channels = setup_channels(env::properties());
        CASPAR_LOG(info) << L"Initialized channels.";

        setup_websocket_controllers(env::properties());
        CASPAR_LOG(info) << L"Initialized WebSocket servers.";

        module_dependencies dependencies(
            cg_registry_, producer_registry_, consumer_registry_, amcp_command_repo_wrapper_);
        initialize_modules(dependencies);
        CASPAR_LOG(info) << L"Initialized modules.";

        setup_channel_producers_and_consumers(xml_channels);
        CASPAR_LOG(info) << L"Initialized startup consumer, producers and mixer transforms.";

        setup_controllers(env::properties());
        CASPAR_LOG(info) << L"Initialized controllers.";

        setup_osc(env::properties());
        CASPAR_LOG(info) << L"Initialized osc.";
    }

    ~impl()
    {
        std::weak_ptr<boost::asio::io_context> weak_io_context         = io_context_;
        std::weak_ptr<boost::asio::io_context> weak_monitor_io_context = monitor_io_context_;

        io_context_.reset();
        monitor_io_context_.reset();
        predefined_osc_subscriptions_.clear();
        osc_client_.reset();

        amcp_command_repo_wrapper_.reset();
        amcp_command_repo_.reset();
        amcp_context_factory_.reset();

        // Shutdown websocket monitor client (lock-free)
        auto client_ptr = websocket_monitor_client_.exchange(nullptr);
        if (client_ptr) {
            try {
                client_ptr->shutdown();
                client_ptr->join();
            } catch (const std::exception& e) {
                CASPAR_LOG(error) << L"WebSocket monitor client shutdown error: " << u16(e.what());
            }
        }

        // Stop websocket servers first
        if (websocket_monitor_server_) {
            try {
                websocket_monitor_server_->stop();
            } catch (const std::exception& e) {
                CASPAR_LOG(error) << L"WebSocket monitor server stop error: " << u16(e.what());
            }
        }

        if (websocket_server_) {
            try {
                websocket_server_->stop();
            } catch (const std::exception& e) {
                CASPAR_LOG(error) << L"WebSocket server stop error: " << u16(e.what());
            }
        }

        // Reset shared pointers
        websocket_monitor_client_shared_.reset();
        websocket_monitor_server_.reset();
        websocket_server_.reset();
        primary_amcp_server_.reset();
        async_servers_.clear();

        destroy_producers_synchronously();
        destroy_consumers_synchronously();
        channels_->clear();

        while (weak_io_context.lock() || weak_monitor_io_context.lock())
            std::this_thread::sleep_for(std::chrono::milliseconds(100));

        uninitialize_modules();
        core::diagnostics::osd::shutdown();
    }

    void setup_video_modes(const boost::property_tree::wptree& pt)
    {
        using boost::property_tree::wptree;

        auto videomodes_config = pt.get_child_optional(L"configuration.video-modes");
        if (videomodes_config) {
            for (auto& xml_channel :
                 pt | witerate_children(L"configuration.video-modes") | welement_context_iteration) {
                ptree_verify_element_name(xml_channel, L"video-mode");

                const std::wstring id = xml_channel.second.get(L"id", L"");
                if (id == L"")
                    CASPAR_THROW_EXCEPTION(user_error() << msg_info(L"Invalid video-mode id: " + id));

                const int width  = xml_channel.second.get<int>(L"width", 0);
                const int height = xml_channel.second.get<int>(L"height", 0);
                if (width == 0 || height == 0)
                    CASPAR_THROW_EXCEPTION(user_error() << msg_info(L"Invalid dimensions: " +
                                                                    boost::lexical_cast<std::wstring>(width) + L"x" +
                                                                    boost::lexical_cast<std::wstring>(height)));

                const int field_count = xml_channel.second.get<int>(L"field-count", 1);
                if (field_count != 1 && field_count != 2)
                    CASPAR_THROW_EXCEPTION(user_error() << msg_info(L"Invalid field-count: " +
                                                                    boost::lexical_cast<std::wstring>(field_count)));

                const int timescale = xml_channel.second.get<int>(L"time-scale", 60000);
                const int duration  = xml_channel.second.get<int>(L"duration", 1000);
                if (timescale == 0 || duration == 0)
                    CASPAR_THROW_EXCEPTION(
                        user_error() << msg_info(L"Invalid framerate: " + boost::lexical_cast<std::wstring>(timescale) +
                                                 L"/" + boost::lexical_cast<std::wstring>(duration)));

                std::vector<int> cadence;
                int              cadence_sum = 0;

                const std::wstring      cadence_str = xml_channel.second.get(L"cadence", L"");
                std::list<std::wstring> cadence_parts;
                boost::split(cadence_parts, cadence_str, boost::is_any_of(L", "));

                for (auto& cad : cadence_parts) {
                    if (cad.empty())
                        continue;

                    const int c = std::stoi(cad);
                    cadence.push_back(c);
                    cadence_sum += c;
                }

                if (cadence.empty()) {
                    // Attempt to calculate in the cadence for integer formats
                    const int c = static_cast<int>(48000 / (static_cast<double>(timescale) / duration) + 0.5);
                    cadence.push_back(c);
                    cadence_sum += c;
                }

                if (cadence_sum * timescale != 48000 * duration * cadence.size()) {
                    auto samples_per_second =
                        static_cast<double>(cadence_sum * timescale) / (duration * cadence.size());
                    CASPAR_THROW_EXCEPTION(user_error() << msg_info(L"Incorrect cadence in video-mode " + id +
                                                                    L". Got " + std::to_wstring(samples_per_second) +
                                                                    L" samples per second, expected 48000"));
                }

                const auto new_format = video_format_desc(
                    video_format::custom, field_count, width, height, width, height, timescale, duration, id, cadence);

                const auto existing = video_format_repository_.find(id);
                if (existing.format != video_format::invalid)
                    CASPAR_THROW_EXCEPTION(user_error() << msg_info(L"Video-mode already exists: " + id));

                video_format_repository_.store(new_format);
            }
        }
    }

    std::vector<boost::property_tree::wptree> setup_channels(const boost::property_tree::wptree& pt)
    {
        using boost::property_tree::wptree;

        std::vector<wptree> xml_channels;

        for (auto& xml_channel : pt | witerate_children(L"configuration.channels") | welement_context_iteration) {
            xml_channels.push_back(xml_channel.second);
            ptree_verify_element_name(xml_channel, L"channel");

            auto format_desc_str = xml_channel.second.get(L"video-mode", L"PAL");
            auto format_desc     = video_format_repository_.find(format_desc_str);
            auto color_depth     = xml_channel.second.get<unsigned char>(L"color-depth", 8);
            if (color_depth != 8 && color_depth != 16)
                CASPAR_THROW_EXCEPTION(user_error()
                                       << msg_info(L"Invalid color-depth: " + std::to_wstring(color_depth)));

            auto color_space_str = boost::to_lower_copy(xml_channel.second.get(L"color-space", L"bt709"));
            if (color_space_str != L"bt709" && color_space_str != L"bt2020")
                CASPAR_THROW_EXCEPTION(user_error() << msg_info(L"Invalid color-space, must be bt709 or bt2020"));

            if (format_desc.format == video_format::invalid)
                CASPAR_THROW_EXCEPTION(user_error() << msg_info(L"Invalid video-mode: " + format_desc_str));

            auto weak_client = std::weak_ptr<osc::client>(osc_client_);
            auto channel_id  = static_cast<int>(channels_->size() + 1);
            auto depth       = color_depth == 16 ? common::bit_depth::bit16 : common::bit_depth::bit8;
            auto default_color_space =
                color_space_str == L"bt2020" ? core::color_space::bt2020 : core::color_space::bt709;

            auto channel_monitor            = std::make_unique<channel_monitor_data>();
            channel_monitor->channel_prefix = "channel/" + std::to_string(channel_id) + "/";
            channel_monitor->frame_rate     = format_desc.fps;
            channel_monitor->current_format = u8(format_desc.name);

            auto* channel_monitor_ptr = channel_monitor.get();
            channel_monitor_data_.push_back(std::move(channel_monitor));

            double current_fastest = global_fastest_fps.load();
            if (format_desc.fps > current_fastest) {
                if (global_fastest_fps.compare_exchange_strong(current_fastest, format_desc.fps)) {
                    int old_fastest = fastest_channel_index.exchange(channel_id - 1);

                    if (old_fastest >= 0 && old_fastest < static_cast<int>(channel_monitor_data_.size())) {
                        channel_monitor_data_[old_fastest]->is_fastest_channel.store(false);
                    }

                    channel_monitor_ptr->is_fastest_channel.store(true);
                    CASPAR_LOG(info) << L"Channel " << channel_id << L" is initial fastest channel at "
                                     << format_desc.fps << L" fps";
                }
            }

            auto channel = spl::make_shared<video_channel>(
                channel_id,
                format_desc,
                default_color_space,
                accelerator_.create_image_mixer(channel_id, depth),
                [this, channel_id, weak_client, channel_monitor_ptr](core::monitor::state channel_state) {
                    bool format_changed = false;

                    for (const auto& [path, values] : channel_state) {
                        if (path == "format" && !values.empty()) {
                            try {
                                std::string new_format;
                                bool        format_found = false;

                                if (const auto* str_val = boost::get<std::string>(&values[0])) {
                                    new_format   = *str_val;
                                    format_found = true;
                                } else if (const auto* wstr_val = boost::get<std::wstring>(&values[0])) {
                                    new_format   = u8(*wstr_val);
                                    format_found = true;
                                } else {
                                    CASPAR_LOG(warning) << L"Unexpected format type for channel " << channel_id;
                                    continue;
                                }

                                if (format_found && new_format != channel_monitor_ptr->current_format) {
                                    format_changed                      = true;
                                    channel_monitor_ptr->current_format = new_format;
                                }
                            } catch (const std::exception& e) {
                                CASPAR_LOG(error)
                                    << L"Error extracting format for channel " << channel_id << L": " << u16(e.what());

                                // Log the actual type for debugging
                                if (!values.empty()) {
                                    CASPAR_LOG(debug) << L"Format value type: "
                                                      << (boost::get<std::int32_t>(&values[0])    ? L"int32"
                                                          : boost::get<std::int64_t>(&values[0])  ? L"int64"
                                                          : boost::get<std::uint32_t>(&values[0]) ? L"uint32"
                                                          : boost::get<std::uint64_t>(&values[0]) ? L"uint64"
                                                          : boost::get<double>(&values[0])        ? L"double"
                                                          : boost::get<float>(&values[0])         ? L"float"
                                                          : boost::get<std::string>(&values[0])   ? L"string"
                                                          : boost::get<std::wstring>(&values[0])  ? L"wstring"
                                                          : boost::get<bool>(&values[0])          ? L"bool"
                                                                                                  : L"unknown");
                                }
                            }
                        }
                    }

                    if (format_changed) {
                        for (const auto& [path, values] : channel_state) {
                            if (path == "framerate" && values.size() >= 2) {
                                try {
                                    int  numerator = 0, denominator = 0;
                                    bool numerator_found = false, denominator_found = false;

                                    // Try different integer types for numerator
                                    if (const auto* i32_val = boost::get<std::int32_t>(&values[0])) {
                                        numerator       = *i32_val;
                                        numerator_found = true;
                                    } else if (const auto* i64_val = boost::get<std::int64_t>(&values[0])) {
                                        numerator       = static_cast<int>(*i64_val);
                                        numerator_found = true;
                                    } else if (const auto* ui32_val = boost::get<std::uint32_t>(&values[0])) {
                                        numerator       = static_cast<int>(*ui32_val);
                                        numerator_found = true;
                                    } else if (const auto* ui64_val = boost::get<std::uint64_t>(&values[0])) {
                                        numerator       = static_cast<int>(*ui64_val);
                                        numerator_found = true;
                                    } else if (const auto* dbl_val = boost::get<double>(&values[0])) {
                                        numerator       = static_cast<int>(*dbl_val);
                                        numerator_found = true;
                                    } else if (const auto* flt_val = boost::get<float>(&values[0])) {
                                        numerator       = static_cast<int>(*flt_val);
                                        numerator_found = true;
                                    }

                                    // Try different integer types for denominator
                                    if (const auto* i32_val = boost::get<std::int32_t>(&values[1])) {
                                        denominator       = *i32_val;
                                        denominator_found = true;
                                    } else if (const auto* i64_val = boost::get<std::int64_t>(&values[1])) {
                                        denominator       = static_cast<int>(*i64_val);
                                        denominator_found = true;
                                    } else if (const auto* ui32_val = boost::get<std::uint32_t>(&values[1])) {
                                        denominator       = static_cast<int>(*ui32_val);
                                        denominator_found = true;
                                    } else if (const auto* ui64_val = boost::get<std::uint64_t>(&values[1])) {
                                        denominator       = static_cast<int>(*ui64_val);
                                        denominator_found = true;
                                    } else if (const auto* dbl_val = boost::get<double>(&values[1])) {
                                        denominator       = static_cast<int>(*dbl_val);
                                        denominator_found = true;
                                    } else if (const auto* flt_val = boost::get<float>(&values[1])) {
                                        denominator       = static_cast<int>(*flt_val);
                                        denominator_found = true;
                                    }

                                    if (numerator_found && denominator_found && denominator > 0) {
                                        double new_fps                  = static_cast<double>(numerator) / denominator;
                                        channel_monitor_ptr->frame_rate = new_fps;
                                        CASPAR_LOG(info)
                                            << L"Channel " << channel_id << L" framerate: " << new_fps << L" fps";

                                        double current_fastest = global_fastest_fps.load();
                                        if (new_fps > current_fastest) {
                                            if (global_fastest_fps.compare_exchange_strong(current_fastest, new_fps)) {
                                                int old_fastest = fastest_channel_index.exchange(channel_id - 1);

                                                if (old_fastest >= 0 &&
                                                    old_fastest < static_cast<int>(channel_monitor_data_.size())) {
                                                    channel_monitor_data_[old_fastest]->is_fastest_channel.store(false);
                                                }

                                                channel_monitor_ptr->is_fastest_channel.store(true);
                                                CASPAR_LOG(info)
                                                    << L"Channel " << channel_id << L" is now fastest channel at "
                                                    << new_fps << L" fps";
                                            }
                                        } else if (channel_monitor_ptr->is_fastest_channel.load()) {
                                            channel_monitor_ptr->is_fastest_channel.store(false);
                                            recalculate_fastest_channel();
                                        }
                                    } else {
                                        CASPAR_LOG(warning)
                                            << L"Could not extract valid framerate values for channel " << channel_id;
                                    }
                                } catch (const std::exception& e) {
                                    CASPAR_LOG(error) << L"Error extracting framerate for channel " << channel_id
                                                      << L": " << u16(e.what());

                                    // Log the actual types for debugging
                                    if (values.size() >= 2) {
                                        CASPAR_LOG(debug) << L"Framerate values types - [0]: "
                                                          << (boost::get<std::int32_t>(&values[0])    ? L"int32"
                                                              : boost::get<std::int64_t>(&values[0])  ? L"int64"
                                                              : boost::get<std::uint32_t>(&values[0]) ? L"uint32"
                                                              : boost::get<std::uint64_t>(&values[0]) ? L"uint64"
                                                              : boost::get<double>(&values[0])        ? L"double"
                                                              : boost::get<float>(&values[0])         ? L"float"
                                                              : boost::get<std::string>(&values[0])   ? L"string"
                                                              : boost::get<std::wstring>(&values[0])  ? L"wstring"
                                                              : boost::get<bool>(&values[0])          ? L"bool"
                                                                                                      : L"unknown")
                                                          << L", [1]: "
                                                          << (boost::get<std::int32_t>(&values[1])    ? L"int32"
                                                              : boost::get<std::int64_t>(&values[1])  ? L"int64"
                                                              : boost::get<std::uint32_t>(&values[1]) ? L"uint32"
                                                              : boost::get<std::uint64_t>(&values[1]) ? L"uint64"
                                                              : boost::get<double>(&values[1])        ? L"double"
                                                              : boost::get<float>(&values[1])         ? L"float"
                                                              : boost::get<std::string>(&values[1])   ? L"string"
                                                              : boost::get<std::wstring>(&values[1])  ? L"wstring"
                                                              : boost::get<bool>(&values[1])          ? L"bool"
                                                                                                      : L"unknown");
                                    }
                                }
                            }
                        }
                    }

                    core::monitor::state prefixed_state;
                    for (const auto& [path, values] : channel_state) {
                        std::string full_path     = channel_monitor_ptr->channel_prefix + path;
                        prefixed_state[full_path] = values;
                    }

                    int current_buffer = channel_monitor_ptr->active_buffer.load();
                    int next_buffer    = 1 - current_buffer;

                    channel_monitor_ptr->state_buffers[next_buffer] = std::move(prefixed_state);
                    channel_monitor_ptr->active_buffer.store(next_buffer);
                    channel_monitor_ptr->state_updated.store(true);

                    if (channel_monitor_ptr->is_fastest_channel.load()) {
                        boost::asio::post(*monitor_io_context_,
                                          [this]() { send_frame_synchronized_websocket_update(); });
                    }

                    auto client = weak_client.lock();
                    if (client) {
                        monitor::state osc_state;
                        osc_state[""]["channel"][channel_id] = channel_state;
                        client->send(std::move(osc_state));
                    }
                });

            const std::wstring lifecycle_key = L"lock" + std::to_wstring(channel_id);
            channels_->emplace_back(channel, channel->stage(), lifecycle_key);
        }

        return xml_channels;
    }

    void setup_osc(const boost::property_tree::wptree& pt)
    {
        using boost::property_tree::wptree;
        using namespace boost::asio::ip;

        auto default_port                 = pt.get<unsigned short>(L"configuration.osc.default-port", 6250);
        auto disable_send_to_amcp_clients = pt.get(L"configuration.osc.disable-send-to-amcp-clients", false);
        auto predefined_clients           = pt.get_child_optional(L"configuration.osc.predefined-clients");

        if (predefined_clients) {
            for (auto& predefined_client :
                 pt | witerate_children(L"configuration.osc.predefined-clients") | welement_context_iteration) {
                ptree_verify_element_name(predefined_client, L"predefined-client");

                const auto address = ptree_get<std::wstring>(predefined_client.second, L"address");
                const auto port    = ptree_get<unsigned short>(predefined_client.second, L"port");

                boost::system::error_code ec;
                auto                      ipaddr = make_address_v4(u8(address), ec);
                if (!ec)
                    predefined_osc_subscriptions_.push_back(
                        osc_client_->get_subscription_token(udp::endpoint(ipaddr, port)));
                else
                    CASPAR_LOG(warning) << "Invalid OSC client. Must be valid ipv4 address: " << address;
            }
        }

        if (!disable_send_to_amcp_clients && primary_amcp_server_)
            primary_amcp_server_->add_client_lifecycle_object_factory(
                [=](const std::string& ipv4_address) -> std::pair<std::wstring, std::shared_ptr<void>> {
                    using namespace boost::asio::ip;

                    return std::make_pair(std::wstring(L"osc_subscribe"),
                                          osc_client_->get_subscription_token(
                                              udp::endpoint(make_address_v4(ipv4_address), default_port)));
                });
    }

    void setup_channel_producers_and_consumers(const std::vector<boost::property_tree::wptree>& xml_channels)
    {
        auto console_client = spl::make_shared<IO::ConsoleClientInfo>();

        std::vector<spl::shared_ptr<core::video_channel>> channels_vec;
        for (auto& cc : *channels_) {
            channels_vec.emplace_back(cc.raw_channel);
        }

        for (auto& channel : *channels_) {
            core::diagnostics::scoped_call_context save;
            core::diagnostics::call_context::for_thread().video_channel = channel.raw_channel->index();

            auto xml_channel = xml_channels.at(channel.raw_channel->index() - 1);

            // Consumers
            if (xml_channel.get_child_optional(L"consumers")) {
                for (auto& xml_consumer : xml_channel | witerate_children(L"consumers") | welement_context_iteration) {
                    auto name = xml_consumer.first;

                    try {
                        if (name != L"<xmlcomment>")
                            channel.raw_channel->output().add(
                                consumer_registry_->create_consumer(name,
                                                                    xml_consumer.second,
                                                                    video_format_repository_,
                                                                    channels_vec,
                                                                    channel.raw_channel->get_consumer_channel_info()));
                    } catch (...) {
                        CASPAR_LOG_CURRENT_EXCEPTION();
                    }
                }
            }

            // Producers
            if (xml_channel.get_child_optional(L"producers")) {
                for (auto& xml_producer : xml_channel | witerate_children(L"producers") | welement_context_iteration) {
                    ptree_verify_element_name(xml_producer, L"producer");

                    const std::wstring command = xml_producer.second.get_value(L"");
                    const auto         attrs   = xml_producer.second.get_child(L"<xmlattr>");
                    const int          id      = attrs.get(L"id", -1);

                    try {
                        std::list<std::wstring> tokens{
                            L"PLAY", (boost::wformat(L"%i-%i") % channel.raw_channel->index() % id).str()};
                        IO::tokenize(command, tokens);
                        auto cmd = amcp_command_repo_->parse_command(console_client, tokens, L"");

                        if (cmd) {
                            std::wstring res = cmd->Execute(channels_).get();
                            console_client->send(std::move(res), false);
                        }
                    } catch (const user_error&) {
                        CASPAR_LOG(error) << "Failed to parse command: " << command;
                    } catch (...) {
                        CASPAR_LOG_CURRENT_EXCEPTION();
                    }
                }
            }

            // Mixer transforms
            if (xml_channel.get_child_optional(L"mixer-transforms")) {
                for (auto& xml_transform :
                     xml_channel | witerate_children(L"mixer-transforms") | welement_context_iteration) {
                    ptree_verify_element_name(xml_transform, L"transform");

                    const std::wstring command_suffix = xml_transform.second.get_value(L"");
                    const auto         attrs          = xml_transform.second.get_child_optional(L"<xmlattr>");
                    const int          layer_id       = attrs ? attrs->get(L"id", -1) : -1;

                    try {
                        std::wstring full_command;

                        // Construct the full MIXER command
                        if (layer_id != -1) {
                            // Layer-specific mixer command
                            full_command = L"MIXER " +
                                           (boost::wformat(L"%i-%i") % channel.raw_channel->index() % layer_id).str() +
                                           L" " + command_suffix;
                        } else {
                            // Channel-level mixer command
                            full_command =
                                L"MIXER " + std::to_wstring(channel.raw_channel->index()) + L" " + command_suffix;
                        }

                        std::list<std::wstring> tokens;
                        IO::tokenize(full_command, tokens);
                        auto cmd = amcp_command_repo_->parse_command(console_client, tokens, L"");

                        if (cmd) {
                            std::wstring res = cmd->Execute(channels_).get();
                            console_client->send(std::move(res), false);
                            CASPAR_LOG(info) << L"Applied mixer transform: " << full_command;
                        }
                    } catch (const user_error&) {
                        CASPAR_LOG(error) << "Failed to parse mixer transform command: " << command_suffix;
                    } catch (...) {
                        CASPAR_LOG_CURRENT_EXCEPTION();
                    }
                }
            }
        }
    }

    void setup_amcp_command_repo()
    {
        amcp_command_repo_ = std::make_shared<amcp::amcp_command_repository>(channels_);

        auto ogl_device = accelerator_.get_device();
        auto ctx        = std::make_shared<amcp::amcp_command_static_context>(
            video_format_repository_,
            cg_registry_,
            producer_registry_,
            consumer_registry_,
            amcp_command_repo_,
            shutdown_server_now_,
            u8(caspar::env::properties().get(L"configuration.amcp.media-server.host", L"127.0.0.1")),
            u8(caspar::env::properties().get(L"configuration.amcp.media-server.port", L"8000")),
            ogl_device,
            spl::make_shared_ptr(osc_client_));

        amcp_context_factory_ = std::make_shared<amcp::command_context_factory>(ctx);

        amcp_command_repo_wrapper_ =
            std::make_shared<amcp::amcp_command_repository_wrapper>(amcp_command_repo_, amcp_context_factory_);

        amcp::register_commands(amcp_command_repo_wrapper_);
    }

    void setup_controllers(const boost::property_tree::wptree& pt)
    {
        using boost::property_tree::wptree;
        for (auto& xml_controller : pt | witerate_children(L"configuration.controllers") | welement_context_iteration) {
            auto name     = xml_controller.first;
            auto protocol = ptree_get<std::wstring>(xml_controller.second, L"protocol");

            if (name == L"tcp") {
                auto port = ptree_get<unsigned int>(xml_controller.second, L"port");

                try {
                    auto asyncbootstrapper = spl::make_shared<IO::AsyncEventServer>(
                        io_context_,
                        create_protocol(protocol, L"TCP Port " + std::to_wstring(port)),
                        static_cast<short>(port));
                    async_servers_.push_back(asyncbootstrapper);

                    if (!primary_amcp_server_ && boost::iequals(protocol, L"AMCP"))
                        primary_amcp_server_ = asyncbootstrapper;
                } catch (...) {
                    CASPAR_LOG(fatal) << L"Failed to setup " << protocol << L" controller on port "
                                      << boost::lexical_cast<std::wstring>(port) << L". It is likely already in use";
                    throw;
                    // CASPAR_LOG_CURRENT_EXCEPTION();
                }
            } else
                CASPAR_LOG(warning) << "Invalid controller: " << name;
        }
    }

    void setup_websocket_controllers(const boost::property_tree::wptree& pt)
    {
        using boost::property_tree::wptree;

        auto websocket_config = pt.get_child_optional(L"configuration.websocket");
        if (!websocket_config) {
            CASPAR_LOG(info) << L"WebSocket configuration not found, skipping WebSocket servers";
            return;
        }

        try {
            auto amcp_port    = websocket_config->get<uint16_t>(L"amcp-port", 5251);
            auto monitor_port = websocket_config->get<uint16_t>(L"monitor-port", 5252);

            // Create WebSocket monitor server (client already created earlier)
            websocket_monitor_server_ = std::make_shared<protocol::websocket::websocket_monitor_server>(
                monitor_io_context_, websocket_monitor_client_shared_, monitor_port);

            // Start the WebSocket monitor server
            websocket_monitor_server_->start();

            // Create AMCP WebSocket server (without monitor functionality)
            websocket_server_ = std::make_shared<IO::websocket_server>(
                io_context_,
                amcp::create_wchar_amcp_strategy_factory(L"WebSocket", spl::make_shared_ptr(amcp_command_repo_)),
                amcp_port);

            // Start the AMCP websocket server
            websocket_server_->start();

            // Add OSC lifecycle factory for WebSocket AMCP clients
            auto default_port = pt.get<unsigned short>(L"configuration.osc.default-port", 6250);
            websocket_server_->add_client_lifecycle_object_factory(
                [=](const std::string& ipv4_address) -> std::pair<std::wstring, std::shared_ptr<void>> {
                    using namespace boost::asio::ip;
                    return std::make_pair(std::wstring(L"osc_subscribe"),
                                          osc_client_->get_subscription_token(
                                              udp::endpoint(make_address_v4(ipv4_address), default_port)));
                });

            CASPAR_LOG(info) << L"Started WebSocket servers on ports " << amcp_port << L" (AMCP) and " << monitor_port
                             << L" (Monitor)";
        } catch (const std::exception& e) {
            CASPAR_LOG(error) << L"Failed to setup WebSocket controllers: " << u16(e.what());
            // Don't throw - WebSocket is optional
        }
    }

    IO::protocol_strategy_factory<char>::ptr create_protocol(const std::wstring& name,
                                                             const std::wstring& port_description) const
    {
        using namespace IO;

        if (boost::iequals(name, L"AMCP"))
            return amcp::create_char_amcp_strategy_factory(port_description, spl::make_shared_ptr(amcp_command_repo_));

        CASPAR_THROW_EXCEPTION(user_error() << msg_info(L"Invalid protocol: " + name));
    }

    void send_frame_synchronized_websocket_update()
    {
        core::monitor::state complete_state;
        int                  total_channels = 0;
        int                  total_paths    = 0;

        for (const auto& channel_monitor : channel_monitor_data_) {
            if (!channel_monitor)
                continue;

            int         active_buffer = channel_monitor->active_buffer.load();
            const auto& current_state = channel_monitor->state_buffers[active_buffer];

            for (const auto& [path, values] : current_state) {
                complete_state[path] = values;
                total_paths++;
            }

            total_channels++;
        }

        auto client_ptr = websocket_monitor_client_.load();
        if (client_ptr && total_paths > 0) {
            try {
                client_ptr->send(complete_state);
            } catch (const std::exception& e) {
                CASPAR_LOG(error) << L"WebSocket monitor client send error: " << u16(e.what());
            }
        }
    }

    void recalculate_fastest_channel()
    {
        double fastest_fps   = 0.0;
        int    fastest_index = -1;

        for (size_t i = 0; i < channel_monitor_data_.size(); ++i) {
            if (channel_monitor_data_[i] && channel_monitor_data_[i]->frame_rate > fastest_fps) {
                fastest_fps   = channel_monitor_data_[i]->frame_rate;
                fastest_index = static_cast<int>(i);
            }
        }

        global_fastest_fps.store(fastest_fps);
        fastest_channel_index.store(fastest_index);

        for (size_t i = 0; i < channel_monitor_data_.size(); ++i) {
            if (channel_monitor_data_[i]) {
                channel_monitor_data_[i]->is_fastest_channel.store(i == static_cast<size_t>(fastest_index));
            }
        }
    }

    core::monitor::state get_current_monitor_state() const
    {
        core::monitor::state complete_state;
        int                  total_channels = 0;
        int                  total_paths    = 0;

        for (const auto& channel_monitor : channel_monitor_data_) {
            if (!channel_monitor)
                continue;

            int         active_buffer = channel_monitor->active_buffer.load();
            const auto& current_state = channel_monitor->state_buffers[active_buffer];

            for (const auto& [path, values] : current_state) {
                complete_state[path] = values;
                total_paths++;
            }

            total_channels++;
        }

        return complete_state;
    }
};

server::server(std::function<void(bool)> shutdown_server_now)
    : impl_(new impl(std::move(shutdown_server_now)))
{
}
void                                                     server::start() { impl_->start(); }
spl::shared_ptr<protocol::amcp::amcp_command_repository> server::get_amcp_command_repository() const
{
    return spl::make_shared_ptr(impl_->amcp_command_repo_);
}

} // namespace caspar
