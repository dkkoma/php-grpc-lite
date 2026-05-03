FROM golang:1.22-bookworm AS build

RUN apt-get update && apt-get install -y --no-install-recommends \
        protobuf-compiler \
    && rm -rf /var/lib/apt/lists/* \
    && go install google.golang.org/protobuf/cmd/protoc-gen-go@v1.34.1 \
    && go install google.golang.org/grpc/cmd/protoc-gen-go-grpc@v1.3.0

ENV PATH="/root/go/bin:${PATH}"

WORKDIR /src

COPY test-server/go.mod ./
COPY test-server/main.go ./
COPY proto ./proto

RUN mkdir -p pb \
    && protoc -I=proto \
        --go_out=pb --go_opt=paths=source_relative \
        --go-grpc_out=pb --go-grpc_opt=paths=source_relative \
        proto/helloworld.proto \
    && go mod tidy \
    && CGO_ENABLED=0 go build -o /server .

FROM debian:trixie-slim
COPY --from=build /server /server
COPY test-server/certs/server.crt /certs/server.crt
COPY test-server/certs/server.key /certs/server.key
COPY test-server/certs/client.crt /certs/client.crt
EXPOSE 50051 50052 50053 50055 50056 50057
ENTRYPOINT ["/server"]
