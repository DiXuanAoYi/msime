// Appended to the pinned audio.cpp C ABI translation unit by Build-Voxtral.ps1.
// Upstream's pull-event ABI cannot expose synchronous TTS sink callbacks.
// This facade forwards each NEW PCM chunk while start_stream is generating it.
// Enumerate without loading weights. Prefer a discrete GPU to an integrated one.
extern "C" __declspec(dllexport) int msime_voxtral_preferred_vulkan_device()
{
    try {
        int fallback = -1;
        for (const auto &device : engine::core::list_backend_devices()) {
            if (device.backend != "Vulkan") continue;
            if (device.type == "GPU") return device.index;
            if (fallback < 0) fallback = device.index;
        }
        return fallback;
    } catch (...) { return -1; }
}
extern "C" __declspec(dllexport) int msime_voxtral_stream(
    audiocpp_session *session, const audiocpp_request *request,
    int (*callback)(const float *, size_t, int, int, void *), void *opaque)
{
    if (!session || !request || !callback)
        return fail(AUDIOCPP_ERR_INVALID_ARGUMENT, "session, request and callback are required");
    const auto supported = require_streaming(session);
    if (supported != AUDIOCPP_OK)
        return supported;
    return guard([&] {
        auto &stream = session->streaming();
        stream.set_stream_event_sink([&](const rt::StreamEvent &event) {
            if (event.audio_output && !event.audio_output->samples.empty())
            {
                const auto &audio = *event.audio_output;
                if (audio.channels < 1 ||
                    !callback(audio.samples.data(), audio.samples.size() / audio.channels,
                              audio.sample_rate, audio.channels, opaque))
                    throw std::runtime_error("Speech stream cancelled by client");
            }
        });
        try
        {
            session->session->prepare(rt::build_preparation_request(request->request));
            stream.start_stream(request->request);
            stream.finish_stream();
            stream.set_stream_event_sink({});
        }
        catch (...)
        {
            stream.set_stream_event_sink({});
            stream.reset();
            throw;
        }
        return AUDIOCPP_OK;
    });
}
