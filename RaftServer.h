#pragma once

#include "RaftInterface.h"
#include <memory>
#include <vector>
#include <mutex>
#include <random>

using std::unique_ptr;
using std::string;

enum class ServerState { Leader, Candidate, Follower };

class RaftServerImpl {
public:
  RaftServerImpl(
    int id,
    unique_ptr<RequestInterface> o_requestImpl,
    unique_ptr<ResponseInterface> o_responseImpl,
    unique_ptr<StateMachineInterface> o_stateMachine,
    unique_ptr<StorageInterface> o_storageImpl,
    unique_ptr<AlarmServiceInterface> o_alarmService,
    const std::vector<std::string>& hostList,
    const string& logFile
  );
  void Run();
  void HandleAppendEntries(
    int leaderTerm,
    int leaderId,
    int prevLogIndex,
    int prevLogTerm,
    const std::vector<LogEntry>& entries,
    int leaderCommit,
    int * responseTerm,
    bool * success
  );
  void HandleRequestVote(
    int candidateTerm,
    int candidateId,
    int lastLogIndex,
    int lastLogTerm,
    int * responseTerm,
    bool * voteGranted
  );
  void AppendEntriesCallback(int responseTerm, bool success);
  void RequestVoteCallback(int responseTerm, bool voteGranted);

private:
  void BecomeFollower();
  void BecomeCandidate();
  void BecomeLeader();
  void SendHeartbeat();

  int myId;
  unique_ptr<RequestInterface> requestImpl;
  unique_ptr<ResponseInterface> responseImpl;
  unique_ptr<StateMachineInterface> stateMachine;
  unique_ptr<StorageInterface> storageImpl;
  unique_ptr<AlarmServiceInterface> alarmService;

  std::mutex overallLock;
  bool mustBecomeCandidate;
  int hostCount;
  int votesGained;

  // persistent state
  int currentTerm, votedFor;
  std::vector<LogEntry> log;

  // volatile state (common)
  int commitIndex, lastApplied, currentLeader;
  ServerState serverState;

  // volatile state (valid if this object is leader)
  std::vector<int> nextIndex, matchIndex;

  const static int minElectionTimeout = 150000, maxElectionTimeout = 300000;
  std::default_random_engine generator;
  std::uniform_int_distribution<int> distribution;
  std::function<int(void)> PickElectionTimeout;
};
