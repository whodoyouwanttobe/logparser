@echo off
rem Windows wrapper (cmd/PowerShell). Example: lp import /logs/syslog
docker compose run --rm logparser %*
