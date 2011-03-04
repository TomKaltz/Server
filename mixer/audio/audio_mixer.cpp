#include "../stdafx.h"

#include "audio_mixer.h"
#include "audio_transform.h"

namespace caspar { namespace core {
	
struct audio_mixer::implementation
{
	std::vector<short> audio_data_;
	std::stack<audio_transform> transform_stack_;

	std::map<int, audio_transform> prev_audio_transforms_;
	std::map<int, audio_transform> next_audio_transforms_;

public:
	implementation()
	{
		transform_stack_.push(audio_transform());
	}

	void begin(const audio_transform& transform)
	{
		transform_stack_.push(transform_stack_.top()*transform);
	}

	void process(const std::vector<short>& audio_data, int tag)
	{		
		if(audio_data_.empty())
			audio_data_.resize(audio_data.size(), 0);
		
		auto prev = prev_audio_transforms_[tag];
		auto next = transform_stack_.top();
				
		prev_audio_transforms_[tag] = next;
		next_audio_transforms_[tag] = next;

		auto prev_gain = prev.get_gain();
		auto next_gain = next.get_gain();
		
		if(prev_gain < 0.001 && next_gain < 0.001)
			return;

		tbb::parallel_for
		(
			tbb::blocked_range<size_t>(0, audio_data.size()),
			[&](const tbb::blocked_range<size_t>& r)
			{
				for(size_t n = r.begin(); n < r.end(); ++n)
				{
					double delta = static_cast<double>(n)/static_cast<double>(audio_data_.size());
					double sample_gain = prev_gain * (1.0 - delta) + next_gain * delta;
					int sample = static_cast<int>(audio_data[n]);
					sample = (static_cast<int>(sample_gain*8192.0)*sample)/8192;
					audio_data_[n] = static_cast<short>((static_cast<int>(audio_data_[n]) + sample) & 0xFFFF);
				}
			}
		);

	}
	
	void end()
	{
		transform_stack_.pop();
	}

	std::vector<short> begin_pass()
	{
		return std::move(audio_data_);
	}

	void end_pass()
	{
		if(prev_audio_transforms_.size() != next_audio_transforms_.size())
			CASPAR_LOG(info) << L"Removed tags: " << (prev_audio_transforms_.size() - next_audio_transforms_.size());

		prev_audio_transforms_ = next_audio_transforms_;
		next_audio_transforms_.clear();
	}
};

audio_mixer::audio_mixer() : impl_(new implementation()){}
void audio_mixer::begin(const audio_transform& transform){impl_->begin(transform);}
void audio_mixer::process(const std::vector<short>& audio_data, int tag){impl_->process(audio_data, tag);}
void audio_mixer::end(){impl_->end();}
std::vector<short> audio_mixer::begin_pass(){return impl_->begin_pass();}	
void audio_mixer::end_pass(){impl_->end_pass();}

}}