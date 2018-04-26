#pragma once

#include <memory>
#include <vector>
#include <mutex>
#include <random>
#include <list>

#include <grpc/grpc.h>
#include <grpc++/server.h>
#include <grpc++/server_builder.h>
#include <grpc++/server_context.h>
#include <grpc++/security/server_credentials.h>
#include "raft.grpc.pb.h"

#include "Storage.h"

#include <csignal>
#include <ctime>
#include <cerrno>

using std::string;
using grpc::Status;
using grpc::Server;
using grpc::ServerContext;
using grpc::ServerCompletionQueue;
using raft::Raft;
using raft::ClientService;
using raft::LogEntry;
using raft::AppendEntriesRequest;
using raft::AppendEntriesResponse;
using raft::RequestVoteRequest;
using raft::RequestVoteResponse;
using raft::ServeClientRequest;
using raft::ServeClientResponse;

enum class ServerState { Leader, Candidate, Follower };

class StateMachineInterface {
 public:
  virtual void Apply(const string& command) = 0;
};

class NoopStateMachine : public StateMachineInterface {
 public:
  void Apply(const string& command) override;
};

class RaftServer final : public Raft::Service {
 public:
  RaftServer(int o_id, const std::vector<string>& o_hostList,
             const std::string& storageDir,
             std::unique_ptr<StateMachineInterface> o_stateMachine);
  void Run();
  void Wait();

  // Needs to be public to be called by the signal handler, which is a free
  // function.
  void AlarmCallback();

 private:
  Status AppendEntries(ServerContext* context,
                       const AppendEntriesRequest* request,
                       AppendEntriesResponse *response) override;
  Status RequestVote(ServerContext* context,
                     const RequestVoteRequest* request,
                     RequestVoteResponse* response) override;

  void ServeClients();
  void RunForElection();
  void ReplicateEntries();
  void InvokeRequestVote(int voterId, std::atomic<int> *votesGained);
  void InvokeAppendEntries(int o_id);

  // void AppendEntriesCallback(int responseTerm, bool success);
  // void RequestVoteCallback(int responseTerm, bool voteGranted);

  void BecomeFollower();
  void BecomeCandidate();
  void BecomeLeader();

  void SetAlarm(int after_us);
  void ResetElectionTimeout();

  void Narrate(const string& text);
  void PrintLog();

  int id;
  std::mutex overallLock, stateLock;
  int hostCount;
  int votesGained;
  Storage storage;

  std::vector<std::unique_ptr<Raft::Stub>> stubs;
  std::unique_ptr<ServerCompletionQueue> cq;
  std::unique_ptr<Server> server;
  const std::vector<std::string> hostList;
  
  std::list<grpc::ServerAsyncResponseWriter<ServeClientResponse>> responders;
  int firstResponderIndex;
  ClientService::AsyncService clientService;

  // persistent state
  int currentTerm, votedFor;
  std::vector<LogEntry> log;

  // volatile state (all servers)
  int commitIndex, lastApplied, currentLeader;
  int electionTimeout;
  ServerState serverState;

  // volatile state (for leaders)
  std::vector<int> nextIndex, matchIndex;

  std::unique_ptr<StateMachineInterface> stateMachine;

  // values are in milliseconds
  const static int minElectionTimeout, maxElectionTimeout, heartbeatInterval;
};
