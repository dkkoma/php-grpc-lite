package main

import (
	"bufio"
	"bytes"
	"context"
	"crypto/tls"
	"crypto/x509"
	"encoding/binary"
	"fmt"
	"io"
	"log"
	"net"
	"net/http"
	"os"
	"runtime"
	"strconv"
	"sync"
	"sync/atomic"
	"time"

	pb "example.com/helloworld/pb"
	"golang.org/x/net/http2"
	"golang.org/x/net/http2/h2c"
	"golang.org/x/net/http2/hpack"
	"google.golang.org/grpc"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/credentials"
	"google.golang.org/grpc/metadata"
	"google.golang.org/grpc/reflection"
	"google.golang.org/grpc/stats"
	"google.golang.org/grpc/status"
)

type server struct {
	pb.UnimplementedGreeterServer
}

type benchStatsContextKey struct{}

type benchRPCStats struct {
	mu                          sync.Mutex
	beginAt                     time.Time
	inPayloadSinceBeginNs       int64
	outHeaderSinceBeginNs       int64
	firstOutPayloadSinceBeginNs int64
	lastOutPayloadSinceBeginNs  int64
	outPayloadCount             int
	outPayloadSinceBeginNs      int64
	outPayloadLengthBytes       int
	outPayloadWireLengthBytes   int
	outPayloadCompressedBytes   int
}

type benchStatsHandler struct{}

var benchPayloadCache = map[int][]byte{}
var eofLifecycleConnections int64
var goAwayRefusedThenOKUnaryConnections int64
var goAwayRefusedThenOKStreamingConnections int64
var goAwayRefusedThenDelayedOKConnections int64

const maxHTTP2ClientStreamID = uint32((1 << 31) - 1)

func init() {
	for _, size := range []int{0, 100, 1024, 10 * 1024, 100 * 1024, 1024 * 1024} {
		benchPayloadCache[size] = make([]byte, size)
	}
}

func (s *server) SayHello(ctx context.Context, req *pb.HelloRequest) (*pb.HelloReply, error) {
	if d := req.GetDelayMs(); d > 0 {
		time.Sleep(time.Duration(d) * time.Millisecond)
	}
	return &pb.HelloReply{Message: "Hello, " + req.GetName()}, nil
}

func (s *server) SayManyHellos(req *pb.HelloRequest, stream pb.Greeter_SayManyHellosServer) error {
	log.Printf("SayManyHellos: name=%q", req.GetName())
	for i := 1; i <= 5; i++ {
		if err := stream.Send(&pb.HelloReply{
			Message: fmt.Sprintf("Hello #%d, %s", i, req.GetName()),
		}); err != nil {
			return err
		}
		time.Sleep(100 * time.Millisecond)
	}
	return nil
}

func (s *server) BenchUnary(ctx context.Context, req *pb.BenchRequest) (*pb.BenchReply, error) {
	started := time.Now()
	serverTiming := benchServerTimingEnabled(ctx)
	if err := benchErrorFromContext(ctx); err != nil {
		return nil, err
	}
	sendBenchAllMetadata(ctx)
	if d := req.GetServerDelayMs(); d > 0 {
		time.Sleep(time.Duration(d) * time.Millisecond)
	}
	payloadBytes := int(req.GetPayloadBytes())
	requestPayloadBytes := len(req.GetRequestPayload())
	payloadStarted := time.Now()
	payload := benchPayload(payloadBytes, benchCachedPayloadEnabled(ctx))
	payloadAllocDuration := time.Since(payloadStarted)
	if serverTiming {
		pairs := []string{
			"x-bench-server-handler-ns", strconv.FormatInt(time.Since(started).Nanoseconds(), 10),
			"x-bench-server-payload-alloc-ns", strconv.FormatInt(payloadAllocDuration.Nanoseconds(), 10),
			"x-bench-server-payload-bytes", strconv.Itoa(len(payload)),
			"x-bench-server-request-payload-bytes", strconv.Itoa(requestPayloadBytes),
		}
		if rpcStats := benchRPCStatsFromContext(ctx); rpcStats != nil {
			pairs = append(pairs,
				"x-bench-server-stats-handler-start-ns", strconv.FormatInt(rpcStats.sinceBegin(started), 10),
				"x-bench-server-stats-handler-end-ns", strconv.FormatInt(rpcStats.sinceBegin(time.Now()), 10),
			)
		}
		grpc.SetTrailer(ctx, metadata.Pairs(pairs...))
	}
	return &pb.BenchReply{Payload: payload}, nil
}

func benchPayload(size int, cached bool) []byte {
	if size <= 0 {
		return nil
	}
	if cached {
		if payload, ok := benchPayloadCache[size]; ok {
			return payload
		}
	}
	return make([]byte, size)
}

func benchServerTimingEnabled(ctx context.Context) bool {
	incoming, ok := metadata.FromIncomingContext(ctx)
	if !ok {
		return false
	}
	values := incoming.Get("x-bench-server-timing")
	return len(values) > 0 && values[0] == "1"
}

func benchServerStatsEnabled(ctx context.Context) bool {
	incoming, ok := metadata.FromIncomingContext(ctx)
	if !ok {
		return false
	}
	values := incoming.Get("x-bench-server-stats")
	return len(values) > 0 && values[0] == "1"
}

func benchCachedPayloadEnabled(ctx context.Context) bool {
	incoming, ok := metadata.FromIncomingContext(ctx)
	if !ok {
		return false
	}
	values := incoming.Get("x-bench-server-cached-payload")
	return len(values) > 0 && values[0] == "1"
}

func benchErrorFromContext(ctx context.Context) error {
	incoming, ok := metadata.FromIncomingContext(ctx)
	if !ok {
		return nil
	}
	code := firstMetadataInt(incoming, "x-bench-error-code")
	if code <= 0 {
		return nil
	}
	message := "bench error"
	if values := incoming.Get("x-bench-error-message"); len(values) > 0 && values[0] != "" {
		message = values[0]
	}
	// x-bench-error-details が指定されたら rich error model 相当の detail を付け、
	// grpc-status-details-bin trailer をクライアントに観測させる。
	if values := incoming.Get("x-bench-error-details"); len(values) > 0 && values[0] != "" {
		st := status.New(codes.Code(code), message)
		if detailed, err := st.WithDetails(&pb.BenchReply{Payload: []byte(values[0])}); err == nil {
			return detailed.Err()
		}
		return st.Err()
	}
	return status.Error(codes.Code(code), message)
}

func benchRPCStatsFromContext(ctx context.Context) *benchRPCStats {
	value, ok := ctx.Value(benchStatsContextKey{}).(*benchRPCStats)
	if !ok {
		return nil
	}
	return value
}

func (s *benchRPCStats) sinceBegin(t time.Time) int64 {
	if s == nil || s.beginAt.IsZero() || t.IsZero() {
		return 0
	}
	return t.Sub(s.beginAt).Nanoseconds()
}

func (s *benchRPCStats) setInPayload(t time.Time) {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.inPayloadSinceBeginNs = s.sinceBegin(t)
}

func (s *benchRPCStats) setOutHeader(t time.Time) {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.outHeaderSinceBeginNs = s.sinceBegin(t)
}

func (s *benchRPCStats) setOutPayload(t time.Time, lengthBytes int, wireLengthBytes int, compressedBytes int) {
	s.mu.Lock()
	defer s.mu.Unlock()
	sinceBegin := s.sinceBegin(t)
	if s.outPayloadCount == 0 {
		s.firstOutPayloadSinceBeginNs = sinceBegin
	}
	s.outPayloadCount++
	s.lastOutPayloadSinceBeginNs = sinceBegin
	s.outPayloadSinceBeginNs = sinceBegin
	s.outPayloadLengthBytes = lengthBytes
	s.outPayloadWireLengthBytes = wireLengthBytes
	s.outPayloadCompressedBytes = compressedBytes
}

func (s *benchRPCStats) trailerPairs() []string {
	s.mu.Lock()
	defer s.mu.Unlock()
	return []string{
		"x-bench-server-stats-in-payload-ns", strconv.FormatInt(s.inPayloadSinceBeginNs, 10),
		"x-bench-server-stats-out-header-ns", strconv.FormatInt(s.outHeaderSinceBeginNs, 10),
		"x-bench-server-stats-out-payload-ns", strconv.FormatInt(s.outPayloadSinceBeginNs, 10),
		"x-bench-server-stats-first-out-payload-ns", strconv.FormatInt(s.firstOutPayloadSinceBeginNs, 10),
		"x-bench-server-stats-last-out-payload-ns", strconv.FormatInt(s.lastOutPayloadSinceBeginNs, 10),
		"x-bench-server-stats-out-payload-count", strconv.Itoa(s.outPayloadCount),
		"x-bench-server-stats-out-payload-bytes", strconv.Itoa(s.outPayloadLengthBytes),
		"x-bench-server-stats-out-payload-wire-bytes", strconv.Itoa(s.outPayloadWireLengthBytes),
		"x-bench-server-stats-out-payload-compressed-bytes", strconv.Itoa(s.outPayloadCompressedBytes),
	}
}

func (h benchStatsHandler) TagRPC(ctx context.Context, info *stats.RPCTagInfo) context.Context {
	if !benchServerStatsEnabled(ctx) {
		return ctx
	}
	return context.WithValue(ctx, benchStatsContextKey{}, &benchRPCStats{beginAt: time.Now()})
}

func (h benchStatsHandler) HandleRPC(ctx context.Context, rpcStats stats.RPCStats) {
	collector := benchRPCStatsFromContext(ctx)
	if collector == nil {
		return
	}

	switch event := rpcStats.(type) {
	case *stats.InPayload:
		collector.setInPayload(event.RecvTime)
	case *stats.OutHeader:
		collector.setOutHeader(time.Now())
	case *stats.OutPayload:
		collector.setOutPayload(event.SentTime, event.Length, event.WireLength, event.CompressedLength)
		grpc.SetTrailer(ctx, metadata.Pairs(collector.trailerPairs()...))
	}
}

func (h benchStatsHandler) TagConn(ctx context.Context, info *stats.ConnTagInfo) context.Context {
	return ctx
}

func (h benchStatsHandler) HandleConn(ctx context.Context, connStats stats.ConnStats) {}

func sendBenchAllMetadata(ctx context.Context) {
	incoming, ok := metadata.FromIncomingContext(ctx)
	if !ok {
		return
	}

	initial := metadata.New(nil)
	trailing := metadata.New(nil)

	count := firstMetadataInt(incoming, "x-bench-response-metadata-count")
	valueBytes := firstMetadataInt(incoming, "x-bench-response-metadata-value-bytes")
	if valueBytes < 0 {
		valueBytes = 0
	}
	value := makeMetadataValue(valueBytes)
	for i := 0; i < count; i++ {
		initial.Append(fmt.Sprintf("x-bench-initial-%03d", i), value)
		trailing.Append(fmt.Sprintf("x-bench-trailing-%03d", i), value)
	}

	appendMetadataValues(initial, "x-bench-initial-bin", incoming.Get("x-bench-echo-bin"))
	appendMetadataValues(trailing, "x-bench-trailing-bin", incoming.Get("x-bench-echo-bin"))
	appendMetadataValues(initial, "x-bench-initial-ascii", incoming.Get("x-bench-echo-ascii"))
	appendMetadataValues(trailing, "x-bench-trailing-ascii", incoming.Get("x-bench-echo-ascii"))
	appendMetadataValues(initial, "x-bench-initial-duplicate", incoming.Get("x-bench-response-duplicate"))
	appendMetadataValues(trailing, "x-bench-trailing-duplicate", incoming.Get("x-bench-response-duplicate"))
	appendMetadataValues(initial, "x-bench-initial-duplicate-bin", incoming.Get("x-bench-response-duplicate-bin"))
	appendMetadataValues(trailing, "x-bench-trailing-duplicate-bin", incoming.Get("x-bench-response-duplicate-bin"))
	appendObservedMetadata(initial, incoming)

	if len(initial) > 0 {
		grpc.SendHeader(ctx, initial)
	}
	if len(trailing) > 0 {
		grpc.SetTrailer(ctx, trailing)
	}
}

func appendMetadataValues(md metadata.MD, key string, values []string) {
	for _, value := range values {
		md.Append(key, value)
	}
}

func appendObservedMetadata(md metadata.MD, incoming metadata.MD) {
	keys := incoming.Get("x-bench-observe-metadata-key")
	for keyIndex, key := range keys {
		values := incoming.Get(key)
		prefix := fmt.Sprintf("x-bench-seen-%03d", keyIndex)
		md.Append(prefix+"-key-bin", key)
		md.Append(prefix+"-count", strconv.Itoa(len(values)))
		for valueIndex, value := range values {
			valueKey := fmt.Sprintf("%s-value-%03d-bin", prefix, valueIndex)
			md.Append(valueKey, value)
		}
	}
}

func firstMetadataInt(md metadata.MD, key string) int {
	values := md.Get(key)
	if len(values) == 0 {
		return 0
	}
	var parsed int
	if _, err := fmt.Sscanf(values[0], "%d", &parsed); err != nil {
		return 0
	}
	return parsed
}

func makeMetadataValue(size int) string {
	if size <= 0 {
		return ""
	}
	buf := make([]byte, size)
	for i := range buf {
		buf[i] = byte('a' + (i % 26))
	}
	return string(buf)
}

func (s *server) BenchServerStream(req *pb.BenchRequest, stream pb.Greeter_BenchServerStreamServer) error {
	if err := benchErrorFromContext(stream.Context()); err != nil {
		return err
	}
	sendBenchAllMetadata(stream.Context())
	payload := make([]byte, req.GetPayloadBytes())
	delay := time.Duration(req.GetServerDelayMs()) * time.Millisecond
	count := int(req.GetMessageCount())
	for i := 0; i < count; i++ {
		if i > 0 && delay > 0 {
			time.Sleep(delay)
		}
		if err := stream.Send(&pb.BenchReply{Payload: payload}); err != nil {
			return err
		}
	}
	if rpcStats := benchRPCStatsFromContext(stream.Context()); rpcStats != nil {
		stream.SetTrailer(metadata.Pairs(rpcStats.trailerPairs()...))
	}
	return nil
}

func newServer(extraOptions ...grpc.ServerOption) *grpc.Server {
	options := []grpc.ServerOption{grpc.StatsHandler(benchStatsHandler{})}
	options = append(options, benchGrpcServerOptionsFromEnv()...)
	options = append(options, extraOptions...)
	s := grpc.NewServer(options...)
	pb.RegisterGreeterServer(s, &server{})
	reflection.Register(s)
	return s
}

func benchGrpcServerOptionsFromEnv() []grpc.ServerOption {
	// upload sweep (BenchRequest.request_payload) は 4MB payload + proto framing で
	// gRPC default の 4MB 受信上限を超えるため、上限を広げておく。
	options := []grpc.ServerOption{grpc.MaxRecvMsgSize(64 << 20)}
	if value, ok := envInt("TEST_SERVER_GRPC_MAX_RECV_MSG_SIZE"); ok {
		options = append(options, grpc.MaxRecvMsgSize(value))
	}
	if value, ok := envInt("TEST_SERVER_GRPC_INITIAL_WINDOW_SIZE"); ok {
		options = append(options, grpc.InitialWindowSize(int32(value)))
	}
	if value, ok := envInt("TEST_SERVER_GRPC_INITIAL_CONN_WINDOW_SIZE"); ok {
		options = append(options, grpc.InitialConnWindowSize(int32(value)))
	}
	if value, ok := envInt("TEST_SERVER_GRPC_READ_BUFFER_SIZE"); ok {
		options = append(options, grpc.ReadBufferSize(value))
	}
	if value, ok := envInt("TEST_SERVER_GRPC_WRITE_BUFFER_SIZE"); ok {
		options = append(options, grpc.WriteBufferSize(value))
	}
	return options
}

func envInt(key string) (int, bool) {
	raw := os.Getenv(key)
	if raw == "" {
		return 0, false
	}
	value, err := strconv.Atoi(raw)
	if err != nil {
		log.Printf("ignoring invalid %s=%q: %v", key, raw, err)
		return 0, false
	}
	log.Printf("%s=%d", key, value)
	return value, true
}

func serveNonGrpcH2C() {
	handler := http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Header.Get("x-bench-early-hints") == "1" {
			earlyHintsCount := 1
			if raw := r.Header.Get("x-bench-early-hints-count"); raw != "" {
				if parsed, err := strconv.Atoi(raw); err == nil && parsed > 0 && parsed <= 16 {
					earlyHintsCount = parsed
				}
			}
			for i := 0; i < earlyHintsCount; i++ {
				if r.Header.Get("x-bench-early-hints-pollution") == "1" {
					w.Header().Set("content-type", "text/plain")
					w.Header().Set("grpc-status", "13")
					w.Header().Set("grpc-message", "informational pollution")
					w.Header().Set("grpc-encoding", "gzip")
					w.Header().Set("x-bench-informational-only", "must-not-leak")
				}
				w.WriteHeader(http.StatusEarlyHints)
				w.Header().Del("content-type")
				w.Header().Del("grpc-status")
				w.Header().Del("grpc-message")
				w.Header().Del("grpc-encoding")
				w.Header().Del("x-bench-informational-only")
			}
		}
		if r.Header.Get("x-bench-observe-authority") == "1" {
			w.Header().Set("content-type", "application/grpc")
			w.Header().Set("x-bench-authority", r.Host)
			w.Header().Set("trailer", "grpc-status")
			w.WriteHeader(http.StatusOK)
			_, _ = w.Write(grpcFrame(0, nil))
			w.Header().Set("grpc-status", "0")
			return
		}
		if r.Header.Get("x-bench-grpc-response") == "compressed-flag" {
			w.Header().Set("content-type", "application/grpc")
			if encoding := r.Header.Get("x-bench-grpc-encoding"); encoding != "" {
				w.Header().Set("grpc-encoding", encoding)
			}
			if grpcStatus := r.Header.Get("x-bench-grpc-status"); grpcStatus != "" {
				w.Header().Set("trailer", "grpc-status")
				defer w.Header().Set("grpc-status", grpcStatus)
			}
			w.WriteHeader(http.StatusOK)
			_, _ = w.Write(grpcFrame(1, nil))
			return
		}
		if r.Header.Get("x-bench-grpc-response") == "headers-only" {
			// grpc-status を x-bench-grpc-status で指定すると trailers-only 応答、
			// 未指定なら grpc-status なしの headers-only END_STREAM になる。
			w.Header().Set("content-type", "application/grpc")
			if encoding := r.Header.Get("x-bench-grpc-encoding"); encoding != "" {
				w.Header().Set("grpc-encoding", encoding)
			}
			if grpcStatus := r.Header.Get("x-bench-grpc-status"); grpcStatus != "" {
				w.Header().Set("grpc-status", grpcStatus)
			}
			w.WriteHeader(http.StatusOK)
			return
		}
		if r.Header.Get("x-bench-grpc-response") == "custom-trailers-no-status" {
			// message のあと grpc-status を含まない trailing HEADERS で閉じる fixture。
			w.Header().Set("content-type", "application/grpc")
			w.Header().Set("trailer", "x-bench-trailer")
			w.WriteHeader(http.StatusOK)
			_, _ = w.Write(grpcFrame(0, nil))
			w.Header().Set("x-bench-trailer", "1")
			return
		}
		if r.Header.Get("x-bench-grpc-response") == "grpc-message-only-trailers" {
			// grpc-message はあるが grpc-status を含まない trailing HEADERS。
			w.Header().Set("content-type", "application/grpc")
			w.Header().Set("trailer", "grpc-message")
			w.WriteHeader(http.StatusOK)
			_, _ = w.Write(grpcFrame(0, nil))
			w.Header().Set("grpc-message", "trailers without status")
			return
		}
		if r.Header.Get("x-bench-grpc-response") == "two-messages" {
			w.Header().Set("content-type", "application/grpc")
			w.Header().Set("trailer", "grpc-status")
			w.WriteHeader(http.StatusOK)
			_, _ = w.Write(grpcFrame(0, nil))
			_, _ = w.Write(grpcFrame(0, nil))
			w.Header().Set("grpc-status", "0")
			return
		}
		if r.Header.Get("x-bench-grpc-response") == "partial-frame-abort" {
			// message 送信途中の RST_STREAM: truncation は framing violation では
			// なく stream abort として扱われることを固定する fixture。
			w.Header().Set("content-type", "application/grpc")
			w.WriteHeader(http.StatusOK)
			_, _ = w.Write([]byte{0, 0, 0, 0, 10, 1, 2, 3})
			if f, ok := w.(http.Flusher); ok {
				f.Flush()
			}
			panic(http.ErrAbortHandler)
		}
		if r.Header.Get("x-bench-grpc-response") == "partial-frame" {
			w.Header().Set("content-type", "application/grpc")
			w.Header().Set("trailer", "grpc-status")
			w.WriteHeader(http.StatusOK)
			_, _ = w.Write([]byte{0, 0, 0, 0, 10, 1, 2, 3})
			w.Header().Set("grpc-status", "0")
			return
		}
		if r.Header.Get("x-bench-grpc-response") == "no-trailers" {
			// message を届けたあと trailers (grpc-status) を送らず clean END_STREAM で
			// 閉じる fixture。trailers 欠落を INTERNAL に分類する経路を固定する。
			w.Header().Set("content-type", "application/grpc")
			w.WriteHeader(http.StatusOK)
			_, _ = w.Write(grpcFrame(0, nil))
			return
		}
		if r.Header.Get("x-bench-grpc-response") == "declared-large-truncated" {
			w.Header().Set("content-type", "application/grpc")
			w.Header().Set("trailer", "grpc-status")
			w.WriteHeader(http.StatusOK)
			_, _ = w.Write([]byte{0, 3, 0, 0, 0})
			w.Header().Set("grpc-status", "0")
			return
		}
		if encoding := r.Header.Get("x-bench-grpc-encoding"); encoding != "" {
			// grpc-encoding を advertise しつつ Compressed-Flag=0 の message を返す。
			// x-bench-grpc-status 指定時は trailer も送る (encoding 宣言だけでは
			// 失敗しないことを固定するため)。
			w.Header().Set("content-type", "application/grpc")
			w.Header().Set("grpc-encoding", encoding)
			if grpcStatus := r.Header.Get("x-bench-grpc-status"); grpcStatus != "" {
				w.Header().Set("trailer", "grpc-status")
				defer w.Header().Set("grpc-status", grpcStatus)
			}
			w.WriteHeader(http.StatusOK)
			_, _ = w.Write(grpcFrame(0, nil))
			return
		}
		if grpcStatus := r.Header.Get("x-bench-grpc-status"); grpcStatus != "" {
			contentType := r.Header.Get("x-bench-content-type")
			if contentType == "" {
				contentType = "application/grpc"
			}
			w.Header().Set("content-type", contentType)
			w.Header().Set("trailer", "grpc-status")
			w.WriteHeader(http.StatusOK)
			_, _ = w.Write(grpcFrame(0, nil))
			w.Header().Set("grpc-status", grpcStatus)
			return
		}
		status := http.StatusOK
		if raw := r.Header.Get("x-bench-http-status"); raw != "" {
			if parsed, err := strconv.Atoi(raw); err == nil {
				status = parsed
			}
		}
		contentType := r.Header.Get("x-bench-content-type")
		if contentType == "" {
			contentType = "text/plain"
		}
		w.Header().Set("content-type", contentType)
		w.WriteHeader(status)
		_, _ = w.Write([]byte("not a grpc response"))
	})
	httpServer := &http.Server{
		Addr:    ":50054",
		Handler: h2c.NewHandler(handler, &http2.Server{}),
	}
	log.Printf("listening on :50054 (h2c, non-gRPC)")
	if err := httpServer.ListenAndServe(); err != nil {
		log.Fatalf("non-grpc h2c serve error: %v", err)
	}
}

func serveLifecycleH2C() {
	go serveRawH2C(":50055", true, false)
	go serveRawH2C(":50056", false, true)
	go serveMidStreamFailureH2C(":50057")
	go serveServerRstThenOKH2C(":50058")
	go serveServerRstThenOKH2C(":50059")
	go serveGoAwayRefusedH2C(":50060")
	go serveGoAwayRefusedThenOKH2C(":50061", &goAwayRefusedThenOKUnaryConnections)
	go serveGoAwayMaxThenOKH2C(":50062")
	go serveGoAwayRefusedThenOKH2C(":50063", &goAwayRefusedThenOKStreamingConnections)
	go serveGoAwayAfterMessageH2C(":50064")
	go serveGoAwayRefusedThenDelayedOKH2C(":50065")
	go serveStreamThenKillOnSecondRequestH2C(":50066")
	go serveGoAwayMaxKeepStreamOpenH2C(":50067")
	go serveBacklogFloodH2C(":50068", ":50069")
	go serveSmallWindowGoAwayDrainingH2C(":50070")
	go serveInformationalAdversarialH2C(":50071")
}

// serveInformationalAdversarialH2C is a raw HTTP/2 fixture for response
// sequences that net/http deliberately refuses to write. Request metadata
// selects a sequence, while the connection remains open after stream-local
// protocol failures so tests can verify that the next RPC reuses it.
type incompleteHeaderRstProbe struct {
	mu                         sync.Mutex
	finished                   chan struct{}
	finishOnce                 sync.Once
	expectedRstCode            http2.ErrCode
	targetRstObserved          bool
	siblingStreamID            uint32
	siblingDataBeforeTargetRst bool
	siblingDataAfterTargetRst  bool
}

func newIncompleteHeaderRstProbe(siblingStreamID uint32, expectedRstCode http2.ErrCode) *incompleteHeaderRstProbe {
	return &incompleteHeaderRstProbe{
		finished:        make(chan struct{}),
		expectedRstCode: expectedRstCode,
		siblingStreamID: siblingStreamID,
	}
}

func (p *incompleteHeaderRstProbe) observeTargetRst(errCode http2.ErrCode) {
	p.mu.Lock()
	if errCode == p.expectedRstCode {
		p.targetRstObserved = true
	}
	p.mu.Unlock()
	if p.siblingStreamID == 0 && errCode == p.expectedRstCode {
		p.finish()
	}
}

func (p *incompleteHeaderRstProbe) observeData(streamID uint32) {
	if p.siblingStreamID == 0 || p.siblingStreamID != streamID {
		return
	}
	p.mu.Lock()
	if p.targetRstObserved {
		p.siblingDataAfterTargetRst = true
	} else {
		p.siblingDataBeforeTargetRst = true
	}
	p.mu.Unlock()
}

func (p *incompleteHeaderRstProbe) targetRstWasObserved() bool {
	p.mu.Lock()
	defer p.mu.Unlock()
	return p.targetRstObserved
}

func (p *incompleteHeaderRstProbe) finish() {
	p.finishOnce.Do(func() {
		close(p.finished)
	})
}

func (p *incompleteHeaderRstProbe) passed() bool {
	p.mu.Lock()
	defer p.mu.Unlock()
	return p.targetRstObserved &&
		(p.siblingStreamID == 0 || p.siblingDataBeforeTargetRst) &&
		!p.siblingDataAfterTargetRst
}

type incompleteHeaderRstProbeStore struct {
	mu          sync.Mutex
	byAuthority map[string]*incompleteHeaderRstProbe
}

func newIncompleteHeaderRstProbeStore() *incompleteHeaderRstProbeStore {
	return &incompleteHeaderRstProbeStore{
		byAuthority: make(map[string]*incompleteHeaderRstProbe),
	}
}

func (s *incompleteHeaderRstProbeStore) begin(authority string, siblingStreamID uint32, expectedRstCode http2.ErrCode) *incompleteHeaderRstProbe {
	probe := newIncompleteHeaderRstProbe(siblingStreamID, expectedRstCode)
	s.mu.Lock()
	s.byAuthority[authority] = probe
	s.mu.Unlock()
	return probe
}

func (s *incompleteHeaderRstProbeStore) waitAndConsume(authority string) bool {
	s.mu.Lock()
	probe := s.byAuthority[authority]
	s.mu.Unlock()
	if probe == nil {
		return false
	}

	select {
	case <-probe.finished:
	case <-time.After(3 * time.Second):
	}

	s.mu.Lock()
	if s.byAuthority[authority] == probe {
		delete(s.byAuthority, authority)
	}
	s.mu.Unlock()
	return probe.passed()
}

func serveInformationalAdversarialH2C(addr string) {
	lis, err := net.Listen("tcp", addr)
	if err != nil {
		log.Fatalf("failed to listen %s: %v", addr, err)
	}
	log.Printf("listening on %s (raw h2c informational adversarial fixture)", addr)
	incompleteRstProbes := newIncompleteHeaderRstProbeStore()
	for {
		conn, err := lis.Accept()
		if err != nil {
			log.Printf("accept %s error: %v", addr, err)
			continue
		}
		go handleInformationalAdversarialH2C(conn, incompleteRstProbes)
	}
}

func handleInformationalAdversarialH2C(conn net.Conn, incompleteRstProbes *incompleteHeaderRstProbeStore) {
	defer conn.Close()
	if !readClientPreface(conn) {
		return
	}
	framer := http2.NewFramer(conn, conn)
	framer.ReadMetaHeaders = hpack.NewDecoder(4096, nil)
	if err := framer.WriteSettings(http2.Setting{ID: http2.SettingInitialWindowSize, Val: 1024}); err != nil {
		return
	}

	requests := make(map[uint32]map[string]string)
	resourceProbeStreamID := uint32(0)
	resourceProbeCancelObserved := false
	statusProbeStreamID := uint32(0)
	statusProbeCancelObserved := false
	heldSiblingStreamID := uint32(0)
	incompleteStreamProbes := make(map[uint32]*incompleteHeaderRstProbe)
	defer func() {
		for _, probe := range incompleteStreamProbes {
			probe.finish()
		}
	}()
	pendingPushedResponses := make(map[uint32]uint32)
	nextPromisedStreamID := uint32(2)
	writeResponse := func(streamID uint32, request map[string]string) {
		control := request["x-bench-raw-response"]
		if control == "require-prior-incomplete-status-cancel" {
			if incompleteRstProbes.waitAndConsume(request[":authority"]) {
				writeRawGrpcOK(framer, streamID, false)
			} else {
				writeRawGrpcFixtureError(framer, streamID, "expected incomplete-block RST was not observed by peer or sibling DATA crossed quarantine")
			}
			return
		}
		if expectedRstCode, incomplete := rawResponseIncompleteExpectedRst(control); incomplete {
			incompleteStreamProbes[streamID] = incompleteRstProbes.begin(request[":authority"], heldSiblingStreamID, expectedRstCode)
			// Keep the peer silent long enough to prove that a deadline-less
			// client terminates itself, while bounding a regression in the test
			// suite instead of retaining the fixture connection indefinitely.
			_ = conn.SetReadDeadline(time.Now().Add(2 * time.Second))
		}
		if control == "foreign-pushed-stream-protocol-rst" {
			promisedStreamID := nextPromisedStreamID
			nextPromisedStreamID += 2
			if err := writeRawForeignPushedStreamProtocolError(framer, streamID, promisedStreamID, request[":authority"]); err != nil {
				writeRawGrpcFixtureError(framer, streamID, "failed to send pushed-stream protocol probe")
				return
			}
			pendingPushedResponses[promisedStreamID] = streamID
			return
		}
		writeInformationalAdversarialResponse(
			framer,
			streamID,
			request,
			&resourceProbeStreamID,
			&resourceProbeCancelObserved,
			&statusProbeStreamID,
			&statusProbeCancelObserved,
			&heldSiblingStreamID,
		)
		if control == "multiplex-incomplete-entry-budget" {
			// Leave the peer unread beyond the transport's fixed terminal-flush
			// grace. A generic session flush would fill the socket with the held
			// sibling's newly unblocked request DATA before returning.
			time.Sleep(750 * time.Millisecond)
		}
	}
	for {
		frame, err := framer.ReadFrame()
		if err != nil {
			return
		}
		switch f := frame.(type) {
		case *http2.SettingsFrame:
			if !f.IsAck() {
				_ = framer.WriteSettingsAck()
			}
		case *http2.MetaHeadersFrame:
			streamID := f.Header().StreamID
			request := make(map[string]string)
			for _, field := range f.Fields {
				request[field.Name] = field.Value
			}
			requests[streamID] = request
			if request["x-bench-raw-response"] == "multiplex-hold-sibling" {
				// Respond before consuming the large request body. The advertised
				// tiny stream window leaves its DATA provider deferred in nghttp2.
				writeResponse(streamID, request)
				delete(requests, streamID)
			} else if f.StreamEnded() {
				writeResponse(streamID, request)
				delete(requests, streamID)
			}
		case *http2.DataFrame:
			streamID := f.Header().StreamID
			if streamID == heldSiblingStreamID && len(incompleteStreamProbes) == 0 {
				// Replenish only the connection window before the malformed target
				// response. The reduced sibling stream window stays exhausted, while
				// the small target request can still reach END_STREAM and trigger the
				// quarantine probe.
				if err := framer.WriteWindowUpdate(0, uint32(len(f.Data()))); err != nil {
					return
				}
			}
			for _, probe := range incompleteStreamProbes {
				probe.observeData(streamID)
				if probe.targetRstWasObserved() {
					_ = conn.SetReadDeadline(time.Now().Add(100 * time.Millisecond))
				}
			}
			if f.StreamEnded() {
				if request, ok := requests[streamID]; ok {
					writeResponse(streamID, request)
					delete(requests, streamID)
				}
			}
		case *http2.RSTStreamFrame:
			streamID := f.Header().StreamID
			if probe := incompleteStreamProbes[streamID]; probe != nil {
				probe.observeTargetRst(f.ErrCode)
				if probe.siblingStreamID != 0 {
					// Drain any bytes already queued after the RST until the socket is
					// idle; only then publish the no-sibling-DATA result cross-connection.
					_ = conn.SetReadDeadline(time.Now().Add(100 * time.Millisecond))
				}
			}
			if streamID == resourceProbeStreamID && f.ErrCode == http2.ErrCodeCancel {
				resourceProbeCancelObserved = true
			}
			if streamID == statusProbeStreamID && f.ErrCode == http2.ErrCodeCancel {
				statusProbeCancelObserved = true
			}
			if mainStreamID, ok := pendingPushedResponses[streamID]; ok {
				if f.ErrCode == http2.ErrCodeProtocol {
					writeRawGrpcOK(framer, mainStreamID, false)
				} else {
					writeRawGrpcFixtureError(framer, mainStreamID, "pushed stream was not reset with PROTOCOL_ERROR")
				}
				delete(pendingPushedResponses, streamID)
			}
			delete(requests, streamID)
		}
	}
}

func writeInformationalAdversarialResponse(
	framer *http2.Framer,
	streamID uint32,
	request map[string]string,
	resourceProbeStreamID *uint32,
	resourceProbeCancelObserved *bool,
	statusProbeStreamID *uint32,
	statusProbeCancelObserved *bool,
	heldSiblingStreamID *uint32,
) {
	control := request["x-bench-raw-response"]
	switch control {
	case "trailer-without-end-stream":
		writeRawGrpcInitial(framer, streamID, "application/grpc")
		_ = framer.WriteData(streamID, false, grpcFrame(0, nil))
		writeRawHeaders(framer, streamID, false, []hpack.HeaderField{
			{Name: "grpc-status", Value: "0"},
		})
	case "informational-end-stream":
		writeRawHeaders(framer, streamID, true, []hpack.HeaderField{
			{Name: ":status", Value: "103"},
		})
	case "incomplete-informational-end-stream":
		writeRawHeadersWithEndHeaders(framer, streamID, true, false, []hpack.HeaderField{
			{Name: ":status", Value: "103"},
		})
	case "incomplete-trailer-without-end-stream":
		writeRawGrpcInitial(framer, streamID, "application/grpc")
		_ = framer.WriteData(streamID, false, grpcFrame(0, nil))
		writeRawHeadersWithEndHeaders(framer, streamID, false, false, []hpack.HeaderField{
			{Name: "grpc-status", Value: "0"},
		})
	case "informational-then-missing-status":
		writeRawInformational(framer, streamID, nil)
		writeRawHeaders(framer, streamID, true, []hpack.HeaderField{
			{Name: "x-after", Value: "missing-status"},
		})
	case "post-informational-incomplete-regular-before-status":
		writeRawPostInformationalIncompleteFieldBeforeStatus(framer, streamID, hpack.HeaderField{
			Name: "x-after", Value: "before-status",
		})
	case "post-informational-incomplete-invalid-before-status":
		writeRawPostInformationalIncompleteFieldBeforeStatus(framer, streamID, hpack.HeaderField{
			Name: "x-after", Value: "a\x00b",
		})
	case "post-informational-incomplete-empty-name-before-status":
		writeRawPostInformationalIncompleteEmptyNameBeforeStatus(framer, streamID)
	case "informational-then-data":
		writeRawInformational(framer, streamID, nil)
		_ = framer.WriteData(streamID, true, grpcFrame(0, nil))
	case "informational-entry-budget":
		*resourceProbeStreamID = streamID
		*resourceProbeCancelObserved = false
		for i := 0; i < 65; i++ {
			writeRawInformational(framer, streamID, []hpack.HeaderField{
				{Name: "x-info", Value: "a"},
			})
		}
	case "informational-incomplete-entry-budget":
		writeRawIncompleteInformationalEntryBudget(framer, streamID)
	case "informational-incomplete-invalid-entry-budget":
		writeRawIncompleteInformationalInvalidEntryBudget(framer, streamID)
	case "informational-byte-budget":
		*resourceProbeStreamID = streamID
		*resourceProbeCancelObserved = false
		value := makeMetadataValue(241)
		for i := 0; i < 4; i++ {
			writeRawInformational(framer, streamID, []hpack.HeaderField{
				{Name: "x-info", Value: value},
			})
		}
	case "informational-invalid-entry-budget":
		*resourceProbeStreamID = streamID
		*resourceProbeCancelObserved = false
		fields := []hpack.HeaderField{{Name: ":status", Value: "103"}}
		for i := 0; i < 129; i++ {
			fields = append(fields, hpack.HeaderField{Name: "x-ignored", Value: "a\x00b"})
		}
		writeRawHeaders(framer, streamID, false, fields)
	case "informational-invalid-byte-budget":
		*resourceProbeStreamID = streamID
		*resourceProbeCancelObserved = false
		writeRawInformational(framer, streamID, []hpack.HeaderField{
			{Name: "x-ignored", Value: makeMetadataValue(2048) + "\x00"},
		})
	case "informational-default-byte-budget":
		*resourceProbeStreamID = streamID
		*resourceProbeCancelObserved = false
		value := makeMetadataValue(8192)
		for i := 0; i < 8; i++ {
			writeRawInformational(framer, streamID, []hpack.HeaderField{
				{Name: "x-info", Value: value},
			})
		}
	case "require-prior-resource-probe":
		if *resourceProbeCancelObserved {
			writeRawGrpcOK(framer, streamID, false)
		} else {
			writeRawGrpcFixtureError(framer, streamID, "resource probe CANCEL was not observed on this connection")
		}
		*resourceProbeStreamID = 0
		*resourceProbeCancelObserved = false
	case "post-informational-silent-grpc-status":
		*statusProbeStreamID = streamID
		*statusProbeCancelObserved = false
		writeRawPostInformationalNonterminalStatusField(framer, streamID, hpack.HeaderField{
			Name: "grpc-status", Value: "0",
		})
	case "post-informational-silent-grpc-message":
		*statusProbeStreamID = streamID
		*statusProbeCancelObserved = false
		writeRawPostInformationalNonterminalStatusField(framer, streamID, hpack.HeaderField{
			Name: "grpc-message", Value: "premature status message",
		})
	case "post-informational-silent-status-details":
		*statusProbeStreamID = streamID
		*statusProbeCancelObserved = false
		writeRawPostInformationalNonterminalStatusField(framer, streamID, hpack.HeaderField{
			Name: "grpc-status-details-bin", Value: "AA==",
		})
	case "post-informational-incomplete-grpc-status":
		writeRawPostInformationalIncompleteStatusField(framer, streamID, hpack.HeaderField{
			Name: "grpc-status", Value: "0",
		})
	case "post-informational-incomplete-grpc-message":
		writeRawPostInformationalIncompleteStatusField(framer, streamID, hpack.HeaderField{
			Name: "grpc-message", Value: "premature status message",
		})
	case "post-informational-incomplete-status-details":
		writeRawPostInformationalIncompleteStatusField(framer, streamID, hpack.HeaderField{
			Name: "grpc-status-details-bin", Value: "AA==",
		})
	case "multiplex-hold-sibling":
		if *heldSiblingStreamID != 0 {
			writeRawGrpcFixtureError(framer, streamID, "a sibling stream is already held on this connection")
			return
		}
		*heldSiblingStreamID = streamID
		writeRawGrpcInitial(framer, streamID, "application/grpc")
		_ = framer.WriteData(streamID, false, grpcFrame(0, nil))
	case "multiplex-incomplete-entry-budget":
		if *heldSiblingStreamID == 0 || *heldSiblingStreamID == streamID {
			writeRawGrpcFixtureError(framer, streamID, "incomplete budget target did not reuse the held sibling connection")
			return
		}
		// Make the held request's multi-megabyte DATA sendable before the
		// malformed target response. The delay intentionally gives the client a
		// separate recv/send turn; the probe positively requires this legal
		// pre-target-RST DATA and rejects only DATA observed after the target RST.
		_ = framer.WriteWindowUpdate(0, 32*1024*1024)
		_ = framer.WriteWindowUpdate(*heldSiblingStreamID, 32*1024*1024)
		time.Sleep(50 * time.Millisecond)
		writeRawIncompleteInformationalEntryBudget(framer, streamID)
	case "require-prior-status-probe":
		if *statusProbeCancelObserved {
			writeRawGrpcOK(framer, streamID, false)
		} else {
			writeRawGrpcFixtureError(framer, streamID, "status-field probe CANCEL was not observed on this connection")
		}
		*statusProbeStreamID = 0
		*statusProbeCancelObserved = false
	case "invalid-status-metadata":
		writeRawHeaders(framer, streamID, true, []hpack.HeaderField{
			{Name: ":status", Value: "200"},
			{Name: "content-type", Value: "application/grpc"},
			{Name: "x-before", Value: "a"},
			{Name: "grpc-status", Value: "17"},
			{Name: "x-after", Value: "b"},
		})
	case "post-informational-terminal-message-only-metadata":
		writeRawInformational(framer, streamID, nil)
		writeRawHeaders(framer, streamID, true, []hpack.HeaderField{
			{Name: ":status", Value: "200"},
			{Name: "content-type", Value: "application/grpc"},
			{Name: "x-before", Value: "a"},
			{Name: "grpc-message", Value: "detail"},
			{Name: "x-after", Value: "b"},
		})
	case "post-informational-terminal-status-details-only-metadata":
		writeRawInformational(framer, streamID, nil)
		writeRawHeaders(framer, streamID, true, []hpack.HeaderField{
			{Name: ":status", Value: "200"},
			{Name: "content-type", Value: "application/grpc"},
			{Name: "x-before", Value: "a"},
			{Name: "grpc-status-details-bin", Value: "AA=="},
			{Name: "x-after", Value: "b"},
		})
	case "post-informational-nonterminal-status":
		writeRawInformational(framer, streamID, nil)
		writeRawHeaders(framer, streamID, false, []hpack.HeaderField{
			{Name: ":status", Value: "200"},
			{Name: "content-type", Value: "application/grpc"},
			{Name: "grpc-status", Value: "0"},
		})
		_ = framer.WriteData(streamID, true, grpcFrame(0, nil))
	case "post-informational-nonterminal-status-details":
		writeRawInformational(framer, streamID, nil)
		writeRawHeaders(framer, streamID, false, []hpack.HeaderField{
			{Name: ":status", Value: "200"},
			{Name: "content-type", Value: "application/grpc"},
			{Name: "grpc-status-details-bin", Value: "AA=="},
		})
		_ = framer.WriteData(streamID, false, grpcFrame(0, nil))
		writeRawHeaders(framer, streamID, true, []hpack.HeaderField{
			{Name: "grpc-status", Value: "0"},
		})
	case "valid-informational-iteration-reset":
		for i := 0; i < 60; i++ {
			writeRawInformational(framer, streamID, nil)
		}
		writeRawInformational(framer, streamID, []hpack.HeaderField{
			{Name: "content-type", Value: "text/plain"},
			{Name: "grpc-status", Value: "13"},
			{Name: "grpc-message", Value: "informational pollution"},
			{Name: "grpc-encoding", Value: "gzip"},
			{Name: "x-bench-informational-only", Value: "must-not-leak"},
		})
		writeRawGrpcOK(framer, streamID, false)
	case "valid-informational-byte-iteration-reset":
		infoValue := string(bytes.Repeat([]byte{'a'}, 8192))
		for i := 0; i < 4; i++ {
			writeRawInformational(framer, streamID, []hpack.HeaderField{
				{Name: "x-info", Value: infoValue},
			})
		}
		writeRawGrpcOK(framer, streamID, false)
	default:
		writeRawGrpcOK(framer, streamID, false)
	}
}

func writeRawPostInformationalNonterminalStatusField(framer *http2.Framer, streamID uint32, field hpack.HeaderField) {
	writeRawInformational(framer, streamID, nil)
	writeRawHeaders(framer, streamID, false, []hpack.HeaderField{
		{Name: ":status", Value: "200"},
		{Name: "content-type", Value: "application/grpc"},
		field,
	})
}

func rawResponseIncompleteExpectedRst(control string) (http2.ErrCode, bool) {
	switch control {
	case "incomplete-informational-end-stream",
		"incomplete-trailer-without-end-stream",
		"post-informational-incomplete-regular-before-status",
		"post-informational-incomplete-invalid-before-status",
		"post-informational-incomplete-empty-name-before-status":
		return http2.ErrCodeProtocol, true
	case "informational-incomplete-entry-budget",
		"informational-incomplete-invalid-entry-budget",
		"post-informational-incomplete-grpc-status",
		"post-informational-incomplete-grpc-message",
		"post-informational-incomplete-status-details",
		"multiplex-incomplete-entry-budget":
		return http2.ErrCodeCancel, true
	default:
		return 0, false
	}
}

func writeRawIncompleteInformationalEntryBudget(framer *http2.Framer, streamID uint32) {
	fields := []hpack.HeaderField{{Name: ":status", Value: "103"}}
	for i := 0; i < 128; i++ {
		fields = append(fields, hpack.HeaderField{Name: "x-info", Value: "a"})
	}
	writeRawHeadersWithEndHeaders(framer, streamID, false, false, fields)
}

func writeRawIncompleteInformationalInvalidEntryBudget(framer *http2.Framer, streamID uint32) {
	fields := []hpack.HeaderField{{Name: ":status", Value: "103"}}
	for i := 0; i < 129; i++ {
		fields = append(fields, hpack.HeaderField{Name: "x-ignored", Value: "a\x00b"})
	}
	writeRawHeadersWithEndHeaders(framer, streamID, false, false, fields)
}

func writeRawPostInformationalIncompleteFieldBeforeStatus(framer *http2.Framer, streamID uint32, field hpack.HeaderField) {
	writeRawInformational(framer, streamID, nil)
	writeRawHeadersWithEndHeaders(framer, streamID, true, false, []hpack.HeaderField{field})
}

func writeRawPostInformationalIncompleteEmptyNameBeforeStatus(framer *http2.Framer, streamID uint32) {
	writeRawInformational(framer, streamID, nil)
	// Literal without indexing: empty name, value "v".
	_ = framer.WriteHeaders(http2.HeadersFrameParam{
		StreamID:      streamID,
		BlockFragment: []byte{0x00, 0x00, 0x01, 'v'},
		EndHeaders:    false,
		EndStream:     true,
	})
}

func writeRawPostInformationalIncompleteStatusField(framer *http2.Framer, streamID uint32, field hpack.HeaderField) {
	writeRawInformational(framer, streamID, nil)
	writeRawHeadersWithEndHeaders(framer, streamID, false, false, []hpack.HeaderField{
		{Name: ":status", Value: "200"},
		{Name: "content-type", Value: "application/grpc"},
		field,
	})
}

func writeRawForeignPushedStreamProtocolError(framer *http2.Framer, mainStreamID uint32, promisedStreamID uint32, authority string) error {
	if authority == "" {
		authority = "test-server:50071"
	}
	if err := framer.WritePushPromise(http2.PushPromiseParam{
		StreamID:   mainStreamID,
		PromiseID:  promisedStreamID,
		EndHeaders: true,
		BlockFragment: encodeHeaders([]hpack.HeaderField{
			{Name: ":method", Value: "GET"},
			{Name: ":scheme", Value: "http"},
			{Name: ":authority", Value: authority},
			{Name: ":path", Value: "/pushed-informational-probe"},
		}),
	}); err != nil {
		return err
	}
	return framer.WriteHeaders(http2.HeadersFrameParam{
		StreamID:      promisedStreamID,
		BlockFragment: encodeHeaders([]hpack.HeaderField{{Name: ":status", Value: "103"}}),
		EndHeaders:    true,
		EndStream:     true,
	})
}

func writeRawGrpcFixtureError(framer *http2.Framer, streamID uint32, message string) {
	writeRawHeaders(framer, streamID, true, []hpack.HeaderField{
		{Name: ":status", Value: "200"},
		{Name: "content-type", Value: "application/grpc"},
		{Name: "grpc-status", Value: "13"},
		{Name: "grpc-message", Value: message},
	})
}

func writeRawInformational(framer *http2.Framer, streamID uint32, extra []hpack.HeaderField) {
	fields := []hpack.HeaderField{{Name: ":status", Value: "103"}}
	fields = append(fields, extra...)
	writeRawHeaders(framer, streamID, false, fields)
}

func writeRawGrpcInitial(framer *http2.Framer, streamID uint32, contentType string) {
	writeRawHeaders(framer, streamID, false, []hpack.HeaderField{
		{Name: ":status", Value: "200"},
		{Name: "content-type", Value: contentType},
	})
}

func writeRawHeaders(framer *http2.Framer, streamID uint32, endStream bool, fields []hpack.HeaderField) {
	writeRawHeadersWithEndHeaders(framer, streamID, endStream, true, fields)
}

func writeRawHeadersWithEndHeaders(framer *http2.Framer, streamID uint32, endStream bool, endHeaders bool, fields []hpack.HeaderField) {
	_ = framer.WriteHeaders(http2.HeadersFrameParam{
		StreamID:      streamID,
		BlockFragment: encodeHeaders(fields),
		EndHeaders:    endHeaders,
		EndStream:     endStream,
	})
}

// serveSmallWindowGoAwayDrainingH2C advertises a tiny INITIAL_WINDOW_SIZE
// (1KiB) so a large request body on the first stream (A) stays flow-control
// deferred inside the client's nghttp2 session. When the second stream (B)
// finishes its request the fixture answers B with one message and sends a
// draining-only GOAWAY(MaxInt32). 500ms later it opens A's window (which
// would resume A's deferred DATA if A were still alive), then completes B.
// This reproduces the original draining use-after-free shape: destroying A
// while draining must RST A's stream before the call state is freed, or the
// resumed DATA would read a dangling data provider when B keeps driving the
// session.
func serveSmallWindowGoAwayDrainingH2C(addr string) {
	lis, err := net.Listen("tcp", addr)
	if err != nil {
		log.Fatalf("failed to listen %s: %v", addr, err)
	}
	log.Printf("listening on %s (raw h2c small-window GOAWAY draining fixture)", addr)
	for {
		conn, err := lis.Accept()
		if err != nil {
			log.Printf("accept %s error: %v", addr, err)
			continue
		}
		go handleSmallWindowGoAwayDrainingH2C(conn)
	}
}

func handleSmallWindowGoAwayDrainingH2C(conn net.Conn) {
	defer conn.Close()
	if !readClientPreface(conn) {
		return
	}
	framer := http2.NewFramer(conn, conn)
	var mu sync.Mutex
	if err := framer.WriteSettings(http2.Setting{ID: http2.SettingInitialWindowSize, Val: 1024}); err != nil {
		return
	}
	// Open the connection-level window right away: stream A's large request
	// exhausts the default 64KiB connection window before the client even
	// sees our SETTINGS, and stream B's request DATA must still go through.
	// Only A's (per-stream) window stays tiny so its DATA remains deferred.
	if err := framer.WriteWindowUpdate(0, 1<<20); err != nil {
		return
	}

	var streamA, streamB uint32
	respondedB := false
	for {
		frame, err := framer.ReadFrame()
		if err != nil {
			return
		}
		switch f := frame.(type) {
		case *http2.SettingsFrame:
			if !f.IsAck() {
				mu.Lock()
				_ = framer.WriteSettingsAck()
				mu.Unlock()
			}
		case *http2.HeadersFrame:
			id := f.Header().StreamID
			if streamA == 0 {
				// Stream A: answer with one message right away, without
				// consuming its (large, window-starved) request body, so
				// the client keeps A's remaining request DATA deferred
				// while already delivering messages to userland.
				streamA = id
				mu.Lock()
				headers := encodeHeaders([]hpack.HeaderField{
					{Name: ":status", Value: "200"},
					{Name: "content-type", Value: "application/grpc"},
				})
				_ = framer.WriteHeaders(http2.HeadersFrameParam{
					StreamID:      streamA,
					BlockFragment: headers,
					EndHeaders:    true,
				})
				_ = framer.WriteData(streamA, false, grpcFrame(0, nil))
				mu.Unlock()
			} else if streamB == 0 && id != streamA {
				streamB = id
			}
		case *http2.DataFrame:
			if f.Header().StreamID == streamB && f.StreamEnded() && !respondedB {
				respondedB = true
				mu.Lock()
				headers := encodeHeaders([]hpack.HeaderField{
					{Name: ":status", Value: "200"},
					{Name: "content-type", Value: "application/grpc"},
				})
				_ = framer.WriteHeaders(http2.HeadersFrameParam{
					StreamID:      streamB,
					BlockFragment: headers,
					EndHeaders:    true,
				})
				// GOAWAY before B's message: TCP ordering then guarantees
				// the client is draining by the time B's first message
				// reaches userland (and A's destructor runs).
				_ = framer.WriteGoAway(uint32(2147483647), http2.ErrCodeNo, nil)
				_ = framer.WriteData(streamB, false, grpcFrame(0, nil))
				mu.Unlock()
				a, b := streamA, streamB
				time.AfterFunc(500*time.Millisecond, func() {
					mu.Lock()
					defer mu.Unlock()
					_ = framer.WriteWindowUpdate(0, 1<<20)
					_ = framer.WriteWindowUpdate(a, 1<<20)
					_ = framer.WriteData(b, false, grpcFrame(0, nil))
					trailers := encodeHeaders([]hpack.HeaderField{
						{Name: "grpc-status", Value: "0"},
					})
					_ = framer.WriteHeaders(http2.HeadersFrameParam{
						StreamID:      b,
						BlockFragment: trailers,
						EndHeaders:    true,
						EndStream:     true,
					})
				})
			}
		case *http2.RSTStreamFrame:
			// Stream A cancelled by the client destructor; nothing to do.
		}
	}
}

// serveGoAwayMaxKeepStreamOpenH2C answers the first request with response
// HEADERS plus one gRPC message, then sends GOAWAY(MaxInt32) (draining-only
// two-stage GOAWAY: the stream stays admitted) and keeps the stream open
// without trailers. Lets a client test pin that an explicit cancel on a
// draining connection still emits RST_STREAM for the admitted stream.
func serveGoAwayMaxKeepStreamOpenH2C(addr string) {
	lis, err := net.Listen("tcp", addr)
	if err != nil {
		log.Fatalf("failed to listen %s: %v", addr, err)
	}
	log.Printf("listening on %s (raw h2c GOAWAY-max keep-stream-open fixture)", addr)
	for {
		conn, err := lis.Accept()
		if err != nil {
			log.Printf("accept %s error: %v", addr, err)
			continue
		}
		go handleGoAwayMaxKeepStreamOpenH2C(conn)
	}
}

func handleGoAwayMaxKeepStreamOpenH2C(conn net.Conn) {
	defer conn.Close()
	if !readClientPreface(conn) {
		return
	}
	framer := http2.NewFramer(conn, conn)
	if err := framer.WriteSettings(); err != nil {
		return
	}

	var streamID uint32
	for {
		frame, err := framer.ReadFrame()
		if err != nil {
			return
		}
		switch f := frame.(type) {
		case *http2.SettingsFrame:
			if !f.IsAck() {
				_ = framer.WriteSettingsAck()
			}
		case *http2.HeadersFrame:
			if streamID == 0 {
				streamID = f.Header().StreamID
			}
		case *http2.DataFrame:
			if f.Header().StreamID == streamID && f.StreamEnded() {
				headers := encodeHeaders([]hpack.HeaderField{
					{Name: ":status", Value: "200"},
					{Name: "content-type", Value: "application/grpc"},
				})
				_ = framer.WriteHeaders(http2.HeadersFrameParam{
					StreamID:      streamID,
					BlockFragment: headers,
					EndHeaders:    true,
				})
				// GOAWAY before the message: TCP ordering then guarantees the
				// client has processed the draining GOAWAY by the time it
				// returns the first message to userland.
				_ = framer.WriteGoAway(uint32(2147483647), http2.ErrCodeNo, nil)
				_ = framer.WriteData(streamID, false, grpcFrame(0, nil))
			}
		case *http2.RSTStreamFrame:
			// The client cancelled the admitted stream; nothing further.
		}
	}
}

// serveBacklogFloodH2C serves a stream fixture with a control channel that
// lets the test deterministically place a response backlog in the client
// kernel's receive buffer. "arm\n" marks the next HTTP/2 connection as the
// flood target: its first request gets response HEADERS plus one message and
// the stream stays open. "flood\n" shrinks the server send buffer and writes
// 48KiB of DATA — Write returning with a tiny SO_SNDBUF means the client's
// TCP stack ACKed (received) nearly all of it, which is the barrier — then
// replies "ready\n". Unarmed HTTP/2 connections get a normal OK gRPC
// exchange so a follow-up call succeeds on a fresh connection.
type backlogFloodState struct {
	mu       sync.Mutex
	armed    bool
	conn     *net.TCPConn
	framer   *http2.Framer
	streamID uint32
}

var backlogFlood backlogFloodState

func serveBacklogFloodH2C(dataAddr, controlAddr string) {
	dataLis, err := net.Listen("tcp", dataAddr)
	if err != nil {
		log.Fatalf("failed to listen %s: %v", dataAddr, err)
	}
	controlLis, err := net.Listen("tcp", controlAddr)
	if err != nil {
		log.Fatalf("failed to listen %s: %v", controlAddr, err)
	}
	log.Printf("listening on %s (raw h2c backlog flood fixture, control %s)", dataAddr, controlAddr)
	go func() {
		for {
			conn, err := dataLis.Accept()
			if err != nil {
				log.Printf("accept %s error: %v", dataAddr, err)
				continue
			}
			go func(conn net.Conn) {
				defer conn.Close()
				if !readClientPreface(conn) {
					return
				}
				// "arm" (control channel) marks the next HTTP/2 connection
				// as the flood target; every other connection serves a
				// normal OK exchange. This keeps the fixture re-runnable
				// across test-suite invocations.
				backlogFlood.mu.Lock()
				armed := backlogFlood.armed
				backlogFlood.armed = false
				backlogFlood.mu.Unlock()
				if armed {
					handleBacklogFloodFirstConnH2C(conn)
					return
				}
				handleRawAfterPrefaceH2C(conn, false)
			}(conn)
		}
	}()
	for {
		conn, err := controlLis.Accept()
		if err != nil {
			log.Printf("accept %s error: %v", controlAddr, err)
			continue
		}
		go handleBacklogFloodControl(conn)
	}
}

func handleBacklogFloodFirstConnH2C(conn net.Conn) {
	framer := http2.NewFramer(conn, conn)
	if err := framer.WriteSettings(); err != nil {
		return
	}

	var streamID uint32
	for {
		frame, err := framer.ReadFrame()
		if err != nil {
			return
		}
		switch f := frame.(type) {
		case *http2.SettingsFrame:
			if !f.IsAck() {
				backlogFlood.mu.Lock()
				_ = framer.WriteSettingsAck()
				backlogFlood.mu.Unlock()
			}
		case *http2.HeadersFrame:
			if streamID == 0 {
				streamID = f.Header().StreamID
			}
		case *http2.DataFrame:
			if f.Header().StreamID == streamID && f.StreamEnded() {
				backlogFlood.mu.Lock()
				headers := encodeHeaders([]hpack.HeaderField{
					{Name: ":status", Value: "200"},
					{Name: "content-type", Value: "application/grpc"},
				})
				_ = framer.WriteHeaders(http2.HeadersFrameParam{
					StreamID:      streamID,
					BlockFragment: headers,
					EndHeaders:    true,
				})
				_ = framer.WriteData(streamID, false, grpcFrame(0, nil))
				backlogFlood.conn, _ = conn.(*net.TCPConn)
				backlogFlood.framer = framer
				backlogFlood.streamID = streamID
				backlogFlood.mu.Unlock()
			}
		case *http2.RSTStreamFrame:
			// The client cancelled the flooded stream; keep the connection
			// open so pending backlog stays valid until the client closes.
		}
	}
}

func handleBacklogFloodControl(conn net.Conn) {
	defer conn.Close()
	reader := bufio.NewReader(conn)
	for {
		command, err := reader.ReadString('\n')
		if err != nil {
			return
		}
		switch command {
		case "arm\n":
			backlogFlood.mu.Lock()
			if backlogFlood.conn != nil {
				_ = backlogFlood.conn.Close()
			}
			backlogFlood.armed = true
			backlogFlood.conn = nil
			backlogFlood.framer = nil
			backlogFlood.streamID = 0
			backlogFlood.mu.Unlock()
			if _, err := conn.Write([]byte("armed\n")); err != nil {
				return
			}
		case "flood\n":
			backlogFlood.mu.Lock()
			target := backlogFlood.conn
			framer := backlogFlood.framer
			streamID := backlogFlood.streamID
			backlogFlood.mu.Unlock()
			if target == nil || framer == nil {
				_, _ = conn.Write([]byte("error\n"))
				return
			}
			// Tiny SO_SNDBUF: TCP keeps data in the send buffer until the
			// peer ACKs it, so once these Writes return, all but the last
			// few KiB are resident in the client kernel's receive buffer.
			// That is the arrival barrier the test needs before it adopts
			// the connection. The backlog must fit the client's TCP receive
			// window (~64KiB with default tcp_rmem / tcp_adv_win_scale), so
			// it is 3 x 16KiB = 48KiB and the client test lowers its
			// preflight drain cap below that via ini.
			_ = target.SetWriteBuffer(4096)
			chunk := make([]byte, 16384)
			failed := false
			for i := 0; i < 3; i++ {
				backlogFlood.mu.Lock()
				err := framer.WriteData(streamID, false, chunk)
				backlogFlood.mu.Unlock()
				if err != nil {
					failed = true
					break
				}
			}
			if failed {
				_, _ = conn.Write([]byte("error\n"))
				return
			}
			if _, err := conn.Write([]byte("ready\n")); err != nil {
				return
			}
		default:
			return
		}
	}
}

// serveStreamThenKillOnSecondRequestH2C answers the first request with
// response HEADERS plus one gRPC message and keeps that stream open (no
// trailers). When a second request arrives on the same connection it closes
// the TCP connection abruptly. This lets a client test pin that a connection
// killed under one call is terminal for another in-flight stream that shares
// it.
func serveStreamThenKillOnSecondRequestH2C(addr string) {
	lis, err := net.Listen("tcp", addr)
	if err != nil {
		log.Fatalf("failed to listen %s: %v", addr, err)
	}
	log.Printf("listening on %s (raw h2c stream-then-kill-on-second-request fixture)", addr)
	for {
		conn, err := lis.Accept()
		if err != nil {
			log.Printf("accept %s error: %v", addr, err)
			continue
		}
		go handleStreamThenKillOnSecondRequestH2C(conn)
	}
}

func handleStreamThenKillOnSecondRequestH2C(conn net.Conn) {
	defer conn.Close()
	preface := make([]byte, len(http2.ClientPreface))
	if _, err := io.ReadFull(conn, preface); err != nil {
		return
	}
	framer := http2.NewFramer(conn, conn)
	if err := framer.WriteSettings(); err != nil {
		return
	}

	var firstStreamID uint32
	for {
		frame, err := framer.ReadFrame()
		if err != nil {
			return
		}
		switch f := frame.(type) {
		case *http2.SettingsFrame:
			if !f.IsAck() {
				_ = framer.WriteSettingsAck()
			}
		case *http2.HeadersFrame:
			if firstStreamID == 0 {
				firstStreamID = f.Header().StreamID
			} else if f.Header().StreamID != firstStreamID {
				return
			}
		case *http2.DataFrame:
			if f.Header().StreamID != firstStreamID {
				return
			}
			if f.StreamEnded() {
				headers := encodeHeaders([]hpack.HeaderField{
					{Name: ":status", Value: "200"},
					{Name: "content-type", Value: "application/grpc"},
				})
				_ = framer.WriteHeaders(http2.HeadersFrameParam{
					StreamID:      firstStreamID,
					BlockFragment: headers,
					EndHeaders:    true,
				})
				_ = framer.WriteData(firstStreamID, false, grpcFrame(0, nil))
			}
		}
	}
}

func serveRawH2C(addr string, sendGoAway bool, firstConnectionEOF bool) {
	lis, err := net.Listen("tcp", addr)
	if err != nil {
		log.Fatalf("failed to listen %s: %v", addr, err)
	}
	log.Printf("listening on %s (raw h2c lifecycle fixture)", addr)
	for {
		conn, err := lis.Accept()
		if err != nil {
			log.Printf("accept %s error: %v", addr, err)
			continue
		}
		go handleRawH2C(conn, sendGoAway, firstConnectionEOF)
	}
}

func handleRawH2C(conn net.Conn, sendGoAway bool, firstConnectionEOF bool) {
	defer conn.Close()
	if firstConnectionEOF && atomic.AddInt64(&eofLifecycleConnections, 1)%2 == 1 {
		return
	}

	if !readClientPreface(conn) {
		return
	}
	handleRawAfterPrefaceH2C(conn, sendGoAway)
}

func handleRawAfterPrefaceH2C(conn net.Conn, sendGoAway bool) {
	framer := http2.NewFramer(conn, conn)
	if err := framer.WriteSettings(); err != nil {
		return
	}

	var streamID uint32
	for {
		frame, err := framer.ReadFrame()
		if err != nil {
			return
		}
		switch f := frame.(type) {
		case *http2.SettingsFrame:
			if !f.IsAck() {
				_ = framer.WriteSettingsAck()
			}
		case *http2.HeadersFrame:
			streamID = f.Header().StreamID
			if f.StreamEnded() {
				writeRawGrpcOK(framer, streamID, sendGoAway)
				return
			}
		case *http2.DataFrame:
			if streamID == 0 {
				streamID = f.Header().StreamID
			}
			if f.StreamEnded() {
				writeRawGrpcOK(framer, streamID, sendGoAway)
				return
			}
		}
	}
}

func writeRawGrpcOK(framer *http2.Framer, streamID uint32, sendGoAway bool) {
	headers := encodeHeaders([]hpack.HeaderField{
		{Name: ":status", Value: "200"},
		{Name: "content-type", Value: "application/grpc"},
	})
	_ = framer.WriteHeaders(http2.HeadersFrameParam{
		StreamID:      streamID,
		BlockFragment: headers,
		EndHeaders:    true,
	})
	_ = framer.WriteData(streamID, false, grpcFrame(0, nil))
	if sendGoAway {
		_ = framer.WriteGoAway(streamID, http2.ErrCodeNo, nil)
	}
	trailers := encodeHeaders([]hpack.HeaderField{
		{Name: "grpc-status", Value: "0"},
	})
	_ = framer.WriteHeaders(http2.HeadersFrameParam{
		StreamID:      streamID,
		BlockFragment: trailers,
		EndHeaders:    true,
		EndStream:     true,
	})
}

func serveMidStreamFailureH2C(addr string) {
	lis, err := net.Listen("tcp", addr)
	if err != nil {
		log.Fatalf("failed to listen %s: %v", addr, err)
	}
	log.Printf("listening on %s (raw h2c mid-stream failure fixture)", addr)
	for {
		conn, err := lis.Accept()
		if err != nil {
			log.Printf("accept %s error: %v", addr, err)
			continue
		}
		go handleMidStreamFailureH2C(conn)
	}
}

func handleMidStreamFailureH2C(conn net.Conn) {
	defer conn.Close()
	preface := make([]byte, len(http2.ClientPreface))
	if _, err := io.ReadFull(conn, preface); err != nil {
		return
	}
	framer := http2.NewFramer(conn, conn)
	if err := framer.WriteSettings(); err != nil {
		return
	}

	var streamID uint32
	for {
		frame, err := framer.ReadFrame()
		if err != nil {
			return
		}
		switch f := frame.(type) {
		case *http2.SettingsFrame:
			if !f.IsAck() {
				_ = framer.WriteSettingsAck()
			}
		case *http2.HeadersFrame:
			streamID = f.Header().StreamID
			if f.StreamEnded() {
				writeRawGrpcPartialAndClose(framer, streamID)
				return
			}
		case *http2.DataFrame:
			if streamID == 0 {
				streamID = f.Header().StreamID
			}
			if f.StreamEnded() {
				writeRawGrpcPartialAndClose(framer, streamID)
				return
			}
		}
	}
}

func writeRawGrpcPartialAndClose(framer *http2.Framer, streamID uint32) {
	headers := encodeHeaders([]hpack.HeaderField{
		{Name: ":status", Value: "200"},
		{Name: "content-type", Value: "application/grpc"},
	})
	_ = framer.WriteHeaders(http2.HeadersFrameParam{
		StreamID:      streamID,
		BlockFragment: headers,
		EndHeaders:    true,
	})
	_ = framer.WriteData(streamID, false, []byte{0, 0, 0})
}

func serveServerRstThenOKH2C(addr string) {
	lis, err := net.Listen("tcp", addr)
	if err != nil {
		log.Fatalf("failed to listen %s: %v", addr, err)
	}
	log.Printf("listening on %s (raw h2c server RST_STREAM fixture)", addr)
	for {
		conn, err := lis.Accept()
		if err != nil {
			log.Printf("accept %s error: %v", addr, err)
			continue
		}
		go handleServerRstThenOKH2C(conn)
	}
}

func handleServerRstThenOKH2C(conn net.Conn) {
	defer conn.Close()
	preface := make([]byte, len(http2.ClientPreface))
	if _, err := io.ReadFull(conn, preface); err != nil {
		return
	}
	framer := http2.NewFramer(conn, conn)
	if err := framer.WriteSettings(); err != nil {
		return
	}

	var completedStreams int
	var currentStreamID uint32
	for {
		frame, err := framer.ReadFrame()
		if err != nil {
			return
		}
		switch f := frame.(type) {
		case *http2.SettingsFrame:
			if !f.IsAck() {
				_ = framer.WriteSettingsAck()
			}
		case *http2.HeadersFrame:
			currentStreamID = f.Header().StreamID
			if f.StreamEnded() {
				writeRawRstThenOKResponse(framer, currentStreamID, completedStreams)
				completedStreams++
				currentStreamID = 0
			}
		case *http2.DataFrame:
			if currentStreamID == 0 {
				currentStreamID = f.Header().StreamID
			}
			if f.StreamEnded() {
				writeRawRstThenOKResponse(framer, currentStreamID, completedStreams)
				completedStreams++
				currentStreamID = 0
			}
		}
	}
}

func writeRawRstThenOKResponse(framer *http2.Framer, streamID uint32, completedStreams int) {
	if completedStreams == 0 {
		_ = framer.WriteRSTStream(streamID, http2.ErrCodeRefusedStream)
		return
	}
	writeRawGrpcOK(framer, streamID, false)
}

func serveGoAwayRefusedH2C(addr string) {
	lis, err := net.Listen("tcp", addr)
	if err != nil {
		log.Fatalf("failed to listen %s: %v", addr, err)
	}
	log.Printf("listening on %s (raw h2c GOAWAY refused fixture)", addr)
	for {
		conn, err := lis.Accept()
		if err != nil {
			log.Printf("accept %s error: %v", addr, err)
			continue
		}
		go handleGoAwayRefusedH2C(conn)
	}
}

func handleGoAwayRefusedH2C(conn net.Conn) {
	defer conn.Close()
	if !readClientPreface(conn) {
		return
	}
	handleGoAwayRefusedAfterPrefaceH2C(conn)
}

func readClientPreface(conn net.Conn) bool {
	preface := make([]byte, len(http2.ClientPreface))
	if _, err := io.ReadFull(conn, preface); err != nil {
		return false
	}
	return string(preface) == http2.ClientPreface
}

func handleGoAwayRefusedAfterPrefaceH2C(conn net.Conn) {
	framer := http2.NewFramer(conn, conn)
	if err := framer.WriteSettings(); err != nil {
		return
	}

	for {
		frame, err := framer.ReadFrame()
		if err != nil {
			return
		}
		switch f := frame.(type) {
		case *http2.SettingsFrame:
			if !f.IsAck() {
				_ = framer.WriteSettingsAck()
			}
		case *http2.HeadersFrame:
			if f.Header().StreamID > 0 {
				_ = framer.WriteGoAway(0, http2.ErrCodeNo, nil)
				return
			}
		case *http2.DataFrame:
			if f.Header().StreamID > 0 {
				_ = framer.WriteGoAway(0, http2.ErrCodeNo, nil)
				return
			}
		}
	}
}

func serveGoAwayRefusedThenOKH2C(addr string, counter *int64) {
	lis, err := net.Listen("tcp", addr)
	if err != nil {
		log.Fatalf("failed to listen %s: %v", addr, err)
	}
	log.Printf("listening on %s (raw h2c GOAWAY refused then OK fixture)", addr)
	for {
		conn, err := lis.Accept()
		if err != nil {
			log.Printf("accept %s error: %v", addr, err)
			continue
		}
		go handleGoAwayRefusedThenOKH2C(conn, counter)
	}
}

func handleGoAwayRefusedThenOKH2C(conn net.Conn, counter *int64) {
	defer conn.Close()
	// Count only real HTTP/2 clients: bare TCP preflight probes (SKIPIF,
	// runner readiness checks) must not consume the refused/OK alternation.
	if !readClientPreface(conn) {
		return
	}
	if atomic.AddInt64(counter, 1)%2 == 1 {
		handleGoAwayRefusedAfterPrefaceH2C(conn)
		return
	}
	handleRawAfterPrefaceH2C(conn, false)
}

func serveGoAwayRefusedThenDelayedOKH2C(addr string) {
	lis, err := net.Listen("tcp", addr)
	if err != nil {
		log.Fatalf("failed to listen %s: %v", addr, err)
	}
	log.Printf("listening on %s (raw h2c GOAWAY refused then delayed OK fixture)", addr)
	for {
		conn, err := lis.Accept()
		if err != nil {
			log.Printf("accept %s error: %v", addr, err)
			continue
		}
		go handleGoAwayRefusedThenDelayedOKH2C(conn)
	}
}

func handleGoAwayRefusedThenDelayedOKH2C(conn net.Conn) {
	defer conn.Close()
	// Count only real HTTP/2 clients: bare TCP preflight probes (SKIPIF,
	// runner readiness checks) must not consume the refused/OK alternation.
	if !readClientPreface(conn) {
		return
	}
	if atomic.AddInt64(&goAwayRefusedThenDelayedOKConnections, 1)%2 == 1 {
		handleGoAwayRefusedAfterPrefaceH2C(conn)
		return
	}
	handleRawDelayedOKAfterPrefaceH2C(conn, 100*time.Millisecond)
}

func handleRawDelayedOKAfterPrefaceH2C(conn net.Conn, delay time.Duration) {
	framer := http2.NewFramer(conn, conn)
	if err := framer.WriteSettings(); err != nil {
		return
	}

	var streamID uint32
	for {
		frame, err := framer.ReadFrame()
		if err != nil {
			return
		}
		switch f := frame.(type) {
		case *http2.SettingsFrame:
			if !f.IsAck() {
				_ = framer.WriteSettingsAck()
			}
		case *http2.HeadersFrame:
			streamID = f.Header().StreamID
			if f.StreamEnded() {
				time.Sleep(delay)
				writeRawGrpcOK(framer, streamID, false)
				return
			}
		case *http2.DataFrame:
			if streamID == 0 {
				streamID = f.Header().StreamID
			}
			if f.StreamEnded() {
				time.Sleep(delay)
				writeRawGrpcOK(framer, streamID, false)
				return
			}
		}
	}
}

func serveGoAwayAfterMessageH2C(addr string) {
	lis, err := net.Listen("tcp", addr)
	if err != nil {
		log.Fatalf("failed to listen %s: %v", addr, err)
	}
	log.Printf("listening on %s (raw h2c GOAWAY after message fixture)", addr)
	for {
		conn, err := lis.Accept()
		if err != nil {
			log.Printf("accept %s error: %v", addr, err)
			continue
		}
		go handleGoAwayAfterMessageH2C(conn)
	}
}

func handleGoAwayAfterMessageH2C(conn net.Conn) {
	defer conn.Close()
	preface := make([]byte, len(http2.ClientPreface))
	if _, err := io.ReadFull(conn, preface); err != nil {
		return
	}
	framer := http2.NewFramer(conn, conn)
	if err := framer.WriteSettings(); err != nil {
		return
	}

	var streamID uint32
	for {
		frame, err := framer.ReadFrame()
		if err != nil {
			return
		}
		switch f := frame.(type) {
		case *http2.SettingsFrame:
			if !f.IsAck() {
				_ = framer.WriteSettingsAck()
			}
		case *http2.HeadersFrame:
			streamID = f.Header().StreamID
			if f.StreamEnded() {
				writeRawGrpcMessageThenGoAway(framer, streamID)
				return
			}
		case *http2.DataFrame:
			if streamID == 0 {
				streamID = f.Header().StreamID
			}
			if f.StreamEnded() {
				writeRawGrpcMessageThenGoAway(framer, streamID)
				return
			}
		}
	}
}

func writeRawGrpcMessageThenGoAway(framer *http2.Framer, streamID uint32) {
	headers := encodeHeaders([]hpack.HeaderField{
		{Name: ":status", Value: "200"},
		{Name: "content-type", Value: "application/grpc"},
	})
	_ = framer.WriteHeaders(http2.HeadersFrameParam{
		StreamID:      streamID,
		BlockFragment: headers,
		EndHeaders:    true,
	})
	_ = framer.WriteData(streamID, false, grpcFrame(0, nil))
	_ = framer.WriteGoAway(0, http2.ErrCodeNo, nil)
}

func serveGoAwayMaxThenOKH2C(addr string) {
	lis, err := net.Listen("tcp", addr)
	if err != nil {
		log.Fatalf("failed to listen %s: %v", addr, err)
	}
	log.Printf("listening on %s (raw h2c GOAWAY MaxInt32 then OK fixture)", addr)
	for {
		conn, err := lis.Accept()
		if err != nil {
			log.Printf("accept %s error: %v", addr, err)
			continue
		}
		go handleGoAwayMaxThenOKH2C(conn)
	}
}

func handleGoAwayMaxThenOKH2C(conn net.Conn) {
	defer conn.Close()
	preface := make([]byte, len(http2.ClientPreface))
	if _, err := io.ReadFull(conn, preface); err != nil {
		return
	}
	framer := http2.NewFramer(conn, conn)
	if err := framer.WriteSettings(); err != nil {
		return
	}

	var streamID uint32
	var sentGoAway bool
	for {
		frame, err := framer.ReadFrame()
		if err != nil {
			return
		}
		switch f := frame.(type) {
		case *http2.SettingsFrame:
			if !f.IsAck() {
				_ = framer.WriteSettingsAck()
			}
		case *http2.HeadersFrame:
			streamID = f.Header().StreamID
			if !sentGoAway && streamID > 0 {
				_ = framer.WriteGoAway(maxHTTP2ClientStreamID, http2.ErrCodeNo, nil)
				sentGoAway = true
			}
			if f.StreamEnded() {
				writeRawGrpcOK(framer, streamID, false)
				return
			}
		case *http2.DataFrame:
			if streamID == 0 {
				streamID = f.Header().StreamID
			}
			if !sentGoAway && streamID > 0 {
				_ = framer.WriteGoAway(maxHTTP2ClientStreamID, http2.ErrCodeNo, nil)
				sentGoAway = true
			}
			if f.StreamEnded() {
				writeRawGrpcOK(framer, streamID, false)
				return
			}
		}
	}
}

func encodeHeaders(fields []hpack.HeaderField) []byte {
	var buf bytes.Buffer
	encoder := hpack.NewEncoder(&buf)
	for _, field := range fields {
		_ = encoder.WriteField(field)
	}
	return buf.Bytes()
}

func grpcFrame(flag byte, payload []byte) []byte {
	frame := make([]byte, 5+len(payload))
	frame[0] = flag
	binary.BigEndian.PutUint32(frame[1:5], uint32(len(payload)))
	copy(frame[5:], payload)
	return frame
}

func main() {
	if value, ok := envInt("TEST_SERVER_GOMAXPROCS"); ok {
		runtime.GOMAXPROCS(value)
	}

	// h2c plaintext on :50051
	go func() {
		lis, err := net.Listen("tcp", ":50051")
		if err != nil {
			log.Fatalf("failed to listen :50051: %v", err)
		}
		log.Printf("listening on :50051 (h2c, reflection enabled)")
		if err := newServer().Serve(lis); err != nil {
			log.Fatalf("h2c serve error: %v", err)
		}
	}()

	go serveNonGrpcH2C()
	serveLifecycleH2C()

	// h2 over TLS on :50052
	creds, err := credentials.NewServerTLSFromFile("/certs/server.crt", "/certs/server.key")
	if err != nil {
		log.Fatalf("failed to load TLS keys: %v", err)
	}
	tlsServer := newServer(grpc.Creds(creds))
	go func() {
		lis, err := net.Listen("tcp", ":50052")
		if err != nil {
			log.Fatalf("failed to listen :50052: %v", err)
		}
		log.Printf("listening on :50052 (h2 over TLS)")
		if err := tlsServer.Serve(lis); err != nil {
			log.Fatalf("tls serve error: %v", err)
		}
	}()

	// h2 over mTLS on :50053 — requires the client to present a cert signed
	// by (or equal to) the cert in /certs/client.crt.
	mtlsServer, err := newMtlsServer()
	if err != nil {
		log.Fatalf("failed to set up mTLS: %v", err)
	}

	lis, err := net.Listen("tcp", ":50053")
	if err != nil {
		log.Fatalf("failed to listen :50053: %v", err)
	}
	log.Printf("listening on :50053 (h2 over mTLS)")
	if err := mtlsServer.Serve(lis); err != nil {
		log.Fatalf("mtls serve error: %v", err)
	}
}

func newMtlsServer() (*grpc.Server, error) {
	serverCert, err := tls.LoadX509KeyPair("/certs/server.crt", "/certs/server.key")
	if err != nil {
		return nil, fmt.Errorf("load server keypair: %w", err)
	}
	clientCAPem, err := os.ReadFile("/certs/client.crt")
	if err != nil {
		return nil, fmt.Errorf("read client CA: %w", err)
	}
	clientCAs := x509.NewCertPool()
	if !clientCAs.AppendCertsFromPEM(clientCAPem) {
		return nil, fmt.Errorf("failed to add client CA")
	}
	tlsCfg := &tls.Config{
		Certificates: []tls.Certificate{serverCert},
		ClientCAs:    clientCAs,
		ClientAuth:   tls.RequireAndVerifyClientCert,
	}
	return newServer(grpc.Creds(credentials.NewTLS(tlsCfg))), nil
}
