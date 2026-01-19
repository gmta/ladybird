/*
 * Copyright (c) 2024, Bar Yemini <bar.ye651@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Math.h>
#include <LibJS/Runtime/TypedArray.h>
#include <LibWeb/Bindings/AudioScheduledSourceNodePrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/WebAudio/AudioBuffer.h>
#include <LibWeb/WebAudio/AudioBufferSourceNode.h>
#include <LibWeb/WebAudio/AudioParam.h>
#include <LibWeb/WebAudio/AudioScheduledSourceNode.h>
#include <LibWeb/WebAudio/BaseAudioContext.h>
#include <LibWeb/WebAudio/ControlMessage.h>

namespace Web::WebAudio {

GC_DEFINE_ALLOCATOR(AudioBufferSourceNode);

AudioBufferSourceNode::AudioBufferSourceNode(JS::Realm& realm, GC::Ref<BaseAudioContext> context, AudioBufferSourceOptions const& options)
    : AudioScheduledSourceNode(realm, context)
    , m_buffer(options.buffer)
    , m_playback_rate(AudioParam::create(realm, context, options.playback_rate, NumericLimits<float>::lowest(), NumericLimits<float>::max(), Bindings::AutomationRate::KRate, AudioParam::FixedAutomationRate::Yes))
    , m_detune(AudioParam::create(realm, context, options.detune, NumericLimits<float>::lowest(), NumericLimits<float>::max(), Bindings::AutomationRate::KRate, AudioParam::FixedAutomationRate::Yes))
    , m_loop(options.loop)
    , m_loop_start(options.loop_start)
    , m_loop_end(options.loop_end)
{
}

AudioBufferSourceNode::~AudioBufferSourceNode() = default;

// https://webaudio.github.io/web-audio-api/#dom-audiobuffersourcenode-buffer
WebIDL::ExceptionOr<void> AudioBufferSourceNode::set_buffer(GC::Ptr<AudioBuffer> buffer)
{
    // 1. Let new buffer be the AudioBuffer or null value to be assigned to buffer.
    auto new_buffer = buffer;

    // 2. If new buffer is not null and [[buffer set]] is true, throw an InvalidStateError and abort these steps.
    if (new_buffer && m_buffer_set)
        return WebIDL::InvalidStateError::create(realm(), "Buffer has already been set"_utf16);

    // 3. If new buffer is not null, set [[buffer set]] to true.
    if (new_buffer)
        m_buffer_set = true;

    // 4. Assign new buffer to the buffer attribute.
    m_buffer = new_buffer;

    // FIXME: 5. If start() has previously been called on this node, perform the operation acquire the content on buffer.

    return {};
}

// https://webaudio.github.io/web-audio-api/#dom-audiobuffersourcenode-buffer
GC::Ptr<AudioBuffer> AudioBufferSourceNode::buffer() const
{
    return m_buffer;
}

// https://webaudio.github.io/web-audio-api/#dom-audiobuffersourcenode-playbackrate
GC::Ref<AudioParam> AudioBufferSourceNode::playback_rate() const
{
    return m_playback_rate;
}

// https://webaudio.github.io/web-audio-api/#dom-audiobuffersourcenode-detune
GC::Ref<AudioParam> AudioBufferSourceNode::detune() const
{
    return m_detune;
}

// https://webaudio.github.io/web-audio-api/#dom-audiobuffersourcenode-loop
WebIDL::ExceptionOr<void> AudioBufferSourceNode::set_loop(bool loop)
{
    m_loop = loop;
    return {};
}

// https://webaudio.github.io/web-audio-api/#dom-audiobuffersourcenode-loop
bool AudioBufferSourceNode::loop() const
{
    return m_loop;
}

// https://webaudio.github.io/web-audio-api/#dom-audiobuffersourcenode-loopstart
WebIDL::ExceptionOr<void> AudioBufferSourceNode::set_loop_start(double loop_start)
{
    m_loop_start = loop_start;
    return {};
}

// https://webaudio.github.io/web-audio-api/#dom-audiobuffersourcenode-loopstart
double AudioBufferSourceNode::loop_start() const
{
    return m_loop_start;
}

// https://webaudio.github.io/web-audio-api/#dom-audiobuffersourcenode-loopend
WebIDL::ExceptionOr<void> AudioBufferSourceNode::set_loop_end(double loop_end)
{
    m_loop_end = loop_end;
    return {};
}

// https://webaudio.github.io/web-audio-api/#dom-audiobuffersourcenode-loopend
double AudioBufferSourceNode::loop_end() const
{
    return m_loop_end;
}

// https://webaudio.github.io/web-audio-api/#dom-audiobuffersourcenode-start`
WebIDL::ExceptionOr<void> AudioBufferSourceNode::start(Optional<double> when, Optional<double> offset, Optional<double> duration)
{
    // 1. If this AudioBufferSourceNode internal slot [[source started]] is true, an InvalidStateError exception MUST be thrown.
    if (source_started())
        return WebIDL::InvalidStateError::create(realm(), "AudioBufferSourceNode has already been started"_utf16);

    // 2. Check for any errors that must be thrown due to parameter constraints described below. If any exception is thrown during this step, abort those steps.
    // A RangeError exception MUST be thrown if when is negative.
    if (when.has_value() && when.value() < 0)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::RangeError, "when must not be negative"sv };

    // A RangeError exception MUST be thrown if offset is negative
    if (offset.has_value() && offset.value() < 0)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::RangeError, "offset must not be negative"sv };

    // A RangeError exception MUST be thrown if duration is negative.
    if (duration.has_value() && duration.value() < 0)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::RangeError, "duration must not be negative"sv };

    // 3. Set the internal slot [[source started]] on this AudioBufferSourceNode to true.
    set_source_started(true);

    // 4. Queue a control message to start the AudioBufferSourceNode, including the parameter values in the message.
    context()->queue_control_message(StartSource {
        .node = this,
        .when = static_cast<float>(when.value_or(0)),
        .offset = static_cast<float>(offset.value_or(0)),
        .duration = duration.has_value() ? static_cast<float>(duration.value()) : AK::Infinity<float>,
    });

    // AD-HOC: Invalidate the source node cache so the rendering thread picks up this new source
    context()->invalidate_source_node_cache();

    // FIXME: 5. Acquire the contents of the buffer if the buffer has been set.
    // FIXME: 6. Send a control message to the associated AudioContext to start running its rendering thread only when all the following conditions are met:

    return {};
}

WebIDL::ExceptionOr<GC::Ref<AudioBufferSourceNode>> AudioBufferSourceNode::create(JS::Realm& realm, GC::Ref<BaseAudioContext> context, AudioBufferSourceOptions const& options)
{
    return construct_impl(realm, context, options);
}

// https://webaudio.github.io/web-audio-api/#dom-audiobuffersourcenode-audiobuffersourcenode
WebIDL::ExceptionOr<GC::Ref<AudioBufferSourceNode>> AudioBufferSourceNode::construct_impl(JS::Realm& realm, GC::Ref<BaseAudioContext> context, AudioBufferSourceOptions const& options)
{
    // When the constructor is called with a BaseAudioContext c and an option object option, the user agent
    // MUST initialize the AudioNode this, with context and options as arguments.

    auto node = realm.create<AudioBufferSourceNode>(realm, context, options);

    // Default options for channel count and interpretation
    // https://webaudio.github.io/web-audio-api/#AudioBufferSourceNode
    AudioNodeDefaultOptions default_options;
    default_options.channel_count = 2;
    default_options.channel_count_mode = Bindings::ChannelCountMode::Max;
    default_options.channel_interpretation = Bindings::ChannelInterpretation::Speakers;
    // FIXME: Set tail-time to no

    TRY(node->initialize_audio_node_options(options, default_options));

    return node;
}

void AudioBufferSourceNode::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(AudioBufferSourceNode);
    Base::initialize(realm);
}

void AudioBufferSourceNode::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_buffer);
    visitor.visit(m_playback_rate);
    visitor.visit(m_detune);
}

// https://webaudio.github.io/web-audio-api/#playback-AudioBufferSourceNode
void AudioBufferSourceNode::handle_start(float when, float offset, float duration)
{
    set_start_time(when);
    m_offset = offset;
    m_duration = duration;

    // Initialize playback position based on offset
    if (m_buffer)
        m_playback_position = m_offset * m_buffer->sample_rate();
}

// https://webaudio.github.io/web-audio-api/#audiobuffersourcenode-rendering
void AudioBufferSourceNode::process(Span<float> output_buffer, double sample_rate, size_t frames_to_process)
{
    // Don't process if we haven't started yet
    if (!source_started())
        return;

    // Don't process if we've been stopped
    // FIXME: Properly handle scheduled stop times using currentTime
    if (stop_time().has_value())
        return;

    // Don't process if there's no buffer
    if (!m_buffer)
        return;

    // Get playback rate and detune values
    float playback_rate = m_playback_rate->value();
    float detune = m_detune->value();

    // Apply detune: computedPlaybackRate = playbackRate * pow(2, detune / 1200)
    float computed_rate = playback_rate * AK::pow(2.0f, detune / 1200.0f);

    // Get buffer properties
    auto buffer_length = m_buffer->length();
    auto buffer_sample_rate = m_buffer->sample_rate();

    // Calculate actual playback rate accounting for sample rate differences
    float rate_ratio = buffer_sample_rate / static_cast<float>(sample_rate);
    float effective_rate = computed_rate * rate_ratio;

    // Get channel data (use first channel for mono output)
    auto channel_data_result = m_buffer->get_channel_data(0);
    if (channel_data_result.is_error())
        return;
    auto channel_data = channel_data_result.value();
    auto buffer_data = channel_data->data();

    // Calculate end position based on duration
    float end_position = (m_duration < AK::Infinity<float>)
        ? (m_offset * buffer_sample_rate + m_duration * buffer_sample_rate)
        : static_cast<float>(buffer_length);

    for (size_t i = 0; i < frames_to_process; ++i) {
        // Check if we've reached the end
        if (m_playback_position >= end_position) {
            if (m_loop) {
                // Handle looping
                float loop_start_sample = static_cast<float>(m_loop_start) * buffer_sample_rate;
                float loop_end_sample = (m_loop_end > 0) ? static_cast<float>(m_loop_end) * buffer_sample_rate : static_cast<float>(buffer_length);
                m_playback_position = loop_start_sample + AK::fmod(m_playback_position - loop_start_sample, loop_end_sample - loop_start_sample);
            } else {
                // Stop playback
                return;
            }
        }

        // Linear interpolation for fractional sample positions
        size_t sample_index = static_cast<size_t>(m_playback_position);
        if (sample_index >= buffer_length)
            return;

        float fraction = m_playback_position - static_cast<float>(sample_index);
        float sample = buffer_data[sample_index];

        // Interpolate with next sample if available
        if (sample_index + 1 < buffer_length)
            sample += fraction * (buffer_data[sample_index + 1] - sample);

        output_buffer[i] += sample;

        // Advance playback position
        m_playback_position += effective_rate;
    }
}

}
