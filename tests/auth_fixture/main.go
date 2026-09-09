// This fixture only listens on loopback and uses an ephemeral host key and a
// fixed synthetic password. It never reads the user's SSH or app credentials.
package main

import (
	"crypto/ed25519"
	"crypto/rand"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io"
	"net"
	"os"
	"sync"
	"time"

	"golang.org/x/crypto/ssh"
)

const syntheticPassword = "noxshell-integration-test-only"

var outputMutex sync.Mutex

func report(event string, fields map[string]any) {
	if fields == nil {
		fields = map[string]any{}
	}
	fields["event"] = event
	outputMutex.Lock()
	defer outputMutex.Unlock()
	_ = json.NewEncoder(os.Stdout).Encode(fields)
}

func main() {
	method := flag.String("auth", "password", "password, interactive, or both")
	noneDelay := flag.Int("none-delay-ms", 0, "delay the first authentication response")
	passwordDelay := flag.Int("password-delay-ms", 0, "delay a password response")
	rejectPassword := flag.Bool("reject-password", false, "reject ordinary password but permit interactive fallback")
	flag.Parse()

	_, privateKey, err := ed25519.GenerateKey(rand.Reader)
	if err != nil {
		panic(err)
	}
	signer, err := ssh.NewSignerFromKey(privateKey)
	if err != nil {
		panic(err)
	}
	config := &ssh.ServerConfig{
		BannerCallback: func(c ssh.ConnMetadata) string {
			report("discovery_started", nil)
			time.Sleep(time.Duration(*noneDelay) * time.Millisecond)
			return "NoxShell local synthetic authentication fixture\n"
		},
		AuthLogCallback: func(c ssh.ConnMetadata, authMethod string, err error) {
			report("auth_result", map[string]any{"method": authMethod, "success": err == nil})
		},
	}
	if *method == "password" || *method == "both" {
		config.PasswordCallback = func(c ssh.ConnMetadata, password []byte) (*ssh.Permissions, error) {
			report("method_started", map[string]any{"method": "password"})
			time.Sleep(time.Duration(*passwordDelay) * time.Millisecond)
			if !*rejectPassword && c.User() == "fixture-user" && string(password) == syntheticPassword {
				return nil, nil
			}
			return nil, errors.New("synthetic credential rejected")
		}
	}
	if *method == "interactive" || *method == "both" {
		config.KeyboardInteractiveCallback = func(c ssh.ConnMetadata, challenge ssh.KeyboardInteractiveChallenge) (*ssh.Permissions, error) {
			report("method_started", map[string]any{"method": "keyboard-interactive"})
			answers, err := challenge("fixture", "", []string{"Password:"}, []bool{false})
			if err != nil {
				return nil, err
			}
			if c.User() == "fixture-user" && len(answers) == 1 && answers[0] == syntheticPassword {
				return nil, nil
			}
			return nil, errors.New("synthetic credential rejected")
		}
	}
	config.AddHostKey(signer)
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
	defer listener.Close()
	report("listening", map[string]any{"port": listener.Addr().(*net.TCPAddr).Port})
	conn, err := listener.Accept()
	if err != nil {
		panic(err)
	}
	defer conn.Close()
	server, channels, requests, err := ssh.NewServerConn(conn, config)
	if err != nil {
		report("connection_closed", nil)
		return
	}
	defer server.Close()
	report("authenticated", nil)
	go ssh.DiscardRequests(requests)
	for incoming := range channels {
		if incoming.ChannelType() != "session" {
			_ = incoming.Reject(ssh.UnknownChannelType, "fixture only supports a shell")
			continue
		}
		channel, channelRequests, err := incoming.Accept()
		if err != nil {
			continue
		}
		go func() {
			defer channel.Close()
			go func() { _, _ = io.Copy(io.Discard, channel) }()
			for request := range channelRequests {
				supported := request.Type == "pty-req" || request.Type == "shell" || request.Type == "window-change"
				if request.WantReply {
					_ = request.Reply(supported, nil)
				}
				if request.Type == "shell" {
					report("shell_opened", nil)
					_, _ = channel.Write([]byte("fixture-user@localtest:~$ "))
				}
			}
		}()
	}
}
