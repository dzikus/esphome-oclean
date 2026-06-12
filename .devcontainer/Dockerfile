FROM mcr.microsoft.com/devcontainers/python:3.14

USER root
RUN pip install --no-cache-dir esphome platformio pre-commit "ruff==0.15.20" \
    && pip install --no-cache-dir --no-deps homeassistant
USER vscode
