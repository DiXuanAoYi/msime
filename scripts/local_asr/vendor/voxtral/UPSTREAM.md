# Voxtral native runtime

Runtime source: https://github.com/0xShug0/audio.cpp
Pinned commit: `96269554d00621fb0fca6b52b58919749eba1c8d`, Apache-2.0.

`model_spec.json` is the unmodified model spec from that commit.
`tts_voxtral_convert.py` adapts `tests/voxtral_tts/convert_gguf.py` to Windows
hardlinks, explicit user directories and atomic completion markers.
`Build-Voxtral.ps1` builds only the Voxtral family and appends the documented
callback facade in `native_voxtral_stream.inc`. The upstream pull-event C API
does not expose synchronous TTS event sinks, so the facade is required to
deliver audio during generation instead of waiting for the final result.

The facade also exposes weight-free Vulkan device enumeration to prefer a
discrete GPU. Build-Voxtral.ps1 enables Vulkan with the official LunarG x64
loader supplied by -VulkanLoader. Failed GPU initialization or pre-audio
allocation falls back to CPU. The loader license is distributed in
_internal/licenses/VulkanRT-License.txt.

Model files come from Mistral's official ModelScope repository at the revisions
and SHA-256 hashes recorded in `tts_catalog.json`. They are downloaded only by
an explicit user action. The model and embedded voice references use CC BY-NC
4.0. No arbitrary reference-voice cloning is advertised for these weights.
