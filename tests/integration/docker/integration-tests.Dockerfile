FROM espressif/idf:release-v5.5@sha256:ff21d212f1d942d004b4bc4e303d98a32c78de06184932211f0632d31d0dd309

USER root
SHELL ["/bin/bash", "-o", "pipefail", "-c"]

RUN apt-get update \
    && apt-get install -y --no-install-recommends ca-certificates curl gnupg procps \
    && install -d -m 0755 /etc/apt/keyrings \
    && curl -fsSL https://dl.google.com/linux/linux_signing_key.pub \
        | gpg --dearmor -o /etc/apt/keyrings/google-chrome.gpg \
    && chmod a+r /etc/apt/keyrings/google-chrome.gpg \
    && echo "deb [arch=amd64 signed-by=/etc/apt/keyrings/google-chrome.gpg] http://dl.google.com/linux/chrome/deb/ stable main" \
        > /etc/apt/sources.list.d/google-chrome.list \
    && apt-get update \
    && apt-get install -y --no-install-recommends google-chrome-stable \
    && ln -sf "$(command -v google-chrome-stable)" /usr/local/bin/chromium \
    && rm -rf /var/lib/apt/lists/*

RUN source "${IDF_PATH}/export.sh" >/dev/null \
    && python -m pip install --no-cache-dir pytest \
    && python "${IDF_PATH}/tools/idf_tools.py" install --targets esp32 required qemu-xtensa

ENV CHROME_BIN=/usr/bin/google-chrome-stable
WORKDIR /work
