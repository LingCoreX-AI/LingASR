# Offline microphone example

ESP32-S3 N16R8 / INMP441 left channel: BCLK=5, WS=4, DIN=6, L/R=GND; BOOT=0.
Use ESP-IDF v5.5.6. Run `idf.py build`, then `idf.py -p PORT flash monitor`.

After the eight-second warm-up, release BOOT and press it to capture four seconds.
The transcript, computation time and RTF are printed over UART. No Wi-Fi, LLM or
TTS is required. This intentionally uses a fixed recording interval to demonstrate
the smallest integration; VAD and pause-based segmentation are demonstrated in
`examples/voice_assistant`.
