#include <grpc/grpc.h>
#include <grpc++/server.h>
#include <grpc++/server_builder.h>
#include <grpc++/server_context.h>
#include <grpc++/security/server_credentials.h>
#include <grpc++/create_channel.h>
#include "raft.grpc.pb.h"

#include <utility>
#include <fstream>
#include <memory>
#include <cstdlib>
#include <chrono>
#include <thread>
#include <atomic>

#include <csignal>
#include <sys/time.h>
#include <cerrno>

using std::string;
using grpc::Status;
using grpc::ClientContext;
using raft::ClientService;
using raft::ServeClientRequest;
using raft::ServeClientResponse;

int main(int argc, char **argv) {
  assert(argc == 2);

  std::vector<std::string> hosts;
  std::ifstream fin(argv[1]);

  std::string host;
  while (fin >> host) {
    hosts.push_back(host);
    // std::cerr << host << '\n';
  }

  int hostCount = hosts.size();

  std::vector<std::unique_ptr<ClientService::Stub>> stubs;
  for (int i = 0; i < hostCount; i++) {
    stubs.emplace_back(ClientService::NewStub(
      grpc::CreateChannel(hosts[i], grpc::InsecureChannelCredentials())));
  }

  int leaderId = 0;
  std::cout << "Enter commands to send to the replicated state machine.\n";
  while (true) {
    string command;
    std::cout << "> ";
    std::getline(std::cin, command);

    while (true) {
      ServeClientRequest request;
      request.set_command(command);

      ClientContext context;
      context.set_deadline(
        std::chrono::system_clock::now() + std::chrono::seconds(5000));
      ServeClientResponse response;
      auto status = stubs[leaderId]->ServeClient(&context, request, &response);
      std::cout << "Sent request to " << leaderId << ".\n";

      if (status.error_code() == grpc::StatusCode::DEADLINE_EXCEEDED) {
        leaderId = (leaderId + 1) % hostCount;
        std::cout << "Deadline exceeded. Trying server " << leaderId << ".\n";
      } else {
        if (!response.success()) {
          leaderId = response.leaderid();
          std::cerr << "Redirected to the leader, " << leaderId << ".\n";
        } else {
          std::cerr << "Success!\n";
          break;
        }
      }
    }
  }
}
