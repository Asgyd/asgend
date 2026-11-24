# Voice and Cowork implementation status

- LFM3-Omni is served by external `llama-server` using GGUF + multimodal projector.
- `voice_gateway` uses standard-library HTTP client for OpenAI-compatible multimodal `/v1/chat/completions` and optional `/tts`.
- The gateway maintains a reviewable revisioned report and calls the ASGEND task API only after explicit user confirmation.
- Setup and constraints: [COWORK_VOICE_MODE.md](COWORK_VOICE_MODE.md).
