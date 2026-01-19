/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2025, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Math.h>
#include <LibJS/Runtime/TypedArray.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/OscillatorNodePrototype.h>
#include <LibWeb/WebAudio/AudioParam.h>
#include <LibWeb/WebAudio/BaseAudioContext.h>
#include <LibWeb/WebAudio/OscillatorNode.h>
#include <LibWeb/WebAudio/PeriodicWave.h>

namespace Web::WebAudio {

GC_DEFINE_ALLOCATOR(OscillatorNode);

OscillatorNode::~OscillatorNode() = default;

WebIDL::ExceptionOr<GC::Ref<OscillatorNode>> OscillatorNode::create(JS::Realm& realm, GC::Ref<BaseAudioContext> context, OscillatorOptions const& options)
{
    return construct_impl(realm, context, options);
}

// https://webaudio.github.io/web-audio-api/#dom-oscillatornode-oscillatornode
WebIDL::ExceptionOr<GC::Ref<OscillatorNode>> OscillatorNode::construct_impl(JS::Realm& realm, GC::Ref<BaseAudioContext> context, OscillatorOptions const& options)
{
    if (options.type == Bindings::OscillatorType::Custom && !options.periodic_wave)
        return WebIDL::InvalidStateError::create(realm, "Oscillator node type 'custom' requires PeriodicWave to be provided"_utf16);

    auto node = realm.create<OscillatorNode>(realm, context, options);

    if (options.type == Bindings::OscillatorType::Custom)
        node->set_periodic_wave(options.periodic_wave);

    // Default options for channel count and interpretation
    // https://webaudio.github.io/web-audio-api/#OscillatorNode
    AudioNodeDefaultOptions default_options;
    default_options.channel_count = 2;
    default_options.channel_count_mode = Bindings::ChannelCountMode::Max;
    default_options.channel_interpretation = Bindings::ChannelInterpretation::Speakers;
    // FIXME: Set tail-time to no

    TRY(node->initialize_audio_node_options(options, default_options));

    return node;
}

OscillatorNode::OscillatorNode(JS::Realm& realm, GC::Ref<BaseAudioContext> context, OscillatorOptions const& options)
    : AudioScheduledSourceNode(realm, context)
    , m_type(options.type)
    , m_frequency(AudioParam::create(realm, context, options.frequency, -context->nyquist_frequency(), context->nyquist_frequency(), Bindings::AutomationRate::ARate))
    , m_detune(AudioParam::create(realm, context, options.detune, -1200 * AK::log2(NumericLimits<float>::max()), 1200 * AK::log2(NumericLimits<float>::max()), Bindings::AutomationRate::ARate))
{
}

// https://webaudio.github.io/web-audio-api/#dom-oscillatornode-type
Bindings::OscillatorType OscillatorNode::type() const
{
    return m_type;
}

// https://webaudio.github.io/web-audio-api/#dom-oscillatornode-type
WebIDL::ExceptionOr<void> OscillatorNode::set_type(Bindings::OscillatorType type)
{
    if (type == Bindings::OscillatorType::Custom && m_type != Bindings::OscillatorType::Custom)
        return WebIDL::InvalidStateError::create(realm(), "Oscillator node type cannot be changed to 'custom'"_utf16);

    // FIXME: An appropriate PeriodicWave should be set here based on the given type.
    set_periodic_wave(nullptr);

    m_type = type;
    return {};
}

// https://webaudio.github.io/web-audio-api/#dom-oscillatornode-setperiodicwave
void OscillatorNode::set_periodic_wave(GC::Ptr<PeriodicWave> periodic_wave)
{
    m_periodic_wave = periodic_wave;
    m_type = Bindings::OscillatorType::Custom;
}

void OscillatorNode::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(OscillatorNode);
    Base::initialize(realm);
}

void OscillatorNode::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_frequency);
    visitor.visit(m_detune);
    visitor.visit(m_periodic_wave);
}

// https://webaudio.github.io/web-audio-api/#oscillatornode-signal-generation
void OscillatorNode::process(Span<float> output_buffer, double sample_rate, size_t frames_to_process)
{
    // Don't process if we haven't started yet
    if (!source_started())
        return;

    // Don't process if we've been stopped
    // FIXME: Properly handle scheduled stop times using currentTime
    if (stop_time().has_value())
        return;

    // Get frequency and detune values
    float frequency = m_frequency->value();
    float detune = m_detune->value();

    // Apply detune: computedFrequency = frequency * pow(2, detune / 1200)
    // https://webaudio.github.io/web-audio-api/#computedfrequency
    float computed_freq = frequency * AK::pow(2.0f, detune / 1200.0f);

    // Phase increment per sample: 2π * frequency / sampleRate
    float phase_increment = 2.0f * AK::Pi<float> * computed_freq / static_cast<float>(sample_rate);

    for (size_t i = 0; i < frames_to_process; ++i) {
        float sample = 0.0f;

        switch (m_type) {
        case Bindings::OscillatorType::Sine:
            // Sine wave: sin(phase)
            sample = AK::sin(m_phase);
            break;
        case Bindings::OscillatorType::Square:
            // Square wave: sign of sine
            sample = (m_phase < AK::Pi<float>) ? 1.0f : -1.0f;
            break;
        case Bindings::OscillatorType::Sawtooth:
            // Sawtooth wave
            sample = 2.0f * (m_phase / (2.0f * AK::Pi<float>)) - 1.0f;
            break;
        case Bindings::OscillatorType::Triangle:
            // Triangle wave
            if (m_phase < AK::Pi<float>)
                sample = -1.0f + (2.0f * m_phase / AK::Pi<float>);
            else
                sample = 3.0f - (2.0f * m_phase / AK::Pi<float>);
            break;
        case Bindings::OscillatorType::Custom:
            // Custom waveform using PeriodicWave Fourier coefficients
            // https://webaudio.github.io/web-audio-api/#oscillatornode-signal-generation
            if (m_periodic_wave) {
                auto real = m_periodic_wave->real();
                auto imag = m_periodic_wave->imag();
                if (real && imag) {
                    auto real_data = real->data();
                    auto imag_data = imag->data();
                    size_t num_components = real_data.size();

                    // Sum up all harmonics
                    // signal = sum(n=1 to N-1) { real[n] * cos(n * phase) + imag[n] * sin(n * phase) }
                    for (size_t n = 1; n < num_components; ++n) {
                        float harmonic_phase = static_cast<float>(n) * m_phase;
                        sample += real_data[n] * AK::cos(harmonic_phase)
                            + imag_data[n] * AK::sin(harmonic_phase);
                    }
                }
            }
            break;
        }

        output_buffer[i] += sample;

        // Increment phase and wrap to [0, 2π) to prevent precision loss
        m_phase += phase_increment;
        while (m_phase >= 2.0f * AK::Pi<float>)
            m_phase -= 2.0f * AK::Pi<float>;
        while (m_phase < 0.0f)
            m_phase += 2.0f * AK::Pi<float>;
    }
}

}
