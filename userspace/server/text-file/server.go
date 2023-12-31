package main

import (
	"bufio"
	"fmt"
	"net"
	"os"
	"os/signal"
)

func main() {
	// Check if port number was provided
	if len(os.Args) < 3 {
		fmt.Println("Usage: go run main.go <ipv4> <port>")
		os.Exit(1)
	}

	// Create a channel to catch the signal
	sigCh := make(chan os.Signal, 1)
	signal.Notify(sigCh, os.Interrupt)

	// Set the port
	port := os.Args[1] + ":" + os.Args[2]

	// Listen for incoming connections.
	listener, err := net.Listen("tcp", port)
	if err != nil {
		fmt.Println("Error listening:", err.Error())
		os.Exit(1)
	}

	// Close the listener when the application closes.
	defer listener.Close()

	fmt.Println("Listening on " + port)

	go func() {
		for {
			conn, err := listener.Accept()
			if err != nil {
				fmt.Println("Error accepting: ", err.Error())
				os.Exit(1)
			}

			// Set the socket options
			tcpConn := conn.(*net.TCPConn)
			tcpConn.SetLinger(0)
			tcpConn.SetKeepAlive(false)
			tcpConn.SetKeepAlivePeriod(0)
			//tcpConn.SetReadBuffer(0)

			err = tcpConn.SetNoDelay(true) // Disable Nagle's algorithm
			if err != nil {
				fmt.Println("Error setting TCP_NODELAY option:", err.Error())
				// Handle the error gracefully, e.g., log it and continue accepting connections
				os.Exit(1)
			}

			// Print a message for each connection
			fmt.Println("Received a connection")

			go handleConnection(conn)
		}
	}()

	// Wait for the signal
	<-sigCh
	fmt.Println("\nReceived an interrupt, stopping services...")

	listener.Close()
	os.Exit(0)
}

func handleConnection(conn net.Conn) {
	defer conn.Close()

	// Create a new reader and writer for the connection
	reader := bufio.NewReader(conn)
	writer := bufio.NewWriter(conn)

	for {
		line, err := reader.ReadString('\n')
		if err != nil {
			break
		}
		fmt.Printf("Line received: %s", line)

		// Check if the received line is "end of file"
		if line == "eof\n" {
			// Send the response
			response := "RESP: received all of the request\n"
			_, err := writer.WriteString(response)
			if err != nil {
				fmt.Println("Error writing response:", err.Error())
				return
			}

			err = writer.Flush()
			if err != nil {
				fmt.Println("Error flushing response:", err.Error())
				return
			}

			fmt.Println("Response sent")
			//return
		}
		fmt.Println("here")
	}
}
