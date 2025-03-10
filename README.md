# Adda: A high performance group chat application in C
Adda is a multi-user single-group chat application written in C using only the
standard library. It employs _raw TCP sockets_ and `poll` methods to handle
thousands of users simulataneously with very minimal resource usage.

## Features
- Supports thousands of users all connected to single group
- Very minimal RAM usage with just 3 threads handling the whole thing
- Uses file descriptor IO to build a persistent queue for all the messages
- Demonstrates the power of `poll` syscall with raw linux file descriptors to
  ensure CPU usage is kept at minimum
- Demonstrates building a performant network application with just the standard
  library

## Dependencies
- Linux or any other UNIX based OS
- A C compiler (example `gcc`, `clang`) and the standard C library `libc`
- Telnet (or other TCP client program like `netcat`) to test out

## Usage
There is only one file `main.c`. You need to compile it with any C compiler you
have installed and you are good to go.

If you have `gcc` installed, run `gcc main.c` and Run it using `./a.out`

After that you can connect to the server using `telnet` or any other TCP
connection tool. By default the server runs on port `9999` so you can hit
something like this

`telnet 127.0.0.1 9999`

You will be connected and can now send messages. You can open up another telnet
session and verify messages being sent from one is visible to all

## Implementation Overview
The program starts listening on a raw TCP socket bound to port `9999` by default
on any address on the local machine (`0.0.0.0`)

It opens a pair of **channel socket** which will be used for passing through
messages between connections.

It then spawns two threads
- A **message_reader** thread handling reads from all the connections
- A **message_writer** thread handling writes to all the connections

### Channel Socket
**Adda** uses Socket IO to facilitate group communication

It uses a pair of connected sockets created when the program starts. This allows
the program to `poll` on the socket for data, instead of a regular file, which
is always ready for read.

Using `poll` helps us avoid blocking `read` calls. We can just `poll` on the
socket, which will return when there is data to be read from the socket

### Message Reader
Message reader thread receives a list of file descriptors which corresponds to
the connected clients.

It `polls` on them to know when they sent a message. It reads the message when
available, and writes it to the _file descriptor_ for the **channel socket**

### Message Writer
Message writer also receives the same list of file descriptors as _message
reader_ but it uses them in a different way.

Message writer `polls` only on the **channel socket** for reading. When channel 
socket has messages to be read, it reads message one by one and forwards them to 
all the clients connected (as denoted by the list of file descriptors)

### Main thread
The main thread, after launching the message reader and message writer threads,
`polls` on the server socket for new connections. Upon receiving a new connection,
it accepts the connection and adds it to the list of file descriptors so that it
becomes available for both _message reader_ as well as _message writer_

### Maintaining connections
**Adda** uses simplistic approach of identifying the connections by the file
descriptor number they were given by the OS. No IP address are tracked and as
such, IP blocking or rate limiting is currently not built into **Adda**

Each time a new connection is accepted, it is added to the list of file
descriptors. The index of new connection is calculated using the file descriptor
number it has received

To do this, Adda needs to ensure there are sufficient slots for storing
connection file descriptors.

Adda again, takes a simplistic approach to allocate memory for the maximum
number of connections it will support. Since each `pollfd` structure only takes
the size of 2 integers (or 8 bytes) it doesn't cost much but adds great deal of
simplicity by removing the need to actively allocate / deallocate memory

When a client disconnects, the `pollfd` structure at index pointed to by the
client's file descriptor number is set to `-2`. This helps us in tracking the
connections while polling for messages or forwarding messages to them

This is done using
- The linux `poll` method ignores any file descriptors which are less than zero.
  so setting value to `-2` effectively makes `poll` ignore them
- The **message writer** thread skips writing messages to fds which are `-2`

Doing this we are effectively maintaining active connections without storing
deleting them from the list

When a new connection is given the same fd as the one closed before, the index
in our file descriptor list for the connection becomes same as the one pointed
to by the closed connection. So the main thread just assigns the actual file
descriptor number, replacing the `-2` and the same fd is now used for the new
connection

Linux ensures that active file descriptors are given file descriptor numbers in
incrementing order. This makes our list of file descriptor behave normally.

## Performance benchmarks
The program acheives the following benchmark result on my local machine

CPU: Ryzen 3700x (8 cores 8 threads)
RAM: 16GB
Message length: 211 bytes
Note: CPU usage is divided by the number of cores (top - Iris Mode off)

| Number of Clients | CPU Usage | RAM Usage | 1M messages write | 
| ---  | --- | ---   | --- |
| 0    | 0   | 1.6m  | ~4 seconds |
| 500  | 1.3% | 1.6m  | ~4 seconds
| 1000 | 4.9% | 1.6m  | ~1.3 seconds |
| 2000 | 5.6% | 1.6m  | ~0.9 seconds | 
| 4000 | 5.3% | 1.7m | ~1.5 seconds | 
