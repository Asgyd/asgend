# Voice gateway

Loopback gateway формирует revisioned report и отправляет подтверждённую версию в штатную очередь ASGEND. Inference выполняется отдельным `llama-server` через OpenAI-compatible multimodal HTTP API; gateway не загружает модельные веса и не зависит от PyTorch/Transformers. См. [COWORK_VOICE_MODE.md](../COWORK_VOICE_MODE.md).
