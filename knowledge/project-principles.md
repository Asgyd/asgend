# Project principles

- A task accepted by the server must be durably persisted before the server confirms acceptance.
- Queue claiming must be atomic across workers.
- A server restart must not leave tasks permanently marked as running.
- Repeated submissions with the same idempotency key must not create duplicate jobs.
- Cancellation of queued work should prevent its execution; cancellation of running work is cooperative.
- Tool availability is an allow-list, not a suggestion from the LLM.
- Results must distinguish real model-backed execution from a plan produced while the model is disabled.
- The TCP protocol is not encrypted; external exposure requires a secure transport such as a VPN or TLS proxy.
