# Образ утилиты logparser: собирается из исходников, на компьютере пользователя
# не нужны ни компилятор, ни libpq — только Docker.
FROM debian:bookworm-slim AS build
RUN apt-get update \
 && apt-get install -y --no-install-recommends gcc make libc6-dev libpq-dev \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY Makefile ./
COPY src ./src
RUN make logparser

FROM debian:bookworm-slim
RUN apt-get update \
 && apt-get install -y --no-install-recommends libpq5 \
 && rm -rf /var/lib/apt/lists/*
ENV LANG=C.UTF-8
WORKDIR /app
COPY --from=build /src/logparser /usr/local/bin/logparser
COPY sql ./sql
COPY samples ./samples
ENTRYPOINT ["logparser"]
CMD ["--help"]
