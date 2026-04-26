package main

import (
	"context"
	"crypto/tls"
	"crypto/x509"
	"fmt"
	"log"
	"net"
	"os"
	"time"

	pb "example.com/helloworld/pb"
	"google.golang.org/grpc"
	"google.golang.org/grpc/credentials"
	"google.golang.org/grpc/reflection"
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

func newServer() *grpc.Server {
	s := grpc.NewServer()
	pb.RegisterGreeterServer(s, &server{})
	reflection.Register(s)
	return s
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
