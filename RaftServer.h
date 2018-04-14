#pragma once

#include <memory>
#include <vector>
#include <mutex>
#include <random>

#include <grpc/grpc.h>
#include <grpc++/server.h>
#include <grpc++/server_builder.h>
#include <grpc++/server_context.h>
#include <grpc++/security/server_credentials.h>
#include "raft.grpc.pb.h"

#include "Storage.h"

using std::string;
using grpc::Status;
using grpc::ServerContext;
using raft::Raft;
using raft::LogEntry;
using raft::AppendEntriesRequest;
using raft::AppendEntriesResponse;
using raft::RequestVoteRequest;
using raft::RequestVoteResponse;

enum class ServerState { Leader, Candidate, Follower };

class StateMachineInterface {
public:
  virtual void Apply(const string& command);
};

class RaftServer final : public Raft::Service {
public:
  RaftServer(
    int o_id,
    const std::vector<std::string>& hostList,
    const std::string& storageDir,
    std::unique_ptr<StateMachineInterface> o_stateMachine
  );
  void Run();

private:
  Status AppendEntries(ServerContext* context,
                                   const AppendEntriesRequest* request,
                                   AppendEntriesResponse *response) override;
  Status RequestVote(ServerContext* context,
                             const RequestVoteRequest* request,
                             RequestVoteResponse* response) override;

  void AppendEntriesCallback(int responseTerm, bool success);
  void RequestVoteCallback(int responseTerm, bool voteGranted);

  void BecomeFollower();
  void BecomeCandidate();
  void BecomeLeader();

  void SetAlarm(int after_ms);
  void AlarmCallback(int signum);
  void ResetElectionTimeout();

  int id;
  std::mutex overallLock;
  bool mustBecomeCandidate;
  int hostCount;
  int votesGained;
  Storage storage;

  std::vector<std::unique_ptr<Raft::Stub>> stubs;

  // persistent state
  int currentTerm, votedFor;
  std::vector<LogEntry> log;

  // volatile state (all servers)
  int commitIndex, lastApplied, currentLeader;
  ServerState serverState;

  // volatile state (for leaders)
  std::vector<int> nextIndex, matchIndex;

  std::unique_ptr<StateMachineInterface> stateMachine;

  const static int minElectionTimeout = 150000, maxElectionTimeout = 300000;
  const static int heartbeatInterval = 50000;
};
