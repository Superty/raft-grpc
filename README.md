# raft-grpc

## Build

Run `make` to install. GRPC is required.

Currently just a demo; fill a file, say `hosts.txt` with a list of local addresses to run the servers on. Then run `./Server <id> hosts.txt`, where `<id>` is a 0-indexed address in the file `hosts.txt`. The server will listen at this address. The server will then try to contact the other hosts in the list.

Run `./Client hosts.txt` to open an interface where you can enter commands, which will be sent to the current known leader. If there is no response, the client redirects to the next known host.
