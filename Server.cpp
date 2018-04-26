#include "RaftServer.h"
#include <cstdlib>
#include <cassert>
#include <vector>
#include <fstream>

int main(int argc, char **argv) {
  std::unique_ptr<NoopStateMachine> stateMachine(new NoopStateMachine());

  assert(argc == 3);

  int id = atoi(argv[1]);
  std::vector<std::string> hosts;
  std::ifstream fin(argv[2]);

  std::string host;
  while (fin >> host) {
    hosts.push_back(host);
  }

  RaftServer server(id, hosts, ".", std::move(stateMachine));
  server.Run();
  server.Wait();
}
