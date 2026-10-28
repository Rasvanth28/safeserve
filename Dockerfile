# SafeServe live demo: the four strategy servers behind one front door
# (tools/live_demo.py), listening on $PORT. Used by Render (render.yaml).

# Build stage: compile the C server
FROM gcc:13-bookworm AS build
WORKDIR /src
COPY Makefile ./
COPY include include
COPY src src
RUN make bin/safeserve

# Run stage: just the binary, configs, dashboard and controller
FROM python:3.12-slim-bookworm
WORKDIR /app
COPY --from=build /src/bin/safeserve bin/safeserve
COPY config config
COPY www www
COPY tools/live_demo.py tools/live_demo.py
ENV PORT=10000
EXPOSE 10000
CMD ["python3", "-u", "tools/live_demo.py", "--host", "0.0.0.0", "--hosted"]
