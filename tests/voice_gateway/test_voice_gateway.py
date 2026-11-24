import base64
import tempfile
import unittest
from unittest.mock import patch
from pathlib import Path

from voice_gateway.voice_gateway import CoworkManager, GatewayError, Session, MAX_FILE_BYTES


class FakeRunner:
    def __init__(self):
        self.calls = 0
    def generate(self, session):
        self.calls += 1
        text = session.turns[-1]['text']
        return f"# Отчёт ревизии {self.calls}\n\n{ text }\n\nУчтены ли все требования?", None


class VoiceGatewayContractTests(unittest.TestCase):
    def setUp(self):
        self.runner = FakeRunner()
        self.submissions = []
        self.manager = CoworkManager(self.runner, lambda task, token, key: self._submit(task, token, key))
        self.session = self.manager.create("Проект: demo")

    def tearDown(self):
        for session in self.manager.sessions.values():
            import shutil
            shutil.rmtree(session.temp_dir, ignore_errors=True)

    def _submit(self, task, token, key):
        self.submissions.append((task, token, key))
        return "task-123"

    def test_turn_creates_revision_without_queue_submission(self):
        result = self.manager.ingest(self.session.session_id, {
            "text": "Поменяй заголовок", "selections": [{"kind": "code", "label": "src/main.c", "text": "int main(void) {}"}],
        })
        self.assertEqual(result["revision"], 1)
        self.assertTrue(result["requires_confirmation"])
        self.assertIn("Поменяй заголовок", result["report"])
        self.assertEqual(self.submissions, [])

    def test_new_turn_invalidates_old_report_revision(self):
        sid = self.session.session_id
        first = self.manager.ingest(sid, {"text": "Первое требование"})
        second = self.manager.ingest(sid, {"text": "Уточнение"})
        with self.assertRaises(GatewayError) as error:
            self.manager.confirm(sid, first["revision"], "ui-token")
        self.assertEqual(error.exception.status, 409)
        self.assertEqual(self.submissions, [])
        confirmed = self.manager.confirm(sid, second["revision"], "ui-token")
        self.assertEqual(confirmed["task_id"], "task-123")
        self.assertEqual(len(self.submissions), 1)
        self.assertEqual(self.submissions[0][1], "ui-token")
        self.assertTrue(self.submissions[0][2])

    def test_confirmation_is_idempotent(self):
        sid = self.session.session_id
        result = self.manager.ingest(sid, {"text": "Сделай форму"})
        self.manager.confirm(sid, result["revision"], "token")
        replay = self.manager.confirm(sid, result["revision"], "token")
        self.assertTrue(replay["idempotent_replay"])
        self.assertEqual(len(self.submissions), 1)

    def test_cannot_add_turn_after_confirmation(self):
        result = self.manager.ingest(self.session.session_id, {"text": "Одна задача"})
        self.manager.confirm(self.session.session_id, result["revision"], "token")
        with self.assertRaises(GatewayError) as error:
            self.manager.ingest(self.session.session_id, {"text": "Позднее уточнение"})
        self.assertEqual(error.exception.status, 409)

    def test_attachment_type_and_base64_are_validated(self):
        with self.assertRaises(GatewayError):
            self.manager.ingest(self.session.session_id, {"attachments": [{"name": "evil.exe", "mime_type": "application/x-msdownload", "data_base64": base64.b64encode(b"x").decode()}]})
        with self.assertRaises(GatewayError):
            self.manager.ingest(self.session.session_id, {"attachments": [{"name": "pic.png", "mime_type": "image/png", "data_base64": "%%%"}]})

    def test_audio_attachment_is_stored_and_passed_to_runner(self):
        payload = base64.b64encode(b"RIFFFAKEWAV").decode("ascii")
        result = self.manager.ingest(self.session.session_id, {"text": "Послушай", "attachments": [{"name": "voice.wav", "mime_type": "audio/wav", "data_base64": payload}]})
        self.assertEqual(result["attachment_count"], 1)
        self.assertTrue(self.session.attachments[0].path.exists())
        self.assertEqual(self.session.attachments[0].path.read_bytes(), b"RIFFFAKEWAV")

    def test_large_media_is_rejected(self):
        blob = b"x" * (MAX_FILE_BYTES + 1)
        with self.assertRaises(GatewayError):
            self.manager.ingest(self.session.session_id, {"attachments": [{"name": "a.wav", "mime_type": "audio/wav", "data_base64": base64.b64encode(blob).decode("ascii")}]})

    def test_selected_content_is_bounded(self):
        with self.assertRaises(GatewayError):
            self.manager.ingest(self.session.session_id, {"selections": [{"text": "x" * 30001}]})


    def test_attachment_batch_respects_cumulative_session_limit(self):
        session = self.manager.create("project")
        payload = {
            "text": "", "selections": [],
            "attachments": [
                {"name": "a.png", "mime_type": "image/png", "data_base64": base64.b64encode(b"12345678").decode()},
                {"name": "b.png", "mime_type": "image/png", "data_base64": base64.b64encode(b"abcdefgh").decode()},
            ],
        }
        with patch("voice_gateway.voice_gateway.MAX_SESSION_BYTES", 12):
            with self.assertRaisesRegex(GatewayError, "session attachment limit"):
                self.manager.ingest(session.session_id, payload)
        self.assertEqual(session.attachments, [])
        self.assertEqual(list(session.temp_dir.iterdir()), [])

    def test_invalid_later_attachment_rolls_back_earlier_temp_files(self):
        session = self.manager.create("project")
        payload = {
            "text": "", "selections": [],
            "attachments": [
                {"name": "a.png", "mime_type": "image/png", "data_base64": base64.b64encode(b"image").decode()},
                {"name": "bad.pdf", "mime_type": "application/pdf", "data_base64": base64.b64encode(b"pdf").decode()},
            ],
        }
        with self.assertRaisesRegex(GatewayError, "Unsupported multimodal"):
            self.manager.ingest(session.session_id, payload)
        self.assertEqual(session.attachments, [])
        self.assertEqual(list(session.temp_dir.iterdir()), [])


if __name__ == "__main__":
    unittest.main()


class VoiceGatewayHttpTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        import threading
        from http.server import ThreadingHTTPServer
        from voice_gateway.voice_gateway import make_handler
        cls.submitted = []
        cls.manager = CoworkManager(FakeRunner(), lambda task, token, key: cls._enqueue(task, token, key))
        cls.allowed = {"http://127.0.0.1:8766"}
        cls.server = ThreadingHTTPServer(("127.0.0.1", 0), make_handler(cls.manager, "secret-test-token", cls.allowed))
        cls.server.allowed_origins = cls.allowed
        cls.thread = threading.Thread(target=cls.server.serve_forever, daemon=True)
        cls.thread.start()
        cls.base = f"http://127.0.0.1:{cls.server.server_port}"

    @classmethod
    def _enqueue(cls, task, token, key):
        cls.submitted.append((task, token, key))
        return "http-task-1"

    @classmethod
    def tearDownClass(cls):
        import shutil
        cls.server.shutdown()
        cls.server.server_close()
        cls.thread.join(timeout=2)
        for session in cls.manager.sessions.values():
            shutil.rmtree(session.temp_dir, ignore_errors=True)

    def _post(self, path, body, token="secret-test-token", origin="http://127.0.0.1:8766"):
        import json
        import urllib.error
        import urllib.request
        request = urllib.request.Request(self.base + path, data=json.dumps(body).encode(), method="POST", headers={
            "Content-Type": "application/json", "X-AO-Token": token, "Origin": origin,
        })
        try:
            with urllib.request.urlopen(request, timeout=5) as response:
                return response.status, response.headers, json.loads(response.read())
        except urllib.error.HTTPError as exc:
            return exc.code, exc.headers, json.loads(exc.read())

    def test_http_origin_token_and_session_flow(self):
        status, headers, created = self._post("/v1/sessions", {"project_state": "web project"})
        self.assertEqual(status, 200)
        self.assertEqual(headers.get("Access-Control-Allow-Origin"), "http://127.0.0.1:8766")
        sid = created["session_id"]
        status, _, report = self._post(f"/v1/sessions/{sid}/turn", {"text": "Сделать адаптивную панель"})
        self.assertEqual(status, 200)
        self.assertEqual(report["revision"], 1)
        self.assertEqual(self.submitted, [])
        status, _, denied = self._post(f"/v1/sessions/{sid}/confirm", {"revision": 1}, token="wrong")
        self.assertEqual(status, 401)
        status, _, confirmed = self._post(f"/v1/sessions/{sid}/confirm", {"revision": 1})
        self.assertEqual(status, 200)
        self.assertEqual(confirmed["task_id"], "http-task-1")
        self.assertEqual(len(self.submitted), 1)

    def test_foreign_origin_is_rejected(self):
        status, _, data = self._post("/v1/sessions", {}, origin="http://attacker.invalid")
        self.assertEqual(status, 403)
        self.assertFalse(data["ok"])


if __name__ == "__main__":
    unittest.main()


class LlamaServerRunnerTests(unittest.TestCase):
    def _response(self, body, content_type='application/json'):
        import io
        class Response:
            def __init__(self):
                self.body = io.BytesIO(body)
                self.headers = {'Content-Type': content_type}
            def __enter__(self): return self
            def __exit__(self, *args): return False
            def read(self, size=-1): return self.body.read(size)
        return Response()

    def _session_with_media(self):
        import shutil
        from voice_gateway.voice_gateway import Session, Attachment
        directory = Path(tempfile.mkdtemp(prefix='asgend-llama-test-'))
        files = []
        for name, mime, data in (
            ('screen.png', 'image/png', b'PNGDATA'),
            ('voice.wav', 'audio/wav', b'WAVDATA'),
            ('walkthrough.mp4', 'video/mp4', b'MP4DATA'),
        ):
            path = directory / name
            path.write_bytes(data)
            files.append(Attachment(path, name, mime, len(data)))
        session = Session('session-1', 0, 0, directory, 'A demo project',
                          turns=[{'text': 'Update the header', 'selections': [{'kind': 'code', 'label': 'app.dart', 'text': 'Widget build() => ...'}]}],
                          attachments=files)
        return session, directory

    def test_openai_compatible_runner_sends_typed_multimodal_content(self):
        import json
        from unittest.mock import patch
        from voice_gateway.voice_gateway import LlamaServerRunner
        session, directory = self._session_with_media()
        captured = {}
        payload = {'choices': [{'message': {'content': '# Report\nUpdated header.'}}]}
        def fake_urlopen(request, timeout=None):
            captured['url'] = request.full_url
            captured['headers'] = dict(request.header_items())
            captured['body'] = json.loads(request.data.decode())
            captured['timeout'] = timeout
            return self._response(json.dumps(payload).encode())
        try:
            runner = LlamaServerRunner(model_id='local-model', base_url='http://127.0.0.1:8787',
                                       api_key='test-key', timeout=17)
            with patch('urllib.request.urlopen', side_effect=fake_urlopen):
                text, speech = runner.generate(session)
            self.assertIn('Updated header', text)
            self.assertIsNone(speech)
            self.assertEqual(captured['url'], 'http://127.0.0.1:8787/v1/chat/completions')
            self.assertEqual(captured['headers'].get('Authorization'), 'Bearer test-key')
            self.assertEqual(captured['body']['temperature'], 0.0)
            self.assertEqual(captured['body']['model'], 'local-model')
            parts = captured['body']['messages'][1]['content']
            kinds = [part['type'] for part in parts]
            self.assertIn('image_url', kinds)
            self.assertIn('input_audio', kinds)
            self.assertIn('input_video', kinds)
            self.assertIn('Selected code [app.dart]', parts[0]['text'])
            self.assertTrue(parts[1]['image_url']['url'].startswith('data:image/png;base64,'))
            self.assertTrue(parts[2]['input_audio']['data'].startswith('data:audio/wav;base64,'))
            self.assertTrue(parts[3]['input_video']['data'].startswith('data:video/mp4;base64,'))
        finally:
            import shutil
            shutil.rmtree(directory, ignore_errors=True)

    def test_speech_output_uses_llama_server_tts_endpoint(self):
        import json
        from unittest.mock import patch
        from voice_gateway.voice_gateway import LlamaServerRunner
        session, directory = self._session_with_media()
        calls = []
        chat = {'choices': [{'message': {'content': 'Отчёт готов.'}}]}
        def fake_urlopen(request, timeout=None):
            calls.append((request.full_url, json.loads(request.data.decode()) if request.data else None))
            if request.full_url.endswith('/v1/chat/completions'):
                return self._response(json.dumps(chat).encode())
            return self._response(b'RIFFWAVE', 'audio/wav')
        try:
            runner = LlamaServerRunner(base_url='http://127.0.0.1:8787', speech_output=True)
            with patch('urllib.request.urlopen', side_effect=fake_urlopen):
                text, speech = runner.generate(session)
            self.assertEqual(text, 'Отчёт готов.')
            import base64
            self.assertEqual(base64.b64decode(speech), b'RIFFWAVE')
            self.assertEqual([item[0].rsplit('/', 1)[-1] for item in calls], ['completions', 'tts'])
            self.assertEqual(calls[1][1]['lang'], 'ru')
            self.assertEqual(calls[1][1]['voice'], 'female')
        finally:
            import shutil
            shutil.rmtree(directory, ignore_errors=True)

    def test_invalid_server_url_is_rejected(self):
        from voice_gateway.voice_gateway import LlamaServerRunner
        for url in ('file:///etc/passwd', 'http://user:pass@127.0.0.1:8787', '/relative'):
            with self.assertRaises(ValueError):
                LlamaServerRunner(base_url=url)
