# Tiny: minimal test game for the ESP32-S3 server

A deliberately small game (11 nodes, a hand, mapgen v6 aliases, no ABMs or
entities), so memory and CPU measurements show the engine's cost rather than a
big game's. Textures are not included. A client will show the "unknown
texture" pattern, which is fine for testing.

Install by copying or symlinking this folder to `games/tiny`, then start a world
with `--gameid tiny`.
