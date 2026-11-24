# ASGEND Cowork и LFM3-Omni через llama-server

## Архитектура

Cowork использует два независимых loopback-сервиса:

- `llama-server` — загружает GGUF вес модели и мультимодальный projector, выполняет inference.
- `voice_gateway/voice_gateway.py` — принимает контекст/выделения/медиа, обращается к llama-server по HTTP и ведёт версионируемый отчёт. Gateway не загружает веса и не содержит PyTorch/Transformers.

Поток работы:

1. WebUI передаёт текущее состояние проекта, текст, выделенные объекты и аудио/изображения/видео в gateway.
2. Gateway формирует OpenAI-compatible multimodal запрос на `POST /v1/chat/completions` к llama-server. Изображения передаются как `image_url` data URI, аудио — `input_audio`, видео — `input_video`.
3. LFM3-Omni формирует полный отчёт изменений и критерии приёмки.
4. Каждое новое сообщение/выделение создаёт новую ревизию. Предыдущая ревизия после этого не может быть подтверждена.
5. До подтверждения gateway не вызывает `POST /api/tasks` и не имеет инструмента выполнения.
6. Только кнопка подтверждения передаёт актуальную ревизию в основную очередь ASGEND. После этого работают обычные агенты, инструменты и политики доступа.
7. Если включён `ASGEND_LFM3_SPEECH=1`, gateway дополнительно вызывает llama-server `POST /tts` и возвращает WAV вместе с отчётом.

Модель с `model_role: "voice_interpreter"` зарезервирована для gateway. Core запрещает назначать её `default_model_id`, `analysis_model_id` или `agent.model_id`. Не используйте её как основную модель проекта.

## Требования к llama-server

Нужна сборка llama.cpp с поддержкой LFM3-Omni — официальная GGUF-карточка указывает llama.cpp b11960 / v0.8.0 или новее. Важно использовать multimodal projector, который автоматически скачивается при загрузке поддерживаемого `-hf` репозитория. LFM3-Omni — non-reasoning модель, поэтому используйте `--reasoning off`.

Запуск на стандартном loopback-порту модели:

```bash
llama-server \
  -hf LiquidAI/LFM3-Omni-2B-GGUF:Q4_K_M \
  --reasoning off \
  --temp 0 \
  --host 127.0.0.1 \
  --port 8787
```

При первом запуске llama.cpp загрузит GGUF и multimodal projector из Hugging Face. Можно выбрать другую quantization, заменив суффикс `Q4_K_M`. Для входного видео нужен `ffmpeg` в `PATH`. Для live voice mode llama.cpp предоставляет `/v1/realtime` WebSocket; текущий Cowork gateway использует завершённые аудио-ходы через `/v1/chat/completions`, чтобы сначала построить отчёт и показать его человеку.

## Запуск gateway

В отдельном терминале из корня проекта задайте тот же UI token, что указан в `config/config.json`:

```bash
export ASGEND_UI_TOKEN='тот-же-случайный-токен-из-config.json'
export ASGEND_UI_ORIGIN='http://127.0.0.1:8766'
export ASGEND_CORE_URL='http://127.0.0.1:8766'
export ASGEND_VOICE_BIND='127.0.0.1'
export ASGEND_VOICE_PORT='8788'
export ASGEND_LFM3_SERVER_URL='http://127.0.0.1:8787'
export ASGEND_LFM3_API_MODEL='local-model'
export ASGEND_LFM3_TIMEOUT='300'
# Optional: generate a WAV response through llama-server POST /tts.
export ASGEND_LFM3_SPEECH='1'
export ASGEND_LFM3_TTS_LANG='ru'
export ASGEND_LFM3_TTS_VOICE='female'
python3 voice_gateway/voice_gateway.py
```

Gateway listens on `127.0.0.1:8788`; llama-server listens separately on `127.0.0.1:8787`. The two ports must not be the same. Do not expose either service directly to an untrusted network. Browser CSP allows only the gateway loopback port 8788.

Quick checks:

```bash
curl http://127.0.0.1:8787/health
curl http://127.0.0.1:8788/health
curl http://127.0.0.1:8787/v1/models
```

The first/third calls inspect llama-server; the second inspects the Cowork gateway. A successful gateway `/health` does not alone prove model readiness. Model readiness requires llama-server `/health` and a successful multimodal completion.

## Configuration

`config/models.json` contains a disabled `lfm3-omni-2b` registry profile:

- `base_url`: `http://127.0.0.1:8787/v1`
- `health_url`: `http://127.0.0.1:8787/health`
- `model_name`: `LiquidAI/LFM3-Omni-2B-GGUF:Q4_K_M`
- `runtime_adapter`: `llama_server_openai_compatible`
- `model_role`: `voice_interpreter`
- input modalities: text/audio/image/video
- output modalities: text/audio

This is the model server profile. The WebUI Cowork panel talks to the separate report gateway on port 8788. The UI form preset selects the correct model runtime adapter and does not enable the model as an ordinary project agent.

Environment variables:

- `ASGEND_LFM3_SERVER_URL` — llama-server origin without `/v1`, default `http://127.0.0.1:8787`.
- `ASGEND_LFM3_API_MODEL` — `model` value sent to OpenAI-compatible endpoint, default `local-model`.
- `ASGEND_LFM3_API_KEY` — optional Bearer token for a secured llama-server.
- `ASGEND_LFM3_TIMEOUT` — HTTP inference timeout in seconds, default `300`.
- `ASGEND_LFM3_SPEECH` — set `1` to synthesize a WAV response with `/tts`.
- `ASGEND_LFM3_TTS_LANG` / `ASGEND_LFM3_TTS_VOICE` — default `ru` / `female`.

## Cowork interaction

- Text, microphone recordings, and image/audio/video attachments update the mediator report.
- Selected snippets/code/objects are sent with labels and current project state.
- If an object cannot be selected visually, the user can describe it by voice or text.
- Every new message/selection/attachment increments the report revision. The user can continue editing the request until the latest version is confirmed.
- Only the confirm action creates a normal ASGEND task.
- The right workspace shows project artifacts and previews. HTML is sandboxed, code is shown as source, and active content is not executed automatically.

## Security boundaries

- Gateway binds only to loopback and checks `X-AO-Token` and exact allowed browser Origin.
- The model server is addressed only through the configured local origin; optional API key is passed as Bearer token.
- No queue call occurs before a fresh report revision is explicitly confirmed.
- Media is limited to audio/image/video MIME types, 24 MiB per file, 64 MiB total per session, 12 attachments and 40 turns.
- Gateway temporary files are removed when sessions expire or the process stops.
- Python gateway uses standard library only. There are no PyTorch, Transformers, torchvision, torchcodec, accelerate, librosa or soundfile dependencies. GGUF inference and media decoding are owned by llama.cpp.

## API (loopback)

Gateway POST methods require `X-AO-Token`; browser calls additionally require an allowed Origin.

- `GET /health` — gateway metadata and configured llama-server URL.
- `POST /v1/sessions` — `{ "project_state": "..." }`.
- `POST /v1/sessions/{uuid}/turn` — `{ "text": "...", "project_state": "...", "selections": [], "attachments": [] }`; attachment contains `name`, `mime_type`, `data_base64`.
- `POST /v1/sessions/{uuid}/confirm` — `{ "revision": 3 }`; only current revision creates a Core task.

The gateway API is an approval/report service, not an OpenAI-compatible Core endpoint.
