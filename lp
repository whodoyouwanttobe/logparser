#!/bin/sh
# Обёртка: запускает logparser в Docker.  Пример: ./lp import /logs/syslog
# MSYS_NO_PATHCONV — чтобы Git Bash на Windows не портил пути вида /logs/...
MSYS_NO_PATHCONV=1 exec docker compose run --rm logparser "$@"
