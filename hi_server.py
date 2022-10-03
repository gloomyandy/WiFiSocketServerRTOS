# echo-server.py

from ctypes import sizeof
import socket
import time

HOST = "192.168.10.244"
PORT = 1883

with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind((HOST, PORT))
    s.listen()
    conn, addr = s.accept()
    with conn:
        print(f"Client {addr} connected")

        try:
            i = 0
            while True:
                # Client must send us "Hello"
                conn.recv(len("Hello!"))
                print("Recieved greeting!")
                # Reply with our "Hi"
                reply = "Hi!".encode()
                conn.sendall(reply)
                print("Reply sent! %d" % i)
                i += 1
        except KeyboardInterrupt:
            pass

    s.close()