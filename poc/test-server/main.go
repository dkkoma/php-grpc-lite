package main

import (
	"context"
	"crypto/tls"
	"crypto/x509"
	"fmt"
	"log"
	"net"
	"net/http"
	"os"
	"strconv"
	"time"

	pb "example.com/helloworld/pb"
	"golang.org/x/net/http2"
	"golang.org/x/net/http2/h2c"
	"google.golang.org/grpc"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/credentials"
	"google.golang.org/grpc/metadata"
	"google.golang.org/grpc/reflection"
	"google.golang.org/grpc/status"
)

type server struct {
	pb.UnimplementedGreeterServer
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
	if err := benchErrorFromContext(ctx); err != nil {
		return nil, err
	}
	sendBenchMetadata(ctx)
	if d := req.GetServerDelayMs(); d > 0 {
		time.Sleep(time.Duration(d) * time.Millisecond)
	}
	return &pb.BenchReply{Payload: make([]byte, req.GetPayloadBytes())}, nil
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
	return status.Error(codes.Code(code), message)
}

func sendBenchMetadata(ctx context.Context) {
	incoming, ok := metadata.FromIncomingContext(ctx)
	if !ok {
		return
	}

	count := firstMetadataInt(incoming, "x-bench-response-metadata-count")
	if count <= 0 {
		return
	}
	valueBytes := firstMetadataInt(incoming, "x-bench-response-metadata-value-bytes")
	if valueBytes < 0 {
		valueBytes = 0
	}
	value := makeMetadataValue(valueBytes)

	initial := metadata.New(nil)
	trailing := metadata.New(nil)
	for i := 0; i < count; i++ {
		key := fmt.Sprintf("x-bench-initial-%03d", i)
		initial.Append(key, value)
		key = fmt.Sprintf("x-bench-trailing-%03d", i)
		trailing.Append(key, value)
	}
	grpc.SendHeader(ctx, initial)
	grpc.SetTrailer(ctx, trailing)
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
	return nil
}

func newServer() *grpc.Server {
	s := grpc.NewServer()
	pb.RegisterGreeterServer(s, &server{})
	reflection.Register(s)
	return s
}

func serveNonGrpcH2C() {
	handler := http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
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

func main() {
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

	// h2 over TLS on :50052
	creds, err := credentials.NewServerTLSFromFile("/certs/server.crt", "/certs/server.key")
	if err != nil {
		log.Fatalf("failed to load TLS keys: %v", err)
	}
	tlsServer := grpc.NewServer(grpc.Creds(creds))
	pb.RegisterGreeterServer(tlsServer, &server{})
	reflection.Register(tlsServer)
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
	pb.RegisterGreeterServer(mtlsServer, &server{})
	reflection.Register(mtlsServer)

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
	return grpc.NewServer(grpc.Creds(credentials.NewTLS(tlsCfg))), nil
}
