/*
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Vector.h>
#include <LibMedia/Audio/PlaybackStream.h>
#include <LibThreading/Thread.h>
#include <LibWeb/Bindings/AudioContextPrototype.h>
#include <LibWeb/HighResolutionTime/DOMHighResTimeStamp.h>
#include <LibWeb/WebAudio/AudioNode.h>
#include <LibWeb/WebAudio/BaseAudioContext.h>
#include <LibWeb/WebAudio/MediaElementAudioSourceNode.h>

namespace Web::WebAudio {

struct AudioContextOptions {
    Bindings::AudioContextLatencyCategory latency_hint = Bindings::AudioContextLatencyCategory::Interactive;
    Optional<float> sample_rate;
};

struct AudioTimestamp {
    double context_time { 0 };
    double performance_time { 0 };
};

// https://webaudio.github.io/web-audio-api/#AudioContext
class AudioContext final : public BaseAudioContext {
    WEB_PLATFORM_OBJECT(AudioContext, BaseAudioContext);
    GC_DECLARE_ALLOCATOR(AudioContext);

public:
    static WebIDL::ExceptionOr<GC::Ref<AudioContext>> construct_impl(JS::Realm&, Optional<AudioContextOptions> const& context_options = {});

    virtual ~AudioContext() override;

    double base_latency() const { return m_base_latency; }
    double output_latency() const { return m_output_latency; }
    AudioTimestamp get_output_timestamp();
    WebIDL::ExceptionOr<GC::Ref<WebIDL::Promise>> resume();
    WebIDL::ExceptionOr<GC::Ref<WebIDL::Promise>> suspend();
    WebIDL::ExceptionOr<GC::Ref<WebIDL::Promise>> close();

    WebIDL::ExceptionOr<GC::Ref<MediaElementAudioSourceNode>> create_media_element_source(GC::Ptr<HTML::HTMLMediaElement>);

    void invalidate_source_node_cache() override;

private:
    explicit AudioContext(JS::Realm& realm)
        : BaseAudioContext(realm)
    {
    }

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    double m_base_latency { 0 };
    double m_output_latency { 0 };

    bool m_allowed_to_start = true;
    Vector<GC::Ref<WebIDL::Promise>> m_pending_resume_promises;
    bool m_suspended_by_user = false;
    bool m_sample_rate_explicitly_set = false;

    bool start_rendering_audio_graph();

    // Rendering thread
    RefPtr<Threading::Thread> m_rendering_thread;
    void rendering_thread_loop();
    void start_rendering_thread();

    // Audio output
    RefPtr<Audio::PlaybackStream> m_playback_stream;
    Vector<float> m_render_buffer; // Pre-allocated render buffer
    double m_playback_time { 0.0 };

    // Cached source nodes for rendering (set when rendering starts)
    struct SourceNodeInfo {
        // Store raw pointers for audio thread access
        // These are valid as long as the source nodes are connected
        AudioNode* node { nullptr };
    };
    Vector<SourceNodeInfo> m_cached_source_nodes;
    bool m_sources_cached { false };

    ReadonlySpan<float> render_audio_callback(Span<float> buffer);
    void cache_source_nodes();
    void process_control_messages();
};

}
